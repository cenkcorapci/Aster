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

// An immutable segment: the unit of storage, indexing, compaction, and
// replication repair. Produced by a memtable flush or by compacting other
// segments; never mutated afterwards (lock-free reads).
//
// Rows and the exact index share one vector copy (shared_ptr) so search
// stays cache-friendly without doubling RAM.
class Segment {
 public:
  // `rows` must be sorted by id and deduplicated (one version per id).
  // Tombstones are kept so compaction and read-repair can honor deletes.
  static std::shared_ptr<const Segment> Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows);
  static std::shared_ptr<const Segment> Build(
      uint64_t id, Metric metric,
      std::shared_ptr<std::vector<Row>> rows);

  uint64_t id() const { return id_; }
  size_t row_count() const { return rows_ ? rows_->size() : 0; }
  const std::vector<Row>& rows() const { return *rows_; }
  const TagIndex& tag_index() const { return tag_index_; }

  // Point lookup by binary search over the sorted id index.
  std::optional<Row> Get(const RowId& row_id) const;

  // ANN search over non-tombstoned rows of this segment.
  // When `tags` is non-empty, only ordinals matching the tag bitmap AND are
  // scored (post-filter / bitmap-driven exact scan — docs/indexing.md §7).
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
  std::unique_ptr<VectorIndex> index_;
  TagIndex tag_index_;
};

// Merges segments into one, applying LWW per id. Tombstones may only be
// dropped when the compaction covers every segment that could hold an older
// version of the key (a "full" compaction); otherwise an old value in a
// non-participating segment would resurrect. This is exactly the invariant
// checked by tla/AsterLsmIndex.tla's Compact action (NoResurrection).
std::shared_ptr<const Segment> CompactSegments(
    uint64_t new_id, Metric metric,
    const std::vector<std::shared_ptr<const Segment>>& inputs,
    bool drop_tombstones);

}  // namespace aster
