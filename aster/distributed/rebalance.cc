#include "aster/distributed/rebalance.h"

#include <algorithm>
#include <set>

namespace aster {
namespace {

std::set<NodeId> ReplicaSet(const Ring& ring, const RowId& key, uint32_t rf) {
  auto v = ring.GetReplicas(key, rf);
  return {v.begin(), v.end()};
}

void DedupMigrations(std::vector<Migration>* m) {
  std::sort(m->begin(), m->end(), [](const Migration& a, const Migration& b) {
    if (a.key != b.key) return a.key < b.key;
    if (a.to != b.to) return a.to < b.to;
    return a.from < b.from;
  });
  m->erase(std::unique(m->begin(), m->end(),
                       [](const Migration& a, const Migration& b) {
                         return a.key == b.key && a.from == b.from &&
                                a.to == b.to;
                       }),
           m->end());
}

}  // namespace

std::vector<NodeId> ListNodes(const Ring& ring) { return ring.Nodes(); }

NodeId PrimaryFor(const Ring& ring, const RowId& key) {
  auto r = ring.GetReplicas(key, 1);
  return r.empty() ? NodeId{} : r.front();
}

RebalancePlan PlanRemoveNode(const Ring& ring, const NodeId& leaving,
                             uint32_t rf,
                             const std::vector<RowId>& keys) {
  RebalancePlan plan;
  plan.rf = rf;
  plan.nodes_before = ListNodes(ring);
  if (!ring.HasNode(leaving) || rf == 0) {
    plan.nodes_after = plan.nodes_before;
    plan.durable_without_source = true;
    return plan;
  }

  Ring after = ring;
  after.RemoveNode(leaving);
  plan.nodes_after = ListNodes(after);

  // Surviving copy exists for every affected key ⇒ leaving may die after
  // migrations stream from either leaving or a survivor.
  bool survivors_cover = true;

  for (const auto& key : keys) {
    const auto before = ReplicaSet(ring, key, rf);
    if (!before.count(leaving)) continue;

    plan.affected_keys.push_back(key);

    size_t other_holders = 0;
    for (const auto& n : before) {
      if (n != leaving) ++other_holders;
    }
    if (other_holders == 0) survivors_cover = false;

    // New replica set on the smaller cluster.
    const auto want = after.GetReplicas(key, rf);
    for (const auto& dest : want) {
      if (before.count(dest)) continue;  // already has a copy
      // Prefer streaming from leaving while it is still up; fall back hint
      // is any other previous holder.
      NodeId src = leaving;
      if (!before.count(leaving)) {
        src = other_holders ? *before.begin() : leaving;
      }
      plan.migrations.push_back(Migration{key, src, dest});
    }
  }

  DedupMigrations(&plan.migrations);
  plan.durable_without_source = survivors_cover;
  return plan;
}

RebalancePlan PlanAddNode(const Ring& ring, const NodeId& joining, uint32_t rf,
                          const std::vector<RowId>& keys) {
  RebalancePlan plan;
  plan.rf = rf;
  plan.nodes_before = ListNodes(ring);
  if (ring.HasNode(joining) || rf == 0) {
    plan.nodes_after = plan.nodes_before;
    plan.durable_without_source = true;
    return plan;
  }

  Ring after = ring;
  after.AddNode(joining);
  plan.nodes_after = ListNodes(after);
  plan.durable_without_source = true;

  for (const auto& key : keys) {
    const auto before = ReplicaSet(ring, key, rf);
    const auto after_reps = after.GetReplicas(key, rf);
    const std::set<NodeId> after_set(after_reps.begin(), after_reps.end());
    if (before == after_set) continue;
    if (!after_set.count(joining)) continue;

    plan.affected_keys.push_back(key);
    if (before.empty()) continue;
    plan.migrations.push_back(Migration{key, *before.begin(), joining});
  }
  DedupMigrations(&plan.migrations);
  return plan;
}

bool ReplicationSatisfied(const Ring& ring, uint32_t rf,
                          const std::vector<RowId>& keys) {
  if (rf == 0) return true;
  if (ring.node_count() == 0) return keys.empty();
  const size_t want = std::min<size_t>(rf, ring.node_count());
  for (const auto& key : keys) {
    if (ring.GetReplicas(key, rf).size() < want) return false;
  }
  return true;
}

std::vector<RowId> KeysHeldBy(const Ring& ring, const NodeId& node,
                              uint32_t rf, const std::vector<RowId>& keys) {
  std::vector<RowId> out;
  for (const auto& key : keys) {
    auto reps = ring.GetReplicas(key, rf);
    if (std::find(reps.begin(), reps.end(), node) != reps.end()) {
      out.push_back(key);
    }
  }
  return out;
}

}  // namespace aster
