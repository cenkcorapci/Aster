#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"

namespace aster {

// Single-node, single-collection engine: memtable + immutable indexed
// segments, searched together and merged (LSM-style, docs/design.md).
//
// This is the embedded ("SQLite-mode") entry point and the storage engine
// each vnode runs in cluster mode. WAL wiring, background flush/compaction
// threads, and the SSTable on-disk format are milestone M1; here flush and
// compaction are explicit calls so the semantics stay easy to test against
// the TLA+ spec (tla/AsterLsmIndex.tla).
class Db {
 public:
  struct Options {
    uint32_t dimension = 0;
    Metric metric = Metric::kCosine;
    size_t memtable_flush_bytes = 64 << 20;
  };

  explicit Db(Options options) : options_(options) {}

  Status Upsert(Row row);
  Status Delete(const RowId& id, Timestamp timestamp);

  // Point lookup: memtable first, then segments newest-to-oldest, LWW.
  std::optional<Row> Get(const RowId& id) const;

  // Scatter-gather ANN: searches memtable rows and every segment index,
  // merges top-k, applies tag post-filtering. Tombstones never surface.
  std::vector<SearchHit> Search(const SearchRequest& request) const;

  // Seals the memtable into a new immutable segment (with vector index).
  void Flush();

  // Full compaction: merges all segments, LWW, purges tombstones.
  void Compact();

  size_t segment_count() const { return segments_.size(); }
  size_t memtable_rows() const { return memtable_.row_count(); }

 private:
  Row Reconcile(const RowId& id) const;

  Options options_;
  Memtable memtable_;
  std::vector<std::shared_ptr<const Segment>> segments_;  // oldest first
  uint64_t next_segment_id_ = 1;
};

}  // namespace aster
