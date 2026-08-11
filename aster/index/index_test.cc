#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "aster/index/bloom.h"
#include "aster/index/distance.h"
#include "aster/index/vector_index.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_build.h"
#endif

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
  auto index = BuildExactIndex(Metric::kDot, std::vector<IndexEntry>{});
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
  EXPECT_EQ(HnswLayer0MaxDegree(p), 32u);
}

HnswGraph MakeFixtureGraph() {
  HnswParams params;
  params.m = 4;
  params.ef_construction = 32;
  params.ef_search_default = 16;
  params.max_layers = 8;

  HnswGraph g(params);
  g.set_segment_id(42);

  // Tiny hand-built hierarchy (no insert heuristic — M2-T02):
  //   layer 2: 0
  //   layer 1: 0 — 2
  //   layer 0: 0 — 1 — 2 — 3
  EXPECT_TRUE(g.AddNode(/*row_ordinal=*/10, /*level=*/2).ok());
  EXPECT_TRUE(g.AddNode(/*row_ordinal=*/11, /*level=*/0).ok());
  EXPECT_TRUE(g.AddNode(/*row_ordinal=*/12, /*level=*/1).ok());
  EXPECT_TRUE(g.AddNode(/*row_ordinal=*/13, /*level=*/0).ok());

  EXPECT_EQ(g.entry_point(), 0u);
  EXPECT_EQ(g.max_level(), 2u);
  EXPECT_TRUE(g.SetNeighbors(0, 0, {1, 2}).ok());
  EXPECT_TRUE(g.SetNeighbors(1, 0, {0, 2}).ok());
  EXPECT_TRUE(g.SetNeighbors(2, 0, {0, 1, 3}).ok());
  EXPECT_TRUE(g.SetNeighbors(3, 0, {2}).ok());
  EXPECT_TRUE(g.SetNeighbors(0, 1, {2}).ok());
  EXPECT_TRUE(g.SetNeighbors(2, 1, {0}).ok());
  EXPECT_TRUE(g.SetNeighbors(0, 2, {}).ok());
  return g;
}

TEST(HnswGraph, SerializeLoadRoundTripBytes) {
  const HnswGraph g = MakeFixtureGraph();
  const std::string bytes = g.Serialize();
  ASSERT_GE(bytes.size(),
            static_cast<size_t>(HnswGraph::kHeaderBytes + HnswGraph::kFooterBytes));

  auto loaded = HnswGraph::Load(bytes);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  EXPECT_EQ(loaded.value(), g);
  EXPECT_EQ(loaded.value().segment_id(), 42u);
  EXPECT_EQ(loaded.value().RowOrdinal(2), 12u);
  EXPECT_EQ(loaded.value().Neighbors(2, 0), (std::vector<uint32_t>{0, 1, 3}));
}

TEST(HnswGraph, SerializeLoadRoundTripFile) {
  const HnswGraph g = MakeFixtureGraph();
  const std::string path = "aster_hnsw_roundtrip_test.hnsw";
  ASSERT_TRUE(g.WriteToFile(path).ok());

  auto loaded = HnswGraph::LoadFromFile(path);
  std::remove(path.c_str());
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  EXPECT_EQ(loaded.value(), g);
}

TEST(HnswGraph, EmptyGraphRoundTrip) {
  HnswGraph g(HnswParams{});
  const std::string bytes = g.Serialize();
  auto loaded = HnswGraph::Load(bytes);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  EXPECT_EQ(loaded.value(), g);
  EXPECT_EQ(loaded.value().entry_point(), HnswGraph::kNoEntry);
  EXPECT_EQ(loaded.value().node_count(), 0u);
}

TEST(HnswGraph, CorruptMagicRejected) {
  HnswGraph g = MakeFixtureGraph();
  std::string bytes = g.Serialize();
  bytes[0] = static_cast<char>(bytes[0] ^ 0xFF);
  auto loaded = HnswGraph::Load(bytes);
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status().code(), StatusCode::kCorruption);
}

TEST(HnswGraph, TruncatedRejected) {
  HnswGraph g = MakeFixtureGraph();
  std::string bytes = g.Serialize();
  bytes.resize(bytes.size() / 2);
  auto loaded = HnswGraph::Load(bytes);
  EXPECT_FALSE(loaded.ok());
}

TEST(HnswBuild, BuildsFixtureWithinDegreeBounds) {
  HnswParams params;
  params.m = 4;
  params.ef_construction = 16;
  params.ef_search_default = 8;
  params.max_layers = 6;

  // Small 2-d grid; enough points that degrees can approach M / M0.
  std::vector<std::vector<float>> vectors;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      vectors.push_back({static_cast<float>(x), static_cast<float>(y)});
    }
  }

  HnswBuilder builder(Metric::kL2, params, /*rng_seed=*/42);
  auto built = builder.Build(vectors);
  ASSERT_TRUE(built.ok()) << built.status().message();
  const HnswGraph& g = built.value();

  EXPECT_EQ(g.node_count(), 16u);
  EXPECT_NE(g.entry_point(), HnswGraph::kNoEntry);
  EXPECT_LT(g.max_level(), params.max_layers);

  for (uint32_t n = 0; n < g.node_count(); ++n) {
    EXPECT_EQ(g.RowOrdinal(n), n);
    EXPECT_LT(g.NodeLevel(n), params.max_layers);
    for (uint16_t layer = 0; layer <= g.NodeLevel(n); ++layer) {
      const auto& nbrs = g.Neighbors(n, layer);
      const uint32_t limit =
          layer == 0 ? HnswLayer0MaxDegree(params) : params.m;
      EXPECT_LE(nbrs.size(), limit) << "node=" << n << " layer=" << layer;
      for (uint32_t nb : nbrs) {
        EXPECT_NE(nb, n);
        EXPECT_LE(layer, g.NodeLevel(nb));
      }
    }
  }
}

TEST(HnswBuild, EmptyAndSingleNode) {
  HnswParams params;
  params.m = 8;
  params.ef_construction = 16;
  params.max_layers = 4;

  HnswBuilder builder(Metric::kL2, params, /*rng_seed=*/1);
  auto empty = builder.Build({});
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(empty.value().node_count(), 0u);

  auto one = builder.Build({{1.0f, 0.0f}});
  ASSERT_TRUE(one.ok()) << one.status().message();
  EXPECT_EQ(one.value().node_count(), 1u);
  EXPECT_EQ(one.value().entry_point(), 0u);
  EXPECT_TRUE(one.value().Neighbors(0, 0).empty());
}

TEST(HnswBuild, SelectNeighborsHeuristicPrefersDiversity) {
  // base (0,0); A and B clustered on +x; C on +y.
  // Top-2 by distance alone would keep A,B; heuristic keeps A,C.
  const std::vector<float> base = {0.0f, 0.0f};
  const std::vector<float> a = {1.0f, 0.0f};
  const std::vector<float> b = {1.1f, 0.0f};
  const std::vector<float> c = {0.0f, 1.0f};

  std::vector<std::pair<uint32_t, VectorView>> cands = {
      {10, a}, {11, b}, {12, c}};

  auto picked = HnswBuilder::SelectNeighborsHeuristic(Metric::kL2, base, cands,
                                                      /*max_keep=*/2);
  ASSERT_EQ(picked.size(), 2u);
  EXPECT_EQ(picked[0], 10u);  // nearest overall
  EXPECT_EQ(picked[1], 12u);  // diverse axis, not clustered B

  // Plain closest-2 would be A then B — confirm B loses to the heuristic.
  EXPECT_TRUE(std::find(picked.begin(), picked.end(), 11u) == picked.end());
}

TEST(HnswBuild, HeuristicRespectsMaxKeep) {
  const std::vector<float> base = {0.0f, 0.0f};
  std::vector<std::pair<uint32_t, VectorView>> cands;
  const std::vector<std::vector<float>> pts = {
      {1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
  for (uint32_t i = 0; i < pts.size(); ++i) {
    cands.emplace_back(i, VectorView{pts[i]});
  }
  auto picked =
      HnswBuilder::SelectNeighborsHeuristic(Metric::kDot, base, cands, 3);
  EXPECT_LE(picked.size(), 3u);
  EXPECT_FALSE(picked.empty());
}
#else
TEST(HnswParams, TypeOmittedUnderTiny) {
  static_assert(ASTER_ENABLE_HNSW == 0);
  SUCCEED();
}
#endif

}  // namespace
}  // namespace aster
