#include "aster/distributed/ring.h"

#include <algorithm>

#include "aster/core/hash.h"

namespace aster {

void Ring::AddNode(const NodeId& node) {
  if (!nodes_.insert(node).second) return;
  for (uint32_t i = 0; i < vnodes_per_node_; ++i) {
    const uint64_t token = Hash64(node, /*seed=*/i);
    tokens_.emplace(token, node);
  }
}

void Ring::RemoveNode(const NodeId& node) {
  if (nodes_.erase(node) == 0) return;
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    if (it->second == node) {
      it = tokens_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<NodeId> Ring::Nodes() const {
  return std::vector<NodeId>(nodes_.begin(), nodes_.end());
}

std::vector<NodeId> Ring::GetReplicas(const RowId& key, uint32_t rf) const {
  std::vector<NodeId> replicas;
  if (tokens_.empty() || rf == 0) return replicas;

  const uint64_t token = Hash64(key);
  auto it = tokens_.lower_bound(token);

  // Walk clockwise, collecting distinct physical nodes.
  const size_t want = std::min<size_t>(rf, nodes_.size());
  for (size_t steps = 0; steps < tokens_.size() && replicas.size() < want;
       ++steps) {
    if (it == tokens_.end()) it = tokens_.begin();
    if (std::find(replicas.begin(), replicas.end(), it->second) ==
        replicas.end()) {
      replicas.push_back(it->second);
    }
    ++it;
  }
  return replicas;
}

}  // namespace aster
