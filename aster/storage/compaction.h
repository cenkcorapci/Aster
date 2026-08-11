#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aster {

// Size-tiered (Cassandra-style) compaction picker.
//
// Segments are bucketed by size into exponential tiers with ratio R
// (default 4): tier 0 holds sizes in [1, R), tier 1 in [R, R^2), …
// When any tier contains at least `tier_threshold` segments, those
// segments are selected for merge (lowest overflowing tier first so
// merges cascade upward).
//
// `sizes[i]` is the size of segment i (row count is a fine proxy when
// vectors share a fixed dimension). Returned indices refer into `sizes`
// and are sorted ascending (oldest-first when the caller stores segments
// oldest-first).

struct SizeTieredPick {
  std::vector<size_t> input_indices;
  int tier = 0;
};

// Bucket index for a single segment size. size==0 maps to tier 0.
int SizeTieredBucket(size_t size, size_t bucket_ratio = 4);

// Returns a pick when some tier has >= tier_threshold members.
// tier_threshold==0 or sizes.size()<2 → no pick.
std::optional<SizeTieredPick> SelectSizeTieredCompaction(
    const std::vector<size_t>& sizes, size_t tier_threshold,
    size_t bucket_ratio = 4);

}  // namespace aster
