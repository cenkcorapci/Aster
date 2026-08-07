#include <gtest/gtest.h>

#include <set>

#include "aster/core/hash.h"
#include "aster/core/status.h"
#include "aster/core/types.h"

namespace aster {
namespace {

TEST(Hash64, DeterministicAndSeeded) {
  EXPECT_EQ(Hash64("aster"), Hash64("aster"));
  EXPECT_NE(Hash64("aster"), Hash64("astera"));
  EXPECT_NE(Hash64("aster", 1), Hash64("aster", 2));
}

TEST(Hash64, ReasonableDispersion) {
  std::set<uint64_t> seen;
  for (int i = 0; i < 1000; ++i) {
    seen.insert(Hash64("key-" + std::to_string(i)));
  }
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(Result, HoldsValueOrStatus) {
  Result<int> good(42);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);

  Result<int> bad(Status::NotFound("missing"));
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), StatusCode::kNotFound);
}

TEST(Row, LastWriteWinsOrdering) {
  Row a{.id = "x", .timestamp = 10, .version = 1};
  Row b{.id = "x", .timestamp = 10, .version = 2};
  Row c{.id = "x", .timestamp = 11, .version = 0};
  EXPECT_TRUE(NewerThan(b, a));
  EXPECT_TRUE(NewerThan(c, b));
  EXPECT_FALSE(NewerThan(a, c));
}

}  // namespace
}  // namespace aster
