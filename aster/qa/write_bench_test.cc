// Smoke test for the M1-T14 write microbench path (flush + compaction).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "aster/db/db.h"

namespace aster {
namespace {

TEST(WriteBench, SmokeFlushAndCompact) {
  const std::string dir = ::testing::TempDir() + "/aster_write_bench_smoke";
  Db::Options opt;
  opt.dimension = 8;
  opt.metric = Metric::kL2;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  opt.memtable_flush_bytes = 8 << 10;
  opt.compaction_tier_threshold = 3;
  opt.max_segments_before_compact = 5;

  auto opened = Db::Open(opt);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& db = *opened.value();

  constexpr int kRows = 2000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kRows; ++i) {
    Row row;
    row.id = "r" + std::to_string(i);
    row.vector.assign(8, 0.0f);
    row.vector[0] = static_cast<float>(i) * 0.001f;
    row.timestamp = static_cast<Timestamp>(i + 1);
    ASSERT_TRUE(db.Upsert(std::move(row)).ok());
    if ((i + 1) % 250 == 0) {
      ASSERT_TRUE(db.Flush().ok());
    }
  }
  ASSERT_TRUE(db.Flush().ok());
  const auto sec = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  const double ups = sec > 0.0 ? static_cast<double>(kRows) / sec : 0.0;
  std::printf("write_bench_smoke rows=%d upserts_per_sec=%.1f segments=%zu\n",
              kRows, ups, db.segment_count());
  EXPECT_GT(ups, 0.0);
  EXPECT_TRUE(db.Get("r0").has_value());
  EXPECT_TRUE(db.Get("r1999").has_value());
}

}  // namespace
}  // namespace aster
