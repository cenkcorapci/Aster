#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"
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
  EXPECT_FALSE(mt.Apply(MakeRow("a", {2.0f}, 5)));  // stale write rejected
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
  // Simulate a crash mid-append: garbage header bytes at the tail.
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

TEST(Segment, GetAndSearchSkipTombstones) {
  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 10),
      MakeRow("b", {0.0f, 1.0f}, 10, /*tombstone=*/true),
      MakeRow("c", {0.9f, 0.1f}, 10),
  };
  auto segment = Segment::Build(1, Metric::kL2, std::move(rows));
  ASSERT_TRUE(segment->Get("a").has_value());
  ASSERT_TRUE(segment->Get("b").has_value());  // visible for repair
  EXPECT_TRUE(segment->Get("b")->tombstone);
  EXPECT_FALSE(segment->Get("zzz").has_value());

  const std::vector<float> query = {0.0f, 1.0f};
  auto hits = segment->Search(query, 10, 0);
  ASSERT_EQ(hits.size(), 2u);  // tombstoned "b" excluded from the index
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
  EXPECT_EQ(full->row_count(), 1u);  // "b" purged
  ASSERT_TRUE(full->Get("a").has_value());
  EXPECT_FLOAT_EQ(full->Get("a")->vector[0], 9.0f);  // newest version won

  auto partial = CompactSegments(4, Metric::kL2, {old_seg, new_seg},
                                 /*drop_tombstones=*/false);
  EXPECT_EQ(partial->row_count(), 2u);  // tombstone preserved
  EXPECT_TRUE(partial->Get("b")->tombstone);
}

}  // namespace
}  // namespace aster
