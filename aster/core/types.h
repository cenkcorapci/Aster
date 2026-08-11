#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aster {

// Row identifier. UUIDs are stored in canonical string form for now;
// a 16-byte binary representation is planned (see docs/development-plan.md).
using RowId = std::string;

// Microseconds since epoch. Used for last-write-wins conflict resolution.
using Timestamp = uint64_t;

// Monotonic per-row version counter, paired with Timestamp for LWW.
using Version = uint64_t;

// Non-owning view over a dense float32 vector.
using VectorView = std::span<const float>;

enum class Metric {
  kL2,
  kDot,
  kCosine,
};

// A single row, matching the data model in docs/design.md.
struct Row {
  RowId id;
  std::vector<float> vector;
  // Opaque per-row blob. Storage does not interpret contents; CBOR is the
  // recommended encoding (see aster/core/cbor.h and docs/sstable-format.md).
  std::string metadata;
  std::set<std::string> tags;
  Timestamp timestamp = 0;      // write time, LWW tiebreaker
  Version version = 0;
  bool tombstone = false;       // deletion marker; purged at compaction
};

// LWW ordering: (timestamp, version) — higher wins.
inline bool NewerThan(const Row& a, const Row& b) {
  if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
  return a.version > b.version;
}

// One approximate-nearest-neighbor hit.
struct SearchHit {
  RowId id;
  float score = 0.0f;  // higher is better, regardless of metric
};

struct SearchRequest {
  std::vector<float> vector;
  uint32_t top_k = 10;
  uint32_t ef_search = 64;             // per-query recall/latency knob
  std::set<std::string> tags;          // post-filter; empty = no filter
};

}  // namespace aster
