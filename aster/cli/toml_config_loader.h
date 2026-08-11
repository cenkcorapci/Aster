#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "aster/core/status.h"
#include "aster/rpc/server.h"
#include "aster/server/catalog.h"

namespace aster {
namespace cli {

// Parsed TOML knobs (only for server CLI / catalog startup).
// Missing optional fields mean: "use command/env defaults".
struct TomlServerConfig {
  std::optional<std::string> host;
  std::optional<uint16_t> port;
};

struct TomlCatalogConfig {
  // Required for durable mode: when empty, the CLI must provide it or fail.
  std::optional<std::string> data_dir;

  std::optional<SyncPolicy> wal_sync;
  std::optional<size_t> memtable_flush_bytes;
  std::optional<size_t> compaction_tier_threshold;
  std::optional<size_t> max_segments_before_compact;
};

struct TomlConfig {
  TomlServerConfig server;
  TomlCatalogConfig catalog;
};

// Parse TOML from an in-memory string (tests use this to avoid file IO).
Result<TomlConfig> LoadTomlConfigText(std::string_view source_name,
                                       std::string_view toml_text);

// Parse TOML from a file path.
Result<TomlConfig> LoadTomlConfigFile(const std::string& path);

}  // namespace cli
}  // namespace aster

