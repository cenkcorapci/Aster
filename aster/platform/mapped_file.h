#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "aster/core/status.h"

namespace aster {

// Read-only POSIX mmap of an immutable file (sealed SSTables / .hnsw).
// Untouched pages consume no anonymous RAM; the kernel may reclaim them.
class MappedFile {
 public:
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  ~MappedFile();

  static Result<std::shared_ptr<MappedFile>> Open(const std::string& path);

  size_t size() const { return size_; }
  const uint8_t* data() const { return data_; }
  std::span<const uint8_t> bytes() const {
    return {data_, size_};
  }
  std::string_view view() const {
    return {reinterpret_cast<const char*>(data_), size_};
  }
  std::span<const uint8_t> slice(size_t offset, size_t len) const;

  // Access hints for sealed vector / graph extents.
  void AdviseRandom(size_t offset, size_t len) const;
  void AdviseSequential(size_t offset, size_t len) const;
  void Prefault(size_t offset, size_t len) const;

 private:
  MappedFile(int fd, uint8_t* data, size_t size);

  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace aster
