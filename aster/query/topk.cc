#include "aster/query/topk.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aster {

std::vector<SearchHit> MergeTopK(
    const std::vector<std::vector<SearchHit>>& candidate_lists,
    uint32_t top_k) {
  if (top_k == 0) return {};

  // Hash map avoids O(n log n) ordered inserts while de-duping ids across
  // memtable + segment candidate lists.
  std::unordered_map<RowId, float> best;
  size_t estimate = 0;
  for (const auto& list : candidate_lists) estimate += list.size();
  best.reserve(estimate);

  for (const auto& list : candidate_lists) {
    for (const auto& hit : list) {
      auto [it, inserted] = best.try_emplace(hit.id, hit.score);
      if (!inserted && hit.score > it->second) it->second = hit.score;
    }
  }

  std::vector<SearchHit> merged;
  merged.reserve(best.size());
  for (auto& [id, score] : best) {
    merged.push_back({std::move(id), score});
  }

  const size_t k = std::min<size_t>(top_k, merged.size());
  std::partial_sort(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(k),
                    merged.end(), [](const SearchHit& a, const SearchHit& b) {
                      return a.score > b.score;
                    });
  merged.resize(k);
  return merged;
}

}  // namespace aster
