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
// Segmented HNSW: build via HnswBuilder (hnsw_build.h); query via HnswSearch /
// BuildHnswIndex (hnsw_search.h). Graph: HnswGraph (docs/hnsw-format.md).
std::unique_ptr<VectorIndex> BuildHnswIndex(Metric metric, HnswParams params,
                                            std::vector<IndexEntry> entries,
                                            uint64_t rng_seed = 1);

// Build an HNSW vector index from a pre-built graph and explicit vectors/ids
// arrays (used by compaction merge strategies that reuse an in-memory graph).
std::unique_ptr<VectorIndex> BuildHnswIndexFromGraph(
    Metric metric, HnswGraph graph,
    std::vector<std::vector<float>> vectors, std::vector<RowId> ids);

// Compaction rebuild strategy (docs/indexing.md §6.2): fresh graph over
// live (non-tombstone, non-empty) rows only — never merge input graphs.
std::unique_ptr<VectorIndex> RebuildHnswFromLiveRows(
    Metric metric, HnswParams params, const std::vector<Row>& rows,
    uint64_t rng_seed = 1);
#endif

}  // namespace aster
