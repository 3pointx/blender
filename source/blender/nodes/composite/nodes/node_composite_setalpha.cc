/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_material.h"

#include "VPC_gpu_material_node.hh"

#include "node_composite_util.hh"

/* **************** SET ALPHA ******************** */

namespace blender::nodes::node_composite_setalpha_cc {

static void cmp_node_setalpha_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("Alpha"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(1);
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_init_setalpha(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeSetAlpha *settings = MEM_cnew<NodeSetAlpha>(__func__);
  node->storage = settings;
  settings->mode = CMP_NODE_SETALPHA_MODE_APPLY;
}

static void node_composit_buts_set_alpha(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::viewport_compositor;

class SetAlphaGPUMaterialNode : public GPUMaterialNode {
 public:
  using GPUMaterialNode::GPUMaterialNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    const NodeSetAlpha *node_set_alpha = get_node_set_alpha();

    if (node_set_alpha->mode == CMP_NODE_SETALPHA_MODE_APPLY) {
      GPU_stack_link(material, &bnode(), "node_composite_set_alpha_apply", inputs, outputs);
      return;
    }

    GPU_stack_link(material, &bnode(), "node_composite_set_alpha_replace", inputs, outputs);
  }

  NodeSetAlpha *get_node_set_alpha()
  {
    return static_cast<NodeSetAlpha *>(bnode().storage);
  }
};

static GPUMaterialNode *get_compositor_gpu_material_node(DNode node)
{
  return new SetAlphaGPUMaterialNode(node);
}

}  // namespace blender::nodes::node_composite_setalpha_cc

void register_node_type_cmp_setalpha()
{
  namespace file_ns = blender::nodes::node_composite_setalpha_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SETALPHA, "Set Alpha", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_setalpha_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_set_alpha;
  node_type_init(&ntype, file_ns::node_composit_init_setalpha);
  node_type_storage(
      &ntype, "NodeSetAlpha", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_gpu_material_node = file_ns::get_compositor_gpu_material_node;

  nodeRegisterType(&ntype);
}
