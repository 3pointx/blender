/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 3 Point X. */

#pragma once

#include "COM_Node.h"

namespace blender::compositor {

/**
 * \brief DenoiseNode
 * \ingroup Node
 */
class DenoiseNode : public Node {
 public:
  DenoiseNode(bNode *editor_node);
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
