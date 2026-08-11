#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "aster/core/types.h"

namespace aster {

// Sparse bitmap over row ordinals with a roaring-compatible API.
//
// M2 skeleton: sorted unique uint32 ordinals + dense little-endian bitset
// serialization matching docs/sstable-format.md §7.6 `roaring_blob` (the
// format already written by SSTables). Call sites use Add / Contains / And /
// Cardinality / Serialize so a future CRoaring backend can drop in without
// changing TagIndex or search callers. No third-party dep — keeps Tiny /
// baremetal builds working.
class TagBitmap {
 public:
  TagBitmap() = default;

  void Add(uint32_t ordinal);
  bool Contains(uint32_t ordinal) const;
  uint32_t Cardinality() const {
    return static_cast<uint32_t>(ordinals_.size());
  }
  bool empty() const { return ordinals_.empty(); }

  // In-place intersection (AND). Empty ∩ X = empty.
  void AndInPlace(const TagBitmap& other);
  static TagBitmap And(const TagBitmap& a, const TagBitmap& b);

  const std::vector<uint32_t>& ordinals() const { return ordinals_; }

  // Dense little-endian bitset over [0, universe_size). Matches current
  // SSTable tag payloads (portable Roaring bytes land when CRoaring is wired).
  std::string SerializeDense(uint32_t universe_size) const;
  static TagBitmap DeserializeDense(std::string_view blob,
                                    uint32_t universe_size);

 private:
  std::vector<uint32_t> ordinals_;  // sorted, unique
};

// Per-segment tag → bitmap index (docs/indexing.md §7, docs/design.md).
class TagIndex {
 public:
  TagIndex() = default;

  static TagIndex Build(const std::vector<Row>& rows);

  uint32_t row_count() const { return row_count_; }
  bool empty() const { return tags_.empty(); }

  const TagBitmap* Find(std::string_view tag) const;

  // AND of all requested tags. Missing tag ⇒ empty bitmap (no matches).
  // Empty `wanted` ⇒ full [0, row_count) universe.
  TagBitmap Matching(const std::set<std::string>& wanted) const;

  uint32_t MatchCount(const std::set<std::string>& wanted) const {
    return Matching(wanted).Cardinality();
  }

  // σ = |matching| / row_count. Returns 1.0 when no filter or empty segment.
  double Selectivity(const std::set<std::string>& wanted) const;

  const std::map<std::string, TagBitmap>& tags() const { return tags_; }

  // SSTable §7.6 uncompressed payload (tag_count + tag + roaring_blob…).
  std::string SerializePayload() const;
  static TagIndex ParsePayload(std::string_view payload, uint32_t row_count);

 private:
  uint32_t row_count_ = 0;
  std::map<std::string, TagBitmap> tags_;
};

// Adaptive over-fetch for filtered ANN (docs/indexing.md §7):
//   fetch_k ← clamp(k / max(σ, σ_min), 2k+16, max(ef_search, 2k+16))
// Unfiltered callers should use BaseFetchK(k) (= 2k+16).
inline uint32_t BaseFetchK(uint32_t k) { return k * 2u + 16u; }

inline uint32_t AdaptiveFetchK(uint32_t k, uint32_t ef_search, double sigma,
                               double sigma_min = 0.01) {
  if (k == 0) return 0;
  const uint32_t floor = BaseFetchK(k);
  const double s = sigma > sigma_min ? sigma : sigma_min;
  if (!(s > 0.0)) return floor;
  const double desired = static_cast<double>(k) / s;
  uint32_t fetch = static_cast<uint32_t>(std::ceil(desired));
  if (fetch < floor) fetch = floor;
  const uint32_t ceiling = ef_search > floor ? ef_search : floor;
  if (fetch > ceiling) fetch = ceiling;
  return fetch;
}

}  // namespace aster
