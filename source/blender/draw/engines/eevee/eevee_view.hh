/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * A view is either:
 * - The entire main view.
 * - A fragment of the main view (for panoramic projections).
 * - A shadow map view.
 * - A lightprobe view (either planar, cubemap, irradiance grid).
 *
 * A pass is a container for scene data. It is view agnostic but has specific logic depending on
 * its type. Passes are shared between views.
 */

#pragma once

#include "GPU_framebuffer.h"
#include "GPU_texture.h"

#include "DRW_render.h"

#include "eevee_depth_of_field.hh"
#include "eevee_hizbuffer.hh"
#include "eevee_motion_blur.hh"
#include "eevee_raytracing.hh"
#include "eevee_renderpasses.hh"
#include "eevee_shader.hh"
#include "eevee_shading.hh"
#include "eevee_velocity.hh"

namespace blender::eevee {

class Instance;

/* -------------------------------------------------------------------- */
/** \name ShadingView
 *
 * Render the scene and fill all render passes data.
 * \{ */

class ShadingView {
 private:
  Instance &inst_;
  /** Static string pointer. Used as debug name and as UUID for texture pool. */
  const char *name_;
  /** Matrix to apply to the viewmat. */
  const float (*face_matrix_)[4];

  /** Post-fx modules. */
  DepthOfField dof_;
  MotionBlur mb_;
  Velocity velocity_;

  /** Raytracing persistent buffers. Only opaque and refraction can have surface tracing. */
  RaytraceBuffer rt_buffer_opaque_;
  RaytraceBuffer rt_buffer_refract_;

  /** Owned resources. */
  Framebuffer view_fb_;
  /** Draw resources. Not owned. */
  GPUTexture *combined_tx_ = nullptr;
  GPUTexture *depth_tx_ = nullptr;
  GPUTexture *postfx_tx_ = nullptr;

  /** Main views is created from the camera (or is from the viewport). It is not jittered. */
  DRWView *main_view_ = nullptr;
  /** Sub views is jittered versions or the main views. This allows jitter updates without trashing
   * the visibility culling cache. */
  DRWView *sub_view_ = nullptr;
  /** Same as sub_view_ but has Depth Of Field jitter applied. */
  DRWView *render_view_ = nullptr;

  /** Render size of the view. Can change between scene sample eval. */
  int2 extent_ = {-1, -1};

  bool is_enabled_ = false;

 public:
  ShadingView(Instance &inst, const char *name, const float (*face_matrix)[4])
      : inst_(inst),
        name_(name),
        face_matrix_(face_matrix),
        dof_(inst, name),
        mb_(inst, name),
        velocity_(inst, name),
        rt_buffer_opaque_(inst),
        rt_buffer_refract_(inst){};

  ~ShadingView(){};

  void init(void);

  void sync(int2 render_extent_);

  void render(void);

  GPUTexture *render_post(GPUTexture *input_tx);

 private:
  void update_view(void);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name LightProbeView
 *
 * Render the scene to a lightprobe target cubemap face.
 * \{ */

class LightProbeView {
 private:
  Instance &inst_;
  /** Static string pointer. Used as debug name and as UUID for texture pool. */
  const char *name_;
  /** Matrix to apply to the viewmat. */
  const float (*face_matrix_)[4];
  /** Raytracing persistent buffers. Only opaque and refraction can have surface tracing. */
  RaytraceBuffer rt_buffer_opaque_;
  RaytraceBuffer rt_buffer_refract_;
  /** Owned resources. */
  Framebuffer view_fb_;
  /** DRWView of this face. */
  DRWView *view_ = nullptr;
  /** Render size of the view. */
  int2 extent_ = {-1, -1};

  int layer_ = 0;
  bool is_only_background_ = false;

 public:
  LightProbeView(Instance &inst, const char *name, const float (*face_matrix)[4], int layer_)
      : inst_(inst),
        name_(name),
        face_matrix_(face_matrix),
        rt_buffer_opaque_(inst),
        rt_buffer_refract_(inst),
        layer_(layer_){};

  ~LightProbeView(){};

  void sync(Texture &color_tx,
            Texture &depth_tx,
            const float4x4 &winmat,
            const float4x4 &viewmat,
            bool is_only_background);

  void render(GPUTexture *depth_tx);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main View
 *
 * \{ */

/**
 * Container for all views needed to render the final image.
 * We might need up to 6 views for panoramic cameras.
 */
class MainView {
 private:
  /* WORKAROUND: Defining this as an array does not seems to work on GCC < 9.4.
   * It tries to use the copy constructor and fails because ShadingView is non-copyable and
   * non-movable. */
  ShadingView shading_views_0;
  ShadingView shading_views_1;
  ShadingView shading_views_2;
  ShadingView shading_views_3;
  ShadingView shading_views_4;
  ShadingView shading_views_5;
#define shading_views_ (&shading_views_0)
  /** Internal render size. */
  int render_extent_[2];

 public:
  MainView(Instance &inst)
      : shading_views_0(inst, "posX_view", cubeface_mat[0]),
        shading_views_1(inst, "negX_view", cubeface_mat[1]),
        shading_views_2(inst, "posY_view", cubeface_mat[2]),
        shading_views_3(inst, "negY_view", cubeface_mat[3]),
        shading_views_4(inst, "posZ_view", cubeface_mat[4]),
        shading_views_5(inst, "negZ_view", cubeface_mat[5])
  {
  }

  void init(const int2 full_extent_)
  {
    /* TODO(fclem) parameter hidden in experimental. We need to figure out mipmap bias to preserve
     * texture crispiness. */
    float resolution_scale = 1.0f;
    for (int i = 0; i < 2; i++) {
      render_extent_[i] = max_ii(1, roundf(full_extent_[i] * resolution_scale));
    }

    for (auto i : IndexRange(6)) {
      shading_views_[i].init();
    }
  }

  void sync(void)
  {
    for (auto i : IndexRange(6)) {
      shading_views_[i].sync(render_extent_);
    }
  }

  void render(void)
  {
    for (auto i : IndexRange(6)) {
      shading_views_[i].render();
    }
  }

#undef shading_views_
};

/** \} */

}  // namespace blender::eevee