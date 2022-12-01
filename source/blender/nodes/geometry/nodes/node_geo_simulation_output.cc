/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_compute_cache.hh"
#include "BKE_scene.h"

#include "DEG_depsgraph_query.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_simulation_output_cc {

NODE_STORAGE_FUNCS(NodeGeometrySimulationOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>(N_("Run"));
  b.add_input<decl::Geometry>(N_("Geometry"));
  b.add_output<decl::Bool>(N_("Started"));
  b.add_output<decl::Bool>(N_("Ended"));
  b.add_output<decl::Float>(N_("Elapsed Time"));
  b.add_output<decl::Geometry>(N_("Geometry"));
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "use_cache", 0, IFACE_("Persistent Cache"), ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometrySimulationOutput *data = MEM_cnew<NodeGeometrySimulationOutput>(__func__);
  data->use_persistent_cache = false;
  node->storage = data;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.lazy_require_input("Run")) {
    return;
  }

  const NodeGeometrySimulationOutput &storage = node_storage(params.node());
  const Scene *scene = DEG_get_input_scene(params.depsgraph());
  const float scene_ctime = BKE_scene_ctime_get(scene);
  const int scene_frame = int(scene_ctime);

  const GeoNodesLFUserData &lf_data = *params.user_data();
  bke::ComputeCaches &all_caches = *lf_data.modifier_data->cache_per_frame;
  bke::SimulationCache &cache = all_caches.ensure_for_context(lf_data.compute_context->hash());

  if (cache.geometry_per_frame.is_empty()) {
    if (params.lazy_output_is_required("Started")) {
      params.set_output("Started", false);
    }
  }
  else {
    if (params.lazy_output_is_required("Elapsed Time")) {
      params.set_output("Elapsed Time", scene_ctime - cache.geometry_per_frame.first().time);
    }
    if (params.lazy_output_is_required("Started")) {
      params.set_output("Started", true);
    }
  }

  const bool run = params.get_input<bool>("Run");
  if (run) {
    if (params.lazy_output_is_required("Ended")) {
      params.set_output("Ended", false);
    }
  }
  else {
    if (params.lazy_output_is_required("Ended")) {
      params.set_output("Ended", true);
    }
    if (const bke::GeometryCacheValue *data = cache.value_at_or_before_time(scene_frame)) {
      params.set_output("Geometry", data->geometry_set);
      params.set_input_unused("Geometry");
      return;
    }
  }

  if (const bke::GeometryCacheValue *data = cache.value_at_time(scene_frame)) {
    params.set_output("Geometry", data->geometry_set);
    params.set_input_unused("Geometry");
    return;
  }

  if (params.lazy_require_input("Geometry")) {
    return;
  }

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  geometry_set.ensure_owns_direct_data();
  /* TODO: The "Use cache" input should probably become a "Persistent Cache" option. */
  if (storage.use_persistent_cache || cache.geometry_per_frame.is_empty()) {
    /* If using the cache or there is no cached data yet, write the input in a new cache value. */
    cache.insert(geometry_set, scene_frame, scene_ctime);
  }
  else {
    /* If we aren't using the cache, overrite the cache to only store the last frame. */
    /* TODO: This breaks the elapsed time. */
    /* TODO: Should we allow turning on and off caching procedurally? In that case we wouldn't be
     * able to clear the whole cache, we would have to check if persistent caching was enabled for
     * the *last* frame and then decide to replace it or not. */
    cache.geometry_per_frame.clear();
    cache.insert(geometry_set, scene_frame, scene_ctime);
  }

  params.set_output("Geometry", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_simulation_output_cc

void register_node_type_geo_simulation_output()
{
  namespace file_ns = blender::nodes::node_geo_simulation_output_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_SIMULATION_OUTPUT, "Simulation Output", NODE_CLASS_INTERFACE);
  ntype.initfunc = file_ns::node_init;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  node_type_storage(&ntype,
                    "NodeGeometrySimulationOutput",
                    node_free_standard_storage,
                    node_copy_standard_storage);
  ntype.geometry_node_execute_supports_laziness = true;
  // ntype.declaration_is_dynamic = true;
  nodeRegisterType(&ntype);
}
