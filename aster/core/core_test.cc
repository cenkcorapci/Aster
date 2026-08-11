#include <gtest/gtest.h>

#include <set>
#include <string>

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

TEST(Hash64, EmptyAndBinaryLikeKeys) {
  EXPECT_EQ(Hash64(""), Hash64(""));
  EXPECT_NE(Hash64(""), Hash64("\0", 1));
  EXPECT_NE(Hash64(std::string_view("a\0b", 3)), Hash64("ab"));
}

TEST(Hash64, ReasonableDispersion) {
  std::set<uint64_t> seen;
  for (int i = 0; i < 1000; ++i) {
    seen.insert(Hash64("key-" + std::to_string(i)));
  }
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(Status, FactoriesAndOk) {
  EXPECT_TRUE(Status::Ok().ok());
  EXPECT_EQ(Status::Ok().code(), StatusCode::kOk);
  EXPECT_TRUE(Status::Ok().message().empty());

  EXPECT_FALSE(Status::NotFound("x").ok());
  EXPECT_EQ(Status::NotFound("x").code(), StatusCode::kNotFound);
  EXPECT_EQ(Status::NotFound("x").message(), "x");

  EXPECT_EQ(Status::InvalidArgument("bad").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Status::IoError("io").code(), StatusCode::kIoError);
  EXPECT_EQ(Status::Corruption("crc").code(), StatusCode::kCorruption);
  EXPECT_EQ(Status::Unavailable("down").code(), StatusCode::kUnavailable);
  EXPECT_EQ(Status::ResourceExhausted("full").code(),
            StatusCode::kResourceExhausted);
  EXPECT_EQ(Status::Internal("bug").code(), StatusCode::kInternal);

  Status custom(StatusCode::kUnavailable, "down");
  EXPECT_EQ(custom.code(), StatusCode::kUnavailable);
  EXPECT_EQ(custom.message(), "down");

  Status exhausted(StatusCode::kResourceExhausted, "full");
  EXPECT_EQ(exhausted.code(), StatusCode::kResourceExhausted);
  Status internal(StatusCode::kInternal, "bug");
  EXPECT_EQ(internal.code(), StatusCode::kInternal);
}

TEST(Result, HoldsValueOrStatus) {
  Result<int> good(42);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);
  good.value() = 7;
  EXPECT_EQ(good.value(), 7);

  Result<int> bad(Status::NotFound("missing"));
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), StatusCode::kNotFound);
  EXPECT_EQ(bad.status().message(), "missing");
}

TEST(Result, StringPayload) {
  Result<std::string> ok(std::string("hello"));
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok.value(), "hello");
  Result<std::string> err(Status::IoError("fail"));
  ASSERT_FALSE(err.ok());
}

TEST(Row, LastWriteWinsOrdering) {
  Row a{.id = "x", .timestamp = 10, .version = 1};
  Row b{.id = "x", .timestamp = 10, .version = 2};
  Row c{.id = "x", .timestamp = 11, .version = 0};
  EXPECT_TRUE(NewerThan(b, a));
  EXPECT_TRUE(NewerThan(c, b));
  EXPECT_FALSE(NewerThan(a, c));
  EXPECT_FALSE(NewerThan(a, a));  // equal version not newer
}

TEST(SearchRequest, Defaults) {
  SearchRequest req;
  EXPECT_EQ(req.top_k, 10u);
  // 0 means "use collection/index default" (see SearchRequest in types.h).
  EXPECT_EQ(req.ef_search, 0u);
  EXPECT_TRUE(req.tags.empty());
}

}  // namespace
}  // namespace aster
