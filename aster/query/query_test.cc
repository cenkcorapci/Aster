#include <gtest/gtest.h>

#include "aster/query/topk.h"

namespace aster {
namespace {

TEST(MergeTopK, DedupesAndKeepsBestScore) {
  std::vector<std::vector<SearchHit>> lists = {
      {{"a", 1.0f}, {"b", 0.5f}},
      {{"a", 2.0f}, {"c", 0.9f}},
  };
  auto merged = MergeTopK(lists, 2);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].id, "a");
  EXPECT_FLOAT_EQ(merged[0].score, 2.0f);
  EXPECT_EQ(merged[1].id, "c");
}

TEST(MergeTopK, EmptyInputs) {
  EXPECT_TRUE(MergeTopK({}, 10).empty());
  EXPECT_TRUE(MergeTopK({{}}, 10).empty());
}

}  // namespace
}  // namespace aster
