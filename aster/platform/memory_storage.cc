#include "aster/platform/memory_storage.h"

namespace aster {

Status MemoryStorage::Put(const std::string& path, const std::string& data) {
  objects_[path] = data;
  return Status::Ok();
}

Result<std::string> MemoryStorage::Read(const std::string& path) {
  auto it = objects_.find(path);
  if (it == objects_.end()) return Status::NotFound(path);
  return it->second;
}

Status MemoryStorage::Remove(const std::string& path) {
  if (objects_.erase(path) == 0) return Status::NotFound(path);
  return Status::Ok();
}

Result<std::vector<std::string>> MemoryStorage::List(
    const std::string& prefix) {
  std::vector<std::string> out;
  for (auto it = objects_.lower_bound(prefix); it != objects_.end(); ++it) {
    if (it->first.compare(0, prefix.size(), prefix) != 0) break;
    out.push_back(it->first);
  }
  return out;
}

bool MemoryStorage::Exists(const std::string& path) {
  return objects_.count(path) > 0;
}

}  // namespace aster
