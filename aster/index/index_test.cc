#include <gtest/gtest.h>

#include <vector>

#include "aster/index/distance.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

TEST(Distance, Kernels) {
  const std::vector<float> a = {1.0f, 0.0f, 0.0f};
  const std::vector<float> b = {0.0f, 1.0f, 0.0f};
  EXPECT_FLOAT_EQ(L2Squared(a, a), 0.0f);
  EXPECT_FLOAT_EQ(L2Squared(a, b), 2.0f);
  EXPECT_FLOAT_EQ(Dot(a, b), 0.0f);
  EXPECT_FLOAT_EQ(Dot(a, a), 1.0f);
  EXPECT_FLOAT_EQ(CosineSimilarity(a, a), 1.0f);
  EXPECT_FLOAT_EQ(CosineSimilarity(a, b), 0.0f);
}

TEST(Distance, ScoreIsHigherIsBetterForAllMetrics) {
  const std::vector<float> q = {1.0f, 0.0f};
  const std::vector<float> near = {0.9f, 0.1f};
  const std::vector<float> far = {-1.0f, 0.0f};
  for (Metric m : {Metric::kL2, Metric::kDot, Metric::kCosine}) {
    EXPECT_GT(Score(m, q, near), Score(m, q, far));
  }
}

TEST(ExactIndex, ReturnsNearestNeighborsInScoreOrder) {
  std::vector<std::vector<float>> data = {
      {1.0f, 0.0f}, {0.0f, 1.0f}, {0.9f, 0.1f}, {-1.0f, 0.0f}};
  std::vector<IndexEntry> entries;
  for (size_t i = 0; i < data.size(); ++i) {
    entries.push_back({"row-" + std::to_string(i), data[i]});
  }
  auto index = BuildExactIndex(Metric::kL2, std::move(entries));
  ASSERT_EQ(index->size(), 4u);

  const std::vector<float> query = {1.0f, 0.0f};
  auto hits = index->Search(query, 2, /*ef_search=*/0);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "row-0");
  EXPECT_EQ(hits[1].id, "row-2");
  EXPECT_GE(hits[0].score, hits[1].score);
}

TEST(ExactIndex, TopKLargerThanIndex) {
  const std::vector<float> v = {1.0f};
  std::vector<IndexEntry> entries = {{"only", v}};
  auto index = BuildExactIndex(Metric::kCosine, std::move(entries));
  auto hits = index->Search(v, 10, 0);
  EXPECT_EQ(hits.size(), 1u);
}

#if ASTER_ENABLE_HNSW
TEST(HnswParams, DefaultsPresentWhenEnabled) {
  HnswParams p;
  EXPECT_EQ(p.m, 16u);
  EXPECT_EQ(p.ef_construction, 128u);
}
#else
TEST(HnswParams, TypeOmittedUnderTiny) {
  // HnswParams is not declared when ASTER_ENABLE_HNSW == 0.
  static_assert(ASTER_ENABLE_HNSW == 0);
  SUCCEED();
}
#endif

}  // namespace
}  // namespace aster
