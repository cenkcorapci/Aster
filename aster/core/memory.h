#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "aster/core/types.h"

namespace aster {

// Approximate in-memory footprint of a Row (id, vector, metadata, tags,
// plus sizeof(Row) overhead). Used for write-path memory budgeting.
size_t EstimateRowBytes(const Row& row);

// Bump / arena allocator for short-lived write-path buffers.
// Allocations are freed only on Reset() or destruction — no per-object free.
// Not thread-safe; Db serializes writers on mu_.
class Arena {
 public:
  static constexpr size_t kDefaultBlockSize = 4096;

  explicit Arena(size_t block_size = kDefaultBlockSize);
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept;
  Arena& operator=(Arena&&) noexcept;

  // Allocates `n` bytes (8-byte aligned). Returns nullptr if n == 0.
  // Aborts on out-of-memory (unrecoverable on the write path).
  void* Allocate(size_t n);

  template <typename T>
  T* AllocateArray(size_t count) {
    if (count == 0) return nullptr;
    return static_cast<T*>(Allocate(count * sizeof(T)));
  }

  // Bytes handed out since construction / last Reset (not block capacity).
  size_t MemoryUsage() const { return used_; }
  // Sum of allocated block capacities (upper bound on RSS for this arena).
  size_t AllocatedBytes() const { return capacity_; }
  size_t block_size() const { return block_size_; }

  // Retains allocated blocks for reuse; clears bump pointers and used_.
  void Reset();

 private:
  struct Block {
    char* data = nullptr;
    size_t size = 0;
  };

  void EnsureBlock(size_t need);

  size_t block_size_;
  std::vector<Block> blocks_;
  size_t block_index_ = 0;
  char* current_ = nullptr;
  size_t remaining_ = 0;
  size_t used_ = 0;
  size_t capacity_ = 0;
};

// Fixed-size object free-list backed by an Arena. Free() returns slots to the
// list; Reset the underlying Arena only when no live slab pointers remain.
class Slab {
 public:
  Slab(Arena* arena, size_t slot_size, size_t slots_per_chunk = 64);

  void* Alloc();
  void Free(void* ptr);

  size_t slot_size() const { return slot_size_; }
  size_t in_use() const { return in_use_; }

 private:
  void Grow();

  Arena* arena_;
  size_t slot_size_;
  size_t slots_per_chunk_;
  void* free_list_ = nullptr;
  size_t in_use_ = 0;
};

// Hard cap helper for write-path memory. limit_bytes == 0 means unlimited.
class MemoryBudget {
 public:
  explicit MemoryBudget(size_t limit_bytes = 0) : limit_(limit_bytes) {}

  bool unlimited() const { return limit_ == 0; }
  size_t limit() const { return limit_; }
  size_t used() const { return used_; }

  void ResetUsed(size_t used) { used_ = used; }
  void Add(size_t n) { used_ += n; }
  void Sub(size_t n) { used_ = (n >= used_) ? 0 : used_ - n; }

  // True when adding `n` would stay within the limit (always true if unlimited).
  bool WouldAccept(size_t n) const {
    if (unlimited()) return true;
    return used_ <= limit_ && n <= limit_ - used_;
  }

 private:
  size_t limit_ = 0;
  size_t used_ = 0;
};

}  // namespace aster
