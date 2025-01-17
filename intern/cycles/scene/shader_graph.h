/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 3 Point X */

#ifndef __GRAPH_H__
#define __GRAPH_H__

#include "graph/node.h"
#include "graph/node_type.h"

#include "kernel/types.h"

#include "util/list.h"
#include "util/map.h"
#include "util/param.h"
#include "util/set.h"
#include "util/types.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class AttributeRequestSet;
class Scene;
class Shader;
class ShaderInput;
class ShaderOutput;
class ShaderNode;
class ShaderGraph;
class SVMCompiler;
class OSLCompiler;
class OutputNode;
class ConstantFolder;
class MD5Hash;

/* Bump
 *
 * For bump mapping, a node may be evaluated multiple times, using different
 * samples to reconstruct the normal, this indicates the sample position */

enum ShaderBump { SHADER_BUMP_NONE, SHADER_BUMP_CENTER, SHADER_BUMP_DX, SHADER_BUMP_DY };

/* Identifiers for some special node types.
 *
 * The graph needs to identify these in the clean function.
 * Cannot use dynamic_cast, as this is disabled for OSL. */

enum ShaderNodeSpecialType {
  SHADER_SPECIAL_TYPE_NONE,
  SHADER_SPECIAL_TYPE_PROXY,
  SHADER_SPECIAL_TYPE_AUTOCONVERT,
  SHADER_SPECIAL_TYPE_GEOMETRY,
  SHADER_SPECIAL_TYPE_OSL,
  SHADER_SPECIAL_TYPE_IMAGE_SLOT,
  SHADER_SPECIAL_TYPE_CLOSURE,
  SHADER_SPECIAL_TYPE_COMBINE_CLOSURE,
  SHADER_SPECIAL_TYPE_OUTPUT,
  SHADER_SPECIAL_TYPE_BUMP,
  SHADER_SPECIAL_TYPE_OUTPUT_AOV,
};

/* Input
 *
 * Input socket for a shader node. May be linked to an output or not. If not
 * linked, it will either get a fixed default value, or e.g. a texture
 * coordinate. */

class ShaderInput {
 public:
  ShaderInput(const SocketType &socket_type_, ShaderNode *parent_)
      : socket_type(socket_type_),
        parent(parent_),
        link(NULL),
        stack_offset(SVM_STACK_INVALID),
        constant_folded_in(false)
  {
  }

  ustring name() const
  {
    return socket_type.ui_name;
  }
  int flags() const
  {
    return socket_type.flags;
  }
  SocketType::Type type() const
  {
    return socket_type.type;
  }

  void set(float f)
  {
    ((Node *)parent)->set(socket_type, f);
  }
  void set(float3 f)
  {
    ((Node *)parent)->set(socket_type, f);
  }

  void disconnect();

  const SocketType &socket_type;
  ShaderNode *parent;
  ShaderOutput *link;
  int stack_offset; /* for SVM compiler */

  /* Keeps track of whether a constant was folded in this socket, to avoid over-optimizing when the
   * link is null. */
  bool constant_folded_in;
};

/* Output
 *
 * Output socket for a shader node. */

class ShaderOutput {
 public:
  ShaderOutput(const SocketType &socket_type_, ShaderNode *parent_)
      : socket_type(socket_type_), parent(parent_), stack_offset(SVM_STACK_INVALID)
  {
  }

  ustring name() const
  {
    return socket_type.ui_name;
  }
  SocketType::Type type() const
  {
    return socket_type.type;
  }

  void disconnect();

  const SocketType &socket_type;
  ShaderNode *parent;
  vector<ShaderInput *> links;
  int stack_offset; /* for SVM compiler */
};

/* Node
 *
 * Shader node in graph, with input and output sockets. This is the virtual
 * base class for all node types. */

class ShaderNode : public Node {
 public:
  explicit ShaderNode(const NodeType *type);
  virtual ~ShaderNode();

  void create_inputs_outputs(const NodeType *type);
  void remove_input(ShaderInput *input);

  ShaderInput *input(const char *name);
  ShaderOutput *output(const char *name);
  ShaderInput *input(ustring name);
  ShaderOutput *output(ustring name);

  virtual ShaderNode *clone(ShaderGraph *graph) const = 0;
  virtual void attributes(Shader *shader, AttributeRequestSet *attributes);
  virtual void compile(SVMCompiler &compiler) = 0;
  virtual void compile(OSLCompiler &compiler) = 0;

  /* Expand node into additional nodes. */
  virtual void expand(ShaderGraph * /* graph */)
  {
  }

  /* ** Node optimization ** */
  /* Check whether the node can be replaced with single constant. */
  virtual void constant_fold(const ConstantFolder & /*folder*/)
  {
  }

  /* Simplify settings used by artists to the ones which are simpler to
   * evaluate in the kernel but keep the final result unchanged.
   */
  virtual void simplify_settings(Scene * /*scene*/){};

  virtual bool has_surface_emission()
  {
    return false;
  }
  virtual bool has_surface_transparent()
  {
    return false;
  }
  virtual bool has_surface_bssrdf()
  {
    return false;
  }
  virtual bool has_bump()
  {
    return false;
  }
  virtual bool has_bssrdf_bump()
  {
    return false;
  }
  virtual bool has_spatial_varying()
  {
    return false;
  }
  virtual bool has_attribute_dependency()
  {
    return false;
  }
  virtual bool has_integrator_dependency()
  {
    return false;
  }
  virtual bool has_volume_support()
  {
    return false;
  }
  vector<ShaderInput *> inputs;
  vector<ShaderOutput *> outputs;

  int id;          /* index in graph node array */
  ShaderBump bump; /* for bump mapping utility */

  ShaderNodeSpecialType special_type; /* special node type */

  /* ** Selective nodes compilation ** */

  /* TODO(sergey): More explicitly mention in the function names
   * that those functions are for selective compilation only?
   */

  /* Node feature are used to disable huge nodes inside the group,
   * so it's possible to disable huge nodes inside of the required
   * nodes group.
   */
  virtual int get_feature()
  {
    return bump == SHADER_BUMP_NONE ? 0 : KERNEL_FEATURE_NODE_BUMP;
  }

  /* Get closure ID to which the node compiles into. */
  virtual ClosureType get_closure_type()
  {
    return CLOSURE_NONE_ID;
  }

  /* Check whether settings of the node equals to another one.
   *
   * This is mainly used to check whether two nodes can be merged
   * together. Meaning, runtime stuff like node id and unbound slots
   * will be ignored for comparison.
   *
   * NOTE: If some node can't be de-duplicated for whatever reason it
   * is to be handled in the subclass.
   */
  virtual bool equals(const ShaderNode &other);
};

/* Node definition utility macros */

#define SHADER_NODE_CLASS(type) \
  NODE_DECLARE \
  type(); \
  virtual ShaderNode *clone(ShaderGraph *graph) const \
  { \
    return graph->create_node<type>(*this); \
  } \
  virtual void compile(SVMCompiler &compiler); \
  virtual void compile(OSLCompiler &compiler);

#define SHADER_NODE_NO_CLONE_CLASS(type) \
  NODE_DECLARE \
  type(); \
  virtual void compile(SVMCompiler &compiler); \
  virtual void compile(OSLCompiler &compiler);

#define SHADER_NODE_BASE_CLASS(type) \
  virtual ShaderNode *clone(ShaderGraph *graph) const \
  { \
    return graph->create_node<type>(*this); \
  } \
  virtual void compile(SVMCompiler &compiler); \
  virtual void compile(OSLCompiler &compiler);

class ShaderNodeIDComparator {
 public:
  bool operator()(const ShaderNode *n1, const ShaderNode *n2) const
  {
    return n1->id < n2->id;
  }
};

typedef set<ShaderNode *, ShaderNodeIDComparator> ShaderNodeSet;
typedef map<ShaderNode *, ShaderNode *, ShaderNodeIDComparator> ShaderNodeMap;

/* Graph
 *
 * Shader graph of nodes. Also does graph manipulations for default inputs,
 * bump mapping from displacement, and possibly other things in the future. */

class ShaderGraph : public NodeOwner {
 public:
  list<ShaderNode *> nodes;
  size_t num_node_ids;
  bool finalized;
  bool simplified;
  string displacement_hash;

  ShaderGraph();
  ~ShaderGraph();

  ShaderNode *add(ShaderNode *node);
  OutputNode *output();

  void connect(ShaderOutput *from, ShaderInput *to);
  void disconnect(ShaderOutput *from);
  void disconnect(ShaderInput *to);
  void relink(ShaderInput *from, ShaderInput *to);
  void relink(ShaderOutput *from, ShaderOutput *to);
  void relink(ShaderNode *node, ShaderOutput *from, ShaderOutput *to);

  void remove_proxy_nodes();
  void compute_displacement_hash();
  void simplify(Scene *scene);
  void finalize(Scene *scene,
                bool do_bump = false,
                bool do_simplify = false,
                bool bump_in_object_space = false);

  int get_num_closures();

  void dump_graph(const char *filename);

  /* This function is used to create a node of a specified type instead of
   * calling 'new', and sets the graph as the owner of the node.
   */
  template<typename T, typename... Args> T *create_node(Args &&...args)
  {
    T *node = new T(args...);
    node->set_owner(this);
    return node;
  }

  /* This function is used to delete a node created and owned by the graph.
   */
  template<typename T> void delete_node(T *node)
  {
    assert(node->get_owner() == this);
    delete node;
  }

 protected:
  typedef pair<ShaderNode *const, ShaderNode *> NodePair;

  void find_dependencies(ShaderNodeSet &dependencies, ShaderInput *input);
  void clear_nodes();
  void copy_nodes(ShaderNodeSet &nodes, ShaderNodeMap &nnodemap);

  void break_cycles(ShaderNode *node, vector<bool> &visited, vector<bool> &on_stack);
  void bump_from_displacement(bool use_object_space);
  void refine_bump_nodes();
  void expand();
  void default_inputs(bool do_osl);
  void transform_multi_closure(ShaderNode *node, ShaderOutput *weight_out, bool volume);

  /* Graph simplification routines. */
  void clean(Scene *scene);
  void constant_fold(Scene *scene);
  void simplify_settings(Scene *scene);
  void deduplicate_nodes();
  void verify_volume_output();
};

CCL_NAMESPACE_END

#endif /* __GRAPH_H__ */
