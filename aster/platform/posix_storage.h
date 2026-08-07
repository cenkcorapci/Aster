#pragma once

#include <string>

#include "aster/platform/storage_backend.h"

namespace aster {

// POSIX filesystem StorageBackend (files under a root directory).
class PosixStorage final : public StorageBackend {
 public:
  explicit PosixStorage(std::string root);

  Status Put(const std::string& path, const std::string& data) override;
  Result<std::string> Read(const std::string& path) override;
  Status Remove(const std::string& path) override;
  Result<std::vector<std::string>> List(const std::string& prefix) override;
  bool Exists(const std::string& path) override;

 private:
  std::string Resolve(const std::string& path) const;
  std::string root_;
};

}  // namespace aster
