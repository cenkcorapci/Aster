#include "aster/core/memory.h"

#include <algorithm>
#include <cstdlib>

namespace aster {
namespace {

constexpr size_t kAlign = 8;

size_t AlignUp(size_t n) {
  return (n + (kAlign - 1)) & ~(kAlign - 1);
}

}  // namespace

size_t EstimateRowBytes(const Row& row) {
  size_t bytes = row.id.size() + row.vector.size() * sizeof(float) +
                 row.metadata.size() + sizeof(Row);
  for (const auto& tag : row.tags) bytes += tag.size();
  return bytes;
}

Arena::Arena(size_t block_size)
    : block_size_(std::max(block_size, kAlign)) {}

Arena::~Arena() {
  for (Block& b : blocks_) std::free(b.data);
}

Arena::Arena(Arena&& other) noexcept
    : block_size_(other.block_size_),
      blocks_(std::move(other.blocks_)),
      block_index_(other.block_index_),
      current_(other.current_),
      remaining_(other.remaining_),
      used_(other.used_),
      capacity_(other.capacity_) {
  other.block_index_ = 0;
  other.current_ = nullptr;
  other.remaining_ = 0;
  other.used_ = 0;
  other.capacity_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
  if (this == &other) return *this;
  for (Block& b : blocks_) std::free(b.data);
  block_size_ = other.block_size_;
  blocks_ = std::move(other.blocks_);
  block_index_ = other.block_index_;
  current_ = other.current_;
  remaining_ = other.remaining_;
  used_ = other.used_;
  capacity_ = other.capacity_;
  other.block_index_ = 0;
  other.current_ = nullptr;
  other.remaining_ = 0;
  other.used_ = 0;
  other.capacity_ = 0;
  return *this;
}

void Arena::EnsureBlock(size_t need) {
  while (block_index_ < blocks_.size()) {
    Block& b = blocks_[block_index_++];
    if (b.size >= need) {
      current_ = b.data;
      remaining_ = b.size;
      return;
    }
  }

  const size_t bytes = std::max(block_size_, AlignUp(need));
  char* mem = static_cast<char*>(std::malloc(bytes));
  if (mem == nullptr) std::abort();
  blocks_.push_back(Block{mem, bytes});
  capacity_ += bytes;
  ++block_index_;
  current_ = mem;
  remaining_ = bytes;
}

void* Arena::Allocate(size_t n) {
  if (n == 0) return nullptr;
  const size_t need = AlignUp(n);
  if (need > remaining_) {
    EnsureBlock(need);
  }
  void* out = current_;
  current_ += need;
  remaining_ -= need;
  used_ += need;
  return out;
}

void Arena::Reset() {
  block_index_ = 0;
  current_ = nullptr;
  remaining_ = 0;
  used_ = 0;
}

Slab::Slab(Arena* arena, size_t slot_size, size_t slots_per_chunk)
    : arena_(arena),
      slot_size_(std::max(AlignUp(std::max(slot_size, sizeof(void*))),
                          sizeof(void*))),
      slots_per_chunk_(std::max<size_t>(slots_per_chunk, 1)) {}

void Slab::Grow() {
  char* chunk =
      static_cast<char*>(arena_->Allocate(slot_size_ * slots_per_chunk_));
  for (size_t i = 0; i < slots_per_chunk_; ++i) {
    void* slot = chunk + i * slot_size_;
    *static_cast<void**>(slot) = free_list_;
    free_list_ = slot;
  }
}

void* Slab::Alloc() {
  if (free_list_ == nullptr) Grow();
  void* out = free_list_;
  free_list_ = *static_cast<void**>(free_list_);
  ++in_use_;
  return out;
}

void Slab::Free(void* ptr) {
  if (ptr == nullptr) return;
  *static_cast<void**>(ptr) = free_list_;
  free_list_ = ptr;
  if (in_use_ > 0) --in_use_;
}

}  // namespace aster
