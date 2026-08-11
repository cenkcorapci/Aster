#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aster {

// Collection storage cache modes (docs/client-api.md § Storage modes).
//
// Semantics implemented for M8-T03 (mode switch is safe online):
//
//   HOT  — default. Local durable path only (RAM + SSD). No object-store I/O.
//   WARM — keep the searchable index local; also mirror segment/index objects
//          to the configured object store so a peer can recover without a
//          full resync. Block cache is retained across searches.
//   COLD — prefer object-store-backed durability. Segment/index objects are
//          mirrored to S3; HNSW upper layers are pinned locally (non-evictable).
//          Each Search clears the S3 block cache to model a cold worker while
//          pins survive (docs/indexing.md §10.3.1).
enum class StorageMode : uint8_t {
  kHot = 0,
  kWarm = 1,
  kCold = 2,
};

inline constexpr const char* ToString(StorageMode mode) {
  switch (mode) {
    case StorageMode::kHot:
      return "HOT";
    case StorageMode::kWarm:
      return "WARM";
    case StorageMode::kCold:
      return "COLD";
  }
  return "HOT";
}

inline std::optional<StorageMode> StorageModeFromString(std::string_view s) {
  if (s == "HOT" || s == "hot") return StorageMode::kHot;
  if (s == "WARM" || s == "warm") return StorageMode::kWarm;
  if (s == "COLD" || s == "cold") return StorageMode::kCold;
  return std::nullopt;
}

// Policy derived from a storage mode. Kept as free functions so tests can
// assert mode-switch behaviour without spinning up a full Db.
inline constexpr bool MirrorsToObjectStore(StorageMode mode) {
  return mode == StorageMode::kWarm || mode == StorageMode::kCold;
}

inline constexpr bool PinsHnswUpperLayers(StorageMode mode) {
  return mode == StorageMode::kCold || mode == StorageMode::kWarm;
}

inline constexpr bool ClearsBlockCacheOnSearch(StorageMode mode) {
  return mode == StorageMode::kCold;
}

inline constexpr bool KeepsLocalIndex(StorageMode mode) {
  // All current modes keep a local searchable index; COLD additionally
  // relies on object-store pins for cold-worker recovery.
  (void)mode;
  return true;
}

}  // namespace aster
