/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 3 Point X. All rights reserved. */

/** \file
 * \ingroup depsgraph
 */

#pragma once

#include "pipeline.h"

namespace blender::deg {

class ViewLayerBuilderPipeline : public AbstractBuilderPipeline {
 public:
  ViewLayerBuilderPipeline(::Depsgraph *graph);

 protected:
  virtual void build_nodes(DepsgraphNodeBuilder &node_builder) override;
  virtual void build_relations(DepsgraphRelationBuilder &relation_builder) override;
};

}  // namespace blender::deg
