/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * The shadow module manages shadow update tagging & shadow rendering.
 */

#include "BKE_global.h"
#include "BLI_rect.h"

#include "eevee_instance.hh"

#include "draw_manager_text.h"

#include <iostream>

namespace blender::eevee {

float ShadowTileMap::tile_cone_half_angle = atan(0.5 * M_SQRT2 / (SHADOW_TILEMAP_RES / 2));

/* -------------------------------------------------------------------- */
/** \name Tile map
 *
 * \{ */

void ShadowTileMap::sync_clipmap(const float3 &camera_position,
                                 const float4x4 &object_mat_,
                                 float near_,
                                 float far_,
                                 int2 new_offset,
                                 int clipmap_level)
{
#ifdef SHADOW_DEBUG_NO_CACHING
  set_dirty();
#endif
  if (is_cubeface || (level != clipmap_level) || (near != near_) || (far != far_)) {
    set_dirty();
  }
  is_cubeface = false;
  level = clipmap_level;
  near = near_;
  far = far_;
  cone_direction = float3(1.0f);
  cone_angle_cos = -2.0f;

  if (grid_shift == int2(0)) {
    /* Only replace shift if it is not already dirty. */
    grid_shift = new_offset - grid_offset;
  }
  grid_offset = new_offset;

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  float half_size = tilemap_coverage_get() / 2.0f;
  float tile_size = tile_size_get();
  float3 tilemap_center = object_mat *
                          float3(grid_offset.x * tile_size, grid_offset.y * tile_size, 0.0f);

  float4x4 viewinv = object_mat;
  copy_v3_v3(viewinv.values[3], tilemap_center);

  float camera_distance_to_plane = math::dot(float3(object_mat.values[2]), camera_position);
  float visible_near = camera_distance_to_plane - half_size;
  float visible_far = camera_distance_to_plane + half_size;

  /* Update corners. Used for visibility test of each tile. */
  *(float3 *)(&corners[0]) = viewinv * float3(-half_size, -half_size, visible_near);
  *(float3 *)(&corners[1]) = viewinv * float3(half_size, -half_size, visible_near);
  *(float3 *)(&corners[2]) = viewinv * float3(-half_size, half_size, visible_near);
  *(float3 *)(&corners[3]) = viewinv * float3(-half_size, -half_size, visible_far);
  /* Store deltas. */
  corners[1] = (corners[1] - corners[0]) / float(SHADOW_TILEMAP_RES);
  corners[2] = (corners[2] - corners[0]) / float(SHADOW_TILEMAP_RES);
  corners[3] -= corners[0];

  /* Usage depth range. Used for usage tagging. */
  float range = (far - near);
  /* Need to be after the corners arithmetic because they are stored inside the last component. */
  _min_usage_depth = clamp_f((2.0f * (-visible_far - near) / range) - 1.0f, -1.0f, 1.0f);
  _max_usage_depth = clamp_f((2.0f * (-visible_near - near) / range) - 1.0f, -1.0f, 1.0f);

  viewmat = viewinv.inverted_affine();
  winmat = winmat_get(nullptr);
  mul_m4_m4m4(tilemat.ptr(), tilemat_scale_bias_mat, winmat.ptr());
  tilemat = tilemat * viewmat;
}

void ShadowTileMap::sync_cubeface(
    const float4x4 &object_mat_, float near_, float far_, float cone_aperture, eCubeFace face)
{
#ifdef SHADOW_DEBUG_NO_CACHING
  set_dirty();
#endif
  if (!is_cubeface || (cubeface != face) || (near != near_) || (far != far_)) {
    set_dirty();
  }
  is_cubeface = true;
  cubeface = face;
  near = near_;
  far = far_;

  if (cone_aperture > DEG2RADF(180.0f)) {
    cone_angle_cos = -2.0;
  }
  else {
    cone_angle_cos = cosf(min_ff((cone_aperture * 0.5f) + tile_cone_half_angle, M_PI_2));
  }
  cone_direction = -float3(object_mat_.values[2]);

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  viewmat = float4x4(shadow_face_mat[cubeface]) * object_mat.inverted_affine();
  winmat = winmat_get(nullptr);
  mul_m4_m4m4(tilemat.ptr(), tilemat_scale_bias_mat, winmat.ptr());
  tilemat = tilemat * viewmat;

  /* Update corners. */
  float4x4 viewinv = viewmat.inverted();
  *reinterpret_cast<float3 *>(&corners[0]) = viewinv.translation();
  *reinterpret_cast<float3 *>(&corners[1]) = viewinv * float3(-far, -far, -far);
  *reinterpret_cast<float3 *>(&corners[2]) = viewinv * float3(far, -far, -far);
  *reinterpret_cast<float3 *>(&corners[3]) = viewinv * float3(-far, far, -far);
  /* Store deltas. */
  corners[2] = (corners[2] - corners[1]) / float(SHADOW_TILEMAP_RES);
  corners[3] = (corners[3] - corners[1]) / float(SHADOW_TILEMAP_RES);
  /* Need to be after the corners arithmetic because they are stored inside the last component. */
  _min_usage_depth = -1.0f;
  _max_usage_depth = 1.0f;
  _punctual_far = far_;
  _punctual_near = near_;
}

float4x4 ShadowTileMap::winmat_get(const rcti *tile_minmax) const
{
  float2 min = float2(-1.0f);
  float2 max = float2(1.0f);

  if (tile_minmax != nullptr) {
    min = shadow_tile_coord_to_ndc(int2(tile_minmax->xmin, tile_minmax->ymin));
    max = shadow_tile_coord_to_ndc(int2(tile_minmax->xmax, tile_minmax->ymax));
  }

  float4x4 winmat;
  if (is_cubeface) {
    perspective_m4(
        winmat.ptr(), near * min.x, near * max.x, near * min.y, near * max.y, near, far);
  }
  else {
    float half_size = tilemap_coverage_get() / 2.0f;
    orthographic_m4(winmat.ptr(),
                    half_size * min.x,
                    half_size * max.x,
                    half_size * min.y,
                    half_size * max.y,
                    near,
                    far);
  }
  return winmat;
}

void ShadowTileMap::setup_view(const rcti &rect, DRWView *&view) const
{
  float4x4 culling_mat = winmat_get(&rect);

  if (view == nullptr) {
    view = DRW_view_create(viewmat.ptr(), winmat.ptr(), nullptr, culling_mat.ptr(), nullptr);
  }
  else {
    DRW_view_update(view, viewmat.ptr(), winmat.ptr(), nullptr, culling_mat.ptr());
  }

#if 0 /* Debug. */
  float4 debug_color[6] = {
      {1, .1, .1, 1}, {.1, 1, .1, 1}, {0, .2, 1, 1}, {1, 1, .3, 1}, {.1, .1, .1, 1}, {1, 1, 1, 1}};
  float4 color = debug_color[((is_cubeface ? cubeface : level) + 9999) % 6];
  float4x4 persinv_culling = (culling_mat * viewmat).inverted();
  DRW_debug_m4_as_bbox(persinv_culling.values, color, false);
#endif
}

void ShadowTileMap::debug_draw(void) const
{
  /** Used for debug drawing. */
  float4 debug_color[6] = {
      {1, .1, .1, 1}, {.1, 1, .1, 1}, {0, .2, 1, 1}, {1, 1, .3, 1}, {.1, .1, .1, 1}, {1, 1, 1, 1}};
  float4 color = debug_color[((is_cubeface ? cubeface : level) + 9999) % 6];

  float4x4 winmat = winmat_get(nullptr);
  float persinv[4][4];
  mul_m4_m4m4(persinv, winmat.ptr(), viewmat.ptr());
  invert_m4(persinv);
  DRW_debug_m4_as_bbox(persinv, color, false);

  int64_t div = ShadowTileAllocator::maps_per_row;
  std::stringstream ss;
  ss << "[" << index % div << ":" << index / div << "]";
  std::string text = ss.str();

  float3 pos = float3(0.0f, 0.0f, (is_cubeface) ? 1.0f : 0.0f);
  mul_project_m4_v3(persinv, pos);

  uchar ucolor[4];
  rgba_float_to_uchar(ucolor, color);
  struct DRWTextStore *dt = DRW_text_cache_ensure();
  DRW_text_cache_add(dt, pos, text.c_str(), text.size(), 0, 0, DRW_TEXT_CACHE_GLOBALSPACE, ucolor);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile map allocator
 *
 * \{ */

ShadowTileAllocator::ShadowTileAllocator()
{
  for (auto &bit : usage_bitmap_) {
    bit = false;
  }

  /* Try to not have a very long texture since it is
   * potentially wasteful on most GPU using tiled memory. */
  int tilemap_res = ShadowTileMap::tile_map_resolution;
  int2 extent;
  extent.x = min_ii(size, maps_per_row) * tilemap_res;
  extent.y = (size / maps_per_row) * tilemap_res;
  /* Add half the height for LODs. */
  extent.y += extent.y / 2;

  tilemap_tx.ensure_2d(GPU_R32UI, extent);
  tilemap_tx.clear(uint4(0));

  /* Allocate one pixel for each tilemap and each lod. */
  tilemap_rects_tx.ensure_2d(GPU_RGBA32I, int2(SHADOW_TILEMAP_LOD + 1, size));
  tilemap_rects_tx.clear(int4(0));
}

ShadowTileAllocator::~ShadowTileAllocator()
{
  for (ShadowTileMap *map : maps) {
    delete map;
  }
}

/** Returns empty span on failure. */
Span<ShadowTileMap *> ShadowTileAllocator::alloc(int64_t count)
{
  int64_t candidate = -1;
  /* Iterate through the whole buffer starting from the last known alloc position. */
  for (int64_t j = 0; j < size; j++) {
    int64_t i = (next_index + j) % size;
    if (usage_bitmap_[i] == false) {
      if (candidate == -1) {
        candidate = i;
      }
      if (i - candidate + 1 == count) {
        int64_t start = maps.size();
        for (auto j : IndexRange(candidate, count)) {
          usage_bitmap_[j] = true;
          maps.append(new ShadowTileMap(j));
        }
        next_index = candidate + count;
        return maps.as_span().slice(IndexRange(start, count));
      }
    }
    else {
      candidate = -1;
    }
  }
  return Span<ShadowTileMap *>();
}

void ShadowTileAllocator::free(Vector<ShadowTileMap *> &free_list)
{
  for (ShadowTileMap *map : free_list) {
    maps.remove_first_occurrence_and_reorder(map);
    usage_bitmap_[map->index] = false;
    maps_deleted.append(map);
    /* Actual deletion happens in end_sync(). */
  }
  free_list.clear();
}

void ShadowTileAllocator::end_sync()
{
  active_maps_len = 0;
  for (ShadowTileMap *map : maps) {
    tilemaps_data[active_maps_len++] = *map;
  }

  deleted_maps_len = 0;
  for (ShadowTileMap *map : maps_deleted) {
    /* Push to the ShadowTileMapsDataBuf in order to release the tiles.
     * Only do that if the slot was not reused for another map. */
    if (usage_bitmap_[map->index] == false) {
      /* This will effectively release all pages since they will be marked to update but not
       * marked as visible. */
      map->set_dirty();
      tilemaps_data[active_maps_len + deleted_maps_len++] = *map;
    }
    delete map;
  }
  maps_deleted.clear();

  tilemaps_data.push_update();

  for (ShadowTileMap *map : maps) {
    map->set_updated();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Common
 *
 * \{ */

void ShadowCommon::free_resources()
{
  shadows_->tilemap_allocator.free(tilemaps);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Punctual
 *
 * \{ */

void ShadowPunctual::sync(eLightType light_type,
                          const float4x4 &object_mat,
                          float cone_aperture,
                          float near_clip,
                          float far_clip,
                          float bias)
{
  bool is_wide_cone = cone_aperture > DEG2RADF(90.0f);
  bool is_omni = cone_aperture > DEG2RADF(180.0f);

  far_ = max_ff(far_clip, 3e-4f);
  near_ = min_ff(near_clip, far_clip - 1e-4f);
  bias_ = bias;
  light_type_ = light_type;

  /* Keep custom data. */
  size_x_ = _area_size_x;
  size_y_ = _area_size_y;

  position_ = float3(object_mat[3]);
  random_offset_ = float3(0.0f);

  int face_needed = is_omni ? 6 : (is_wide_cone ? 5 : 1);
  if (tilemaps.size() != face_needed) {
    shadows_->tilemap_allocator.free(tilemaps);
    tilemaps = shadows_->tilemap_allocator.alloc(face_needed);
  }

  /* Clear embedded custom data. */
  float4x4 obmat_tmp = float4x4(object_mat);
  obmat_tmp.values[0][3] = obmat_tmp.values[1][3] = obmat_tmp.values[2][3] = 0.0f;
  obmat_tmp.values[3][3] = 1.0f;

  tilemaps[Z_NEG]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, Z_NEG);
  if (is_wide_cone) {
    tilemaps[X_POS]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, X_POS);
    tilemaps[X_NEG]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, X_NEG);
    tilemaps[Y_POS]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, Y_POS);
    tilemaps[Y_NEG]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, Y_NEG);
  }
  if (is_omni) {
    tilemaps[Z_POS]->sync_cubeface(obmat_tmp, near_, far_, cone_aperture, Z_POS);
  }
}

ShadowPunctual::operator ShadowData()
{
  ShadowData data;
  cubeface_winmat_get(data.mat, near_, far_);
  invert_m4(data.mat.ptr());
  data.offset = random_offset_;
  data.bias = bias_;
  data.clip_near = near_;
  data.clip_far = far_;
  data.tilemap_index = tilemaps.first()->index;
  data.tilemap_last = data.tilemap_index + tilemaps.size() - 1;
  return data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Directional Shadow Maps
 *
 * \{ */

void ShadowDirectional::sync(const float4x4 &object_mat, float bias, float min_resolution)
{
  object_mat_ = object_mat;
  /* Clear embedded custom data. */
  object_mat_.values[0][3] = object_mat_.values[1][3] = object_mat_.values[2][3] = 0.0f;
  object_mat_.values[3][3] = 1.0f;
  /* Remove translation. */
  zero_v3(object_mat_.values[3]);

  min_resolution_ = min_resolution;
  bias_ = bias;
}

void ShadowDirectional::end_sync(int min_level,
                                 int max_level,
                                 const float3 &camera_position,
                                 const AABB &casters_bounds,
                                 /* Ortho only. */
                                 const BoundBox &casters_visible,
                                 const Camera &camera)
{
  int user_min_level = floorf(log2(min_resolution_));

  /* FIXME(fclem): We center the clipmap around the camera position which is arbitrary and
   * can affect lightprobe shadowing quality. To fix, just change camera position during bake and
   * profit!!! */

  float3 z_axis = object_mat_[2];
  /* Near & far values used for rendering. Bounds the shadow casters. */
  near_ = 1.0e30f;
  far_ = -1.0e30f;
  BoundBox bbox = casters_bounds;
  for (auto i : IndexRange(8)) {
    float dist = -math::dot(z_axis, float3(bbox.vec[i]));
    near_ = min_ff(near_, dist);
    far_ = max_ff(far_, dist);
  }
  near_ -= 1e-8f;
  far_ += 1e-8f;

  if (camera.is_orthographic()) {
    /* Compute the unique clipmap level based on the coverage of the visible casters bounds. */
    float4x4 imat = object_mat_.inverted_affine();

    AABB local_visible;
    local_visible.init_min_max();
    for (auto i : IndexRange(8)) {
      local_visible.merge(imat * float3(casters_visible.vec[i]));
    }
    float3 extent = local_visible.extent();
    max_level = ceil(log2(max_ff(extent.x, extent.y)));
    min_level = max_level;
  }

  min_level = clamp_i(user_min_level, min_level, max_level);
  int level_count = max_level - min_level + 1;
  /* The maximum level count is bounded by the mantissa of a 32bit float. */
  if (level_count > 23) {
    level_count = 23;
    min_level = max_level - level_count + 1;
  }

  if (tilemaps.size() != level_count) {
    shadows_->tilemap_allocator.free(tilemaps);
    tilemaps = shadows_->tilemap_allocator.alloc(level_count);
  }
  ShadowTileMap &first_clipmap = *tilemaps.first();
  /* Meh... in order to make tile_size_get() work properly. */
  first_clipmap.set_level(min_level);
  first_clipmap.set_is_cubemap(false);

  /* Compute full offset from origin to the smallest clipmap tile size. */
  float tile_size = first_clipmap.tile_size_get();
  base_offset_ = int2(
      roundf(math::dot(float3(object_mat_.values[0]), camera_position) / tile_size),
      roundf(math::dot(float3(object_mat_.values[1]), camera_position) / tile_size));

  int level = min_level;
  int divisor = 1;
  for (ShadowTileMap *tilemap : tilemaps) {
    tilemap->sync_clipmap(
        camera_position, object_mat_, near_, far_, base_offset_ / divisor, level++);
    divisor <<= 1;
  }
  divisor >>= 1;
  /* Save only the offset from the first clipmap level to the last. */
  base_offset_ = base_offset_ - (base_offset_ / divisor) * divisor;
}

ShadowDirectional::operator ShadowData()
{
  ShadowData data;
  ShadowTileMap &last_level = *tilemaps.last();
  mul_m4_m4m4(data.mat.ptr(), shadow_clipmap_scale_mat, last_level.winmat.ptr());
  mul_m4_m4m4(data.mat.ptr(), data.mat.ptr(), last_level.viewmat.ptr());
  data.bias = bias_;
  data.clip_near = near_;
  data.clip_far = far_;
  data.base_offset = base_offset_;
  data.tilemap_index = tilemaps.first()->index;
  data.tilemap_last = data.tilemap_index + tilemaps.size() - 1;
  data.clipmap_lod_min = min_resolution_;
  data.clipmap_lod_max = min_resolution_ + tilemaps.size() - 1;
  return data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Module
 *
 * \{ */

void ShadowModule::init(void)
{
  /* TODO(fclem) New resolution parameter. */
  // if (cube_shadow_res_ != inst_.scene->eevee.shadow_cube_size) {
  //   inst_.sampling.reset();
  // }

  eGPUTextureFormat shadow_format = (inst_.scene->eevee.flag & SCE_EEVEE_SHADOW_HIGH_BITDEPTH) ?
                                        GPU_DEPTH_COMPONENT32F :
                                        GPU_DEPTH_COMPONENT16;

  if (shadow_format_ != shadow_format) {
    shadow_format_ = shadow_format;
    inst_.sampling.reset();

    int2 atlas_extent = int2(shadow_page_size_ * SHADOW_PAGE_PER_ROW);
    int2 render_extent = int2(shadow_page_size_ * SHADOW_TILEMAP_RES);

    /* Global update. */
    if ((atlas_tx_.is_valid() == false) || GPU_texture_format(atlas_tx_) != shadow_format_ ||
        GPU_texture_width(atlas_tx_) != atlas_extent.x ||
        GPU_texture_height(atlas_tx_) != atlas_extent.y) {
      for (ShadowTileMap *tilemap : tilemap_allocator.maps) {
        tilemap->set_dirty();
      }
    }

    /* TODO(fclem) GPU_DEPTH_COMPONENT16 support in copy shader? */
    /* TODO(fclem) Make allocation safe. */
    atlas_tx_.ensure_2d(GPU_R32F, atlas_extent);
    atlas_tx_.filter_mode(false);
#if DEBUG
    atlas_tx_.clear(float4(0.0f));
#endif
    /* Temporary render buffer. */
    render_tx_.ensure_2d(GPU_DEPTH_COMPONENT32F, render_extent);
    render_fb_.ensure(GPU_ATTACHMENT_TEXTURE(render_tx_));
  }

  const bool soft_shadow_enabled = (inst_.scene->eevee.flag & SCE_EEVEE_SHADOW_SOFT) != 0;
  if (soft_shadows_enabled_ != soft_shadow_enabled) {
    soft_shadows_enabled_ = soft_shadow_enabled;
    inst_.sampling.reset();
  }

#ifndef SHADOW_DEBUG_PAGE_ALLOCATION_ENABLED
  if (inst_.debug_mode == SHADOW_DEBUG_PAGE_ALLOCATION) {
    BLI_assert_msg(0,
                   "Error: EEVEE: SHADOW_DEBUG_PAGE_ALLOCATION used but "
                   "SHADOW_DEBUG_PAGE_ALLOCATION_ENABLED "
                   "is not defined");
  }
#endif

  tilemap_pixel_radius_ = M_SQRT2 * 2.0f / (SHADOW_TILEMAP_RES * shadow_page_size_);

  debug_data_.type = inst_.debug_mode;
}

void ShadowModule::begin_sync(void)
{
  casters_bounds_.init_min_max();
  receivers_non_opaque_ = DRW_call_buffer_create(&aabb_format_);
  casters_updated_ = DRW_call_buffer_create(&aabb_format_);

  for (int i = 0; i < ARRAY_SIZE(views_); i++) {
    views_[i] = nullptr;
  }
}

void ShadowModule::sync_object(Object *ob,
                               const ObjectHandle &handle,
                               bool is_shadow_caster,
                               bool is_alpha_blend)
{
#ifdef SHADOW_DEBUG_NO_DEPTH_SCAN
  is_alpha_blend = true;
#endif
  if (!is_shadow_caster && !is_alpha_blend) {
    return;
  }

  ShadowObject &shadow_ob = objects_.lookup_or_add_default(handle.object_key);
  shadow_ob.used = true;
  if (handle.recalc != 0 || !shadow_ob.initialized) {
    if (is_shadow_caster && shadow_ob.initialized) {
      DRW_buffer_add_entry_struct(casters_updated_, &shadow_ob.aabb);
    }
    shadow_ob.sync(ob);
    if (is_shadow_caster) {
      DRW_buffer_add_entry_struct(casters_updated_, &shadow_ob.aabb);
    }
  }

  if (is_shadow_caster) {
    casters_bounds_.merge(shadow_ob.aabb);
  }

  if (is_alpha_blend) {
    DRW_buffer_add_entry_struct(receivers_non_opaque_, &shadow_ob.aabb);
  }
}

void ShadowModule::end_sync(void)
{
  /* Search for deleted or updated shadow casters */
  Vector<ObjectKey, 0> deleted_keys;
  for (auto item : objects_.items()) {
    ShadowObject &shadow_ob = item.value;
    if (!shadow_ob.used) {
      deleted_keys.append(item.key);
      /* May not be a caster, but it does not matter, be conservative. */
      DRW_buffer_add_entry_struct(casters_updated_, &shadow_ob.aabb);
    }
    else {
      /* Clear for next sync. */
      shadow_ob.used = false;
    }
  }
  for (auto key : deleted_keys) {
    objects_.remove(key);
  }
  if (deleted_keys.size() > 0) {
    inst_.sampling.reset();
  }

  if (casters_bounds_.is_empty()) {
    /* Avoid problems down the road. */
    casters_bounds_ = AABB(1.0f);
  }

  /* Finish setting up the tilemaps. */
  punctuals.resize();
  directionals.resize();

  {
    struct State {
      float far_dist;
      float near_dist;
      float max_dimension;
      float min_dimension;
      float3 cam_position;
      /* Intersection between shadow casters bounds and the view frustum.
       * Used by orthographic directional shadows. */
      BoundBox bbox_visible;
    } state;

    state.cam_position = inst_.camera.position();

    /* Get the farthest point from camera to know what distance to cover. */
    float left, right, bottom, top;
    projmat_dimensions(inst_.camera.data_get().winmat.ptr(),
                       &left,
                       &right,
                       &bottom,
                       &top,
                       &state.near_dist,
                       &state.far_dist);

    if (inst_.camera.is_orthographic()) {
      /* Viewspace AABBs. */
      AABB aabb_frustum({left, bottom, state.near_dist}, {right, top, state.far_dist});
      AABB aabb_casters = inst_.camera.data_get().viewmat * casters_bounds_;
      /* Compute intersection. */
      /* TODO(@fclem): This could be refined to have tighter bounds. */
      /* Do intersection in viewspace as this gives us tighter bounds for this approximation. */
      AABB aabb_isect = AABB::intersect(aabb_frustum, aabb_casters);

      state.bbox_visible = inst_.camera.data_get().viewinv * aabb_isect;
      state.cam_position = math::midpoint(float3(state.bbox_visible.vec[0]),
                                          float3(state.bbox_visible.vec[6]));
    }

#ifdef SHADOW_DEBUG_FREEZE_CAMERA
    static bool valid = false;
    static State state_freeze = {};
    if (G.debug_value < 4 || !valid) {
      valid = true;
      state_freeze = state;
    }
    else {
      state = state_freeze;
    }
    debug_data_.camera_position = state.cam_position;
#endif

    int max_level = ceil(log2(state.far_dist));
    int min_level = floor(log2(state.near_dist));

    for (ShadowDirectional &directional : directionals) {
      directional.end_sync(min_level,
                           max_level,
                           state.cam_position,
                           casters_bounds_,
                           state.bbox_visible,
                           inst_.camera);
    }
  }

  tilemap_allocator.end_sync();

  last_processed_view = nullptr;

  /**
   * Tilemap Management
   */

  int64_t tilemaps_len = tilemap_allocator.active_maps_len;
  {
    tilemap_setup_ps_ = DRW_pass_create("ShadowTilemapSetup", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_SETUP);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_setup_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
    DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_bool(grp, "do_tilemap_setup", &do_tilemap_setup_, 1);
    int64_t tilemaps_updated_len = tilemaps_len + tilemap_allocator.deleted_maps_len;
    if (tilemaps_updated_len > 0) {
      DRW_shgroup_call_compute(grp, 1, 1, tilemaps_updated_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE);
    }
    do_tilemap_setup_ = true;

    if (G.debug & G_DEBUG_GPU) {
      debug_page_map_call(tilemap_setup_ps_);
    }
  }
  {
    tilemap_visibility_ps_ = DRW_pass_create("ShadowVisibilityTag", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_TAG_VISIBILITY);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_visibility_ps_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_float(grp, "tilemap_pixel_radius", &tilemap_pixel_radius_, 1);
    DRW_shgroup_uniform_float(grp, "screen_pixel_radius_inv", &screen_pixel_radius_inv_, 1);
    if (tilemaps_len > 0) {
      DRW_shgroup_call_compute(grp, 1, 1, tilemaps_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }
  {
    tilemap_usage_tag_ps_ = DRW_pass_create("ShadowUsageTag", (DRWState)0);

    GPUVertBuf *receivers_aabbs = DRW_call_buffer_as_vertbuf(receivers_non_opaque_);
    uint aabb_len = GPU_vertbuf_get_vertex_len(receivers_aabbs);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_TAG_USAGE);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_usage_tag_ps_);
    DRW_shgroup_vertex_buffer(grp, "aabbs_buf", receivers_aabbs);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_float(grp, "tilemap_pixel_radius", &tilemap_pixel_radius_, 1);
    DRW_shgroup_uniform_float(grp, "screen_pixel_radius_inv", &screen_pixel_radius_inv_, 1);
    DRW_shgroup_uniform_int_copy(grp, "aabb_len", aabb_len);
    if (tilemaps_len > 0 && aabb_len > 0) {
      uint group_len = divide_ceil_u(aabb_len, SHADOW_AABB_TAG_GROUP_SIZE);
      DRW_shgroup_call_compute(grp, group_len, 1, tilemaps_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }
  {
    tilemap_depth_scan_ps_ = DRW_pass_create("ShadowDepthScan", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_DEPTH_SCAN);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_depth_scan_ps_);
    DRW_shgroup_uniform_texture_ref(grp, "depth_tx", &input_depth_tx_);
    DRW_shgroup_storage_block_ref(grp, "lights_buf", &inst_.lights.culling_light_buf);
    DRW_shgroup_storage_block_ref(grp, "lights_cull_buf", &inst_.lights.culling_data);
    DRW_shgroup_storage_block_ref(grp, "lights_zbin_buf", &inst_.lights.culling_zbin_buf);
    DRW_shgroup_storage_block_ref(grp, "lights_tile_buf", &inst_.lights.culling_tile_buf);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_float(grp, "tilemap_pixel_radius", &tilemap_pixel_radius_, 1);
    DRW_shgroup_uniform_float(grp, "screen_pixel_radius_inv", &screen_pixel_radius_inv_, 1);
    DRW_shgroup_call_compute_ref(grp, scan_dispatch_size_);
    DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  {
    tilemap_update_tag_ps_ = DRW_pass_create("ShadowUpdateTag", (DRWState)0);

    GPUVertBuf *casters_aabbs = DRW_call_buffer_as_vertbuf(casters_updated_);
    uint aabb_len = GPU_vertbuf_get_vertex_len(casters_aabbs);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_TAG_UPDATE);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_update_tag_ps_);
    DRW_shgroup_vertex_buffer(grp, "aabbs_buf", casters_aabbs);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_int_copy(grp, "aabb_len", GPU_vertbuf_get_vertex_len(casters_aabbs));
    if (tilemaps_len > 0 && aabb_len > 0) {
      uint group_len = divide_ceil_u(aabb_len, SHADOW_AABB_TAG_GROUP_SIZE);
      DRW_shgroup_call_compute(grp, group_len, 1, tilemaps_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }
  {
    tilemap_lod_mask_ps_ = DRW_pass_create("ShadowLodMaskTag", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_TILE_LOD_MASK);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, tilemap_lod_mask_ps_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    if (tilemaps_len > 0) {
      DRW_shgroup_call_compute(grp, 1, 1, tilemaps_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }

  /**
   * Page Management
   */

  {
    page_init_ps_ = DRW_pass_create("ShadowPageInit", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_INIT);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_init_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
    DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_call_compute(grp, SHADOW_MAX_PAGE / SHADOW_PAGE_PER_ROW, 1, 1);
    DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE);

    if (G.debug & G_DEBUG_GPU) {
      debug_page_map_call(page_init_ps_);
    }
  }
  {
    page_free_ps_ = DRW_pass_create("ShadowPageFree", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_FREE);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_free_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
    DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    int64_t tilemaps_updated_len = tilemaps_len + tilemap_allocator.deleted_maps_len;
    if (tilemaps_updated_len > 0) {
      DRW_shgroup_call_compute(grp, 1, 1, tilemaps_updated_len);
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE);
    }

    if (G.debug & G_DEBUG_GPU) {
      debug_page_map_call(page_free_ps_);
    }
  }
  {
    page_defrag_ps_ = DRW_pass_create("ShadowPageDefrag", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_DEFRAG);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_defrag_ps_);
    DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_call_compute(grp, 1, 1, 1);
    DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE);
  }
  {
    page_alloc_ps_ = DRW_pass_create("ShadowPageAllocate", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_ALLOC);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_alloc_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
    DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_image(grp, "tilemap_rects_img", tilemap_allocator.tilemap_rects_tx);
    if (tilemaps_len > 0) {
      DRW_shgroup_call_compute(grp, 1, 1, tilemaps_len);
      eGPUBarrier barrier = GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE;
      barrier |= GPU_BARRIER_TEXTURE_FETCH;  /* Needed for ShadowPageMark / ShadowPageCopy. */
      barrier |= GPU_BARRIER_TEXTURE_UPDATE; /* Needed for readback. */
      DRW_shgroup_barrier(grp, barrier);
    }

    if (G.debug & G_DEBUG_GPU) {
      debug_page_map_call(page_alloc_ps_);
    }
  }
  {
    page_list_ps_ = DRW_pass_create("ShadowPageList", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_LIST);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_list_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_list_buf", pages_list_data_);
    DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_int(grp, "tilemap_lod", &rendering_lod_, 1);
    DRW_shgroup_uniform_int(grp, "tilemap_index", &rendering_tilemap_, 1);
    DRW_shgroup_call_compute(grp, 1, 1, 1);
    DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_STORAGE);
  }
  {
    DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS;
    page_mark_ps_ = DRW_pass_create("ShadowPageMark", state);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_MARK);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_mark_ps_);
    DRW_shgroup_uniform_texture(grp, "tilemaps_tx", tilemap_allocator.tilemap_tx);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_list_buf", pages_list_data_);
    DRW_shgroup_uniform_int(grp, "tilemap_lod", &rendering_lod_, 1);
    DRW_shgroup_uniform_int(grp, "tilemap_index", &rendering_tilemap_, 1);
    DRW_shgroup_clear_framebuffer(grp, GPU_DEPTH_BIT, 0, 0, 0, 0, 0.0f, 0x0);
    DRW_shgroup_call_procedural_triangles(grp, nullptr, square_i(SHADOW_TILEMAP_RES) * 2);
  }
  {
    page_copy_ps_ = DRW_pass_create("ShadowPageCopy", (DRWState)0);

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_COPY);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, page_copy_ps_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_list_buf", pages_list_data_);
    DRW_shgroup_uniform_texture(grp, "render_tx", render_tx_);
    DRW_shgroup_uniform_image(grp, "out_atlas_img", atlas_tx_);
    DRW_shgroup_uniform_int(grp, "tilemap_lod", &rendering_lod_, 1);
    DRW_shgroup_uniform_int(grp, "tilemap_index", &rendering_tilemap_, 1);
    DRW_shgroup_call_compute_ref(grp, copy_dispatch_size_);
    DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
  }

  debug_end_sync();
}

void ShadowModule::debug_page_map_call(DRWPass *pass)
{
  if (debug_data_.type == SHADOW_DEBUG_NONE) {
    return;
  }
  debug_page_tx_.ensure_2d(GPU_R32UI, int2(SHADOW_PAGE_PER_ROW));

  GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_PAGE_DEBUG);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, pass);
  DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
  DRW_shgroup_storage_block(grp, "pages_free_buf", pages_free_data_);
  DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
  DRW_shgroup_uniform_image(grp, "tilemaps_img", tilemap_allocator.tilemap_tx);
  DRW_shgroup_uniform_image(grp, "debug_img", debug_page_tx_);
  DRW_shgroup_call_compute(grp, 1, 1, 1);
  DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
}

void ShadowModule::debug_end_sync(void)
{
  debug_draw_ps_ = nullptr;
  debug_view_ = nullptr;

  if (debug_data_.type == SHADOW_DEBUG_NONE) {
    return;
  }

  const bool need_active_light_data = !ELEM(
      debug_data_.type, SHADOW_DEBUG_TILE_ALLOCATION, SHADOW_DEBUG_PAGE_ALLOCATION);

  if (need_active_light_data) {
    Object *obact = DRW_context_state_get()->obact;
    if (obact && (obact->type == OB_LAMP)) {
      /* Dangerous. But only used for debug. */
      debug_light_key = inst_.sync.sync_object(obact).object_key;
    }

    if (debug_light_key.ob == nullptr) {
      return;
    }

    LightModule &light_module = inst_.lights;
    if (light_module.lights_.contains(debug_light_key) == false) {
      return;
    }
    Light &light = light_module.lights_.lookup(debug_light_key);
    if (light.shadow_id == LIGHT_NO_SHADOW) {
      return;
    }

    debug_data_.light = light;
    if (light.type == LIGHT_SUN) {
      debug_data_.shadow = directionals[light.shadow_id];
    }
    else {
      debug_data_.shadow = punctuals[light.shadow_id];
    }
    /* Find index of tilemap data. */
    for (auto index : IndexRange(tilemap_allocator.size)) {
      if (debug_data_.shadow.tilemap_index == tilemap_allocator.tilemaps_data[index].index) {
        debug_data_.tilemap_data_index = index;
        break;
      }
    }
  }

  debug_data_.push_update();

  {
    DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                     DRW_STATE_BLEND_CUSTOM;
    debug_draw_ps_ = DRW_pass_create("ShadowDebugDraw", state);

    if (debug_data_.type == SHADOW_DEBUG_PAGE_ALLOCATION) {
      debug_page_map_call(debug_draw_ps_);
    }

    GPUShader *sh = inst_.shaders.static_shader_get(SHADOW_DEBUG);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, debug_draw_ps_);
    DRW_shgroup_storage_block(grp, "tilemaps_buf", tilemap_allocator.tilemaps_data);
    DRW_shgroup_uniform_texture(grp, "tilemaps_tx", tilemap_allocator.tilemap_tx);
    DRW_shgroup_uniform_texture_ref(grp, "depth_tx", &input_depth_tx_);
    DRW_shgroup_uniform_texture(grp, "atlas_tx", atlas_tx_);
    DRW_shgroup_uniform_block(grp, "debug", debug_data_);
    DRW_shgroup_storage_block(grp, "pages_infos_buf", pages_infos_data_);
    DRW_shgroup_storage_block(grp, "pages_cached_buf", pages_cached_data_);
    if (debug_data_.type == SHADOW_DEBUG_PAGE_ALLOCATION) {
      DRW_shgroup_uniform_texture(grp, "debug_page_tx", debug_page_tx_);
    }
    else {
      DRW_shgroup_uniform_texture(grp, "debug_page_tx", tilemap_allocator.tilemap_tx);
    }
    DRW_shgroup_call_procedural_triangles(grp, nullptr, 1);
  }
}

/* Update all shadow regions visible inside the view.
 * If called multiple time for the same view, it will only do the depth buffer scanning
 * to check any new opaque surfaces.
 * Needs to be called after LightModule::set_view(); */
void ShadowModule::set_view(const DRWView *view, GPUTexture *depth_tx)
{
  /* Only process each view once. */
  bool do_process_view = (view != last_processed_view);
  last_processed_view = view;

#if 0 /* TODO */
  bool force_update = false;
  if (soft_shadows_enabled_ && (inst_.sampling.sample_get() != last_sample_)) {
    force_update = true;
    last_sample_ = inst_.sampling.sample_get();
  }
  else {
    last_sample_ = 0;
  }
#endif

  int2 extent(GPU_texture_width(depth_tx), GPU_texture_height(depth_tx));
  input_depth_tx_ = depth_tx;

  scan_dispatch_size_.x = divide_ceil_u(extent.x, SHADOW_DEPTH_SCAN_GROUP_SIZE);
  scan_dispatch_size_.y = divide_ceil_u(extent.y, SHADOW_DEPTH_SCAN_GROUP_SIZE);
  scan_dispatch_size_.z = 1;

  DRW_view_set_active(view);

  float4x4 wininv;
  DRW_view_winmat_get(view, wininv.values, true);

  {
    /* Compute approximate screen pixel density (as world space radius). */
    float min_dim = float(min_ii(extent.x, extent.y));
    float3 p0 = float3(-1.0f, -1.0f, 0.0f);
    float3 p1 = float3(
        (min_dim / extent.x) * 2.0f - 1.0f, (min_dim / extent.y) * 2.0f - 1.0f, 0.0f);
    mul_project_m4_v3(wininv.values, p0);
    mul_project_m4_v3(wininv.values, p1);
    /* Compute radius at unit plane from the camera. */
    if (DRW_view_is_persp_get(view)) {
      p0 = p0 / p0.z;
      p1 = p1 / p1.z;
    }
    screen_pixel_radius_inv_ = min_dim / math::distance(p0, p1);
  }

#ifdef SHADOW_DEBUG_FREEZE_CAMERA
  static bool valid = false;
  static float4x4 viewmat_freezed;
  if (G.debug_value < 4 || !valid) {
    valid = true;
    DRW_view_viewmat_get(view, viewmat_freezed.values, false);
  }
  else {
    float4x4 winmat;
    DRW_view_winmat_get(view, winmat.values, false);
    debug_view_ = DRW_view_create(
        viewmat_freezed.values, winmat.values, nullptr, nullptr, nullptr);
    DRW_view_set_active(debug_view_);

    float4 color(1.0f);
    float4x4 persinv;
    DRW_view_persmat_get(debug_view_, persinv.values, true);
    DRW_debug_m4_as_bbox(persinv.values, color, false);
  }
#endif

  DRW_stats_group_start("ShadowUpdate");
  {
    if (G.debug & G_DEBUG_GPU) {
      /* Bind another framebuffer in order to avoid triggering the feedback loop check.
       * This is safe because we only use compute shaders in this section of the code.
       * Ideally the check should be smarter. */
      GPU_framebuffer_bind(render_fb_);
    }

    if (do_tilemap_setup_) {
      if (do_page_init_) {
#ifndef SHADOW_DEBUG_NO_CACHING
        do_page_init_ = false;
#endif
        tilemap_allocator.tilemap_tx.clear(uint4(0));
        DRW_draw_pass(page_init_ps_);
      }
    }
    /* Run every every time but only process tilemap update once. */
    DRW_draw_pass(tilemap_setup_ps_);
    if (do_tilemap_setup_) {
      DRW_draw_pass(tilemap_update_tag_ps_);
      do_tilemap_setup_ = false;
    }
    if (do_process_view) {
      DRW_draw_pass(tilemap_visibility_ps_);
      DRW_draw_pass(tilemap_usage_tag_ps_);
    }
#ifndef SHADOW_DEBUG_NO_DEPTH_SCAN
    if (input_depth_tx_ != nullptr) {
      DRW_draw_pass(tilemap_depth_scan_ps_);
    }
#endif
    DRW_draw_pass(tilemap_lod_mask_ps_);
    DRW_draw_pass(page_free_ps_);
    DRW_draw_pass(page_defrag_ps_);
    DRW_draw_pass(page_alloc_ps_);
  }
  DRW_stats_group_end();

  DRW_stats_group_start("ShadowRender");
  {
    /* Readback update list. Ugly sync point. */
    rcti *rect = tilemap_allocator.tilemap_rects_tx.read<rcti>(GPU_DATA_INT);

    Span<rcti> regions(rect, tilemap_allocator.maps.size() * (SHADOW_TILEMAP_LOD + 1));
    Span<ShadowTileMap *> tilemaps = tilemap_allocator.maps.as_span();

    int rect_idx = 0;
    while (!regions.is_empty()) {
      Vector<int, 6> regions_index;
      Vector<int, 6> regions_lod;

      /* Group updates by pack of 6. This is to workaround the current DRWView limitation.
       * Future goal is to have GPU culling and create the views on GPU. */
      while (!regions.is_empty() && regions_index.size() < 6) {
        int lod = rect_idx % (SHADOW_TILEMAP_LOD + 1);
        ShadowTileMap &tilemap = *tilemaps.first();
        if (!tilemap.is_cubeface && lod > 0) {
          /* Do not process lod for clipmaps as they are undefined. */
        }
        else if (!BLI_rcti_is_empty(&regions.first())) {
          tilemap.setup_view(regions.first(), views_[regions_index.size()]);
          regions_index.append(tilemap.index);
          regions_lod.append(lod);
        }
        rect_idx++;
        regions = regions.drop_front(1);
        if (lod == SHADOW_TILEMAP_LOD) {
          tilemaps = tilemaps.drop_front(1);
        }
      }

      for (auto i : regions_index.index_range()) {
        rendering_tilemap_ = regions_index[i];
        rendering_lod_ = regions_lod[i];

        DRW_draw_pass(page_list_ps_);

        DRW_view_set_active(views_[i]);
        GPU_framebuffer_bind(render_fb_);

        copy_dispatch_size_.x = shadow_page_size_ / SHADOW_PAGE_COPY_GROUP_SIZE;
        copy_dispatch_size_.y = copy_dispatch_size_.x;
        copy_dispatch_size_.z = 1;

        int viewport_size = render_tx_.width() >> rendering_lod_;
        GPU_framebuffer_viewport_set(render_fb_, 0, 0, viewport_size, viewport_size);

        DRW_draw_pass(page_mark_ps_);
        inst_.shading_passes.shadow.render();

        if (G.debug & G_DEBUG_GPU) {
          /* Bind another framebuffer in order to avoid triggering the feedback loop check.
           * This is safe because we only use compute shaders in this section of the code.
           * Ideally the check should be smarter. */
          GPU_framebuffer_restore();
        }

        DRW_draw_pass(page_copy_ps_);
      }
    }

    MEM_freeN(rect);
  }
  DRW_stats_group_end();

  DRW_view_set_active(view);
}

void ShadowModule::debug_draw(GPUFrameBuffer *view_fb, HiZBuffer &hiz)
{
  if (debug_draw_ps_ == nullptr) {
    return;
  }
  input_depth_tx_ = hiz.texture_get();

  const DRWView *view = DRW_view_get_active();

  if (debug_view_ != nullptr) {
    DRW_view_set_active(debug_view_);
  }

  GPU_framebuffer_bind(view_fb);
  DRW_draw_pass(debug_draw_ps_);

  DRW_view_set_active(view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Pass
 *
 * \{ */

void ShadowPass::sync(void)
{
  {
    DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS | DRW_STATE_SHADOW_OFFSET;
    surface_ps_ = DRW_pass_create("ShadowSurface", state);
  }
}

DRWShadingGroup *ShadowPass::material_add(::Material *UNUSED(material), GPUMaterial *gpumat)
{
  DRWShadingGroup *grp = DRW_shgroup_material_create(gpumat, surface_ps_);
  return grp;
}

void ShadowPass::render(void)
{
  DRW_draw_pass(surface_ps_);
}

/** \} */

}  // namespace blender::eevee
