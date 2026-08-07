#include <gtest/gtest.h>

#include <vector>

#include "aster/index/bloom.h"
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

TEST(Distance, ZeroVectorCosineIsZero) {
  const std::vector<float> z = {0.0f, 0.0f};
  const std::vector<float> a = {1.0f, 0.0f};
  EXPECT_FLOAT_EQ(CosineSimilarity(z, a), 0.0f);
  EXPECT_FLOAT_EQ(CosineSimilarity(z, z), 0.0f);
}

TEST(Distance, ScoreIsHigherIsBetterForAllMetrics) {
  const std::vector<float> q = {1.0f, 0.0f};
  const std::vector<float> near = {0.9f, 0.1f};
  const std::vector<float> far = {-1.0f, 0.0f};
  for (Metric m : {Metric::kL2, Metric::kDot, Metric::kCosine}) {
    EXPECT_GT(Score(m, q, near), Score(m, q, far));
  }
}

TEST(Distance, ScoreMatchesMetricDefinition) {
  const std::vector<float> a = {1.0f, 2.0f};
  const std::vector<float> b = {3.0f, 4.0f};
  EXPECT_FLOAT_EQ(Score(Metric::kL2, a, b), -L2Squared(a, b));
  EXPECT_FLOAT_EQ(Score(Metric::kDot, a, b), Dot(a, b));
  EXPECT_FLOAT_EQ(Score(Metric::kCosine, a, b), CosineSimilarity(a, b));
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

TEST(ExactIndex, EmptyIndex) {
  auto index = BuildExactIndex(Metric::kDot, {});
  EXPECT_EQ(index->size(), 0u);
  const std::vector<float> q = {1.0f};
  EXPECT_TRUE(index->Search(q, 5, 0).empty());
}

TEST(ExactIndex, DotProductOrdering) {
  const std::vector<float> a = {1.0f, 0.0f};
  const std::vector<float> b = {0.5f, 0.0f};
  const std::vector<float> c = {-1.0f, 0.0f};
  std::vector<IndexEntry> entries = {{"a", a}, {"b", b}, {"c", c}};
  auto index = BuildExactIndex(Metric::kDot, std::move(entries));
  const std::vector<float> q = {1.0f, 0.0f};
  auto hits = index->Search(q, 3, 0);
  ASSERT_EQ(hits.size(), 3u);
  EXPECT_EQ(hits[0].id, "a");
  EXPECT_EQ(hits[1].id, "b");
  EXPECT_EQ(hits[2].id, "c");
}

TEST(Bloom, NegativesAreExcluded) {
  BloomFilter bloom = BloomFilter::Build({"a", "b", "c"});
  EXPECT_TRUE(bloom.MayContain("a"));
  EXPECT_TRUE(bloom.MayContain("b"));
  EXPECT_FALSE(bloom.MayContain("definitely-missing-key-xyz"));
}

TEST(Bloom, EmptyKeySetStillConstructs) {
  BloomFilter bloom = BloomFilter::Build({});
  EXPECT_GT(bloom.num_bits(), 0u);
  EXPECT_GT(bloom.num_hashes(), 0u);
}

TEST(Bloom, RoundTripFromBits) {
  BloomFilter built = BloomFilter::Build({"alpha", "beta"});
  auto copy = BloomFilter::FromBits(built.num_bits(), built.num_hashes(),
                                    built.seed(), built.bits());
  EXPECT_TRUE(copy.MayContain("alpha"));
  EXPECT_TRUE(copy.MayContain("beta"));
  EXPECT_FALSE(copy.MayContain("gamma-not-present-zzzz"));
}

TEST(Bloom, ZeroBitsAlwaysMaybeContains) {
  BloomFilter empty(0, 0, 0);
  EXPECT_TRUE(empty.MayContain("anything"));
  empty.Add("x");  // no-op
  EXPECT_TRUE(empty.MayContain("x"));
}

#if ASTER_ENABLE_HNSW
TEST(HnswParams, DefaultsPresentWhenEnabled) {
  HnswParams p;
  EXPECT_EQ(p.m, 16u);
  EXPECT_EQ(p.ef_construction, 128u);
  EXPECT_EQ(p.ef_search_default, 64u);
  EXPECT_EQ(p.max_layers, 16u);
}
#else
TEST(HnswParams, TypeOmittedUnderTiny) {
  static_assert(ASTER_ENABLE_HNSW == 0);
  SUCCEED();
}
#endif

}  // namespace
}  // namespace aster
