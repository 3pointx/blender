
/**
 * Virtual shadowmapping: Schedule phase for tilemaps.
 * This is the most complex part in the entire shadow pipeline.
 * This step will read each updated tilemap to see if any tile is both visible and to be
 * updated. If that is the case, it computes the bounds of the tiles to update and write it
 * in a texture to be read back by the CPU. This is a sync step that is the main performance
 * bottleneck of the pipeline.
 *
 * Unused tile might be reallocated at this stage.
 *
 * For each unallocated tile it will reserve a new page in the atlas. If the tile is to be
 * rendered, it will also write the tile copy coordinates required in another buffer.
 * This is also a slow part and should be improved in the future by moving the least amount of
 * tiles.
 */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_shadow_page_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_shadow_tilemap_lib.glsl)

void main()
{
  ShadowTileMapData tilemap_data = tilemaps[gl_GlobalInvocationID.z];
  int tilemap_idx = tilemap_data.index;
  int lod_max = tilemap_data.is_cubeface ? SHADOW_TILEMAP_LOD : 0;

  int lod_valid = 0;
  for (int lod = lod_max; lod >= 0; lod--) {
    ivec2 tile_co = ivec2(gl_GlobalInvocationID.xy) >> lod;
    int tile_index = (SHADOW_TILEMAP_RES / 2) * tile_co.y + tile_co.x;
    uint stride = 1u << lod;
    /* We load the same data for each thread covering the same LOD tile, but we avoid
     * allocating the same tile twice. This is because we need uniform control flow for the
     * barriers to be valid. */
    bool valid_thread = (gl_GlobalInvocationID.xy % stride) == uvec2(0);

    ivec2 texel = shadow_tile_coord_in_atlas(tile_co, tilemap_idx, lod);
    ShadowTileData tile = shadow_tile_data_unpack(imageLoad(tilemaps_img, texel).x);

    if (valid_thread) {
      if (tile.is_visible && tile.is_used && !tile.is_allocated) {
        /** Tile allocation. */
        int free_index = atomicAdd(infos.page_free_next, -1);
        if (free_index >= 0) {
          ivec2 owner_texel = ivec2(unpackUvec2x16(free_page_owners[free_index]));
          free_page_owners[free_index] = uint(-1);

          tile.page = shadow_tile_data_unpack(imageLoad(tilemaps_img, owner_texel).x).page;
          tile.do_update = true;
          tile.is_allocated = true;
          tile.is_cached = false;
          imageStore(tilemaps_img, texel, uvec4(shadow_tile_data_pack(tile)));

          const uint flag = SHADOW_TILE_IS_ALLOCATED | SHADOW_TILE_IS_CACHED;
          imageAtomicAnd(tilemaps_img, owner_texel, ~flag);
        }
        else {
          /* Well, hum ... you blew up your budget! */
        }
      }
    }

    barrier();

    /* Save highest quality valid lod for this thread. */
    if (tile.is_visible && tile.is_used) {
      lod_valid = lod;
    }
    else if (lod == 0) {
      /* If the tile is not used, store the valid LOD level. */
      /* This is tricky because the tile might be processed by another thread doing an allocation.
       * So we need to set the LOD using atomics. */
      uint lod_mask = 7u << 12u;
      uint lod_store = uint(lod_valid) << 12u;
      imageAtomicAnd(tilemaps_img, texel, ~lod_mask);
      imageAtomicOr(tilemaps_img, texel, lod_store);
    }

    /** Compute area to render and write to buffer for CPU to read. */
    {
      shared ivec2 min_tile;
      shared ivec2 max_tile;
      ivec2 tile_co = ivec2(gl_GlobalInvocationID.xy);

      if (gl_GlobalInvocationID.xy == uvec2(0)) {
        min_tile = ivec2(SHADOW_TILEMAP_RES - 1);
        max_tile = ivec2(0);
      }
      /* Makes initial value visible to other threads. */
      barrier();

      if (valid_thread && tile.do_update && tile.is_visible && tile.is_used) {
        atomicAdd(infos.page_updated_count, 1);
        atomicMin(min_tile.x, tile_co.x);
        atomicMin(min_tile.y, tile_co.y);
        atomicMax(max_tile.x, tile_co.x);
        atomicMax(max_tile.y, tile_co.y);
      }
      /* Makes final value visible to first threads. */
      barrier();

      if (gl_GlobalInvocationID.xy == uvec2(0)) {
        max_tile += 1;
        /* Must match the rcti structure. */
        ivec4 out_data = ivec4(min_tile.x, max_tile.x, min_tile.y, max_tile.y);
        imageStore(tilemap_rects_img, ivec2(lod, gl_GlobalInvocationID.z), out_data);
      }
    }
  }
}