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

#include "tensorflow/core/grappler/clusters/single_machine.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/cost_graph.pb.h"
#include "tensorflow/core/framework/step_stats.pb.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/inputs/trivial_test_graph_input_yielder.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace grappler {
namespace {

class SingleMachineTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Provision a single machine with 3 cpu cores, and a short timeout of 5
    // seconds: since there isn't much work to process a test graph that should
    // be plenty.
    cluster_.reset(new SingleMachine(5, 3, 0));
    TF_CHECK_OK(cluster_->Provision());
  }

  void TearDown() override {
    cluster_.reset();
  }

 protected:
  std::unique_ptr<SingleMachine> cluster_;
};

TEST_F(SingleMachineTest, CostModel) {
  TrivialTestGraphInputYielder fake_input(4, 1, 10, false,
                                          cluster_->GetDeviceNames());
  GrapplerItem item;
  CHECK(fake_input.NextItem(&item));

  TF_CHECK_OK(cluster_->Initialize(item));

  RunMetadata metadata;
  const int64 start_micros = Env::Default()->NowMicros();
  TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata));
  const int64 run_duration_micros = Env::Default()->NowMicros() - start_micros;

  // There should be at least 4 nodes corresponding to the 4 stages we created
  // in the fake input.
  EXPECT_LE(4, metadata.cost_graph().node_size());
  for (const auto& node : metadata.cost_graph().node()) {
    // Skip the special nodes inserted by TF: these are prefixed with an
    // underscore.
    if (node.name()[0] == '_' || node.name().find("/_") != string::npos) {
      continue;
    }
    EXPECT_EQ(1, node.output_info_size());
    EXPECT_LE(8, node.output_info(0).size());
    const TensorShapeProto& shape = node.output_info(0).shape();
    EXPECT_EQ(2, shape.dim_size());
    EXPECT_EQ(10, shape.dim(0).size());
    EXPECT_EQ(1, shape.dim(1).size());
    EXPECT_LE(0, node.compute_cost());
    EXPECT_GE(run_duration_micros, node.compute_cost());
  }
}

TEST_F(SingleMachineTest, Queue) {
  TrivialTestGraphInputYielder fake_input(4, 1, 10, true,
                                          cluster_->GetDeviceNames());
  GrapplerItem item;
  CHECK(fake_input.NextItem(&item));

  TF_CHECK_OK(cluster_->Initialize(item));
  RunMetadata metadata;
  TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata));
}

TEST_F(SingleMachineTest, MultipleItems) {
  TrivialTestGraphInputYielder fake_input(4, 1, 10, false,
                                          cluster_->GetDeviceNames());

  for (int i = 0; i < 3; ++i) {
    GrapplerItem item;
    CHECK(fake_input.NextItem(&item));
    TF_CHECK_OK(cluster_->Initialize(item));
    RunMetadata metadata1;
    TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata1));
    RunMetadata metadata2;
    TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata2));

    // There should be at least 4 nodes corresponding to the 4 stages we created
    // in the fake input, plus 1 enqueue and 1 dequeue node.
    EXPECT_LE(6, metadata1.cost_graph().node_size());
    for (const auto& node : metadata1.cost_graph().node()) {
      if (node.name()[0] == '_' || node.name().find("/_") != string::npos ||
          node.name() == "queue") {
        continue;
      }
      EXPECT_EQ(1, node.output_info_size());
      const TensorShapeProto& shape = node.output_info(0).shape();
      EXPECT_EQ(2, shape.dim_size());
      EXPECT_EQ(10, shape.dim(0).size());
      EXPECT_EQ(1, shape.dim(1).size());
    }

    for (int i = 0; i < metadata1.cost_graph().node_size(); ++i) {
      metadata1.mutable_cost_graph()->mutable_node(i)->set_compute_cost(0);
      metadata1.clear_step_stats();
    }
    for (int i = 0; i < metadata2.cost_graph().node_size(); ++i) {
      metadata2.mutable_cost_graph()->mutable_node(i)->set_compute_cost(0);
      metadata2.clear_step_stats();
    }
    string s1;
    ::tensorflow::protobuf::TextFormat::PrintToString(metadata1, &s1);
    string s2;
    ::tensorflow::protobuf::TextFormat::PrintToString(metadata2, &s2);
    EXPECT_EQ(s1, s2);
  }
}

TEST_F(SingleMachineTest, GraphOptimizations) {
  // Create a graph that can be fully precomputed
  tensorflow::Scope root = tensorflow::Scope::NewRootScope();
  auto zero = ops::Const(root.WithOpName("zero"), 0.0f, {2, 3});
  auto one = ops::Const(root.WithOpName("one"), 1.0f, {2, 3});
  auto add = ops::Add(root.WithOpName("add"), zero, one);
  auto square = ops::Square(root.WithOpName("square"), add);

  auto new_shape = ops::Const(root.WithOpName("new_shape"), {3, -1}, {2});
  auto reshaped = ops::Reshape(root.WithOpName("reshaped"), square, new_shape);
  auto final_shape = ops::Shape(root.WithOpName("final_shape"), reshaped);

  auto expected_shape =
      ops::Const(root.WithOpName("expected_shape"), {3, 2}, {2});
  auto valid =
      ops::Equal(root.WithOpName("valid"), final_shape, expected_shape);
  auto all_dims = ops::Const(root.WithOpName("all_dims"), {0}, {1});

  auto all_valid = ops::All(root.WithOpName("all_valid"), valid, all_dims);
  auto assert_valid = ops::Assert(root.WithOpName("assert_valid"), all_valid,
                                  {final_shape.output});

  GrapplerItem item;
  TF_CHECK_OK(root.ToGraphDef(&item.graph));
  item.fetch.push_back("assert_valid");

  // Force the placement of all the nodes on CPU since TF attempts to use a GPU
  // when possible event though we created the session to have a single CPU !.
  for (auto& node : *item.graph.mutable_node()) {
    node.set_device("/cpu:0");
  }

  // With optimizations turned on, some nodes could have been optimized away,
  // and the cost model could be partial. Restart the cluster with optimizations
  // disabled and make sure we have all the information we're looking for.
  cluster_.reset();
  cluster_.reset(new SingleMachine(5, 3, 0));
  cluster_->DisableOptimizer(true);
  TF_CHECK_OK(cluster_->Provision());

  RunMetadata metadata;
  TF_CHECK_OK(cluster_->Initialize(item));
  TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata));
  std::set<string> cost_nodes;
  for (const auto& node : metadata.cost_graph().node()) {
    // Skip nodes added by TF internally.
    if (node.name()[0] != '_') {
      cost_nodes.insert(node.name());
    }
  }
  const std::set<string> expected_cost_nodes = {
      "zero",      "one",      "add",         "square",
      "new_shape", "reshaped", "final_shape", "expected_shape",
      "valid",     "all_dims", "all_valid",   "assert_valid"};
  EXPECT_EQ(expected_cost_nodes, cost_nodes);
}

TEST_F(SingleMachineTest, TimeOuts) {
  // Create a graph that will block forever: Just try to dequeue data from a
  // queue that is never fed.
  tensorflow::Scope root = tensorflow::Scope::NewRootScope();
  auto q = ops::FIFOQueue(root.WithOpName("queue"), {DataType::DT_INT32});
  auto dequeue =
      ops::QueueDequeue(root.WithOpName("dequeue"), q, {DataType::DT_INT32});

  GrapplerItem item;
  TF_CHECK_OK(root.ToGraphDef(&item.graph));
  item.fetch.push_back("dequeue");

  TF_CHECK_OK(cluster_->Initialize(item));
  RunMetadata metadata;
  Status s1 = cluster_->Run(item.graph, item.feed, item.fetch, &metadata);
  EXPECT_TRUE(errors::IsDeadlineExceeded(s1));
  Status s2 = cluster_->Run(item.graph, item.feed, item.fetch, &metadata);
  EXPECT_TRUE(errors::IsDeadlineExceeded(s2));
}

static void RunInfiniteTFLoop() {
  // Create a while(true) loop
  GrapplerItem item;

  NodeDef* shp = item.graph.add_node();
  shp->set_name("shape");
  shp->set_op("Const");
  (*shp->mutable_attr())["dtype"].set_type(DT_INT32);
  Tensor shp_tensor(DT_INT32, TensorShape({1}));
  shp_tensor.flat<int32>()(0) = 1;
  shp_tensor.AsProtoTensorContent(
      (*shp->mutable_attr())["value"].mutable_tensor());

  NodeDef* r = item.graph.add_node();
  r->set_name("random");
  r->set_op("RandomUniform");
  (*r->mutable_attr())["dtype"].set_type(DT_FLOAT);
  (*r->mutable_attr())["T"].set_type(DT_INT32);
  *r->add_input() = "shape";

  NodeDef* e = item.graph.add_node();
  e->set_name("while/Enter");
  e->set_op("Enter");
  (*e->mutable_attr())["T"].set_type(DT_FLOAT);
  (*e->mutable_attr())["frame_name"].set_s("while/while/");
  *e->add_input() = "random";

  NodeDef* m = item.graph.add_node();
  m->set_name("while/Merge");
  m->set_op("Merge");
  (*m->mutable_attr())["T"].set_type(DT_FLOAT);
  (*m->mutable_attr())["N"].set_i(2);
  *m->add_input() = "while/Enter";
  *m->add_input() = "while/NextIteration";

  NodeDef* t = item.graph.add_node();
  t->set_name("always_true");
  t->set_op("Const");
  (*t->mutable_attr())["dtype"].set_type(DT_BOOL);
  *t->add_input() = "^while/Merge";
  Tensor true_tensor(DT_BOOL, TensorShape());
  true_tensor.flat<bool>()(0) = true;
  true_tensor.AsProtoTensorContent(
      (*t->mutable_attr())["value"].mutable_tensor());

  NodeDef* c = item.graph.add_node();
  c->set_name("while/LoopCond");
  c->set_op("LoopCond");
  *c->add_input() = "always_true";

  NodeDef* s = item.graph.add_node();
  s->set_name("while/Switch");
  (*s->mutable_attr())["T"].set_type(DT_FLOAT);
  s->set_op("Switch");
  *s->add_input() = "while/Merge";
  *s->add_input() = "while/LoopCond";

  NodeDef* i = item.graph.add_node();
  i->set_name("while/Identity");
  i->set_op("Identity");
  (*i->mutable_attr())["T"].set_type(DT_FLOAT);
  *i->add_input() = "while/Switch:1";

  NodeDef* n = item.graph.add_node();
  n->set_name("while/NextIteration");
  n->set_op("NextIteration");
  (*n->mutable_attr())["T"].set_type(DT_FLOAT);
  *n->add_input() = "while/Identity";

  NodeDef* x = item.graph.add_node();
  x->set_name("while/Exit");
  x->set_op("Exit");
  (*x->mutable_attr())["T"].set_type(DT_FLOAT);
  *x->add_input() = "while/Switch";

  item.fetch.push_back("while/Exit");

  // Create our own cluster to run it
  SingleMachine cluster(5, 3, 0);
  TF_CHECK_OK(cluster.Provision());
  TF_CHECK_OK(cluster.Initialize(item));

  Status s1 = cluster.Run(item.graph, item.feed, item.fetch, nullptr);
  if (!errors::IsDeadlineExceeded(s1)) {
    LOG(ERROR) << "Expected 'deadline exceeded' error, got " << s1;
    // Exit to break the infinite loop
    _exit(1);
  }

  // Attempt to shutdown the cluster and make sure we get the proper error code.
  Status s2 = cluster.Shutdown();
  if (!errors::IsUnavailable(s2)) {
    LOG(ERROR) << "Expected 'unavailable' error, got " << s2;
    // Exit to break the infinite loop
    _exit(2);
  }

  // The isn't much we can do at this point. Exit with the error code 0 to
  // indicate everything went according to plan.
  _exit(0);
}

TEST_F(SingleMachineTest, InfiniteLoops) {
  // The RunInfiniteTFLoop function creates its own cluster.
  cluster_.reset();

  EXPECT_EXIT(RunInfiniteTFLoop(), ::testing::ExitedWithCode(0), ".*");
}

TEST_F(SingleMachineTest, InitializationMemory) {
  // Build a variable and its initialization graph.
  tensorflow::Scope s = tensorflow::Scope::NewRootScope();
  int batch_size = 10;
  Output x =
      ops::RandomNormal(s.WithOpName("x"), {batch_size, 1}, DataType::DT_FLOAT);
  Output v = ops::Variable(s.WithOpName("v"), TensorShape({batch_size, 1}),
                           DataType::DT_FLOAT);
  Output init = ops::Assign(s.WithOpName("init"), v, x);

  GrapplerItem item;
  TF_CHECK_OK(s.ToGraphDef(&item.graph));
  item.init_ops.push_back(init.name());
  item.fetch.push_back(v.name());

  TF_CHECK_OK(cluster_->Initialize(item));
  RunMetadata metadata;
  TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata));

  // Check that the initialization op is present in the cost model.
  bool found = false;
  for (const auto& node : metadata.cost_graph().node()) {
    found |= (node.name() == NodeName(init.name()));
  }
  EXPECT_TRUE(found);
}

namespace {
template <class T>
inline void SetNodeAttr(const string& key, const T& value, NodeDef* node) {
  AttrValue attr_value;
  SetAttrValue(value, &attr_value);
  auto* attr_map = node->mutable_attr();
  (*attr_map)[key] = attr_value;
}
template <>
inline void SetNodeAttr(const string& key, const Tensor& tensor,
                        NodeDef* node) {
  TensorProto tensor_proto;
  tensor.AsProtoTensorContent(&tensor_proto);
  SetNodeAttr(key, tensor_proto, node);
}

}  // namespace

TEST_F(SingleMachineTest, PersistentMemory) {
  // Build a hashtable and its initialization graph.
  GrapplerItem item;
  const DataType key_dtype = DT_INT64;
  const DataType data_dtype = DT_INT64;

  NodeDef* hashtable_node = item.graph.add_node();
  hashtable_node->set_op("HashTable");
  hashtable_node->set_name("hash_table");
  SetNodeAttr("key_dtype", key_dtype, hashtable_node);
  SetNodeAttr("value_dtype", data_dtype, hashtable_node);

  // Initial hashtable keys and values
  NodeDef* keys_node = item.graph.add_node();
  keys_node->set_op("Const");
  keys_node->set_name("table_keys");
  SetNodeAttr("dtype", key_dtype, keys_node);
  Tensor keys(key_dtype, TensorShape{2});
  keys.vec<int64>()(0) = 123;
  keys.vec<int64>()(1) = 321;
  SetNodeAttr("value", keys, keys_node);

  NodeDef* values_node = item.graph.add_node();
  values_node->set_op("Const");
  values_node->set_name("table_values");
  SetNodeAttr("dtype", data_dtype, values_node);
  Tensor values(data_dtype, TensorShape{2});
  values.vec<int64>()(0) = 789;
  values.vec<int64>()(1) = 987;
  SetNodeAttr("value", values, values_node);

  // InitializeTable node
  NodeDef* init_table_node = item.graph.add_node();
  init_table_node->set_op("InitializeTable");
  init_table_node->set_name("initialize_table");
  SetNodeAttr("Tkey", key_dtype, init_table_node);
  SetNodeAttr("Tval", data_dtype, init_table_node);
  *init_table_node->add_input() = "hash_table";
  *init_table_node->add_input() = "table_keys";
  *init_table_node->add_input() = "table_values";
  item.init_ops.push_back(init_table_node->name());

  // Key to lookup
  NodeDef* query_node = item.graph.add_node();
  query_node->set_op("Const");
  query_node->set_name("query");
  SetNodeAttr("dtype", key_dtype, query_node);
  Tensor query(key_dtype, TensorShape({}));
  query.flat<int64>()(0) = 0;
  SetNodeAttr("value", query, query_node);

  // Default return value of hashtable lookup
  NodeDef* default_value_node = item.graph.add_node();
  default_value_node->set_op("Const");
  default_value_node->set_name("default_table_value");
  SetNodeAttr("dtype", data_dtype, default_value_node);
  Tensor dflt(data_dtype, TensorShape({}));
  dflt.flat<int64>()(0) = 456;
  SetNodeAttr("value", dflt, default_value_node);

  // HashTable lookup node
  NodeDef* lookup_node = item.graph.add_node();
  lookup_node->set_op("LookupTableFind");
  lookup_node->set_name("table_lookup");
  SetNodeAttr("Tin", key_dtype, lookup_node);
  SetNodeAttr("Tout", data_dtype, lookup_node);
  *lookup_node->add_input() = "hash_table";
  *lookup_node->add_input() = "query";
  *lookup_node->add_input() = "default_table_value";
  item.fetch.push_back(lookup_node->name());

  // Run the graph
  TF_CHECK_OK(cluster_->Initialize(item));
  RunMetadata metadata;
  TF_CHECK_OK(cluster_->Run(item.graph, item.feed, item.fetch, &metadata));

  // Check the cost model.
  bool found_table_init = false;
  bool found_hashtable = false;
  for (const auto& node : metadata.cost_graph().node()) {
    if (node.name() == "hash_table") {
      found_hashtable = true;
      // Persistent memory usage should be 0 since it's recorded as part of the
      // initialize_table op.
      EXPECT_EQ(0, node.host_persistent_memory_size());
      EXPECT_EQ(0, node.device_persistent_memory_size());
    } else if (node.name() == "initialize_table") {
      found_table_init = true;
      // Persistent memory should hold 2 keys and 2 values.
      EXPECT_LE(4 * sizeof(int64), node.host_persistent_memory_size());
      EXPECT_EQ(0, node.device_persistent_memory_size());
    }
  }
  EXPECT_TRUE(found_table_init);
  EXPECT_TRUE(found_hashtable);
}

}  // namespace
}  // namespace grappler
}  // namespace tensorflow
