/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 3 Point X. */

#pragma once

#include "COM_Node.h"

namespace blender::compositor {

/**
 * \brief MathNode
 * \ingroup Node
 */
class MathNode : public Node {
 public:
  MathNode(bNode *editor_node) : Node(editor_node)
  {
  }
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
