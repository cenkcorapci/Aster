#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "aster/core/types.h"
#include "aster/index/tags.h"
#include "aster/index/vector_index.h"

namespace aster {

// Per-segment HNSW build lifecycle (tla/AsterLsmIndex.tla SegState,
// docs/indexing.md §4.3). Rows are searchable via exact scan in every state;
// search switches to the graph only after READY.
enum class SegState : uint8_t {
  kPending = 0,
  kBuilding = 1,
  kReady = 2,
};

// An immutable segment: the unit of storage, indexing, compaction, and
// replication repair. Produced by a memtable flush or by compacting other
// segments; row data is never mutated afterwards (lock-free reads of rows).
//
// Index build state (PENDING→BUILDING→READY) and the optional HNSW graph are
// updated under the caller's synchronization (Db::mu_). Rows and the exact
// index share one vector copy (shared_ptr) so search stays cache-friendly
// without doubling RAM.
class Segment {
 public:
  // `rows` must be sorted by id and deduplicated (one version per id).
  // Tombstones are kept so compaction and read-repair can honor deletes.
  // New segments start in PENDING with an exact index only.
  static std::shared_ptr<const Segment> Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows);
  static std::shared_ptr<const Segment> Build(
      uint64_t id, Metric metric,
      std::shared_ptr<std::vector<Row>> rows);

  uint64_t id() const { return id_; }
  size_t row_count() const { return rows_ ? rows_->size() : 0; }
  const std::vector<Row>& rows() const { return *rows_; }
  const TagIndex& tag_index() const { return tag_index_; }

  // Current index build state. Callers that mutate state must serialize
  // transitions (Db holds mu_ across TryBegin/Complete/Abort).
  SegState index_state() const { return index_state_; }

  // True when Search uses the HNSW graph (READY with a installed graph).
  bool search_uses_hnsw() const {
    return index_state_ == SegState::kReady && hnsw_index_ != nullptr;
  }

  // Number of vectors/nodes currently inside the installed HNSW index.
  // Returns 0 when the segment has no installed HNSW graph (PENDING/BUILDING
  // or Tiny / HNSW disabled builds).
  size_t hnsw_index_size() const {
    return hnsw_index_ ? hnsw_index_->size() : 0u;
  }

  // PENDING → BUILDING. Returns false if not PENDING.
  bool TryBeginIndexBuild() const;
  // BUILDING → READY after installing `hnsw` (may be null on Tiny / empty).
  // No-op if not BUILDING.
  void CompleteIndexBuild(std::unique_ptr<VectorIndex> hnsw) const;
  // BUILDING → PENDING (build interrupted / abandoned).
  void AbortIndexBuild() const;

  // Point lookup by binary search over the sorted id index.
  std::optional<Row> Get(const RowId& row_id) const;

  // ANN search over non-tombstoned rows of this segment.
  // When `tags` is non-empty, only ordinals matching the tag bitmap AND are
  // scored (post-filter / bitmap-driven exact scan — docs/indexing.md §7).
  // Unfiltered: exact while not READY; HNSW graph after READY (when present).
  std::vector<SearchHit> Search(
      VectorView query, uint32_t top_k, uint32_t ef_search,
      const std::set<std::string>& tags = {}) const;

 private:
  Segment(uint64_t id, Metric metric,
          std::shared_ptr<const std::vector<Row>> rows,
          std::unique_ptr<VectorIndex> index, TagIndex tag_index)
      : id_(id),
        metric_(metric),
        rows_(std::move(rows)),
        index_(std::move(index)),
        tag_index_(std::move(tag_index)) {}

  uint64_t id_;
  Metric metric_;
  std::shared_ptr<const std::vector<Row>> rows_;
  std::unique_ptr<VectorIndex> index_;  // exact; always present
  TagIndex tag_index_;

  // Build state machine + optional HNSW (mutable under Db::mu_).
  mutable SegState index_state_ = SegState::kPending;
  mutable std::unique_ptr<VectorIndex> hnsw_index_;
};

// Merges segments into one, applying LWW per id. Tombstones may only be
// dropped when the compaction covers every segment that could hold an older
// version of the key (a "full" compaction); otherwise an old value in a
// non-participating segment would resurrect. This is exactly the invariant
// checked by tla/AsterLsmIndex.tla's Compact action (NoResurrection).
//
// When HNSW is enabled, rebuilds one fresh graph over live rows (docs/
// indexing.md §6.2 Rebuild) and leaves the segment READY — input graphs are
// never merged. Tiny / no-HNSW builds stay PENDING with exact search only.
std::shared_ptr<const Segment> CompactSegments(
    uint64_t new_id, Metric metric,
    const std::vector<std::shared_ptr<const Segment>>& inputs,
    bool drop_tombstones
#if ASTER_ENABLE_HNSW
    ,
    HnswParams hnsw_params = {}, uint64_t hnsw_rng_seed = 1,
    bool enable_insert_into_largest = false,
    double insert_largest_staleness_debt_threshold = 0.3
#endif
);

}  // namespace aster
