/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 3 Point X. All rights reserved. */

/** \file
 * \ingroup draw
 *
 * \brief Contains procedural GPU hair drawing methods.
 */

#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "DNA_customdata_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"

#include "GPU_batch.h"
#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "GPU_vertex_buffer.h"

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.h"

#include "draw_attributes.h"
#include "draw_cache_impl.h"
#include "draw_common.h"
#include "draw_manager.h"
#include "draw_pointcloud_private.hh"

static GPUVertBuf *g_dummy_vbo = nullptr;

void DRW_pointcloud_init()
{
  if (g_dummy_vbo == nullptr) {
    /* initialize vertex format */
    GPUVertFormat format = {0};
    uint dummy_id = GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);

    g_dummy_vbo = GPU_vertbuf_create_with_format_ex(
        &format, GPU_USAGE_STATIC | GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);

    const float vert[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_vertbuf_data_alloc(g_dummy_vbo, 1);
    GPU_vertbuf_attr_fill(g_dummy_vbo, dummy_id, vert);
  }
}

DRWShadingGroup *DRW_shgroup_pointcloud_create_sub(Object *object,
                                                   DRWShadingGroup *shgrp_parent,
                                                   GPUMaterial *gpu_material)
{
  PointCloud &pointcloud = *static_cast<PointCloud *>(object->data);

  DRWShadingGroup *shgrp = DRW_shgroup_create_sub(shgrp_parent);

  /* Fix issue with certain driver not drawing anything if there is no texture bound to
   * "ac", "au", "u" or "c". */
  DRW_shgroup_buffer_texture(shgrp, "u", g_dummy_vbo);
  DRW_shgroup_buffer_texture(shgrp, "au", g_dummy_vbo);
  DRW_shgroup_buffer_texture(shgrp, "c", g_dummy_vbo);
  DRW_shgroup_buffer_texture(shgrp, "ac", g_dummy_vbo);

  GPUVertBuf *pos_rad_buf = pointcloud_position_and_radius_get(&pointcloud);
  DRW_shgroup_buffer_texture(shgrp, "ptcloud_pos_rad_tx", pos_rad_buf);

  if (gpu_material != nullptr) {

    // const DRW_Attributes &attrs = cache->attr_used;
    // for (int i = 0; i < attrs.num_requests; i++) {
    //   const DRW_AttributeRequest &request = attrs.requests[i];

    //   char sampler_name[32];
    //   /* \note reusing curve attribute function. */
    //   drw_curves_get_attribute_sampler_name(request.attribute_name, sampler_name);

    //   GPUTexture *attribute_buf = DRW_pointcloud_evaluated_attribute(&pointcloud);
    //   if (!cache->attributes_buf[i]) {
    //     continue;
    //   }
    //   DRW_shgroup_buffer_texture_ref(shgrp, sampler_name, attribute_buf);
    // }

    /* Only single material supported for now. */
    GPUBatch **geom = pointcloud_surface_shaded_get(&pointcloud, &gpu_material, 1);
    DRW_shgroup_call(shgrp, geom[0], object);
  }
  else {
    GPUBatch *geom = pointcloud_surface_get(&pointcloud);
    DRW_shgroup_call(shgrp, geom, object);
  }
  return shgrp;
}

void DRW_pointcloud_free()
{
  GPU_VERTBUF_DISCARD_SAFE(g_dummy_vbo);
}
