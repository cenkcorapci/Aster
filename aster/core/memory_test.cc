#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "aster/core/memory.h"
#include "aster/core/types.h"

namespace aster {
namespace {

TEST(EstimateRowBytes, CountsPayloadFields) {
  Row row;
  row.id = "abc";
  row.vector = {1.0f, 2.0f};
  row.metadata = "meta";
  row.tags = {"t1", "t2"};
  const size_t got = EstimateRowBytes(row);
  EXPECT_EQ(got, row.id.size() + row.vector.size() * sizeof(float) +
                     row.metadata.size() + sizeof(Row) + 2u + 2u);
}

TEST(Arena, AllocateAndResetReusesCapacity) {
  Arena arena(128);
  void* a = arena.Allocate(40);
  void* b = arena.Allocate(40);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_GT(arena.MemoryUsage(), 0u);
  EXPECT_GE(arena.AllocatedBytes(), 128u);

  const size_t cap = arena.AllocatedBytes();
  arena.Reset();
  EXPECT_EQ(arena.MemoryUsage(), 0u);
  EXPECT_EQ(arena.AllocatedBytes(), cap);

  void* c = arena.Allocate(40);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c, a);  // first block reused from the bump start
  EXPECT_EQ(arena.AllocatedBytes(), cap);
}

TEST(Arena, ManyAllocationsStayBoundedAfterReset) {
  Arena arena(256);
  size_t peak_capacity = 0;
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 32; ++i) {
      void* p = arena.Allocate(24);
      ASSERT_NE(p, nullptr);
      std::memset(p, static_cast<int>(i & 0xff), 24);
    }
    peak_capacity = std::max(peak_capacity, arena.AllocatedBytes());
    arena.Reset();
  }
  // Capacity should stabilize (reuse), not grow every round.
  EXPECT_LE(arena.AllocatedBytes(), peak_capacity);
  EXPECT_LE(peak_capacity, 256u * 8);
}

TEST(Slab, AllocFreeRoundTrip) {
  Arena arena(512);
  Slab slab(&arena, sizeof(uint64_t), 8);
  std::vector<void*> ptrs;
  for (int i = 0; i < 20; ++i) {
    void* p = slab.Alloc();
    ASSERT_NE(p, nullptr);
    *static_cast<uint64_t*>(p) = static_cast<uint64_t>(i);
    ptrs.push_back(p);
  }
  EXPECT_EQ(slab.in_use(), 20u);
  for (void* p : ptrs) slab.Free(p);
  EXPECT_EQ(slab.in_use(), 0u);

  void* again = slab.Alloc();
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(slab.in_use(), 1u);
  // Freed slots are reused from the free-list (LIFO).
  EXPECT_EQ(again, ptrs.back());
}

TEST(MemoryBudget, UnlimitedAndHardCap) {
  MemoryBudget unlimited(0);
  EXPECT_TRUE(unlimited.unlimited());
  EXPECT_TRUE(unlimited.WouldAccept(1ull << 40));

  MemoryBudget budget(100);
  EXPECT_FALSE(budget.unlimited());
  EXPECT_TRUE(budget.WouldAccept(100));
  budget.Add(80);
  EXPECT_TRUE(budget.WouldAccept(20));
  EXPECT_FALSE(budget.WouldAccept(21));
  budget.Sub(40);
  EXPECT_EQ(budget.used(), 40u);
  EXPECT_TRUE(budget.WouldAccept(60));
}

}  // namespace
}  // namespace aster
