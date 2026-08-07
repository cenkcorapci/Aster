#include <gtest/gtest.h>

#include <map>
#include <string>

#include "aster/distributed/ring.h"

namespace aster {
namespace {

TEST(Ring, ReplicasAreDistinctAndStable) {
  Ring ring(64);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");

  auto replicas = ring.GetReplicas("some-key", 3);
  ASSERT_EQ(replicas.size(), 3u);
  EXPECT_NE(replicas[0], replicas[1]);
  EXPECT_NE(replicas[1], replicas[2]);
  EXPECT_NE(replicas[0], replicas[2]);
  EXPECT_EQ(ring.GetReplicas("some-key", 3), replicas);
}

TEST(Ring, RfLargerThanCluster) {
  Ring ring(16);
  ring.AddNode("only");
  auto replicas = ring.GetReplicas("k", 3);
  ASSERT_EQ(replicas.size(), 1u);
  EXPECT_EQ(replicas[0], "only");
}

TEST(Ring, LoadIsRoughlyBalanced) {
  Ring ring(256);
  ring.AddNode("a");
  ring.AddNode("b");
  ring.AddNode("c");

  std::map<NodeId, int> primary_count;
  const int kKeys = 3000;
  for (int i = 0; i < kKeys; ++i) {
    primary_count[ring.GetReplicas("key-" + std::to_string(i), 1)[0]]++;
  }
  for (const auto& [node, count] : primary_count) {
    EXPECT_GT(count, kKeys * 15 / 100) << node;
  }
}

TEST(Ring, RemovingNodeOnlyMovesItsKeys) {
  Ring ring(128);
  ring.AddNode("a");
  ring.AddNode("b");
  ring.AddNode("c");

  std::map<std::string, NodeId> before;
  for (int i = 0; i < 500; ++i) {
    const std::string key = "key-" + std::to_string(i);
    before[key] = ring.GetReplicas(key, 1)[0];
  }

  ring.RemoveNode("c");
  for (const auto& [key, owner] : before) {
    const NodeId now = ring.GetReplicas(key, 1)[0];
    if (owner != "c") {
      EXPECT_EQ(now, owner) << key;
    } else {
      EXPECT_NE(now, "c") << key;
    }
  }
}

TEST(Ring, EmptyAndZeroRf) {
  Ring ring(8);
  EXPECT_TRUE(ring.GetReplicas("k", 1).empty());
  ring.AddNode("a");
  EXPECT_TRUE(ring.GetReplicas("k", 0).empty());
}

TEST(Ring, AddDuplicateIsNoOp) {
  Ring ring(8);
  ring.AddNode("a");
  ring.AddNode("a");
  EXPECT_EQ(ring.node_count(), 1u);
  EXPECT_TRUE(ring.HasNode("a"));
  EXPECT_FALSE(ring.HasNode("b"));
}

TEST(Ring, RemoveMissingIsNoOp) {
  Ring ring(8);
  ring.AddNode("a");
  ring.RemoveNode("missing");
  EXPECT_EQ(ring.node_count(), 1u);
  ring.RemoveNode("a");
  EXPECT_EQ(ring.node_count(), 0u);
  EXPECT_TRUE(ring.GetReplicas("k", 1).empty());
}

TEST(Ring, RfTwoOnTwoNodes) {
  Ring ring(32);
  ring.AddNode("a");
  ring.AddNode("b");
  auto reps = ring.GetReplicas("key", 2);
  ASSERT_EQ(reps.size(), 2u);
  EXPECT_NE(reps[0], reps[1]);
}

}  // namespace
}  // namespace aster
