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

TEST(MergeTopK, TopKLargerThanUniqueIds) {
  std::vector<std::vector<SearchHit>> lists = {{{"a", 1.0f}}, {{"b", 0.5f}}};
  auto merged = MergeTopK(lists, 100);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].id, "a");
  EXPECT_EQ(merged[1].id, "b");
}

TEST(MergeTopK, TopKZero) {
  std::vector<std::vector<SearchHit>> lists = {{{"a", 1.0f}}};
  EXPECT_TRUE(MergeTopK(lists, 0).empty());
}

TEST(MergeTopK, KeepsLowerScoreWhenItIsTheOnlyInstance) {
  std::vector<std::vector<SearchHit>> lists = {
      {{"a", 10.0f}, {"b", -1.0f}},
  };
  auto merged = MergeTopK(lists, 2);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].id, "a");
  EXPECT_EQ(merged[1].id, "b");
}

TEST(MergeTopK, ManyLists) {
  std::vector<std::vector<SearchHit>> lists;
  for (int i = 0; i < 20; ++i) {
    lists.push_back({{"k" + std::to_string(i), static_cast<float>(i)}});
  }
  auto merged = MergeTopK(lists, 5);
  ASSERT_EQ(merged.size(), 5u);
  EXPECT_EQ(merged[0].id, "k19");
  EXPECT_EQ(merged[4].id, "k15");
}

}  // namespace
}  // namespace aster
