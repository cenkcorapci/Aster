#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/index/hnsw_graph.h"

namespace aster {
namespace {

std::vector<float> MakeVector(uint32_t dim, uint32_t i) {
  // Deterministic small vectors with mild structure (not pure random).
  std::vector<float> v(dim);
  for (uint32_t d = 0; d < dim; ++d) {
    const uint32_t x = (i * 1103515245u + d * 12345u) % 997u;
    v[d] = static_cast<float>(x) / 500.0f - 1.0f;
  }
  return v;
}

}  // namespace

#if ASTER_ENABLE_HNSW

TEST(AccuracyProfile, HnswParamsMappingExact) {
  const HnswParams cost = HnswParamsFromAccuracyProfile(
      AccuracyProfile::kCostOptimized);
  EXPECT_EQ(cost.m, 8u);
  EXPECT_EQ(cost.ef_construction, 50u);
  EXPECT_EQ(cost.ef_search_default, 32u);
  EXPECT_EQ(cost.max_layers, 16u);

  const HnswParams balanced = HnswParamsFromAccuracyProfile(
      AccuracyProfile::kBalanced);
  EXPECT_EQ(balanced.m, 16u);
  EXPECT_EQ(balanced.ef_construction, 128u);
  EXPECT_EQ(balanced.ef_search_default, 64u);
  EXPECT_EQ(balanced.max_layers, 16u);

  const HnswParams high = HnswParamsFromAccuracyProfile(
      AccuracyProfile::kHighRecall);
  EXPECT_EQ(high.m, 32u);
  EXPECT_EQ(high.ef_construction, 250u);
  EXPECT_EQ(high.ef_search_default, 256u);
  EXPECT_EQ(high.max_layers, 16u);

  const HnswParams max = HnswParamsFromAccuracyProfile(
      AccuracyProfile::kMaxRecall);
  EXPECT_EQ(max.m, 48u);
  EXPECT_EQ(max.ef_construction, 500u);
  EXPECT_EQ(max.ef_search_default, 768u);
  EXPECT_EQ(max.max_layers, 16u);
}

TEST(AccuracyProfile, DbPersistsHnswParamsAndUsesEfSearchDefault) {
  const std::string dir =
      ::testing::TempDir() + "/aster_acc_profile_persist";

  const AccuracyProfile profile = AccuracyProfile::kMaxRecall;
  const HnswParams expected = HnswParamsFromAccuracyProfile(profile);

  Db::Options opt;
  opt.dimension = /*dimension=*/4;
  opt.metric = Metric::kL2;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  opt.memtable_flush_bytes = 1ull << 20;
  opt.memtable_flush_ms = 0;
  opt.background_index_build = false;  // deterministic: call BuildPendingIndexes()
  opt.compaction_tier_threshold = 0;
  opt.max_segments_before_compact = 0;
  opt.accuracy_profile = profile;

  auto opened = Db::Open(opt);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& db = *opened.value();

  const uint32_t n = 64;
  for (uint32_t i = 0; i < n; ++i) {
    aster::Row row;
    row.id = "b" + std::to_string(i);
    row.vector = MakeVector(opt.dimension, i);
    row.timestamp = i + 1;
    row.version = 1;
    row.tombstone = false;
    ASSERT_TRUE(db.Upsert(std::move(row)).ok());
  }

  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.BuildPendingIndexes().ok());

  // READY HNSW file must exist and include the profile-selected params.
  const std::string hnsw_path = dir + "/index/seg_000001.hnsw";
  auto loaded = HnswGraph::LoadFromFile(hnsw_path);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  const auto& actual = loaded.value().params();
  EXPECT_EQ(actual.m, expected.m);
  EXPECT_EQ(actual.ef_construction, expected.ef_construction);
  EXPECT_EQ(actual.ef_search_default, expected.ef_search_default);
  EXPECT_EQ(actual.max_layers, expected.max_layers);

  // Search default should match explicit `expected.ef_search_default`.
  SearchRequest req;
  req.vector = MakeVector(opt.dimension, /*i=*/7);
  req.top_k = 10;

  req.ef_search = 0;  // use graph default
  auto hits_default = db.Search(req);

  req.ef_search = expected.ef_search_default;
  auto hits_explicit = db.Search(req);

  ASSERT_EQ(hits_default.size(), hits_explicit.size());
  for (size_t i = 0; i < hits_default.size(); ++i) {
    EXPECT_EQ(hits_default[i].id, hits_explicit[i].id);
  }
}

#endif  // ASTER_ENABLE_HNSW

}  // namespace aster

