/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 3 Point X. */

#pragma once

#include "COM_Node.h"
#include "DNA_node_types.h"

namespace blender::compositor {

/**
 * \brief TextureNode
 * \ingroup Node
 */
class TextureNode : public Node {
 public:
  TextureNode(bNode *editor_node);
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
