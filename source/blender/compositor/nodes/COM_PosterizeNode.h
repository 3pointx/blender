/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 3 Point X. */

#pragma once

#include "COM_Node.h"

namespace blender::compositor {

/**
 * \brief PosterizeNode
 * \ingroup Node
 */
class PosterizeNode : public Node {
 public:
  PosterizeNode(bNode *editor_node);
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
