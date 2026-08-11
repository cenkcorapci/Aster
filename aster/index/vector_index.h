#pragma once

#include <memory>
#include <vector>

#include "aster/core/features.h"
#include "aster/core/types.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_graph.h"  // HnswParams, HnswGraph (M2-T01)
#endif

namespace aster {

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

// Zero-copy build over Segment-owned rows (vectors live once in `rows`).
std::unique_ptr<VectorIndex> BuildExactIndex(
    Metric metric, std::shared_ptr<const std::vector<Row>> rows);

#if ASTER_ENABLE_HNSW
// Segmented HNSW build/search land in M2-T02 / M2-T03:
// std::unique_ptr<VectorIndex> BuildHnswIndex(Metric, HnswParams,
//                                             std::vector<IndexEntry>);
// Graph topology + on-disk format: HnswGraph (hnsw_graph.h, docs/hnsw-format.md).
#endif

}  // namespace aster
