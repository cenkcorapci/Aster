#pragma once

#include <vector>

#include "aster/core/types.h"

namespace aster {

// Merges per-segment (or per-replica) candidate lists into a single top-k
// result. Duplicate ids can appear when the same row lives in several
// sources (memtable + segment before compaction, or multiple replicas);
// the highest-scoring instance wins. Scores are "higher is better".
std::vector<SearchHit> MergeTopK(
    const std::vector<std::vector<SearchHit>>& candidate_lists,
    uint32_t top_k);

}  // namespace aster
