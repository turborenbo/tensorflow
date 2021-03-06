/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/grappler/costs/graph_properties.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/grappler/clusters/single_machine.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/inputs/trivial_test_graph_input_yielder.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace grappler {
namespace {

class GraphPropertiesTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Provision a single machine with 3 cpu cores
    cluster_.reset(new SingleMachine(5 * 60, 3, 0));
    TF_CHECK_OK(cluster_->Provision());
  }

  void TearDown() override { cluster_.reset(); }

 protected:
  // Returns a string form of <p>, suitable for comparing type and shape.
  // Example output for 4-d float tensor: "float: [10,2,30,4]"
  string PropToString(const OpInfo::TensorProperties& p) {
    string s = strings::StrCat(DataTypeString(p.dtype()), ": ");
    if (p.shape().unknown_rank()) {
      strings::StrAppend(&s, "?");
    } else {
      strings::StrAppend(&s, "[");
      for (int i = 0; i < p.shape().dim_size(); ++i) {
        strings::StrAppend(&s, i == 0 ? "" : ",", p.shape().dim(i).size());
      }
      strings::StrAppend(&s, "]");
    }
    return s;
  }

  std::unique_ptr<SingleMachine> cluster_;
};

TEST_F(GraphPropertiesTest, StaticProperties) {
  TrivialTestGraphInputYielder fake_input(4, 1, 10, false,
                                          cluster_->GetDeviceNames());
  GrapplerItem item;
  CHECK(fake_input.NextItem(&item));

  GraphProperties properties(item);
  Status s = properties.InferStatically();
  TF_CHECK_OK(s);

  for (const auto& node : item.graph.node()) {
    if (node.op() == "RandomStandardNormal") {
      // The node has one input (the shape of the tensor to generate).
      EXPECT_EQ(1, properties.GetInputProperties(node.name()).size());
      // The const node has one output.
      const auto props = properties.GetOutputProperties(node.name());
      EXPECT_EQ(1, props.size());
      const OpInfo::TensorProperties& prop = props[0];
      EXPECT_EQ(DT_FLOAT, prop.dtype());
      EXPECT_FALSE(prop.shape().unknown_rank());
      EXPECT_EQ(2, prop.shape().dim_size());
      EXPECT_EQ(10, prop.shape().dim(0).size());
      EXPECT_EQ(1, prop.shape().dim(1).size());
    } else if (node.op() == "AddN") {
      const auto in_props = properties.GetInputProperties(node.name());
      EXPECT_EQ(1, in_props.size());
      const OpInfo::TensorProperties& in_prop = in_props[0];
      EXPECT_EQ(DT_FLOAT, in_prop.dtype());
      EXPECT_FALSE(in_prop.shape().unknown_rank());
      EXPECT_EQ(2, in_prop.shape().dim_size());
      EXPECT_EQ(10, in_prop.shape().dim(0).size());
      EXPECT_EQ(1, in_prop.shape().dim(1).size());
      const auto out_props = properties.GetOutputProperties(node.name());
      EXPECT_EQ(1, out_props.size());
      string in_prop_str;
      ::tensorflow::protobuf::TextFormat::PrintToString(in_prop, &in_prop_str);
      string out_prop_str;
      ::tensorflow::protobuf::TextFormat::PrintToString(out_props[0],
                                                        &out_prop_str);
      EXPECT_EQ(in_prop_str, out_prop_str);
    }
  }
}

TEST_F(GraphPropertiesTest, DynamicProperties) {
  TrivialTestGraphInputYielder fake_input(4, 1, 10, false,
                                          cluster_->GetDeviceNames());
  GrapplerItem item;
  CHECK(fake_input.NextItem(&item));

  GraphProperties properties(item);
  TF_CHECK_OK(cluster_->Initialize(item));
  Status s = properties.InferDynamically(cluster_.get());
  TF_CHECK_OK(s);

  for (const auto& node : item.graph.node()) {
    if (node.op() == "RandomStandardNormal") {
      // The random node is missing from the cost graph (why ?)
      EXPECT_EQ(0, properties.GetInputProperties(node.name()).size());
    } else if (node.op() == "AddN") {
      // Since the random node is missing, we can't infer the input properties
      // of the first AddN node. The other AddN nodes have the expected
      // properties.
      if (node.name() == "AddN") {
        const auto props = properties.GetInputProperties(node.name());
        EXPECT_EQ(1, props.size());
        const OpInfo::TensorProperties& prop = props[0];
        EXPECT_EQ(DT_INVALID, prop.dtype());
        EXPECT_TRUE(prop.shape().unknown_rank());
      } else {
        const auto props = properties.GetInputProperties(node.name());
        EXPECT_EQ(1, props.size());
        const OpInfo::TensorProperties& prop = props[0];
        EXPECT_EQ(DT_FLOAT, prop.dtype());
        EXPECT_FALSE(prop.shape().unknown_rank());
        EXPECT_EQ(2, prop.shape().dim_size());
        EXPECT_EQ(10, prop.shape().dim(0).size());
        EXPECT_EQ(1, prop.shape().dim(1).size());
        const auto out_props = properties.GetOutputProperties(node.name());
        EXPECT_EQ(1, out_props.size());
        string prop_str;
        ::tensorflow::protobuf::TextFormat::PrintToString(prop, &prop_str);
        string out_prop_str;
        ::tensorflow::protobuf::TextFormat::PrintToString(out_props[0],
                                                          &out_prop_str);
        EXPECT_EQ(prop_str, out_prop_str);
      }
    }
  }
}

TEST_F(GraphPropertiesTest, Variables) {
  GrapplerItem item;
  TF_CHECK_OK(NodeDefBuilder("Var", "Variable")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("shape", TensorShape({3, 7}))
                  .Finalize(item.graph.add_node()));
  item.fetch.push_back("Var");

  Tensor initial_val(DT_FLOAT, TensorShape({3, 7}));
  test::FillIota<float>(&initial_val, 0);
  TF_CHECK_OK(NodeDefBuilder("InitialVal", "Const")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("value", initial_val)
                  .Finalize(item.graph.add_node()));
  TF_CHECK_OK(NodeDefBuilder("InitVar", "Assign")
                  .Input("Var", 0, DT_FLOAT_REF)
                  .Input("InitialVal", 0, DT_FLOAT)
                  .Finalize(item.graph.add_node()));
  item.init_ops.push_back("InitVar");

  {
    GraphProperties static_properties(item);
    TF_CHECK_OK(static_properties.InferStatically());

    const auto props = static_properties.GetOutputProperties("Var");
    EXPECT_EQ(1, props.size());
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT_REF, prop.dtype());
    EXPECT_FALSE(prop.shape().unknown_rank());
    EXPECT_EQ(2, prop.shape().dim_size());
    EXPECT_EQ(3, prop.shape().dim(0).size());
    EXPECT_EQ(7, prop.shape().dim(1).size());
  }
  {
    TF_CHECK_OK(cluster_->Initialize(item));
    GraphProperties dynamic_properties(item);
    TF_CHECK_OK(dynamic_properties.InferDynamically(cluster_.get()));

    const auto props = dynamic_properties.GetOutputProperties("Var");
    EXPECT_EQ(1, props.size());
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT_REF, prop.dtype());
    EXPECT_FALSE(prop.shape().unknown_rank());
    EXPECT_EQ(2, prop.shape().dim_size());
    EXPECT_EQ(3, prop.shape().dim(0).size());
    EXPECT_EQ(7, prop.shape().dim(1).size());
  }
}

TEST_F(GraphPropertiesTest, VarHandles) {
  GrapplerItem item;
  TF_CHECK_OK(NodeDefBuilder("Var", "VarHandleOp")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("shape", TensorShape({3, 7}))
                  .Finalize(item.graph.add_node()));

  TF_CHECK_OK(NodeDefBuilder("VarRead", "ReadVariableOp")
                  .Attr("dtype", DT_FLOAT)
                  .Input("Var", 0, DT_RESOURCE)
                  .Finalize(item.graph.add_node()));

  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  const auto props = properties.GetOutputProperties("VarRead");
  EXPECT_EQ(1, props.size());
  const OpInfo::TensorProperties& prop = props[0];
  EXPECT_EQ(DT_FLOAT, prop.dtype());
  EXPECT_FALSE(prop.shape().unknown_rank());
  EXPECT_EQ(2, prop.shape().dim_size());
  EXPECT_EQ(3, prop.shape().dim(0).size());
  EXPECT_EQ(7, prop.shape().dim(1).size());
}

TEST_F(GraphPropertiesTest, Queues) {
  // Create a graph with known input shapes, and propagate the shapes through a
  // couple of queues.
  tensorflow::Scope root = tensorflow::Scope::NewRootScope();

  auto q1 = ops::FIFOQueue(root.WithOpName("Queue1"), {DataType::DT_FLOAT});
  Output rnd =
      ops::RandomNormal(root.WithOpName("rnd"), {3, 7}, DataType::DT_FLOAT);
  Output square1 = ops::Square(root.WithOpName("Square1"), rnd);
  auto enqueue1 = ops::QueueEnqueue(root.WithOpName("Enqueue1"), q1, {square1});
  auto dequeue1 =
      ops::QueueDequeue(root.WithOpName("Dequeue1"), q1, {DataType::DT_FLOAT});

  auto q2 =
      ops::RandomShuffleQueue(root.WithOpName("Queue2"), {DataType::DT_FLOAT});
  Output square2 = ops::Square(root.WithOpName("Square2"), dequeue1[0]);
  auto enqueue2 = ops::QueueEnqueue(root.WithOpName("Enqueue2"), q2, {square2});
  auto dequeue2 =
      ops::QueueDequeue(root.WithOpName("Dequeue2"), q2, {DataType::DT_FLOAT});

  // Create a queue that feeds itself.
  auto q3 =
      ops::RandomShuffleQueue(root.WithOpName("Queue3"), {DataType::DT_FLOAT});
  auto dequeue3 =
      ops::QueueDequeue(root.WithOpName("Dequeue3"), q3, {DataType::DT_FLOAT});
  auto merge3 = ops::Merge(root.WithOpName("Merge3"), {dequeue3[0], square2});
  auto enqueue3 =
      ops::QueueEnqueue(root.WithOpName("Enqueue3"), q3, {merge3.output});

  auto q4 =
      ops::RandomShuffleQueue(root.WithOpName("Queue4"), {DataType::DT_FLOAT});
  auto enqueue4 = ops::QueueEnqueue(root.WithOpName("Enqueue4"), q4, {square2});
  auto enqueue4_2 =
      ops::QueueEnqueue(root.WithOpName("Enqueue4_2"), q4, {dequeue3[0]});
  auto dequeue4 =
      ops::QueueDequeue(root.WithOpName("Dequeue4"), q4, {DataType::DT_FLOAT});

  // Create a queue that takes in three tensors.
  auto q5 = ops::RandomShuffleQueue(
      root.WithOpName("Queue5"),
      {DataType::DT_FLOAT, DataType::DT_DOUBLE, DataType::DT_FLOAT});
  Output rnd2 =
      ops::RandomNormal(root.WithOpName("rnd"), {10}, DataType::DT_DOUBLE);
  Output rnd3 =
      ops::RandomNormal(root.WithOpName("rnd"), {1, 2, 3}, DataType::DT_FLOAT);
  auto enqueue5 =
      ops::QueueEnqueue(root.WithOpName("Enqueue5"), q5, {rnd, rnd2, rnd3});
  auto dequeue5 = ops::QueueDequeue(
      root.WithOpName("Dequeue5"), q5,
      {DataType::DT_FLOAT, DataType::DT_DOUBLE, DataType::DT_FLOAT});

  GrapplerItem item;
  TF_CHECK_OK(root.ToGraphDef(&item.graph));

  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  const auto props1 = properties.GetOutputProperties("Dequeue1");
  ASSERT_EQ(1, props1.size());
  EXPECT_EQ("float: [3,7]", PropToString(props1[0]));

  const auto props2 = properties.GetOutputProperties("Dequeue2");
  ASSERT_EQ(1, props2.size());
  EXPECT_EQ("float: [3,7]", PropToString(props2[0]));

  // The dequeue3 op shape is unknown.
  const auto props3 = properties.GetOutputProperties("Dequeue3");
  ASSERT_EQ(1, props3.size());
  EXPECT_EQ("float: ?", PropToString(props3[0]));

  // The dequeue3 op shape is unknown. The square2 op shape is known. Verify
  // that we merge the 2 properly to determine the shape of the data coming out
  // of the queue.
  const auto props4 = properties.GetOutputProperties("Dequeue4");
  ASSERT_EQ(1, props4.size());
  EXPECT_EQ("float: [3,7]", PropToString(props4[0]));

  // The dequeue5 op shape is known.
  const auto props5 = properties.GetOutputProperties("Dequeue5");
  ASSERT_EQ(3, props5.size());
  EXPECT_EQ("float: [3,7]", PropToString(props5[0]));
  EXPECT_EQ("double: [10]", PropToString(props5[1]));
  EXPECT_EQ("float: [1,2,3]", PropToString(props5[2]));
}

TEST_F(GraphPropertiesTest, MergeWithoutLoops) {
  // Python code used to generate the graph is below.
  const string gdef_ascii = R"EOF(
node {
  name: "Const"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 7
      }
    }
  }
}
node {
  name: "Const_1"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 5
      }
    }
  }
}
node {
  name: "ones"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_FLOAT
        tensor_shape {
          dim {
            size: 1
          }
          dim {
            size: 1
          }
          dim {
            size: 1
          }
        }
        float_val: 1.0
      }
    }
  }
}
node {
  name: "Less"
  op: "Less"
  input: "Const"
  input: "Const_1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "cond/Switch"
  op: "Switch"
  input: "Less"
  input: "Less"
  attr {
    key: "T"
    value {
      type: DT_BOOL
    }
  }
}
node {
  name: "cond/switch_t"
  op: "Identity"
  input: "cond/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_BOOL
    }
  }
}
node {
  name: "cond/switch_f"
  op: "Identity"
  input: "cond/Switch"
  attr {
    key: "T"
    value {
      type: DT_BOOL
    }
  }
}
node {
  name: "cond/pred_id"
  op: "Identity"
  input: "Less"
  attr {
    key: "T"
    value {
      type: DT_BOOL
    }
  }
}
node {
  name: "cond/concat/axis"
  op: "Const"
  input: "^cond/switch_t"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "cond/concat/Switch"
  op: "Switch"
  input: "ones"
  input: "cond/pred_id"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@ones"
      }
    }
  }
}
node {
  name: "cond/concat"
  op: "ConcatV2"
  input: "cond/concat/Switch:1"
  input: "cond/concat/Switch:1"
  input: "cond/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "cond/concat_1/axis"
  op: "Const"
  input: "^cond/switch_f"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "cond/concat_1/Switch"
  op: "Switch"
  input: "ones"
  input: "cond/pred_id"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@ones"
      }
    }
  }
}
node {
  name: "cond/concat_1"
  op: "ConcatV2"
  input: "cond/concat_1/Switch"
  input: "cond/concat_1/Switch"
  input: "cond/concat_1/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "cond/Merge"
  op: "Merge"
  input: "cond/concat"
  input: "cond/concat_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "concat/axis"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 2
      }
    }
  }
}
node {
  name: "concat"
  op: "ConcatV2"
  input: "cond/Merge"
  input: "cond/Merge"
  input: "concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
versions {
  producer: 21
}
  )EOF";

  // Test graph produced in python using:
  /*
    with tf.Graph().as_default():
      x = tf.constant(2)
      y = tf.constant(5)
      z = tf.ones([1,1,1])
      def f1(): return tf.concat([z, z], axis=0)
      def f2(): return tf.concat([z, z], axis=1)
      r = tf.cond(tf.less(x, y), f1, f2)
      tf.concat([r, r], axis=2)
      with open('/tmp/graph.pbtxt', 'w') as f:
        f.write(str(tf.get_default_graph().as_graph_def()))
   */

  GrapplerItem item;
  CHECK(protobuf::TextFormat::ParseFromString(gdef_ascii, &item.graph));
  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  std::vector<string> nodes{"cond/Merge", "cond/concat", "cond/concat_1"};
  std::vector<string> expected_outputs{"float: [-1,-1,1]", "float: [2,1,1]",
                                       "float: [1,2,1]"};
  for (int i = 0; i < nodes.size(); i++) {
    const auto props = properties.GetOutputProperties(nodes[i]);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ(expected_outputs[i], PropToString(prop));
  }
}

TEST_F(GraphPropertiesTest, WhileLoop) {
  // Python code used to generate the graph is below.
  const string gdef_ascii = R"EOF(
node {
  name: "Const"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "ones"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_FLOAT
        tensor_shape {
          dim {
            size: 2
          }
          dim {
            size: 2
          }
        }
        float_val: 1.0
      }
    }
  }
}
node {
  name: "while/Enter"
  op: "Enter"
  input: "Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Enter_1"
  op: "Enter"
  input: "ones"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Merge"
  op: "Merge"
  input: "while/Enter"
  input: "while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Merge_1"
  op: "Merge"
  input: "while/Enter_1"
  input: "while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Less/y"
  op: "Const"
  input: "^while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 10
      }
    }
  }
}
node {
  name: "while/Less"
  op: "Less"
  input: "while/Merge"
  input: "while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/LoopCond"
  op: "LoopCond"
  input: "while/Less"
}
node {
  name: "while/Switch"
  op: "Switch"
  input: "while/Merge"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge"
      }
    }
  }
}
node {
  name: "while/Switch_1"
  op: "Switch"
  input: "while/Merge_1"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge_1"
      }
    }
  }
}
node {
  name: "while/Identity"
  op: "Identity"
  input: "while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Identity_1"
  op: "Identity"
  input: "while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/add/y"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/add"
  op: "Add"
  input: "while/Identity"
  input: "while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/concat/axis"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/concat"
  op: "ConcatV2"
  input: "while/Identity_1"
  input: "while/Identity_1"
  input: "while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration"
  op: "NextIteration"
  input: "while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration_1"
  op: "NextIteration"
  input: "while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Exit"
  op: "Exit"
  input: "while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Exit_1"
  op: "Exit"
  input: "while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
versions {
  producer: 21
}
  )EOF";

  // Test graph produced in python using:
  /*
     with tf.Graph().as_default():
       i0 = tf.constant(0)
       m0 = tf.ones([2, 2])
       c = lambda i, m: i < 10
       b = lambda i, m: [i+1, tf.concat([m, m], axis=0)]
       r = tf.while_loop(
              c, b, loop_vars=[i0, m0],
              shape_invariants=[i0.get_shape(), tf.TensorShape([None, 2])])
       with open('/tmp/graph.pbtxt', 'w') as f:
         f.write(str(tf.get_default_graph().as_graph_def()))
  */

  GrapplerItem item;
  CHECK(protobuf::TextFormat::ParseFromString(gdef_ascii, &item.graph));
  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  std::vector<string> nodes{"while/Merge_1", "while/NextIteration_1",
                            "while/Exit_1"};
  for (const string& node : nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [-1,2]", PropToString(prop));
  }
}

TEST_F(GraphPropertiesTest, NestedLoop) {
  // Python code used to generate the graph is below.
  const string gdef_ascii = R"EOF(
node {
  name: "Const"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "ones"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_FLOAT
        tensor_shape {
          dim {
            size: 1
          }
          dim {
            size: 1
          }
          dim {
            size: 1
          }
        }
        float_val: 1.0
      }
    }
  }
}
node {
  name: "while/Enter"
  op: "Enter"
  input: "Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Enter_1"
  op: "Enter"
  input: "ones"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Merge"
  op: "Merge"
  input: "while/Enter"
  input: "while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Merge_1"
  op: "Merge"
  input: "while/Enter_1"
  input: "while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Less/y"
  op: "Const"
  input: "^while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 3
      }
    }
  }
}
node {
  name: "while/Less"
  op: "Less"
  input: "while/Merge"
  input: "while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/LoopCond"
  op: "LoopCond"
  input: "while/Less"
}
node {
  name: "while/Switch"
  op: "Switch"
  input: "while/Merge"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge"
      }
    }
  }
}
node {
  name: "while/Switch_1"
  op: "Switch"
  input: "while/Merge_1"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge_1"
      }
    }
  }
}
node {
  name: "while/Identity"
  op: "Identity"
  input: "while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Identity_1"
  op: "Identity"
  input: "while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/Const"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/while/Enter"
  op: "Enter"
  input: "while/while/Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/while/Enter_1"
  op: "Enter"
  input: "while/Identity_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/while/Merge"
  op: "Merge"
  input: "while/while/Enter"
  input: "while/while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Merge_1"
  op: "Merge"
  input: "while/while/Enter_1"
  input: "while/while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/Less/y"
  op: "Const"
  input: "^while/while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 3
      }
    }
  }
}
node {
  name: "while/while/Less"
  op: "Less"
  input: "while/while/Merge"
  input: "while/while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/LoopCond"
  op: "LoopCond"
  input: "while/while/Less"
}
node {
  name: "while/while/Switch"
  op: "Switch"
  input: "while/while/Merge"
  input: "while/while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/while/Merge"
      }
    }
  }
}
node {
  name: "while/while/Switch_1"
  op: "Switch"
  input: "while/while/Merge_1"
  input: "while/while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/while/Merge_1"
      }
    }
  }
}
node {
  name: "while/while/Identity"
  op: "Identity"
  input: "while/while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Identity_1"
  op: "Identity"
  input: "while/while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/add/y"
  op: "Const"
  input: "^while/while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/while/add"
  op: "Add"
  input: "while/while/Identity"
  input: "while/while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/concat/axis"
  op: "Const"
  input: "^while/while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 2
      }
    }
  }
}
node {
  name: "while/while/concat"
  op: "ConcatV2"
  input: "while/while/Identity_1"
  input: "while/while/Identity_1"
  input: "while/while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/NextIteration"
  op: "NextIteration"
  input: "while/while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/NextIteration_1"
  op: "NextIteration"
  input: "while/while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/Exit"
  op: "Exit"
  input: "while/while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Exit_1"
  op: "Exit"
  input: "while/while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/add/y"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/add"
  op: "Add"
  input: "while/Identity"
  input: "while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/concat/axis"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/concat"
  op: "ConcatV2"
  input: "while/Identity_1"
  input: "while/Identity_1"
  input: "while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration"
  op: "NextIteration"
  input: "while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration_1"
  op: "NextIteration"
  input: "while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Exit"
  op: "Exit"
  input: "while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Exit_1"
  op: "Exit"
  input: "while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
versions {
  producer: 21
}
  )EOF";

  // Test graph produced in python using:
  /*
    with tf.Graph().as_default():
      i0 = tf.constant(0)

      def inner(j, y):
        def inner_cond(j, y):
          return j < 3

        def inner_body(j, y):
          return j+1, tf.concat([y, y], axis=2)

        return tf.while_loop(inner_cond, inner_body, loop_vars=[j, y],
                             shape_invariants=[i0.get_shape(),
                                              tf.TensorShape([None, 1, None])])

      def outer_cond(i, x):
        return i < 3

      def outer_body(i, x):
        j, y = inner(0, x)
        return i+1, tf.concat([x, x], axis=0)

      r = tf.while_loop(outer_cond, outer_body,
                        loop_vars=[i0, tf.ones([1, 1, 1])],
                        shape_invariants=[i0.get_shape(),
                                          tf.TensorShape([None, 1, None])])

      with open('/tmp/graph.pbtxt', 'w') as f:
        f.write(str(tf.get_default_graph().as_graph_def()))
  */

  GrapplerItem item;
  CHECK(protobuf::TextFormat::ParseFromString(gdef_ascii, &item.graph));
  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  std::vector<string> outer_nodes{"while/Merge_1", "while/NextIteration_1",
                                  "while/Exit_1"};
  std::vector<string> inner_nodes{"while/while/Merge_1",
                                  "while/while/NextIteration_1",
                                  "while/while/Exit_1"};
  for (const string& node : outer_nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [-1,1,1]", PropToString(prop));
  }
  for (const string& node : inner_nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [-1,1,-1]", PropToString(prop));
  }
}

TEST_F(GraphPropertiesTest, LoopsAndQueues) {
  // Python code used to generate the graph is below.
  const string gdef_ascii = R"EOF(
node {
  name: "Const"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "fifo_queue"
  op: "FIFOQueueV2"
  attr {
    key: "capacity"
    value {
      i: 1
    }
  }
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "container"
    value {
      s: ""
    }
  }
  attr {
    key: "shapes"
    value {
      list {
      }
    }
  }
  attr {
    key: "shared_name"
    value {
      s: ""
    }
  }
}
node {
  name: "ones"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_FLOAT
        tensor_shape {
          dim {
            size: 1
          }
          dim {
            size: 1
          }
          dim {
            size: 1
          }
        }
        float_val: 1.0
      }
    }
  }
}
node {
  name: "while/Enter"
  op: "Enter"
  input: "Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Enter_1"
  op: "Enter"
  input: "ones"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Merge"
  op: "Merge"
  input: "while/Enter"
  input: "while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Merge_1"
  op: "Merge"
  input: "while/Enter_1"
  input: "while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Less/y"
  op: "Const"
  input: "^while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 3
      }
    }
  }
}
node {
  name: "while/Less"
  op: "Less"
  input: "while/Merge"
  input: "while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/LoopCond"
  op: "LoopCond"
  input: "while/Less"
}
node {
  name: "while/Switch"
  op: "Switch"
  input: "while/Merge"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge"
      }
    }
  }
}
node {
  name: "while/Switch_1"
  op: "Switch"
  input: "while/Merge_1"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge_1"
      }
    }
  }
}
node {
  name: "while/Identity"
  op: "Identity"
  input: "while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Identity_1"
  op: "Identity"
  input: "while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/fifo_queue_enqueue/Enter"
  op: "Enter"
  input: "fifo_queue"
  attr {
    key: "T"
    value {
      type: DT_RESOURCE
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: true
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/fifo_queue_enqueue"
  op: "QueueEnqueueV2"
  input: "while/fifo_queue_enqueue/Enter"
  input: "while/Identity_1"
  attr {
    key: "Tcomponents"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "while/concat/axis"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 2
      }
    }
  }
}
node {
  name: "while/concat"
  op: "ConcatV2"
  input: "while/Identity_1"
  input: "while/Identity_1"
  input: "while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/fifo_queue_Dequeue"
  op: "QueueDequeueV2"
  input: "while/fifo_queue_enqueue/Enter"
  input: "^while/Identity"
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "while/while/Const"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/while/Enter"
  op: "Enter"
  input: "while/while/Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/while/Enter_1"
  op: "Enter"
  input: "while/fifo_queue_Dequeue"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/while/Merge"
  op: "Merge"
  input: "while/while/Enter"
  input: "while/while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Merge_1"
  op: "Merge"
  input: "while/while/Enter_1"
  input: "while/while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/Less/y"
  op: "Const"
  input: "^while/while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 3
      }
    }
  }
}
node {
  name: "while/while/Less"
  op: "Less"
  input: "while/while/Merge"
  input: "while/while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/LoopCond"
  op: "LoopCond"
  input: "while/while/Less"
}
node {
  name: "while/while/Switch"
  op: "Switch"
  input: "while/while/Merge"
  input: "while/while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/while/Merge"
      }
    }
  }
}
node {
  name: "while/while/Switch_1"
  op: "Switch"
  input: "while/while/Merge_1"
  input: "while/while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/while/Merge_1"
      }
    }
  }
}
node {
  name: "while/while/Identity"
  op: "Identity"
  input: "while/while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Identity_1"
  op: "Identity"
  input: "while/while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/add/y"
  op: "Const"
  input: "^while/while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/while/add"
  op: "Add"
  input: "while/while/Identity"
  input: "while/while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/concat/axis"
  op: "Const"
  input: "^while/while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/while/concat"
  op: "ConcatV2"
  input: "while/while/Identity_1"
  input: "while/while/Identity_1"
  input: "while/while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/NextIteration"
  op: "NextIteration"
  input: "while/while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/NextIteration_1"
  op: "NextIteration"
  input: "while/while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/while/Exit"
  op: "Exit"
  input: "while/while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/while/Exit_1"
  op: "Exit"
  input: "while/while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/add/y"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/add"
  op: "Add"
  input: "while/Identity"
  input: "while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration"
  op: "NextIteration"
  input: "while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration_1"
  op: "NextIteration"
  input: "while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Exit"
  op: "Exit"
  input: "while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Exit_1"
  op: "Exit"
  input: "while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
versions {
  producer: 21
}
  )EOF";

  // Test graph produced in python using:
  /*
    with tf.Graph().as_default():
      i0 = tf.constant(0)
      q = tf.FIFOQueue(1, "float")

      def inner(j, y):
        def inner_cond(j, y):
          return j < 3

        def inner_body(j, y):
          return j+1, tf.concat([y, y], axis=0)

        return tf.while_loop(inner_cond, inner_body,
                             loop_vars=[j, y],
                             shape_invariants=[i0.get_shape(),
                                               tf.TensorShape(None)])

      def outer_cond(i, x):
        return i < 3

      def outer_body(i, x):
        q.enqueue(x)
        y = tf.concat([x, x], axis=2)
        inner(0, q.dequeue())
        return i+1, y

      i, z = tf.while_loop(outer_cond, outer_body,
                           loop_vars=[i0, tf.ones([1, 1, 1])],
                           shape_invariants=[i0.get_shape(),
                                             tf.TensorShape([None, 1, None])])

      with open('/tmp/graph.pbtxt', 'w') as f:
        f.write(str(tf.get_default_graph().as_graph_def()))
   */

  GrapplerItem item;
  CHECK(protobuf::TextFormat::ParseFromString(gdef_ascii, &item.graph));
  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  std::vector<string> outer_nodes{"while/Merge_1", "while/NextIteration_1",
                                  "while/Exit_1"};
  std::vector<string> inner_nodes{"while/while/Merge_1",
                                  "while/while/NextIteration_1",
                                  "while/while/Exit_1"};
  for (const string& node : outer_nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [1,1,-1]", PropToString(prop));
  }
  for (const string& node : inner_nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [-1,1,-1]", PropToString(prop));
  }
}

TEST_F(GraphPropertiesTest, QueuesAndLoops) {
  // Python code used to generate the graph is below.
  const string gdef_ascii = R"EOF(
node {
  name: "Const"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "fifo_queue"
  op: "FIFOQueueV2"
  attr {
    key: "capacity"
    value {
      i: 1
    }
  }
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "container"
    value {
      s: ""
    }
  }
  attr {
    key: "shapes"
    value {
      list {
      }
    }
  }
  attr {
    key: "shared_name"
    value {
      s: ""
    }
  }
}
node {
  name: "ones"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_FLOAT
        tensor_shape {
          dim {
            size: 2
          }
          dim {
            size: 2
          }
        }
        float_val: 1.0
      }
    }
  }
}
node {
  name: "fifo_queue_enqueue"
  op: "QueueEnqueueV2"
  input: "fifo_queue"
  input: "ones"
  attr {
    key: "Tcomponents"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "fifo_queue_1"
  op: "FIFOQueueV2"
  attr {
    key: "capacity"
    value {
      i: 1
    }
  }
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "container"
    value {
      s: ""
    }
  }
  attr {
    key: "shapes"
    value {
      list {
      }
    }
  }
  attr {
    key: "shared_name"
    value {
      s: ""
    }
  }
}
node {
  name: "fifo_queue_Dequeue"
  op: "QueueDequeueV2"
  input: "fifo_queue"
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "while/Enter"
  op: "Enter"
  input: "Const"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Enter_1"
  op: "Enter"
  input: "fifo_queue_Dequeue"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "frame_name"
    value {
      s: "while/while/"
    }
  }
  attr {
    key: "is_constant"
    value {
      b: false
    }
  }
  attr {
    key: "parallel_iterations"
    value {
      i: 10
    }
  }
}
node {
  name: "while/Merge"
  op: "Merge"
  input: "while/Enter"
  input: "while/NextIteration"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Merge_1"
  op: "Merge"
  input: "while/Enter_1"
  input: "while/NextIteration_1"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Less/y"
  op: "Const"
  input: "^while/Merge"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 10
      }
    }
  }
}
node {
  name: "while/Less"
  op: "Less"
  input: "while/Merge"
  input: "while/Less/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/LoopCond"
  op: "LoopCond"
  input: "while/Less"
}
node {
  name: "while/Switch"
  op: "Switch"
  input: "while/Merge"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge"
      }
    }
  }
}
node {
  name: "while/Switch_1"
  op: "Switch"
  input: "while/Merge_1"
  input: "while/LoopCond"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_class"
    value {
      list {
        s: "loc:@while/Merge_1"
      }
    }
  }
}
node {
  name: "while/Identity"
  op: "Identity"
  input: "while/Switch:1"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Identity_1"
  op: "Identity"
  input: "while/Switch_1:1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/add/y"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "while/add"
  op: "Add"
  input: "while/Identity"
  input: "while/add/y"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/concat/axis"
  op: "Const"
  input: "^while/Identity"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 0
      }
    }
  }
}
node {
  name: "while/concat"
  op: "ConcatV2"
  input: "while/Identity_1"
  input: "while/Identity_1"
  input: "while/concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration"
  op: "NextIteration"
  input: "while/add"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/NextIteration_1"
  op: "NextIteration"
  input: "while/concat"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "while/Exit"
  op: "Exit"
  input: "while/Switch"
  attr {
    key: "T"
    value {
      type: DT_INT32
    }
  }
}
node {
  name: "while/Exit_1"
  op: "Exit"
  input: "while/Switch_1"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "fifo_queue_1_enqueue"
  op: "QueueEnqueueV2"
  input: "fifo_queue_1"
  input: "while/Exit_1"
  attr {
    key: "Tcomponents"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "fifo_queue_1_Dequeue"
  op: "QueueDequeueV2"
  input: "fifo_queue_1"
  attr {
    key: "component_types"
    value {
      list {
        type: DT_FLOAT
      }
    }
  }
  attr {
    key: "timeout_ms"
    value {
      i: -1
    }
  }
}
node {
  name: "concat/axis"
  op: "Const"
  attr {
    key: "dtype"
    value {
      type: DT_INT32
    }
  }
  attr {
    key: "value"
    value {
      tensor {
        dtype: DT_INT32
        tensor_shape {
        }
        int_val: 1
      }
    }
  }
}
node {
  name: "concat"
  op: "ConcatV2"
  input: "fifo_queue_1_Dequeue"
  input: "fifo_queue_1_Dequeue"
  input: "concat/axis"
  attr {
    key: "N"
    value {
      i: 2
    }
  }
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "Tidx"
    value {
      type: DT_INT32
    }
  }
}
versions {
  producer: 21
}
  )EOF";

  // Test graph produced in python using:
  /*
    with tf.Graph().as_default():
      i0 = tf.constant(0)
      q0 = tf.FIFOQueue(1, "float")
      q0.enqueue(tf.ones([2, 2]))
      q1 = tf.FIFOQueue(1, "float")

      def c(i, m):
        return i < 10

      def b(i, m):
        return i+1, tf.concat([m, m], axis=0)

      i, m = tf.while_loop(
          c, b, loop_vars=[i0,  q0.dequeue()],
          shape_invariants=[i0.get_shape(), tf.TensorShape(None)])

      q1.enqueue(m)
      v = q1.dequeue();
      tf.concat([v, v], axis=1)
      with open('/tmp/graph.pbtxt', 'w') as f:
        f.write(str(tf.get_default_graph().as_graph_def()))
  */

  GrapplerItem item;
  CHECK(protobuf::TextFormat::ParseFromString(gdef_ascii, &item.graph));
  GraphProperties properties(item);
  TF_CHECK_OK(properties.InferStatically());

  std::vector<string> nodes{"while/Merge_1", "while/NextIteration_1",
                            "while/Exit_1"};

  for (const string& node : nodes) {
    const auto props = properties.GetOutputProperties(node);
    const OpInfo::TensorProperties& prop = props[0];
    EXPECT_EQ(DT_FLOAT, prop.dtype());
    EXPECT_EQ("float: [-1,2]", PropToString(prop));
  }

  const auto props = properties.GetOutputProperties("concat");
  const OpInfo::TensorProperties& prop = props[0];
  EXPECT_EQ(DT_FLOAT, prop.dtype());
  EXPECT_EQ("float: [-1,4]", PropToString(prop));
}

}  // namespace
}  // namespace grappler
}  // namespace tensorflow
