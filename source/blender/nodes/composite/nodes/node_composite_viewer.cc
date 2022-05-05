/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BKE_global.h"
#include "BKE_image.h"

#include "RNA_access.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_state.h"
#include "GPU_texture.h"

#include "VPC_node_operation.hh"

#include "node_composite_util.hh"

/* **************** VIEWER ******************** */

namespace blender::nodes::node_composite_viewer_cc {

static void cmp_node_viewer_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>(N_("Alpha")).default_value(1.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>(N_("Z")).default_value(1.0f).min(0.0f).max(1.0f);
}

static void node_composit_init_viewer(bNodeTree *UNUSED(ntree), bNode *node)
{
  ImageUser *iuser = MEM_cnew<ImageUser>(__func__);
  node->storage = iuser;
  iuser->sfra = 1;
  node->custom3 = 0.5f;
  node->custom4 = 0.5f;

  node->id = (ID *)BKE_image_ensure_viewer(G.main, IMA_TYPE_COMPOSITE, "Viewer Node");
}

static void node_composit_buts_viewer(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "use_alpha", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

static void node_composit_buts_viewer_ex(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayout *col;

  uiItemR(layout, ptr, "use_alpha", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "tile_order", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  if (RNA_enum_get(ptr, "tile_order") == 0) {
    col = uiLayoutColumn(layout, true);
    uiItemR(col, ptr, "center_x", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
    uiItemR(col, ptr, "center_y", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  }
}

using namespace blender::viewport_compositor;

class ViewerOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_image = get_input("Image");
    GPUTexture *viewport_texture = context().get_viewport_texture();

    /* If the input image is a texture, copy the input texture to the viewport texture. */
    if (input_image.is_texture()) {
      /* Make sure any prior writes to the texture are reflected before copying it. */
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

      GPU_texture_copy(viewport_texture, input_image.texture());
    }
    else {
      /* Otherwise, if the input image is a single color value, clear the viewport texture to that
       * color. */
      GPU_texture_clear(viewport_texture, GPU_DATA_FLOAT, input_image.get_color_value());
    }
  }

  /* The operation domain have the same dimensions of the viewport without any transformations. */
  Domain compute_domain() override
  {
    return Domain(context().get_viewport_size());
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new ViewerOperation(context, node);
}

}  // namespace blender::nodes::node_composite_viewer_cc

void register_node_type_cmp_viewer()
{
  namespace file_ns = blender::nodes::node_composite_viewer_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_VIEWER, "Viewer", NODE_CLASS_OUTPUT);
  ntype.declare = file_ns::cmp_node_viewer_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_viewer;
  ntype.draw_buttons_ex = file_ns::node_composit_buts_viewer_ex;
  ntype.flag |= NODE_PREVIEW;
  node_type_init(&ntype, file_ns::node_composit_init_viewer);
  node_type_storage(&ntype, "ImageUser", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  ntype.no_muting = true;

  nodeRegisterType(&ntype);
}
