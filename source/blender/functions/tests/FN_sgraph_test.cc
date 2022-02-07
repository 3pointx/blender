/* Apache License, Version 2.0 */

#include "testing/testing.h"

#include "FN_sgraph.hh"
#include "FN_sgraph_evaluate.hh"
#include "FN_sgraph_to_dot.hh"

namespace blender::fn::sgraph::tests {

struct ExampleSGraphAdapter {
  using NodeID = int;

  int node_inputs_size(const NodeID &node) const
  {
    switch (node) {
      case 1:
        return 2;
      case 2:
        return 2;
      case 3:
        return 3;
    }
    BLI_assert_unreachable();
    return 0;
  }

  int node_outputs_size(const NodeID &node) const
  {
    switch (node) {
      case 1:
        return 2;
      case 2:
        return 1;
      case 3:
        return 3;
    }
    BLI_assert_unreachable();
    return 0;
  }

  template<typename F> void foreach_node(const F &f) const
  {
    f(1);
    f(2);
    f(3);
  }

  template<typename F>
  void foreach_linked_input(const NodeID &node, const int output_socket_index, const F &f) const
  {
    switch (node * 1000 + output_socket_index) {
      case 1000:
        f(2, 0);
        break;
      case 1001:
        f(2, 1);
        f(3, 2);
        break;
      case 2000:
        f(3, 0);
        break;
      default:
        break;
    }
  }

  template<typename F>
  void foreach_linked_output(const NodeID &node, const int input_socket_index, const F &f) const
  {
    switch (node * 1000 + input_socket_index) {
      case 2000:
        f(1, 0);
        break;
      case 2001:
        f(1, 1);
        break;
      case 3000:
        f(2, 0);
        break;
      case 3002:
        f(1, 1);
        break;
      default:
        break;
    }
  }

  std::string node_debug_name(const NodeID &node) const
  {
    return std::to_string(node);
  }

  std::string input_socket_debug_name(const NodeID &node, const int input_socket_index) const
  {
    return std::to_string(node * 1000 + input_socket_index);
  }

  std::string output_socket_debug_name(const NodeID &node, const int output_socket_index) const
  {
    return std::to_string(node * 1000 + output_socket_index);
  }
};

class ExampleExecutor {
 public:
  SGraphT<ExampleSGraphAdapter> graph_;
  using NodeID = ExampleSGraphAdapter::NodeID;

  const CPPType *input_socket_type(const NodeID &UNUSED(node), const int UNUSED(input_index)) const
  {
    return &CPPType::get<int>();
  }

  const CPPType *output_socket_type(const NodeID &UNUSED(node),
                                    const int UNUSED(output_index)) const
  {
    return &CPPType::get<int>();
  }

  void load_unlinked_single_input(const NodeID &UNUSED(node),
                                  const int UNUSED(input_index),
                                  GMutablePointer r_value) const
  {
    *r_value.get<int>() = 2;
  }

  bool is_multi_input(const NodeID &UNUSED(node), const int UNUSED(input_index)) const
  {
    return false;
  }

  template<typename F>
  void foreach_always_required_input_index(const NodeID node, const F &f) const
  {
    for (const int input_index : IndexRange(graph_.adapter().node_inputs_size(node))) {
      f(input_index);
    }
  }

  std::optional<Vector<int>> always_required_input_indices(const NodeID &UNUSED(node)) const
  {
    return std::nullopt;
  }

  void execute_node(const NodeID &node, ExecuteNodeParams &params) const
  {
    switch (node) {
      case 1: {
        const int a = *params.get_input(0).get<int>();
        const int b = *params.get_input(1).get<int>();
        const int out1 = a + b;
        const int out2 = a * b;
        params.set_output_by_copy(0, &out1);
        params.set_output_by_copy(1, &out2);
        break;
      }
      case 2: {
        const int a = *params.get_input(0).get<int>();
        const int b = *params.get_input(1).get<int>();
        const int out = a + b;
        params.set_output_by_copy(0, &out);
        break;
      }
      case 3: {
        const int a = *params.get_input(0).get<int>();
        const int b = *params.get_input(1).get<int>();
        const int c = *params.get_input(2).get<int>();
        const int out1 = a + b + c;
        const int out2 = a * b * c;
        const int out3 = out1 + out2;
        params.set_output_by_copy(0, &out1);
        params.set_output_by_copy(1, &out2);
        params.set_output_by_copy(2, &out3);
        break;
      }
    }
  }
};

TEST(sgraph, ToDot)
{
  ExampleSGraphAdapter adapter;
  SGraphT graph{adapter};
  std::cout << sgraph_to_dot(graph) << "\n";

  ExampleExecutor executor{graph};

  SocketT<ExampleSGraphAdapter> output_socket{3, 0, false};
  SGraphEvaluator graph_evaluator{graph, executor, {}, {output_socket}};
}

}  // namespace blender::fn::sgraph::tests
