#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aster/core/status.h"

namespace aster {

// One live SSTable referenced by a published manifest generation.
struct ManifestEntry {
  uint64_t segment_id = 0;
  std::string path;  // relative or absolute .ast path
};

// Durable list of live segments. Publication is write-temp + rename
// (docs/sstable-format.md §10, tasks M1-T05).
struct Manifest {
  uint64_t generation = 0;
  std::vector<ManifestEntry> segments;
};

Status WriteManifest(const std::string& path, const Manifest& manifest);
Result<Manifest> ReadManifest(const std::string& path);

}  // namespace aster
