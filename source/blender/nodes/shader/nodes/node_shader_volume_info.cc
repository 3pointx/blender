/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 3 Point X. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_volume_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>(N_("Color"));
  b.add_output<decl::Float>(N_("Density"));
  b.add_output<decl::Float>(N_("Flame"));
  b.add_output<decl::Float>(N_("Temperature"));
}

static int node_shader_gpu_volume_info(GPUMaterial *mat,
                                       bNode * /*node*/,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack * /*in*/,
                                       GPUNodeStack *out)
{
  if (out[0].hasoutput) {
    out[0].link = GPU_attribute(mat, CD_AUTO_FROM_NAME, "color");
    GPU_link(mat, "node_attribute_color", out[0].link, &out[0].link);
  }
  if (out[1].hasoutput) {
    out[1].link = GPU_attribute(mat, CD_AUTO_FROM_NAME, "density");
    GPU_link(mat, "node_attribute_density", out[1].link, &out[1].link);
  }
  if (out[2].hasoutput) {
    out[2].link = GPU_attribute(mat, CD_AUTO_FROM_NAME, "flame");
    GPU_link(mat, "node_attribute_flame", out[2].link, &out[2].link);
  }
  if (out[3].hasoutput) {
    out[3].link = GPU_attribute(mat, CD_AUTO_FROM_NAME, "temperature");
    GPU_link(mat, "node_attribute_temperature", out[3].link, &out[3].link);
  }

  return true;
}

}  // namespace blender::nodes::node_shader_volume_info_cc

void register_node_type_sh_volume_info()
{
  namespace file_ns = blender::nodes::node_shader_volume_info_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_VOLUME_INFO, "Volume Info", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_volume_info;

  nodeRegisterType(&ntype);
}
