#pragma once

#include <cstddef>
#include <cstdint>

namespace aster {

// Placement of sealed immutable data (Qdrant-style cold/cached/pinned).
// Encoding (float16/int8/…) is orthogonal — see docs/vector-encoding.md.
enum class Residency : uint8_t {
  kPinned = 0,  // materialised on the heap; never reclaimed
  kCached = 1,  // mmap + prefault / bounded hot-page promotion
  kCold = 2,    // mmap / demand-paged (lowest steady-state RAM)
};

struct StoragePolicy {
  // Default cold: sealed vector payloads must not pin anonymous RSS.
  Residency vectors = Residency::kCold;
  Residency graph_base = Residency::kCold;
  Residency graph_upper = Residency::kPinned;
  Residency approx_codes = Residency::kCached;
  // Soft budget for optional hot-page promotion (Milestone B).
  size_t cache_budget_bytes = 256ull << 20;
};

inline constexpr const char* ToString(Residency r) {
  switch (r) {
    case Residency::kPinned:
      return "pinned";
    case Residency::kCached:
      return "cached";
    case Residency::kCold:
      return "cold";
  }
  return "cold";
}

}  // namespace aster
