#include "aster/storage/compaction.h"

#include <algorithm>
#include <limits>
#include <map>

namespace aster {

int SizeTieredBucket(size_t size, size_t bucket_ratio) {
  if (bucket_ratio < 2) bucket_ratio = 2;
  if (size <= 1) return 0;
  int bucket = 0;
  // upper is the exclusive end of the current bucket: [prev, upper).
  size_t upper = bucket_ratio;
  while (size >= upper && bucket < 62) {
    if (upper > std::numeric_limits<size_t>::max() / bucket_ratio) {
      ++bucket;
      break;
    }
    upper *= bucket_ratio;
    ++bucket;
  }
  return bucket;
}

std::optional<SizeTieredPick> SelectSizeTieredCompaction(
    const std::vector<size_t>& sizes, size_t tier_threshold,
    size_t bucket_ratio) {
  if (tier_threshold == 0 || sizes.size() < 2) return std::nullopt;
  if (bucket_ratio < 2) bucket_ratio = 2;

  // tier → indices in that tier (oldest-first order preserved).
  std::map<int, std::vector<size_t>> by_tier;
  for (size_t i = 0; i < sizes.size(); ++i) {
    by_tier[SizeTieredBucket(sizes[i], bucket_ratio)].push_back(i);
  }

  for (auto& [tier, indices] : by_tier) {
    if (indices.size() < tier_threshold) continue;
    SizeTieredPick pick;
    pick.tier = tier;
    pick.input_indices = std::move(indices);
    return pick;
  }
  return std::nullopt;
}

}  // namespace aster
