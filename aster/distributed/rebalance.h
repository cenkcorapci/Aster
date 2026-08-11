#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aster/core/types.h"
#include "aster/distributed/ring.h"

namespace aster {

// One key that must be (re)copied after a ring membership change so that
// replication factor `rf` still holds on the new membership.
struct Migration {
  RowId key;
  NodeId from;  // source replica that still holds the row (best-effort hint)
  NodeId to;    // destination that must receive a copy
};

struct RebalancePlan {
  std::vector<NodeId> nodes_before;
  std::vector<NodeId> nodes_after;
  uint32_t rf = 1;
  // Keys (from the provided universe) whose replica set changed.
  std::vector<RowId> affected_keys;
  // Explicit copy edges to restore RF after the change.
  std::vector<Migration> migrations;
  // True when every affected key still has ≥1 replica among nodes_after
  // before migrations run (i.e. safe to drop a node only after these copies).
  bool durable_without_source = false;
};

// List physical nodes currently on the ring (sorted).
std::vector<NodeId> ListNodes(const Ring& ring);

// Primary owner (RF=1 preference). Empty if ring empty.
NodeId PrimaryFor(const Ring& ring, const RowId& key);

// Build a rebalance plan for removing `leaving` from `ring` given a key
// universe. The returned plan's migrations assume `leaving` is still readable
// until copies complete — callers must drain before destroying the node.
RebalancePlan PlanRemoveNode(const Ring& ring, const NodeId& leaving,
                             uint32_t rf,
                             const std::vector<RowId>& keys);

// Build a rebalance plan for adding `joining` to `ring`.
RebalancePlan PlanAddNode(const Ring& ring, const NodeId& joining, uint32_t rf,
                          const std::vector<RowId>& keys);

// True iff every key still has at least `rf` replicas on `ring` (after a
// prospective remove, pass a ring with the node already removed and rf as
// the *remaining* requirement, typically rf-0 with data already copied).
bool ReplicationSatisfied(const Ring& ring, uint32_t rf,
                          const std::vector<RowId>& keys);

// Keys for which `node` is among the RF replicas.
std::vector<RowId> KeysHeldBy(const Ring& ring, const NodeId& node,
                              uint32_t rf, const std::vector<RowId>& keys);

}  // namespace aster
