/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_assert.h"
#include "BLI_transformation_2d.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "VPC_node_operation.hh"

#include "node_composite_util.hh"

/* **************** Rotate  ******************** */

namespace blender::nodes::node_composite_rotate_cc {

static void cmp_node_rotate_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("Degr"))
      .default_value(0.0f)
      .min(-10000.0f)
      .max(10000.0f)
      .subtype(PROP_ANGLE)
      .compositor_expects_single_value();
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_init_rotate(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = 1; /* Bilinear Filter. */
}

static void node_composit_buts_rotate(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "filter_type", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

using namespace blender::viewport_compositor;

class RotateOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &input = get_input("Image");
    Result &result = get_result("Image");
    input.pass_through(result);

    const float rotation = get_input("Degr").get_float_value_default(0.0f);

    const Transformation2D transformation = Transformation2D::from_rotation(rotation);

    result.transform(transformation);
    result.get_realization_options().interpolation = get_interpolation();
  }

  Interpolation get_interpolation()
  {
    switch (bnode().custom1) {
      case 0:
        return Interpolation::Nearest;
      case 1:
        return Interpolation::Bilinear;
      case 2:
        return Interpolation::Bicubic;
    }

    BLI_assert_unreachable();
    return Interpolation::Nearest;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new RotateOperation(context, node);
}

}  // namespace blender::nodes::node_composite_rotate_cc

void register_node_type_cmp_rotate()
{
  namespace file_ns = blender::nodes::node_composite_rotate_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_ROTATE, "Rotate", NODE_CLASS_DISTORT);
  ntype.declare = file_ns::cmp_node_rotate_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_rotate;
  node_type_init(&ntype, file_ns::node_composit_init_rotate);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
