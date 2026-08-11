#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aster/db/db.h"
#include "aster/core/features.h"
#include "aster/index/tags.h"
#include "aster/storage/segment.h"

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts,
            std::set<std::string> tags = {}) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  row.tags = std::move(tags);
  return row;
}

Db::Options SmallDb() {
  Db::Options options;
  options.dimension = 2;
  options.metric = Metric::kL2;
  return options;
}

TEST(Db, SearchSpansMemtableAndSegments) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("seg-row", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Upsert(MakeRow("mem-row", {0.9f, 0.0f}, 20)).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 2;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "seg-row");
  EXPECT_EQ(hits[1].id, "mem-row");
}

TEST(Db, DeleteHidesRowEvenIfIndexedInSegment) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Delete("a", 20).ok());

  EXPECT_FALSE(db.Get("a").has_value());
  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  auto hits = db.Search(req);
  EXPECT_TRUE(hits.empty());
}

TEST(Db, UpdateInMemtableShadowsSegmentVersion) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {-1.0f, 0.0f}, 20)).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_FLOAT_EQ(hits[0].score, -4.0f);
}

TEST(Db, TagFiltering) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("x", {1.0f, 0.0f}, 10, {"red", "sale"})).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("y", {1.0f, 0.1f}, 10, {"blue"})).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.tags = {"red"};
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "x");
}

TEST(Db, CompactionPurgesTombstonesAndKeepsLatest) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Delete("b", 20).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {0.5f, 0.5f}, 30)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_EQ(db.segment_count(), 2u);

  ASSERT_TRUE(db.Compact().ok());
  ASSERT_EQ(db.segment_count(), 1u);
  EXPECT_FALSE(db.Get("b").has_value());
  ASSERT_TRUE(db.Get("a").has_value());
  EXPECT_FLOAT_EQ(db.Get("a")->vector[0], 0.5f);

  SearchRequest req;
  req.vector = {0.5f, 0.5f};
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "a");
}

TEST(Db, DimensionValidation) {
  Db db(SmallDb());
  EXPECT_FALSE(db.Upsert(MakeRow("bad", {1.0f}, 10)).ok());
}

TEST(Db, DurableOpenRecoversFlushedAndWalRows) {
  const std::string dir = ::testing::TempDir() + "/aster_db_durable";
  {
    Db::Options options = SmallDb();
    options.data_dir = dir;
    options.wal_sync = SyncPolicy::kNever;
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok()) << db.status().message();
    ASSERT_TRUE(db.value()
                    ->Upsert(MakeRow("flushed", {1.0f, 0.0f}, 10))
                    .ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    ASSERT_TRUE(
        db.value()->Upsert(MakeRow("wal-only", {0.0f, 1.0f}, 20)).ok());
  }

  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok()) << db.status().message();
  EXPECT_EQ(db.value()->segment_count(), 1u);
  ASSERT_TRUE(db.value()->Get("flushed").has_value());
  ASSERT_TRUE(db.value()->Get("wal-only").has_value());
  EXPECT_FLOAT_EQ(db.value()->Get("wal-only")->vector[1], 1.0f);
}

TEST(Db, OpenRequiresDataDir) {
  Db::Options options = SmallDb();
  auto db = Db::Open(options);
  ASSERT_FALSE(db.ok());
  EXPECT_EQ(db.status().code(), StatusCode::kInvalidArgument);
}

TEST(Db, AutoCompactOnMaxSegments) {
  Db::Options options = SmallDb();
  options.compaction_tier_threshold = 0;  // force the hard-cap path
  options.max_segments_before_compact = 2;
  Db db(options);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_EQ(db.segment_count(), 1u);
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());
  ASSERT_TRUE(db.Flush().ok());
  // Second flush trips auto-compact into a single segment.
  EXPECT_EQ(db.segment_count(), 1u);
  EXPECT_TRUE(db.Get("a").has_value());
  EXPECT_TRUE(db.Get("b").has_value());
}

TEST(Db, FlushEmptyAndCompactSingleAreNoops) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Compact().ok());
  EXPECT_EQ(db.segment_count(), 0u);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Compact().ok());  // single segment: no-op
  EXPECT_EQ(db.segment_count(), 1u);
}

TEST(Db, AutoFlushOnMemtableSize) {
  Db::Options options = SmallDb();
  options.memtable_flush_bytes = 64;  // tiny threshold
  Db db(options);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  // Background flush thread should seal without an explicit Flush().
  for (int i = 0; i < 200 && db.segment_count() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(db.segment_count(), 1u);
  EXPECT_EQ(db.memtable_rows(), 0u);
  EXPECT_TRUE(db.Get("a").has_value());
}

TEST(Db, AutoFlushOnMemtableAge) {
  Db::Options options = SmallDb();
  options.memtable_flush_bytes = 1u << 30;  // size trigger off
  options.memtable_flush_ms = 50;
  Db db(options);
  ASSERT_TRUE(db.Upsert(MakeRow("aged", {0.0f, 1.0f}, 1)).ok());
  EXPECT_EQ(db.segment_count(), 0u);
  for (int i = 0; i < 200 && db.segment_count() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(db.segment_count(), 1u);
  EXPECT_EQ(db.memtable_rows(), 0u);
  EXPECT_TRUE(db.Get("aged").has_value());
}

TEST(Db, DurableAutoFlushUnderWriteLoad) {
  const std::string dir = ::testing::TempDir() + "/aster_db_autoflush";
  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  options.memtable_flush_bytes = 64;
  options.compaction_tier_threshold = 0;
  options.max_segments_before_compact = 0;  // keep segments visible
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(
        db.value()
            ->Upsert(MakeRow("r" + std::to_string(i), {1.0f, 0.0f}, i + 1))
            .ok());
  }
  for (int i = 0; i < 200 && db.value()->memtable_rows() != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(db.value()->memtable_rows(), 0u);
  EXPECT_GE(db.value()->segment_count(), 1u);
  EXPECT_TRUE(db.value()->Get("r0").has_value());
  EXPECT_TRUE(db.value()->Get("r7").has_value());
}

TEST(Db, MissingGetAndEmptySearch) {
  Db db(SmallDb());
  EXPECT_FALSE(db.Get("missing").has_value());
  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  EXPECT_TRUE(db.Search(req).empty());
}

TEST(Db, DurableDeleteSurvivesReopen) {
  const std::string dir = ::testing::TempDir() + "/aster_db_del";
  {
    Db::Options options = SmallDb();
    options.data_dir = dir;
    options.wal_sync = SyncPolicy::kNever;
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok());
    ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    ASSERT_TRUE(db.value()->Delete("a", 20).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
  }
  Db::Options options = SmallDb();
  options.data_dir = dir;
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  EXPECT_FALSE(db.value()->Get("a").has_value());
}

TEST(Db, DurableCompactRewritesManifest) {
  const std::string dir = ::testing::TempDir() + "/aster_db_compact";
  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.value()->Flush().ok());
  ASSERT_TRUE(db.value()->Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());
  ASSERT_TRUE(db.value()->Flush().ok());
  ASSERT_EQ(db.value()->segment_count(), 2u);
  ASSERT_TRUE(db.value()->Compact().ok());
  EXPECT_EQ(db.value()->segment_count(), 1u);

  // Reopen and confirm both rows still visible from compacted SSTable.
  db.value().reset();
  auto reopened = Db::Open(options);
  ASSERT_TRUE(reopened.ok());
  EXPECT_EQ(reopened.value()->segment_count(), 1u);
  EXPECT_TRUE(reopened.value()->Get("a").has_value());
  EXPECT_TRUE(reopened.value()->Get("b").has_value());
}

TEST(Db, TagFilterRequiresAllTags) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("x", {1.0f, 0.0f}, 1, {"red"})).ok());
  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.tags = {"red", "sale"};
  EXPECT_TRUE(db.Search(req).empty());
}

TEST(Db, FilteredSearchMatchesExactSemanticsOnSegment) {
  // Many near-query rows without the filter tag; one far tagged row.
  // Unfiltered top-k never sees the tagged row; bitmap-driven filtered
  // search must still return it (docs/indexing.md §7).
  Db db(SmallDb());
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(db.Upsert(MakeRow("near-" + std::to_string(i),
                                  {1.0f, static_cast<float>(i) * 0.001f},
                                  static_cast<Timestamp>(i + 1)))
                    .ok());
  }
  ASSERT_TRUE(
      db.Upsert(MakeRow("tagged", {-1.0f, 0.0f}, 100, {"rare"})).ok());
  ASSERT_TRUE(db.Flush().ok());

  SearchRequest unfiltered;
  unfiltered.vector = {1.0f, 0.0f};
  unfiltered.top_k = 5;
  auto plain = db.Search(unfiltered);
  ASSERT_FALSE(plain.empty());
  for (const auto& hit : plain) {
    EXPECT_NE(hit.id, "tagged");
  }

  SearchRequest filtered;
  filtered.vector = {1.0f, 0.0f};
  filtered.top_k = 5;
  filtered.tags = {"rare"};
  auto hits = db.Search(filtered);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "tagged");
}

TEST(Db, AdaptiveFetchKUsedForSelectiveFilter) {
  // Fixture where σ is low enough that AdaptiveFetchK exceeds BaseFetchK.
  // Pin the policy function against the segment tag index selectivity.
  std::vector<Row> rows;
  for (int i = 0; i < 100; ++i) {
    Row row;
    row.id = "r" + std::to_string(i);
    row.vector = {1.0f, 0.0f};
    row.timestamp = static_cast<Timestamp>(i + 1);
    if (i == 0) row.tags = {"needle"};
    rows.push_back(std::move(row));
  }
  auto segment = Segment::Build(1, Metric::kL2, std::move(rows));
  const double sigma = segment->tag_index().Selectivity({"needle"});
  EXPECT_DOUBLE_EQ(sigma, 0.01);
  const uint32_t k = 10;
  const uint32_t ef = 128;
  EXPECT_GT(AdaptiveFetchK(k, ef, sigma), BaseFetchK(k));
  EXPECT_EQ(AdaptiveFetchK(k, ef, sigma), ef);

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = k;
  req.ef_search = ef;
  req.tags = {"needle"};
  Db db(SmallDb());
  for (const Row& row : segment->rows()) {
    ASSERT_TRUE(db.Upsert(row).ok());
  }
  ASSERT_TRUE(db.Flush().ok());
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "r0");
}

TEST(Db, TombstoneUpsertAllowedWithoutDimension) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  // Delete path uses tombstone with empty vector.
  ASSERT_TRUE(db.Delete("a", 2).ok());
  EXPECT_FALSE(db.Get("a").has_value());
}

TEST(Db, CorruptWalRowIsBoundsChecked) {
  // Open a durable db, write one row, then corrupt the WAL to verify that
  // DecodeRow returns Corruption rather than reading out-of-bounds.
  const std::string dir = ::testing::TempDir() + "/aster_db_corrupt_wal";
  Db::Options options;
  options.dimension = 2;
  options.metric = Metric::kL2;
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  {
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok());
    ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  }
  // Truncate the WAL to create a torn record.
  const std::string wal_path = dir + "/WAL";
  {
    std::FILE* f = std::fopen(wal_path.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    ASSERT_GT(sz, 0);
    ::ftruncate(::fileno(f), sz / 2);
    std::fclose(f);
  }
  // Open should succeed (partial WAL is tolerated — torn record is silently
  // dropped, matching the same behaviour as ReplayWal's CRC stop).
  auto reopened = Db::Open(options);
  EXPECT_TRUE(reopened.ok());

  // Also verify a checksum-valid but malformed payload is rejected by DecodeRow.
  ::remove(wal_path.c_str());
  auto wal = WalWriter::Open(wal_path, SyncPolicy::kNever);
  ASSERT_TRUE(wal.ok());
  std::string corrupt(4, static_cast<char>(0xff));  // id_len=0xffffffff
  ASSERT_TRUE(wal.value().Append(corrupt).ok());
  auto bad = Db::Open(options);
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), StatusCode::kCorruption);
}

TEST(Db, CompactRemovesOrphanedSSTables) {
  const std::string dir = ::testing::TempDir() + "/aster_db_compact_cleanup";
  Db::Options options;
  options.dimension = 2;
  options.metric = Metric::kL2;
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  options.compaction_tier_threshold = 0;
  options.max_segments_before_compact = 0;  // disable auto-compact

  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.value()->Flush().ok());
  ASSERT_TRUE(db.value()->Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());
  ASSERT_TRUE(db.value()->Flush().ok());
  ASSERT_EQ(db.value()->segment_count(), 2u);

  // Both SSTable files must exist before compaction.
  EXPECT_EQ(access((dir + "/seg_000001.ast").c_str(), F_OK), 0);
  EXPECT_EQ(access((dir + "/seg_000002.ast").c_str(), F_OK), 0);

  ASSERT_TRUE(db.value()->Compact().ok());
  ASSERT_EQ(db.value()->segment_count(), 1u);

  // After compaction the old SSTable files must have been deleted.
  EXPECT_NE(access((dir + "/seg_000001.ast").c_str(), F_OK), 0);
  EXPECT_NE(access((dir + "/seg_000002.ast").c_str(), F_OK), 0);
  // The merged SSTable must be present.
  EXPECT_EQ(access((dir + "/seg_000003.ast").c_str(), F_OK), 0);
}

TEST(Db, CompactPurgesTombstonesInSingleSegment) {
  const std::string dir = ::testing::TempDir() + "/aster_db_single_tomb";
  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  options.compaction_tier_threshold = 0;
  options.max_segments_before_compact = 0;

  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.value()->Delete("a", 2).ok());
  ASSERT_TRUE(db.value()->Flush().ok());
  ASSERT_EQ(db.value()->segment_count(), 1u);
  EXPECT_EQ(db.value()->approximate_row_count(), 1u);  // tombstone retained
  EXPECT_EQ(access((dir + "/seg_000001.ast").c_str(), F_OK), 0);

  ASSERT_TRUE(db.value()->Compact().ok());
  EXPECT_EQ(db.value()->segment_count(), 0u);
  EXPECT_EQ(db.value()->approximate_row_count(), 0u);
  EXPECT_NE(access((dir + "/seg_000001.ast").c_str(), F_OK), 0);
}

TEST(Db, OpenGarbageCollectsOrphanSegmentsAndTmp) {
  const std::string dir = ::testing::TempDir() + "/aster_db_orphan_gc";
  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;

  {
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok());
    ASSERT_TRUE(db.value()->Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
  }

  // Simulate a crashed flush/compaction leaving orphans behind.
  {
    std::FILE* f = std::fopen((dir + "/seg_009999.ast").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("orphan", f);
    std::fclose(f);
  }
  {
    std::FILE* f = std::fopen((dir + "/seg_000001.ast.tmp").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("tmp", f);
    std::fclose(f);
  }
  {
    std::FILE* f = std::fopen((dir + "/MANIFEST.tmp").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("tmp", f);
    std::fclose(f);
  }

  auto reopened = Db::Open(options);
  ASSERT_TRUE(reopened.ok());
  EXPECT_TRUE(reopened.value()->Get("a").has_value());
  EXPECT_EQ(access((dir + "/seg_000001.ast").c_str(), F_OK), 0);
  EXPECT_NE(access((dir + "/seg_009999.ast").c_str(), F_OK), 0);
  EXPECT_NE(access((dir + "/seg_000001.ast.tmp").c_str(), F_OK), 0);
  EXPECT_NE(access((dir + "/MANIFEST.tmp").c_str(), F_OK), 0);
}

TEST(Db, SizeTieredMergesWhenTierThresholdHit) {
  Db::Options options = SmallDb();
  options.compaction_tier_threshold = 4;
  options.compaction_bucket_ratio = 4;
  options.max_segments_before_compact = 0;  // size-tiered only
  Db db(options);

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("r" + std::to_string(i), {1.0f, 0.0f}, i + 1)).ok());
    ASSERT_TRUE(db.Flush().ok());
  }
  EXPECT_EQ(db.segment_count(), 3u);

  ASSERT_TRUE(db.Upsert(MakeRow("r3", {0.0f, 1.0f}, 4)).ok());
  ASSERT_TRUE(db.Flush().ok());
  // Fourth same-sized flush trips a tier merge into one larger segment.
  EXPECT_EQ(db.segment_count(), 1u);
  EXPECT_TRUE(db.Get("r0").has_value());
  EXPECT_TRUE(db.Get("r3").has_value());
}

TEST(Db, SizeTieredBoundsSegmentCountUnderWriteSoak) {
  Db::Options options = SmallDb();
  options.memtable_flush_bytes = 32;  // force many small flushes
  options.compaction_tier_threshold = 4;
  options.max_segments_before_compact = 0;
  Db db(options);

  constexpr int kWrites = 200;
  size_t peak_segments = 0;
  for (int i = 0; i < kWrites; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("w" + std::to_string(i), {1.0f, 0.0f}, i + 1)).ok());
    // Drain background flush so each tiny memtable seals promptly.
    for (int j = 0; j < 50 && db.memtable_rows() != 0; ++j) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (db.memtable_rows() != 0) {
      ASSERT_TRUE(db.Flush().ok());
    }
    peak_segments = std::max(peak_segments, db.segment_count());
  }
  if (db.memtable_rows() != 0) {
    ASSERT_TRUE(db.Flush().ok());
  }
  peak_segments = std::max(peak_segments, db.segment_count());

  // Without compaction, ~200 flush-sized segments would accumulate.
  // Size-tiered keeps live fan-out O(threshold × tiers).
  EXPECT_LT(db.segment_count(), 20u);
  EXPECT_LT(peak_segments, 20u);
  EXPECT_TRUE(db.Get("w0").has_value());
  EXPECT_TRUE(db.Get("w199").has_value());
}

TEST(Db, SizeTieredPartialMergeLeavesOlderTierIntact) {
  Db::Options options = SmallDb();
  options.compaction_tier_threshold = 4;
  options.max_segments_before_compact = 0;
  Db db(options);

  // One larger segment (tier ≥1) that must survive a later L0 merge.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("big" + std::to_string(i), {1.0f, 0.0f}, i + 1))
            .ok());
  }
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_EQ(db.segment_count(), 1u);

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("s" + std::to_string(i), {0.0f, 1.0f}, 10 + i))
            .ok());
    ASSERT_TRUE(db.Flush().ok());
  }
  // Four size-1 segments merge; the size-4 segment stays.
  EXPECT_EQ(db.segment_count(), 2u);
  EXPECT_TRUE(db.Get("big0").has_value());
  EXPECT_TRUE(db.Get("s3").has_value());
}

// M1-T11 / tla NoResurrection: size-tiered partial merge must keep
// tombstones so an older live row in a non-participating segment cannot
// resurface; full Compact() may purge them.
TEST(Db, SizeTieredPartialMergeKeepsTombstonesFullCompactPurges) {
  Db::Options options = SmallDb();
  options.compaction_tier_threshold = 4;
  options.max_segments_before_compact = 0;
  Db db(options);

  // Older tier: live "victim" plus fillers (size 4).
  ASSERT_TRUE(db.Upsert(MakeRow("victim", {1.0f, 0.0f}, 1)).ok());
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("big" + std::to_string(i), {1.0f, 0.0f}, 2 + i))
            .ok());
  }
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_EQ(db.segment_count(), 1u);
  EXPECT_TRUE(db.Get("victim").has_value());

  // Newer tier: tombstone for victim, then three size-1 flushes to trip merge.
  ASSERT_TRUE(db.Delete("victim", 100).ok());
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_FALSE(db.Get("victim").has_value());

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("s" + std::to_string(i), {0.0f, 1.0f}, 110 + i))
            .ok());
    ASSERT_TRUE(db.Flush().ok());
  }
  // Partial merge of the four size-1 segments; older size-4 segment remains.
  EXPECT_EQ(db.segment_count(), 2u);
  // Tombstone must still win — dropping it would resurrect victim@1.
  EXPECT_FALSE(db.Get("victim").has_value());
  EXPECT_TRUE(db.Get("s2").has_value());
  // Row count still accounts for the retained tombstone (+ older live copy).
  EXPECT_GE(db.approximate_row_count(), 8u);

  ASSERT_TRUE(db.Compact().ok());
  EXPECT_EQ(db.segment_count(), 1u);
  EXPECT_FALSE(db.Get("victim").has_value());
  EXPECT_TRUE(db.Get("s2").has_value());
  // Full-overlap purge dropped victim's tombstone (and shadowed big rows).
  EXPECT_EQ(db.approximate_row_count(), 6u);
}

#if ASTER_ENABLE_HNSW
// M2-T04: flushed segments start PENDING (exact search); BuildPendingIndexes
// advances PENDING→BUILDING→READY and switches Search onto the HNSW graph.
TEST(Db, SegStatePendingThenReadySwitchesSearchPath) {
  Db::Options options = SmallDb();
  options.background_index_build = false;
  options.max_segments_before_compact = 0;
  Db db(options);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_EQ(db.segment_count(), 1u);

  auto states = db.segment_index_states();
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0], SegState::kPending);
  EXPECT_FALSE(db.segment_uses_hnsw()[0]);

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;
  req.ef_search = 64;
  auto pending_hits = db.Search(req);
  ASSERT_EQ(pending_hits.size(), 1u);
  EXPECT_EQ(pending_hits[0].id, "a");

  ASSERT_TRUE(db.BuildPendingIndexes().ok());
  states = db.segment_index_states();
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0], SegState::kReady);
  EXPECT_TRUE(db.segment_uses_hnsw()[0]);

  auto ready_hits = db.Search(req);
  ASSERT_EQ(ready_hits.size(), 1u);
  EXPECT_EQ(ready_hits[0].id, "a");
}

TEST(Db, BackgroundIndexBuildReachesReady) {
  Db::Options options = SmallDb();
  options.background_index_build = true;
  options.max_segments_before_compact = 0;
  Db db(options);

  ASSERT_TRUE(db.Upsert(MakeRow("x", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Flush().ok());

  bool ready = false;
  for (int i = 0; i < 200; ++i) {
    auto states = db.segment_index_states();
    if (!states.empty() && states[0] == SegState::kReady) {
      ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(ready);
  EXPECT_TRUE(db.segment_uses_hnsw()[0]);

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;
  req.ef_search = 32;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "x");
}

TEST(Db, DurableReadyGraphSurvivesReopen) {
  const std::string dir = ::testing::TempDir() + "/aster_m2t04_hnsw";

  {
    Db::Options options = SmallDb();
    options.data_dir = dir;
    options.background_index_build = false;
    options.max_segments_before_compact = 0;
    options.wal_sync = SyncPolicy::kNever;
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok()) << db.status().message();
    ASSERT_TRUE(db.value()->Upsert(MakeRow("r", {1.0f, 0.0f}, 1)).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    ASSERT_TRUE(db.value()->BuildPendingIndexes().ok());
    EXPECT_EQ(db.value()->segment_index_states()[0], SegState::kReady);
  }

  Db::Options reopen = SmallDb();
  reopen.data_dir = dir;
  reopen.background_index_build = false;
  reopen.wal_sync = SyncPolicy::kNever;
  auto db = Db::Open(reopen);
  ASSERT_TRUE(db.ok()) << db.status().message();
  ASSERT_EQ(db.value()->segment_count(), 1u);
  EXPECT_EQ(db.value()->segment_index_states()[0], SegState::kReady);
  EXPECT_TRUE(db.value()->segment_uses_hnsw()[0]);

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;
  auto hits = db.value()->Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "r");
}

// M2-T05: Compact rebuilds one READY graph over live rows (no background
// index thread required). Deleted rows are not searchable after compact.
TEST(Db, CompactRebuildsSingleReadyGraphOverLiveRows) {
  Db::Options options = SmallDb();
  options.background_index_build = false;
  options.max_segments_before_compact = 0;
  options.compaction_tier_threshold = 0;
  Db db(options);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Upsert(MakeRow("c", {0.5f, 0.5f}, 3)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_EQ(db.segment_count(), 2u);
  // Flushed segments stay PENDING without background builds.
  EXPECT_EQ(db.segment_index_states()[0], SegState::kPending);
  EXPECT_EQ(db.segment_index_states()[1], SegState::kPending);

  ASSERT_TRUE(db.Delete("b", 10).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Compact().ok());

  ASSERT_EQ(db.segment_count(), 1u);
  EXPECT_EQ(db.segment_index_states()[0], SegState::kReady);
  EXPECT_TRUE(db.segment_uses_hnsw()[0]);
  EXPECT_FALSE(db.Get("b").has_value());
  EXPECT_TRUE(db.Get("a").has_value());
  EXPECT_TRUE(db.Get("c").has_value());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 10;
  req.ef_search = 32;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "a");
  for (const auto& h : hits) {
    EXPECT_NE(h.id, "b");
  }
}
#endif  // ASTER_ENABLE_HNSW

}  // namespace
}  // namespace aster
