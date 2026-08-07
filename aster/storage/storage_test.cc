#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "aster/index/bloom.h"
#include "aster/storage/manifest.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"
#include "aster/storage/sstable.h"
#include "aster/storage/wal.h"

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts,
            bool tombstone = false) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  row.tombstone = tombstone;
  return row;
}

TEST(Memtable, LastWriteWins) {
  Memtable mt;
  EXPECT_TRUE(mt.Apply(MakeRow("a", {1.0f}, 10)));
  EXPECT_FALSE(mt.Apply(MakeRow("a", {2.0f}, 5)));
  EXPECT_TRUE(mt.Apply(MakeRow("a", {3.0f}, 20)));
  auto row = mt.Get("a");
  ASSERT_TRUE(row.has_value());
  EXPECT_FLOAT_EQ(row->vector[0], 3.0f);
  EXPECT_EQ(mt.row_count(), 1u);
  EXPECT_GT(mt.approximate_bytes(), 0u);
}

TEST(Memtable, TombstonesAreKept) {
  Memtable mt;
  mt.Apply(MakeRow("a", {1.0f}, 10));
  mt.Apply(MakeRow("a", {}, 20, /*tombstone=*/true));
  auto row = mt.Get("a");
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->tombstone);
  EXPECT_EQ(mt.Scan().size(), 1u);
}

TEST(Wal, AppendReplayRoundTrip) {
  std::string path = ::testing::TempDir() + "/aster_wal_test.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kNever);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("first").ok());
    ASSERT_TRUE(writer.value().Append("second").ok());
    ASSERT_TRUE(writer.value().Append("").ok());
  }
  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().size(), 3u);
  EXPECT_EQ(records.value()[0], "first");
  EXPECT_EQ(records.value()[1], "second");
  EXPECT_EQ(records.value()[2], "");
  std::remove(path.c_str());
}

TEST(Wal, TornTailIsDropped) {
  std::string path = ::testing::TempDir() + "/aster_wal_torn.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kNever);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("intact").ok());
  }
  FILE* f = ::fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  ::fwrite("\x01\x02\x03", 1, 3, f);
  ::fclose(f);

  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().size(), 1u);
  EXPECT_EQ(records.value()[0], "intact");
  std::remove(path.c_str());
}

TEST(Wal, TruncateAfterFlush) {
  std::string path = ::testing::TempDir() + "/aster_wal_trunc.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kNever);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("old").ok());
    ASSERT_TRUE(writer.value().Truncate().ok());
    ASSERT_TRUE(writer.value().Append("new").ok());
  }
  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().size(), 1u);
  EXPECT_EQ(records.value()[0], "new");
  std::remove(path.c_str());
}

TEST(Segment, GetAndSearchSkipTombstones) {
  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 10),
      MakeRow("b", {0.0f, 1.0f}, 10, /*tombstone=*/true),
      MakeRow("c", {0.9f, 0.1f}, 10),
  };
  auto segment = Segment::Build(1, Metric::kL2, std::move(rows));
  ASSERT_TRUE(segment->Get("a").has_value());
  ASSERT_TRUE(segment->Get("b").has_value());
  EXPECT_TRUE(segment->Get("b")->tombstone);
  EXPECT_FALSE(segment->Get("zzz").has_value());

  const std::vector<float> query = {0.0f, 1.0f};
  auto hits = segment->Search(query, 10, 0);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "c");
}

TEST(Compaction, LwwMergeAndTombstonePurge) {
  auto old_seg = Segment::Build(
      1, Metric::kL2,
      {MakeRow("a", {1.0f}, 10), MakeRow("b", {2.0f}, 10)});
  auto new_seg = Segment::Build(
      2, Metric::kL2,
      {MakeRow("a", {9.0f}, 20), MakeRow("b", {}, 20, /*tombstone=*/true)});

  auto full = CompactSegments(3, Metric::kL2, {old_seg, new_seg},
                              /*drop_tombstones=*/true);
  EXPECT_EQ(full->row_count(), 1u);
  ASSERT_TRUE(full->Get("a").has_value());
  EXPECT_FLOAT_EQ(full->Get("a")->vector[0], 9.0f);

  auto partial = CompactSegments(4, Metric::kL2, {old_seg, new_seg},
                                 /*drop_tombstones=*/false);
  EXPECT_EQ(partial->row_count(), 2u);
  EXPECT_TRUE(partial->Get("b")->tombstone);
}

TEST(Bloom, NegativesAreExcluded) {
  BloomFilter bloom = BloomFilter::Build({"a", "b", "c"});
  EXPECT_TRUE(bloom.MayContain("a"));
  EXPECT_TRUE(bloom.MayContain("b"));
  EXPECT_FALSE(bloom.MayContain("definitely-missing-key-xyz"));
}

TEST(Sstable, WriteReadRoundTrip) {
  const std::string path = ::testing::TempDir() + "/seg_000001.ast";
  std::remove(path.c_str());

  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 10),
      MakeRow("b", {0.0f, 1.0f}, 11, /*tombstone=*/true),
      MakeRow("c", {0.5f, 0.5f}, 12),
  };
  rows[0].metadata = "meta-a";
  ASSERT_TRUE(WriteSstable(path, /*segment_id=*/1, Metric::kL2, rows).ok());

  auto reader = SstableReader::Open(path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ(reader.value()->segment_id(), 1u);
  EXPECT_EQ(reader.value()->row_count(), 3u);

  auto a = reader.value()->Get("a");
  ASSERT_TRUE(a.has_value());
  EXPECT_FALSE(a->tombstone);
  ASSERT_EQ(a->vector.size(), 2u);
  EXPECT_FLOAT_EQ(a->vector[0], 1.0f);
  EXPECT_EQ(a->metadata, "meta-a");

  auto b = reader.value()->Get("b");
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(b->tombstone);

  EXPECT_FALSE(reader.value()->Get("zzz").has_value());
  EXPECT_FALSE(reader.value()->MayContain("zzz"));

  auto all = reader.value()->LoadAll();
  EXPECT_EQ(all.size(), 3u);
  std::remove(path.c_str());
}

TEST(Manifest, AtomicSwapLeavesPriorGeneration) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/MANIFEST";
  std::remove(path.c_str());

  Manifest m1;
  m1.generation = 1;
  m1.segments.push_back({1, "seg_000001.ast"});
  ASSERT_TRUE(WriteManifest(path, m1).ok());

  {
    std::ofstream out(path + ".tmp");
    out << "ASTMANIFEST1\ngeneration=2\nsegment=2\tseg_000002.ast\n";
  }

  auto loaded = ReadManifest(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().generation, 1u);
  ASSERT_EQ(loaded.value().segments.size(), 1u);
  EXPECT_EQ(loaded.value().segments[0].segment_id, 1u);

  Manifest m2;
  m2.generation = 2;
  m2.segments.push_back({2, "seg_000002.ast"});
  ASSERT_TRUE(WriteManifest(path, m2).ok());
  loaded = ReadManifest(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().generation, 2u);
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
}

}  // namespace
}  // namespace aster
