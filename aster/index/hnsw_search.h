#pragma once

#include "aster/core/features.h"

#if ASTER_ENABLE_HNSW

#include <cstdint>
#include <utility>
#include <vector>

#include "aster/core/types.h"
#include "aster/index/hnsw_graph.h"

namespace aster {

// HNSW query over an immutable graph + in-memory vectors (docs/indexing.md
// §2.1). `vectors[i]` is the vector for graph node i (builder default).
// Returns up to top_k (node_id, score) pairs, best score first.
//
// ef_search is the beam width at layer 0 (per-query recall/latency knob).
// 0 means graph.params().ef_search_default. Always clamped to ≥ top_k.
std::vector<std::pair<uint32_t, float>> HnswSearch(
    Metric metric, const HnswGraph& graph,
    const std::vector<std::vector<float>>& vectors, VectorView query,
    uint32_t top_k, uint32_t ef_search);

}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
