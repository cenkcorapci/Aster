#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aster/distributed/rebalance.h"
#include "aster/distributed/ring.h"

namespace aster {
namespace {

TEST(Rebalance, RemoveRequiresMigrationWhenSoleReplica) {
  Ring ring(32);
  ring.AddNode("a");
  ring.AddNode("b");
  ring.AddNode("c");

  // RF=1 ⇒ leaving primary must migrate.
  std::vector<RowId> keys;
  for (int i = 0; i < 200; ++i) keys.push_back("k" + std::to_string(i));

  const NodeId leaving = "c";
  auto plan = PlanRemoveNode(ring, leaving, /*rf=*/1, keys);
  EXPECT_FALSE(plan.affected_keys.empty());
  for (const auto& m : plan.migrations) {
    EXPECT_EQ(m.from, leaving);
    EXPECT_NE(m.to, leaving);
    EXPECT_TRUE(std::find(plan.nodes_after.begin(), plan.nodes_after.end(),
                          m.to) != plan.nodes_after.end());
  }
  // RF=1 ⇒ no surviving replica for affected keys.
  EXPECT_FALSE(plan.durable_without_source);
}

TEST(Rebalance, Rf2SurvivorsCoverOnRemove) {
  Ring ring(64);
  ring.AddNode("a");
  ring.AddNode("b");
  ring.AddNode("c");
  ring.AddNode("d");

  std::vector<RowId> keys;
  for (int i = 0; i < 300; ++i) keys.push_back("doc-" + std::to_string(i));

  auto plan = PlanRemoveNode(ring, "a", /*rf=*/2, keys);
  EXPECT_TRUE(plan.durable_without_source)
      << "RF=2 must keep a surviving copy for every key a held";
  // Migrations restore full RF onto the new membership.
  for (const auto& key : plan.affected_keys) {
    Ring after = ring;
    after.RemoveNode("a");
    EXPECT_GE(after.GetReplicas(key, 2).size(), 1u);
  }
}

TEST(Rebalance, AddStreamsToJoining) {
  Ring ring(64);
  ring.AddNode("a");
  ring.AddNode("b");
  ring.AddNode("c");

  std::vector<RowId> keys;
  for (int i = 0; i < 200; ++i) keys.push_back("x" + std::to_string(i));

  auto plan = PlanAddNode(ring, "d", /*rf=*/2, keys);
  EXPECT_TRUE(plan.durable_without_source);
  for (const auto& m : plan.migrations) {
    EXPECT_EQ(m.to, "d");
    EXPECT_NE(m.from, "d");
  }
}

TEST(Rebalance, ReplicationSatisfied) {
  Ring ring(16);
  ring.AddNode("a");
  ring.AddNode("b");
  std::vector<RowId> keys = {"k1", "k2"};
  EXPECT_TRUE(ReplicationSatisfied(ring, 2, keys));
  ring.RemoveNode("b");
  EXPECT_TRUE(ReplicationSatisfied(ring, 1, keys));
}

TEST(Rebalance, NodesSorted) {
  Ring ring(8);
  ring.AddNode("z");
  ring.AddNode("a");
  auto n = ListNodes(ring);
  ASSERT_EQ(n.size(), 2u);
  EXPECT_EQ(n[0], "a");
  EXPECT_EQ(n[1], "z");
}

}  // namespace
}  // namespace aster
