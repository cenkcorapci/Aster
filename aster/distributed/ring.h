#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aster/core/types.h"

namespace aster {

using NodeId = std::string;

// Consistent hashing ring with virtual nodes, Cassandra-style
// (docs/design.md, "Cluster topology"). Every physical node owns
// `vnodes_per_node` tokens; a key belongs to the first vnode clockwise from
// hash(key), and replicas are the next RF distinct physical nodes clockwise.
class Ring {
 public:
  explicit Ring(uint32_t vnodes_per_node = 256)
      : vnodes_per_node_(vnodes_per_node) {}

  void AddNode(const NodeId& node);
  void RemoveNode(const NodeId& node);

  bool HasNode(const NodeId& node) const { return nodes_.count(node) > 0; }
  size_t node_count() const { return nodes_.size(); }

  // The RF distinct physical nodes responsible for `key`, in preference
  // order (primary first). Returns fewer than rf nodes if the cluster is
  // smaller than rf.
  std::vector<NodeId> GetReplicas(const RowId& key, uint32_t rf) const;

 private:
  uint32_t vnodes_per_node_;
  std::set<NodeId> nodes_;
  std::map<uint64_t, NodeId> tokens_;  // token -> physical node
};

}  // namespace aster
