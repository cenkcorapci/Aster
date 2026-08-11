#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "aster/index/bloom.h"
#include "aster/core/features.h"
#include "aster/index/vector_index.h"
#include "aster/storage/compaction.h"
#include "aster/storage/manifest.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"
#include "aster/storage/sstable.h"
#include "aster/storage/wal.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_graph.h"
#endif

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts,
            bool tombstone = false,
            std::set<std::string> tags = {}) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  row.tombstone = tombstone;
  row.tags = std::move(tags);
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

TEST(Memtable, TakeMovesRowsAndClears) {
  Memtable mt;
  mt.Apply(MakeRow("a", {1.0f}, 1));
  mt.Apply(MakeRow("b", {2.0f}, 2));
  auto rows = mt.Take();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(mt.empty());
  EXPECT_EQ(mt.approximate_bytes(), 0u);
  EXPECT_EQ(rows[0].id, "a");
  EXPECT_EQ(rows[1].id, "b");
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

TEST(Memtable, MissingGetAndEmpty) {
  Memtable mt;
  EXPECT_TRUE(mt.empty());
  EXPECT_FALSE(mt.Get("nope").has_value());
  EXPECT_TRUE(mt.Scan().empty());
  EXPECT_EQ(mt.approximate_bytes(), 0u);
}

TEST(Memtable, VersionTieBreak) {
  Memtable mt;
  Row a = MakeRow("a", {1.0f}, 10);
  a.version = 1;
  Row b = MakeRow("a", {2.0f}, 10);
  b.version = 2;
  EXPECT_TRUE(mt.Apply(a));
  EXPECT_TRUE(mt.Apply(b));
  EXPECT_FLOAT_EQ(mt.Get("a")->vector[0], 2.0f);
  EXPECT_FALSE(mt.Apply(a));  // older version rejected
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

TEST(Wal, AlwaysAndEveryMsAndSync) {
  std::string path = ::testing::TempDir() + "/aster_wal_sync.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kAlways);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("a").ok());
    ASSERT_TRUE(writer.value().Sync().ok());
  }
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kEveryMs);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("b").ok());
  }
  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  EXPECT_GE(records.value().size(), 2u);
  std::remove(path.c_str());
}

TEST(Wal, CorruptCrcStopsReplay) {
  std::string path = ::testing::TempDir() + "/aster_wal_crc.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kNever);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value().Append("good").ok());
  }
  // Flip a payload byte after the 12-byte header of the first record.
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(12);
    char x = 'X';
    f.write(&x, 1);
  }
  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  EXPECT_TRUE(records.value().empty());  // corrupt first record stops replay
  std::remove(path.c_str());
}

TEST(Wal, TruncateWalFileUtility) {
  std::string path = ::testing::TempDir() + "/aster_wal_util.log";
  std::remove(path.c_str());
  {
    auto writer = WalWriter::Open(path, SyncPolicy::kNever);
    ASSERT_TRUE(writer.value().Append("x").ok());
  }
  ASSERT_TRUE(TruncateWalFile(path).ok());
  auto records = ReplayWal(path);
  ASSERT_TRUE(records.ok());
  EXPECT_TRUE(records.value().empty());
  std::remove(path.c_str());
}

TEST(Wal, OpenMissingDirectoryFails) {
  auto writer = WalWriter::Open("/no/such/dir/wal.log", SyncPolicy::kNever);
  EXPECT_FALSE(writer.ok());
}

TEST(Wal, ReplayMissingFails) {
  auto records = ReplayWal("/no/such/wal.log");
  EXPECT_FALSE(records.ok());
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
  EXPECT_EQ(segment->id(), 1u);
  EXPECT_EQ(segment->row_count(), 3u);

  const std::vector<float> query = {0.0f, 1.0f};
  auto hits = segment->Search(query, 10, 0);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "c");
}

TEST(Segment, EmptySegment) {
  auto segment = Segment::Build(7, Metric::kCosine, std::vector<Row>{});
  EXPECT_EQ(segment->row_count(), 0u);
  EXPECT_TRUE(segment->Search(std::vector<float>{1.0f}, 5, 0).empty());
}

// Pins tla/AsterLsmIndex.tla SegState: PENDING → BUILDING → READY, with
// BUILDING → PENDING on abort. Search stays exact until READY+HNSW.
TEST(Segment, IndexBuildStateMachine) {
  auto segment = Segment::Build(
      1, Metric::kL2,
      {MakeRow("a", {1.0f, 0.0f}, 10), MakeRow("b", {0.0f, 1.0f}, 10)});
  EXPECT_EQ(segment->index_state(), SegState::kPending);
  EXPECT_FALSE(segment->search_uses_hnsw());

  const std::vector<float> query = {1.0f, 0.0f};
  auto pending_hits = segment->Search(query, 1, 32);
  ASSERT_EQ(pending_hits.size(), 1u);
  EXPECT_EQ(pending_hits[0].id, "a");

  ASSERT_TRUE(segment->TryBeginIndexBuild());
  EXPECT_EQ(segment->index_state(), SegState::kBuilding);
  EXPECT_FALSE(segment->TryBeginIndexBuild());  // not PENDING
  auto building_hits = segment->Search(query, 1, 32);
  ASSERT_EQ(building_hits.size(), 1u);
  EXPECT_EQ(building_hits[0].id, "a");

  segment->AbortIndexBuild();
  EXPECT_EQ(segment->index_state(), SegState::kPending);

  ASSERT_TRUE(segment->TryBeginIndexBuild());
#if ASTER_ENABLE_HNSW
  std::vector<IndexEntry> entries = {{"a", segment->rows()[0].vector},
                                     {"b", segment->rows()[1].vector}};
  auto hnsw = BuildHnswIndex(Metric::kL2, HnswParams{}, std::move(entries), 1);
  segment->CompleteIndexBuild(std::move(hnsw));
  EXPECT_EQ(segment->index_state(), SegState::kReady);
  EXPECT_TRUE(segment->search_uses_hnsw());
#else
  segment->CompleteIndexBuild(nullptr);
  EXPECT_EQ(segment->index_state(), SegState::kReady);
  EXPECT_FALSE(segment->search_uses_hnsw());
#endif
  auto ready_hits = segment->Search(query, 1, 32);
  ASSERT_EQ(ready_hits.size(), 1u);
  EXPECT_EQ(ready_hits[0].id, "a");
}

TEST(Compaction, LwwMergeAndTombstonePurge) {
  auto old_seg = Segment::Build(
      1, Metric::kL2,
      {MakeRow("a", {1.0f}, 10), MakeRow("b", {2.0f}, 10)});
  auto new_seg = Segment::Build(
      2, Metric::kL2,
      {MakeRow("a", {9.0f}, 20), MakeRow("b", {}, 20, /*tombstone=*/true)});

  // Full-overlap Compact (tla CompactFull): safe to drop tombstones.
  auto full = CompactSegments(3, Metric::kL2, {old_seg, new_seg},
                              /*drop_tombstones=*/true);
  EXPECT_EQ(full->row_count(), 1u);
  ASSERT_TRUE(full->Get("a").has_value());
  EXPECT_FLOAT_EQ(full->Get("a")->vector[0], 9.0f);

  // Partial Compact (tla CompactPartial): must keep tombstones.
  auto partial = CompactSegments(4, Metric::kL2, {old_seg, new_seg},
                                 /*drop_tombstones=*/false);
  EXPECT_EQ(partial->row_count(), 2u);
  EXPECT_TRUE(partial->Get("b")->tombstone);
}

// Pins tla/AsterLsmIndex.tla NoResurrection: the CompactPartialDropTombstones
// counterexample. Purging a tombstone when an older live version lives
// outside the input set resurrects the delete.
TEST(Compaction, PartialCompactKeepsTombstoneToPreventResurrection) {
  auto older_live = Segment::Build(
      1, Metric::kL2, {MakeRow("k", {1.0f}, 10)});
  auto tomb = Segment::Build(
      2, Metric::kL2, {MakeRow("k", {}, 20, /*tombstone=*/true)});
  auto unrelated = Segment::Build(
      3, Metric::kL2, {MakeRow("other", {3.0f}, 15)});

  // Size-tiered / partial merge of {tomb, unrelated} must retain k's tombstone.
  auto partial = CompactSegments(4, Metric::kL2, {tomb, unrelated},
                                 /*drop_tombstones=*/false);
  ASSERT_TRUE(partial->Get("k").has_value());
  EXPECT_TRUE(partial->Get("k")->tombstone);
  EXPECT_TRUE(partial->Get("other").has_value());

  // Reconcile across older_live + partial: tombstone still wins (no resurrection).
  auto visible = CompactSegments(5, Metric::kL2, {older_live, partial},
                                 /*drop_tombstones=*/false);
  ASSERT_TRUE(visible->Get("k").has_value());
  EXPECT_TRUE(visible->Get("k")->tombstone);

  // Full-overlap over every segment that could hold k: tombstone may be dropped.
  auto full = CompactSegments(6, Metric::kL2, {older_live, partial},
                              /*drop_tombstones=*/true);
  EXPECT_FALSE(full->Get("k").has_value());
  EXPECT_TRUE(full->Get("other").has_value());
}

TEST(Compaction, SingleInputIsIdentityMerge) {
  auto seg = Segment::Build(1, Metric::kL2, {MakeRow("a", {1.0f}, 10)});
  auto out = CompactSegments(2, Metric::kL2, {seg}, /*drop_tombstones=*/true);
  EXPECT_EQ(out->row_count(), 1u);
  EXPECT_EQ(out->Get("a")->id, "a");
}

TEST(Bloom, NegativesAreExcluded) {
  BloomFilter bloom = BloomFilter::Build({"a", "b", "c"});
  EXPECT_TRUE(bloom.MayContain("a"));
  EXPECT_FALSE(bloom.MayContain("definitely-missing-key-xyz"));
}

TEST(Sstable, WriteReadRoundTrip) {
  const std::string path = ::testing::TempDir() + "/seg_000001.ast";
  std::remove(path.c_str());

  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 10, false, {"even", "hot"}),
      MakeRow("b", {0.0f, 1.0f}, 11, /*tombstone=*/true),
      MakeRow("c", {0.5f, 0.5f}, 12, false, {"odd"}),
  };
  rows[0].metadata = "meta-a";
  ASSERT_TRUE(WriteSstable(path, /*segment_id=*/1, Metric::kL2, rows).ok());

  auto reader = SstableReader::Open(path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ(reader.value()->segment_id(), 1u);
  EXPECT_EQ(reader.value()->row_count(), 3u);
  EXPECT_EQ(reader.value()->dimension(), 2u);
  EXPECT_EQ(reader.value()->metric(), Metric::kL2);

  auto a = reader.value()->Get("a");
  ASSERT_TRUE(a.has_value());
  EXPECT_FALSE(a->tombstone);
  ASSERT_EQ(a->vector.size(), 2u);
  EXPECT_FLOAT_EQ(a->vector[0], 1.0f);
  EXPECT_EQ(a->metadata, "meta-a");
  EXPECT_EQ(a->tags, (std::set<std::string>{"even", "hot"}));

  auto b = reader.value()->Get("b");
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(b->tombstone);

  auto c = reader.value()->Get("c");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->tags, (std::set<std::string>{"odd"}));

  EXPECT_FALSE(reader.value()->Get("zzz").has_value());
  EXPECT_FALSE(reader.value()->MayContain("zzz"));

  auto all = reader.value()->LoadAll();
  EXPECT_EQ(all.size(), 3u);
  std::remove(path.c_str());
}

TEST(Sstable, TakeAllMovesVectors) {
  const std::string path = ::testing::TempDir() + "/seg_takeall.ast";
  std::remove(path.c_str());
  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 10, false, {"t"}),
      MakeRow("b", {0.0f, 1.0f}, 11),
  };
  ASSERT_TRUE(WriteSstable(path, 2, Metric::kCosine, rows).ok());
  auto reader = SstableReader::Open(path);
  ASSERT_TRUE(reader.ok());
  auto taken = reader.value()->TakeAll();
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].id, "a");
  ASSERT_EQ(taken[0].vector.size(), 2u);
  EXPECT_FLOAT_EQ(taken[0].vector[0], 1.0f);
  EXPECT_EQ(taken[0].tags, (std::set<std::string>{"t"}));
  EXPECT_EQ(taken[1].id, "b");
  EXPECT_EQ(reader.value()->row_count(), 0u);
  EXPECT_FALSE(reader.value()->Get("a").has_value());
  std::remove(path.c_str());
}

TEST(Sstable, EmptyAndUnsortedRejected) {
  const std::string path = ::testing::TempDir() + "/seg_empty.ast";
  std::remove(path.c_str());
  ASSERT_TRUE(WriteSstable(path, 1, Metric::kDot, {}).ok());
  auto reader = SstableReader::Open(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader.value()->row_count(), 0u);
  EXPECT_TRUE(reader.value()->LoadAll().empty());
  std::remove(path.c_str());

  std::vector<Row> bad = {MakeRow("b", {1.0f}, 1), MakeRow("a", {1.0f}, 1)};
  EXPECT_FALSE(WriteSstable(path, 2, Metric::kL2, bad).ok());
}

TEST(Sstable, DimensionMismatchRejected) {
  const std::string path = ::testing::TempDir() + "/seg_bad_dim.ast";
  std::vector<Row> bad = {MakeRow("a", {1.0f}, 1), MakeRow("b", {1.0f, 2.0f}, 2)};
  EXPECT_FALSE(WriteSstable(path, 1, Metric::kL2, bad).ok());
}

TEST(Sstable, OpenMissingAndCorruptMagic) {
  EXPECT_FALSE(SstableReader::Open("/no/such.ast").ok());
  const std::string path = ::testing::TempDir() + "/seg_bad.ast";
  {
    std::ofstream out(path, std::ios::binary);
    out << "not-an-sstable-file-content-padding-xxxxxxxxxx";
  }
  EXPECT_FALSE(SstableReader::Open(path).ok());
  std::remove(path.c_str());
}

TEST(Sstable, CustomSparseStride) {
  const std::string path = ::testing::TempDir() + "/seg_stride.ast";
  std::vector<Row> rows;
  for (int i = 0; i < 40; ++i) {
    char id[32];
    std::snprintf(id, sizeof(id), "id-%03d", i);  // lexicographic == numeric
    rows.push_back(MakeRow(id, {static_cast<float>(i)},
                           static_cast<Timestamp>(i + 1)));
  }
  SstableWriteOptions opts;
  opts.sparse_stride = 5;
  ASSERT_TRUE(WriteSstable(path, 9, Metric::kL2, rows, opts).ok());
  auto reader = SstableReader::Open(path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ(reader.value()->Get("id-039")->vector[0], 39.0f);
  std::remove(path.c_str());
}

std::vector<Row> MakeCompressibleRows(int n) {
  std::vector<Row> rows;
  rows.reserve(static_cast<size_t>(n));
  const std::string meta(512, 'm');  // highly compressible CBOR-ish blob
  for (int i = 0; i < n; ++i) {
    char id[32];
    std::snprintf(id, sizeof(id), "row-%04d", i);
    // Repeated float pattern compresses well vs random noise.
    std::vector<float> vec(32, 0.125f);
    Row row = MakeRow(id, std::move(vec), static_cast<Timestamp>(i + 1));
    row.metadata = meta;
    rows.push_back(std::move(row));
  }
  return rows;
}

#if ASTER_ENABLE_COMPRESSION

TEST(Sstable, Lz4RoundTripAndSmaller) {
  const auto rows = MakeCompressibleRows(64);
  const std::string none_path = ::testing::TempDir() + "/seg_none.ast";
  const std::string lz4_path = ::testing::TempDir() + "/seg_lz4.ast";
  std::remove(none_path.c_str());
  std::remove(lz4_path.c_str());

  ASSERT_TRUE(WriteSstable(none_path, 1, Metric::kL2, rows).ok());
  SstableWriteOptions opts;
  opts.compression = CompressionCodec::kLz4;
  ASSERT_TRUE(WriteSstable(lz4_path, 1, Metric::kL2, rows, opts).ok());

  auto none_sz = [&](const std::string& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    return static_cast<size_t>(in.tellg());
  };
  EXPECT_LT(none_sz(lz4_path), none_sz(none_path));

  auto reader = SstableReader::Open(lz4_path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ(reader.value()->row_count(), rows.size());
  auto got = reader.value()->Get("row-0000");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->metadata, rows[0].metadata);
  ASSERT_EQ(got->vector.size(), 32u);
  EXPECT_FLOAT_EQ(got->vector[0], 0.125f);
  auto last = reader.value()->Get("row-0063");
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->metadata.size(), 512u);

  std::remove(none_path.c_str());
  std::remove(lz4_path.c_str());
}

TEST(Sstable, ZstdRoundTripAndSmaller) {
  const auto rows = MakeCompressibleRows(64);
  const std::string none_path = ::testing::TempDir() + "/seg_none_z.ast";
  const std::string zstd_path = ::testing::TempDir() + "/seg_zstd.ast";
  std::remove(none_path.c_str());
  std::remove(zstd_path.c_str());

  ASSERT_TRUE(WriteSstable(none_path, 2, Metric::kCosine, rows).ok());
  SstableWriteOptions opts;
  opts.compression = CompressionCodec::kZstd;
  ASSERT_TRUE(WriteSstable(zstd_path, 2, Metric::kCosine, rows, opts).ok());

  auto file_sz = [](const std::string& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    return static_cast<size_t>(in.tellg());
  };
  EXPECT_LT(file_sz(zstd_path), file_sz(none_path));

  auto reader = SstableReader::Open(zstd_path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  auto all = reader.value()->LoadAll();
  ASSERT_EQ(all.size(), rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(all[i].id, rows[i].id);
    EXPECT_EQ(all[i].metadata, rows[i].metadata);
    ASSERT_EQ(all[i].vector.size(), rows[i].vector.size());
    EXPECT_FLOAT_EQ(all[i].vector[0], rows[i].vector[0]);
  }

  std::remove(none_path.c_str());
  std::remove(zstd_path.c_str());
}

#else

TEST(Sstable, CompressionRejectedWhenDisabled) {
  const auto rows = MakeCompressibleRows(4);
  const std::string path = ::testing::TempDir() + "/seg_comp_off.ast";
  SstableWriteOptions opts;
  opts.compression = CompressionCodec::kLz4;
  EXPECT_FALSE(WriteSstable(path, 1, Metric::kL2, rows, opts).ok());
  opts.compression = CompressionCodec::kZstd;
  EXPECT_FALSE(WriteSstable(path, 1, Metric::kL2, rows, opts).ok());
}

#endif

TEST(Manifest, AtomicSwapLeavesPriorGeneration) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/MANIFEST";
  std::remove(path.c_str());

  Manifest m1;
  m1.generation = 1;
  m1.segments.push_back({1, "seg_000001.ast", ""});
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
  EXPECT_TRUE(loaded.value().segments[0].hnsw_path.empty());

  Manifest m2;
  m2.generation = 2;
  m2.segments.push_back({2, "seg_000002.ast", "index/seg_000002.hnsw"});
  ASSERT_TRUE(WriteManifest(path, m2).ok());
  loaded = ReadManifest(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().generation, 2u);
  ASSERT_EQ(loaded.value().segments.size(), 1u);
  EXPECT_EQ(loaded.value().segments[0].hnsw_path, "index/seg_000002.hnsw");
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
}

TEST(Manifest, CorruptAndMissing) {
  EXPECT_FALSE(ReadManifest("/no/such/MANIFEST").ok());
  const std::string path = ::testing::TempDir() + "/MANIFEST_BAD";
  {
    std::ofstream out(path);
    out << "NOTMAGIC\n";
  }
  EXPECT_FALSE(ReadManifest(path).ok());
  std::remove(path.c_str());
}

TEST(Crc32, KnownEmptyAndNonEmpty) {
  EXPECT_NE(Crc32("", 0), Crc32("a", 1));
  EXPECT_EQ(Crc32("abc", 3), Crc32("abc", 3));
}

TEST(SizeTiered, BucketByExponentialSize) {
  EXPECT_EQ(SizeTieredBucket(0), 0);
  EXPECT_EQ(SizeTieredBucket(1), 0);
  EXPECT_EQ(SizeTieredBucket(3), 0);
  EXPECT_EQ(SizeTieredBucket(4), 1);
  EXPECT_EQ(SizeTieredBucket(15), 1);
  EXPECT_EQ(SizeTieredBucket(16), 2);
  EXPECT_EQ(SizeTieredBucket(64), 3);
}

TEST(SizeTiered, NoPickBelowThreshold) {
  std::vector<size_t> sizes = {1, 1, 1};
  EXPECT_FALSE(SelectSizeTieredCompaction(sizes, /*tier_threshold=*/4).has_value());
  EXPECT_FALSE(SelectSizeTieredCompaction(sizes, /*tier_threshold=*/0).has_value());
  EXPECT_FALSE(SelectSizeTieredCompaction({1}, /*tier_threshold=*/4).has_value());
}

TEST(SizeTiered, SelectsLowestOverflowingTier) {
  // Four flush-sized segments → pick all of tier 0.
  std::vector<size_t> sizes = {1, 1, 1, 1};
  auto pick = SelectSizeTieredCompaction(sizes, 4);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(pick->tier, 0);
  ASSERT_EQ(pick->input_indices.size(), 4u);
  EXPECT_EQ(pick->input_indices[0], 0u);
  EXPECT_EQ(pick->input_indices[3], 3u);

  // Mixed tiers: three small + four medium-sized → pick the medium tier (4).
  sizes = {1, 1, 1, 8, 8, 8, 8};
  pick = SelectSizeTieredCompaction(sizes, 4);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(pick->tier, SizeTieredBucket(8));
  ASSERT_EQ(pick->input_indices.size(), 4u);
  EXPECT_EQ(pick->input_indices[0], 3u);
  EXPECT_EQ(pick->input_indices[3], 6u);
}

TEST(SizeTiered, PrefersLowerTierWhenBothOverflow) {
  // Both tier 0 and tier 1 overflow; lowest tier wins.
  std::vector<size_t> sizes = {1, 1, 1, 1, 8, 8, 8, 8};
  auto pick = SelectSizeTieredCompaction(sizes, 4);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(pick->tier, 0);
  EXPECT_EQ(pick->input_indices.size(), 4u);
}

}  // namespace
}  // namespace aster
