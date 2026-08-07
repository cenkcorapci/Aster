#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "aster/core/types.h"

namespace aster {

// In-memory write buffer. Rows accumulate here (after WAL append) until the
// table is flushed into an immutable segment. Single-writer for now; the
// sharded lock-free memtable is milestone M1 work.
//
// Deletes are recorded as tombstone rows so they propagate through flush,
// segment search, and compaction (see tla/AsterLsmIndex.tla).
class Memtable {
 public:
  // Applies a write with LWW semantics: an older write never overwrites a
  // newer one. Returns true if the row was applied.
  bool Apply(Row row);

  // Live row lookup. Tombstoned rows report as not found via `tombstone`.
  std::optional<Row> Get(const RowId& id) const;

  // All rows including tombstones, in id order (copy). Prefer Take()/ForEach.
  std::vector<Row> Scan() const;

  // Move rows out for flush — avoids an extra full copy of the working set.
  std::vector<Row> Take();

  // Zero-copy iteration for search / stats.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& [_, row] : rows_) fn(row);
  }

  size_t row_count() const { return rows_.size(); }
  size_t approximate_bytes() const { return approximate_bytes_; }
  bool empty() const { return rows_.empty(); }

 private:
  std::map<RowId, Row> rows_;
  size_t approximate_bytes_ = 0;
};

}  // namespace aster
