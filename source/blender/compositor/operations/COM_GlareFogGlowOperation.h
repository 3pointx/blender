/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 3 Point X. */

#pragma once

#include "COM_GlareBaseOperation.h"
#include "COM_NodeOperation.h"
#include "DNA_node_types.h"

namespace blender::compositor {

class GlareFogGlowOperation : public GlareBaseOperation {
 public:
  GlareFogGlowOperation() : GlareBaseOperation()
  {
  }

 protected:
  void generate_glare(float *data, MemoryBuffer *input_tile, const NodeGlare *settings) override;
};

}  // namespace blender::compositor
