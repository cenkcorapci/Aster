#pragma once

#include <memory>
#include <vector>

#include "aster/core/types.h"

namespace aster {

// HNSW build/search parameters. See docs/indexing.md for the full reference
// on how these interact with recall, latency, and memory.
struct HnswParams {
  uint32_t m = 16;               // max neighbors per node (layers > 0)
  uint32_t ef_construction = 128;
  uint32_t ef_search_default = 64;
  uint32_t max_layers = 16;
};

// Immutable per-segment vector index. Built once when a memtable is flushed
// or when segments are compacted, then never mutated (see docs/indexing.md,
// "Segmented HNSW"). Implementations must be safe for concurrent Search().
class VectorIndex {
 public:
  virtual ~VectorIndex() = default;

  virtual size_t size() const = 0;

  // Returns up to top_k hits with scores where higher is better.
  // ef_search trades recall for latency; exact implementations ignore it.
  virtual std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                        uint32_t ef_search) const = 0;
};

// Entry describing one vector to index at build time.
struct IndexEntry {
  RowId id;
  VectorView vector;
};

// Exact (brute-force) index. The correctness baseline: HNSW recall is
// always measured against this. Also the search path for the Tiny profile
// (Arduino/ESP32) and for small unmerged segments.
std::unique_ptr<VectorIndex> BuildExactIndex(Metric metric,
                                             std::vector<IndexEntry> entries);

// Segmented HNSW build lands in milestone M2 (docs/development-plan.md):
// std::unique_ptr<VectorIndex> BuildHnswIndex(Metric, HnswParams,
//                                             std::vector<IndexEntry>);

}  // namespace aster
