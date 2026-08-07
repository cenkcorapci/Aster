#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "aster/db/db.h"

namespace aster {
namespace {

// Lightweight correctness + smoke throughput checks at embedding sizes used
// in production (256 / 2048 / 4096). Not a full soak — that is deploy/bench.
class BenchDimTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(BenchDimTest, WriteSearchFlush) {
  const uint32_t dim = GetParam();
  const std::string dir =
      ::testing::TempDir() + "/aster_bench_dim_" + std::to_string(dim);
  ::remove((dir + "/MANIFEST").c_str());

  Db::Options opt;
  opt.dimension = dim;
  opt.metric = Metric::kCosine;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  opt.memtable_flush_bytes = 4 << 20;
  auto opened = Db::Open(opt);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& db = *opened.value();

  std::mt19937 rng(dim * 17u + 3u);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  constexpr int kRows = 64;

  auto make_vec = [&](std::vector<float>& v) {
    v.resize(dim);
    double n2 = 0.0;
    for (float& x : v) {
      x = dist(rng);
      n2 += static_cast<double>(x) * x;
    }
    const float inv = n2 > 0 ? static_cast<float>(1.0 / std::sqrt(n2)) : 1.f;
    for (float& x : v) x *= inv;
  };

  std::vector<float> query;
  make_vec(query);

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kRows; ++i) {
    Row row;
    row.id = "v-" + std::to_string(i);
    make_vec(row.vector);
    row.timestamp = static_cast<Timestamp>(i + 1);
    ASSERT_TRUE(db.Upsert(std::move(row)).ok());
  }
  // Plant an exact match for the query.
  {
    Row row;
    row.id = "target";
    row.vector = query;
    row.timestamp = 1000;
    ASSERT_TRUE(db.Upsert(std::move(row)).ok());
  }
  ASSERT_TRUE(db.Flush().ok());

  SearchRequest req;
  req.vector = query;
  req.top_k = 5;
  auto hits = db.Search(req);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits[0].id, "target");

  const auto ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  std::printf("bench_dim dim=%u rows=%d elapsed_ms=%.2f segments=%zu\n", dim,
              kRows + 1, ms, db.segment_count());
}

INSTANTIATE_TEST_SUITE_P(EmbeddingSizes, BenchDimTest,
                         ::testing::Values(256u, 2048u, 4096u));

}  // namespace
}  // namespace aster
