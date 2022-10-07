/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#ifndef __LIGHT_TREE_H__
#define __LIGHT_TREE_H__

#include "scene/light.h"
#include "scene/scene.h"

#include "util/boundbox.h"
#include "util/types.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

/* Orientation Bounds
 *
 * Bounds the normal axis of the lights,
 * along with their emission profiles */
struct OrientationBounds {
  float3 axis;   /* normal axis of the light */
  float theta_o; /* angle bounding the normals */
  float theta_e; /* angle bounding the light emissions */

  __forceinline OrientationBounds()
  {
  }

  __forceinline OrientationBounds(const float3 &axis_, float theta_o_, float theta_e_)
      : axis(axis_), theta_o(theta_o_), theta_e(theta_e_)
  {
  }

  enum empty_t { empty = 0 };

  /* If the orientation bound is set to empty, the values are set to minumums
   * so that merging it with another non-empty orientation bound guarantees that
   * the return value is equal to non-empty orientation bound. */
  __forceinline OrientationBounds(empty_t)
      : axis(make_float3(0, 0, 0)), theta_o(FLT_MIN), theta_e(FLT_MIN)
  {
  }

  float calculate_measure() const;
};

OrientationBounds merge(const OrientationBounds &cone_a, const OrientationBounds &cone_b);

/* --------------------------------------------------------------------
 * Light Tree Construction
 *
 * The light tree construction is based off PBRT's BVH construction,
 * which first uses build nodes before converting to a more compact structure.
 */

/* Light Tree Primitive Info
 * Struct to keep track of primitives in a vector of primitives,
 * which is used when reordering the vector. */
struct LightTreePrimitiveInfo {
  BoundBox bbox;
  OrientationBounds bcone;
  float3 centroid;
  float energy;
  int prim_num;
};

/* Light Tree Primitive
 * Struct that indexes into the scene's triangle and light arrays. */
struct LightTreePrimitive {
  /* prim_id >= 0 is an index into an object's local triangle index,
   * otherwise -prim_id-1 is an index into device lights array. */
  int prim_id;

  /* The primitive is either a light or an emissive triangle. */
  union {
    int object_id;
    int lamp_id;
  };

  BoundBox calculate_bbox(Scene *scene) const;
  OrientationBounds calculate_bcone(Scene *scene) const;
  float calculate_energy(Scene *scene) const;
};

/* Light Tree Bucket Info
 * Struct used to determine splitting costs in the light BVH. */
struct LightTreeBucketInfo {
  LightTreeBucketInfo()
      : energy(0.0f), bbox(BoundBox::empty), bcone(OrientationBounds::empty), count(0)
  {
  }

  float energy; /* Total energy in the partition */
  BoundBox bbox;
  OrientationBounds bcone;
  int count;

  static const int num_buckets = 12;
};

/* Light Tree Build Node
 * Temporary build node when constructing light tree,
 * and is later converted into a more compact representation for device. */
struct LightTreeBuildNode {
  BoundBox bbox;
  OrientationBounds bcone;
  float energy;
  float energy_variance;
  LightTreeBuildNode *children[2];
  uint first_prim_index;
  uint num_lights;
  uint bit_trail;
  bool is_leaf;

  void init_leaf(uint offset,
                 uint n,
                 const BoundBox &b,
                 const OrientationBounds &c,
                 float e,
                 float e_var,
                 uint bits);
  void init_interior(LightTreeBuildNode *c0, LightTreeBuildNode *c1, uint bits);
};

/* Packed Light Tree Node
 * Compact representation of light tree node
 * that's actually used in the device */
struct PackedLightTreeNode {
  BoundBox bbox;
  OrientationBounds bcone;
  float energy;
  float energy_variance;
  union {
    int first_prim_index;   /* leaf nodes contain an index to first primitive. */
    int second_child_index; /* interior nodes contain an index to second child. */
  };
  int num_lights;
  bool is_leaf_node;

  /* The bit trail traces the traversal from the root to a leaf node.
   * A value of 0 denotes traversing left while a value of 1 denotes traversing right. */
  uint bit_trail;
};

/* Light BVH
 *
 * BVH-like data structure that keeps track of lights
 * and considers additional orientation and energy information */
class LightTree {
  vector<LightTreePrimitive> prims_;
  vector<PackedLightTreeNode> nodes_;
  Scene *scene_;
  uint max_lights_in_leaf_;

 public:
  LightTree(const vector<LightTreePrimitive> &prims, Scene *scene, uint max_lights_in_leaf);

  const vector<LightTreePrimitive> &get_prims() const;
  const vector<PackedLightTreeNode> &get_nodes() const;

 private:
  LightTreeBuildNode *recursive_build(vector<LightTreePrimitiveInfo> &primitive_info,
                                      int start,
                                      int end,
                                      int &total_nodes,
                                      vector<LightTreePrimitive> &ordered_prims,
                                      uint bit_trail,
                                      int depth);
  void split_saoh(const BoundBox &centroid_bounds,
                  const vector<LightTreePrimitiveInfo> &primitive_info,
                  int start,
                  int end,
                  const BoundBox &bbox,
                  const OrientationBounds &bcone,
                  float &min_cost,
                  int &min_dim,
                  int &min_bucket);
  int flatten_tree(const LightTreeBuildNode *node, int &offset, int parent);
};

CCL_NAMESPACE_END

#endif /* __LIGHT_TREE_H__ */
