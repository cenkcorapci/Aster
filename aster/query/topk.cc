#include "aster/query/topk.h"

#include <algorithm>
#include <map>

namespace aster {

std::vector<SearchHit> MergeTopK(
    const std::vector<std::vector<SearchHit>>& candidate_lists,
    uint32_t top_k) {
  std::map<RowId, float> best;
  for (const auto& list : candidate_lists) {
    for (const auto& hit : list) {
      auto it = best.find(hit.id);
      if (it == best.end()) {
        best.emplace(hit.id, hit.score);
      } else if (hit.score > it->second) {
        it->second = hit.score;
      }
    }
  }

  std::vector<SearchHit> merged;
  merged.reserve(best.size());
  for (const auto& [id, score] : best) merged.push_back({id, score});

  const size_t k = std::min<size_t>(top_k, merged.size());
  std::partial_sort(merged.begin(), merged.begin() + k, merged.end(),
                    [](const SearchHit& a, const SearchHit& b) {
                      return a.score > b.score;
                    });
  merged.resize(k);
  return merged;
}

}  // namespace aster
