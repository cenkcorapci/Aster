#pragma once

#include <string>
#include <vector>

#include "aster/core/status.h"

namespace aster {

// Platform abstraction for object storage (docs/code-structure.md). The
// storage engine never knows whether segments live on a local disk, SPI
// flash, or S3 — everything above this interface is identical on all
// targets. Objects are immutable: written once, then only read or removed.
//
// Planned implementations: PosixStorage, S3Storage, LittleFsStorage,
// ArduinoStorage. MemoryStorage (below) exists for tests and the Tiny
// profile.
class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual Status Put(const std::string& path, const std::string& data) = 0;
  virtual Result<std::string> Read(const std::string& path) = 0;
  virtual Status Remove(const std::string& path) = 0;
  virtual Result<std::vector<std::string>> List(const std::string& prefix) = 0;
  virtual bool Exists(const std::string& path) = 0;
};

}  // namespace aster
