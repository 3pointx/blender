/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2012 3 Point X. */

#pragma once

#include "COM_Node.h"

namespace blender::compositor {

/**
 * \brief ConvertAlphaNode
 * \ingroup Node
 */
class ConvertAlphaNode : public Node {
 public:
  ConvertAlphaNode(bNode *editor_node) : Node(editor_node)
  {
  }
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
