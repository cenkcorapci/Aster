#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aster/db/db.h"

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

}  // namespace
}  // namespace aster
