/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "DRW_render.h"

#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "GPU_material.h"

#include "eevee_id_map.hh"

namespace blender::eevee {

class Instance;

/* -------------------------------------------------------------------- */
/** \name Default Material Nodetree
 *
 * In order to support materials without nodetree we reuse and configure a standalone nodetree that
 * we pass for shader generation. The GPUMaterial is still stored inside the Material even if
 * it does not use a nodetree.
 *
 * \{ */

class DefaultSurfaceNodeTree {
 private:
  bNodeTree *ntree_;
  bNodeSocketValueRGBA *color_socket_;
  bNodeSocketValueFloat *metallic_socket_;
  bNodeSocketValueFloat *roughness_socket_;
  bNodeSocketValueFloat *specular_socket_;

 public:
  DefaultSurfaceNodeTree();
  ~DefaultSurfaceNodeTree();

  bNodeTree *nodetree_get(::Material *ma);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material
 *
 * \{ */

struct MaterialPass {
  GPUMaterial *gpumat = nullptr;
  DRWShadingGroup *shgrp = nullptr;
};

struct Material {
  bool init = false;
  bool is_alpha_blend_transparent;
  MaterialPass shadow, shading, prepass;
};

struct MaterialArray {
  Vector<Material *> materials;
  Vector<GPUMaterial *> gpu_materials;
};

class MaterialModule {
 public:
  ::Material *diffuse_mat_;
  ::Material *glossy_mat_;

 private:
  Instance &inst_;

  Map<MaterialKey, Material *> material_map_;
  Map<ShaderKey, DRWShadingGroup **> shader_map_;

  MaterialArray material_array_;

  DefaultSurfaceNodeTree default_surface_ntree_;

  ::Material *error_mat_;

  int64_t queued_shaders_count_ = 0;

 public:
  MaterialModule(Instance &inst);
  ~MaterialModule();

  void begin_sync(void);

  MaterialArray &material_array_get(Object *ob);
  Material &material_get(Object *ob, int mat_nr, eMaterialGeometry geometry_type);

 private:
  Material &material_sync(::Material *blender_mat, eMaterialGeometry geometry_type);

  ::Material *material_from_slot(Object *ob, int slot);
  MaterialPass material_pass_get(::Material *blender_mat,
                                 eMaterialPipeline pipeline_type,
                                 eMaterialGeometry geometry_type);
};

/** \} */

}  // namespace blender::eevee