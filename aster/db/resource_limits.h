#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aster {

// Per-collection tenancy / noisy-neighbor isolation (docs/client-api.md).
//
//   SHARED    — default. Collection shares the node with other collections;
//               configured numeric limits are enforced as soft quotas.
//   DEDICATED — collection is marked for reserved capacity. Limits are still
//               enforced as hard caps; the flag is persisted so control-plane
//               / scheduling layers (M8-T06+) can place dedicated workloads.
enum class IsolationLevel : uint8_t {
  kShared = 0,
  kDedicated = 1,
};

inline constexpr const char* ToString(IsolationLevel level) {
  switch (level) {
    case IsolationLevel::kShared:
      return "SHARED";
    case IsolationLevel::kDedicated:
      return "DEDICATED";
  }
  return "SHARED";
}

inline std::optional<IsolationLevel> IsolationLevelFromString(
    std::string_view s) {
  if (s == "SHARED" || s == "shared") return IsolationLevel::kShared;
  if (s == "DEDICATED" || s == "dedicated") return IsolationLevel::kDedicated;
  return std::nullopt;
}

// Per-collection resource quotas (docs/client-api.md § Resources).
// Numeric fields use 0 to mean unlimited.
struct ResourceLimits {
  // Reject Upsert of a new live id when live row count would exceed this.
  uint64_t max_vectors = 0;
  // Hard cap on write-path memory (memtable + write arena). Synced with
  // Db::Options::memory_budget_bytes when applied via SetResourceLimits /
  // CollectionConfig.
  size_t memory_budget_bytes = 0;
  // Reject Search when the rolling 1s window would exceed this many queries.
  uint32_t max_qps = 0;
  // Reject Upsert when estimated vector storage (live rows × dim × 4) would
  // exceed this many bytes.
  uint64_t storage_quota_bytes = 0;
  IsolationLevel isolation = IsolationLevel::kShared;

  bool unlimited() const {
    return max_vectors == 0 && memory_budget_bytes == 0 && max_qps == 0 &&
           storage_quota_bytes == 0;
  }
};

inline bool operator==(const ResourceLimits& a, const ResourceLimits& b) {
  return a.max_vectors == b.max_vectors &&
         a.memory_budget_bytes == b.memory_budget_bytes &&
         a.max_qps == b.max_qps &&
         a.storage_quota_bytes == b.storage_quota_bytes &&
         a.isolation == b.isolation;
}

inline bool operator!=(const ResourceLimits& a, const ResourceLimits& b) {
  return !(a == b);
}

// True when Dedicated isolation implies hard local enforcement of any set
// numeric limits (vs Shared, which uses the same enforcement today but is
// the multi-tenant default).
inline constexpr bool UsesHardLocalEnforcement(IsolationLevel level) {
  (void)level;
  // Both modes enforce configured caps locally on a single node; Dedicated
  // is a scheduling hint for future multi-tenant placement.
  return true;
}

}  // namespace aster
