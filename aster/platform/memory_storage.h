#pragma once

#include <map>
#include <string>

#include "aster/platform/storage_backend.h"

namespace aster {

// In-memory StorageBackend for unit tests and ephemeral deployments.
class MemoryStorage final : public StorageBackend {
 public:
  Status Put(const std::string& path, const std::string& data) override;
  Result<std::string> Read(const std::string& path) override;
  Status Remove(const std::string& path) override;
  Result<std::vector<std::string>> List(const std::string& prefix) override;
  bool Exists(const std::string& path) override;

 private:
  std::map<std::string, std::string> objects_;
};

}  // namespace aster
