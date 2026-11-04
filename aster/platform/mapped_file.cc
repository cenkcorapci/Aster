#include "aster/platform/mapped_file.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aster {
namespace {

size_t PageSize() {
  const long p = ::sysconf(_SC_PAGESIZE);
  return p > 0 ? static_cast<size_t>(p) : 4096u;
}

void AdviseRange(void* base, size_t mapped_len, int advice) {
  if (base == nullptr || mapped_len == 0) return;
#if defined(MADV_RANDOM) || defined(MADV_SEQUENTIAL) || defined(MADV_WILLNEED)
  (void)::madvise(base, mapped_len, advice);
#else
  (void)base;
  (void)mapped_len;
  (void)advice;
#endif
}

std::pair<void*, size_t> PageAligned(const uint8_t* data, size_t file_size,
                                     size_t offset, size_t len) {
  if (data == nullptr || len == 0 || offset > file_size) {
    return {nullptr, 0};
  }
  const size_t end = offset + len;
  if (end < offset || end > file_size) return {nullptr, 0};
  const size_t page = PageSize();
  const size_t base_off = offset & ~(page - 1);
  const size_t map_len = end - base_off;
  return {const_cast<uint8_t*>(data) + base_off, map_len};
}

}  // namespace

MappedFile::MappedFile(int fd, uint8_t* data, size_t size)
    : fd_(fd), data_(data), size_(size) {}

MappedFile::~MappedFile() {
  if (data_ != nullptr && data_ != MAP_FAILED && size_ > 0) {
    ::munmap(data_, size_);
  }
  data_ = nullptr;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Result<std::shared_ptr<MappedFile>> MappedFile::Open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return Status::IoError(std::string("mmap open failed: ") +
                           std::strerror(errno));
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int err = errno;
    ::close(fd);
    return Status::IoError(std::string("mmap fstat failed: ") +
                           std::strerror(err));
  }
  if (st.st_size < 0) {
    ::close(fd);
    return Status::Corruption("mmap negative file size");
  }
  const size_t size = static_cast<size_t>(st.st_size);
  if (size == 0) {
    // Empty file: no mapping; keep a valid object with null data.
    return std::shared_ptr<MappedFile>(new MappedFile(fd, nullptr, 0));
  }
  void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    const int err = errno;
    ::close(fd);
    return Status::IoError(std::string("mmap failed: ") + std::strerror(err));
  }
  return std::shared_ptr<MappedFile>(
      new MappedFile(fd, static_cast<uint8_t*>(p), size));
}

std::span<const uint8_t> MappedFile::slice(size_t offset, size_t len) const {
  if (data_ == nullptr || len == 0 || offset > size_ ||
      offset + len > size_) {
    return {};
  }
  return {data_ + offset, len};
}

void MappedFile::AdviseRandom(size_t offset, size_t len) const {
  auto [base, map_len] = PageAligned(data_, size_, offset, len);
#ifdef MADV_RANDOM
  AdviseRange(base, map_len, MADV_RANDOM);
#else
  (void)base;
  (void)map_len;
#endif
}

void MappedFile::AdviseSequential(size_t offset, size_t len) const {
  auto [base, map_len] = PageAligned(data_, size_, offset, len);
#ifdef MADV_SEQUENTIAL
  AdviseRange(base, map_len, MADV_SEQUENTIAL);
#else
  (void)base;
  (void)map_len;
#endif
}

void MappedFile::Prefault(size_t offset, size_t len) const {
  auto [base, map_len] = PageAligned(data_, size_, offset, len);
#ifdef MADV_WILLNEED
  AdviseRange(base, map_len, MADV_WILLNEED);
#else
  (void)base;
  (void)map_len;
#endif
  // Touch one byte per page so Prefault is effective even without madvise.
  if (base == nullptr || map_len == 0 || data_ == nullptr) return;
  const size_t page = PageSize();
  volatile uint8_t sink = 0;
  for (size_t i = 0; i < map_len; i += page) {
    sink = static_cast<uint8_t*>(base)[i];
  }
  (void)sink;
}

}  // namespace aster
