#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/db/db.h"
#include "aster/distributed/ring.h"

namespace aster {

// Cassandra-style acknowledgement policy for a single token range.
enum class ConsistencyLevel : uint8_t {
  kOne,
  kQuorum,
  kAll,
};

struct CoordinatorOptions {
  uint32_t replication_factor = 3;
  uint32_t vnodes_per_node = 256;
  // Empty keeps hints in memory only. A non-empty directory persists hinted
  // handoff before a write is acknowledged.
  std::string hint_directory;
};

// In-process coordinator used by a transport to execute the replication
// protocol. Db::Upsert durably appends to each replica WAL before returning,
// so an acknowledgement counted here represents a durable replica copy.
class Coordinator {
 public:
  static Result<std::unique_ptr<Coordinator>> Open(CoordinatorOptions options);

  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Adds an independently owned replica Db to the ring. Node IDs are stable
  // identities; replacing an existing node is rejected.
  Status AddReplica(NodeId id, Db* db);
  void SetReachable(const NodeId& id, bool reachable);
  bool IsReachable(const NodeId& id) const;

  // Applies an LWW row to the key's RF replicas and succeeds only after the
  // requested number of durable acknowledgements. Unreachable replicas first
  // receive a durable hint (when hint_directory is configured).
  Status Write(const Row& row, ConsistencyLevel consistency);
  Status Delete(const RowId& id, Timestamp timestamp,
                ConsistencyLevel consistency);

  // Merges replies from the requested number of replicas, then repairs older
  // reachable copies (including tombstones). A missing or deleted row returns
  // nullopt after a successful read.
  Result<std::optional<Row>> Read(const RowId& id,
                                  ConsistencyLevel consistency);

  // Replays all stored hints for a recovered node. Safe to retry: replica
  // application and hint coalescing both use the same LWW ordering.
  Status DrainHints(const NodeId& id);
  size_t pending_hints(const NodeId& id) const;

  const Ring& ring() const { return ring_; }

 private:
  struct HintJournal;

  explicit Coordinator(CoordinatorOptions options);

  size_t RequiredAcks(ConsistencyLevel consistency,
                      size_t replica_count) const;
  std::vector<NodeId> ReplicasFor(const RowId& id) const;
  Status PersistHint(const NodeId& target, const Row& row);

  CoordinatorOptions options_;
  Ring ring_;
  std::map<NodeId, Db*> replicas_;
  std::map<NodeId, bool> reachable_;
  std::unique_ptr<HintJournal> hints_;
};

}  // namespace aster
