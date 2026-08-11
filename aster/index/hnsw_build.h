#pragma once

#include "aster/core/features.h"

#if ASTER_ENABLE_HNSW

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/index/hnsw_graph.h"

namespace aster {

// Sequential HNSW insert/build over an in-memory vector set (docs/indexing.md
// §2.2). Produces an HnswGraph via AddNode/SetNeighbors; search is M2-T03.
class HnswBuilder {
 public:
  explicit HnswBuilder(Metric metric, HnswParams params = {},
                       uint64_t rng_seed = 1);

  // Node i maps to row_ordinal i. Empty input yields an empty graph.
  Result<HnswGraph> Build(const std::vector<std::vector<float>>& vectors);

  // Same as Build, but RowOrdinal(i) = row_ordinals[i] (must match size).
  Result<HnswGraph> Build(const std::vector<std::vector<float>>& vectors,
                          const std::vector<uint32_t>& row_ordinals);

  // Diversity heuristic from the HNSW paper, using Score (higher = closer):
  // keep a candidate only if it is closer to `base` than to every neighbor
  // already selected. Returns at most `max_keep` node ids.
  static std::vector<uint32_t> SelectNeighborsHeuristic(
      Metric metric, VectorView base,
      const std::vector<std::pair<uint32_t, VectorView>>& candidates,
      uint32_t max_keep);

 private:
  uint8_t SampleLevel();

  Metric metric_;
  HnswParams params_;
  std::mt19937_64 rng_;
};

}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
