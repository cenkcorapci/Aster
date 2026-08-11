#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>

#include <set>
#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/distributed/ring.h"
#include "aster/index/distance.h"
#include "aster/metrics/metrics.h"
#include "aster/platform/posix_storage.h"
#include "aster/query/topk.h"
#include "aster/server/catalog.h"
#include "aster/storage/manifest.h"
#include "aster/storage/sstable.h"

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

uint64_t DirBytes(const std::string& path) {
  uint64_t total = 0;
  DIR* dir = ::opendir(path.c_str());
  if (!dir) return 0;
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    const std::string child = path + "/" + name;
    struct stat st {};
    if (::stat(child.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      total += DirBytes(child);
    } else {
      total += static_cast<uint64_t>(st.st_size);
    }
  }
  ::closedir(dir);
  return total;
}

int CountFilesWithPrefix(const std::string& path, const std::string& prefix) {
  int n = 0;
  DIR* dir = ::opendir(path.c_str());
  if (!dir) return 0;
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name.rfind(prefix, 0) == 0) ++n;
  }
  ::closedir(dir);
  return n;
}

// End-to-end: write → flush → search → delete → compact → reopen.
TEST(Integration, DurableLifecycleSearchAndRecovery) {
  const std::string dir = ::testing::TempDir() + "/aster_integ_life";
  Db::Options options;
  options.dimension = 4;
  options.metric = Metric::kCosine;
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;

  {
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok()) << db.status().message();

    for (int i = 0; i < 50; ++i) {
      std::vector<float> v = {static_cast<float>(i), 0.1f, 0.2f, 0.3f};
      std::set<std::string> tags = {i % 2 == 0 ? "even" : "odd"};
      ASSERT_TRUE(
          db.value()
              ->Upsert(MakeRow("doc-" + std::to_string(i), v,
                               static_cast<Timestamp>(i + 1), tags))
              .ok());
      if ((i + 1) % 10 == 0) {
        ASSERT_TRUE(db.value()->Flush().ok());
      }
    }
    EXPECT_GE(db.value()->segment_count(), 5u);

    SearchRequest req;
    req.vector = {40.0f, 0.1f, 0.2f, 0.3f};
    req.top_k = 3;
    req.tags = {"even"};
    auto hits = db.value()->Search(req);
    ASSERT_FALSE(hits.empty());
    for (const auto& hit : hits) {
      auto row = db.value()->Get(hit.id);
      ASSERT_TRUE(row.has_value());
      EXPECT_TRUE(row->tags.count("even") > 0);
    }

    ASSERT_TRUE(db.value()->Delete("doc-40", 1000).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    EXPECT_FALSE(db.value()->Get("doc-40").has_value());

    ASSERT_TRUE(db.value()->Compact().ok());
    EXPECT_EQ(db.value()->segment_count(), 1u);
  }

  auto reopened = Db::Open(options);
  ASSERT_TRUE(reopened.ok());
  EXPECT_FALSE(reopened.value()->Get("doc-40").has_value());
  EXPECT_TRUE(reopened.value()->Get("doc-41").has_value());

  SearchRequest req;
  req.vector = {41.0f, 0.1f, 0.2f, 0.3f};
  req.top_k = 5;
  auto hits = reopened.value()->Search(req);
  ASSERT_FALSE(hits.empty());
}

// SSTable + manifest + posix layout used together.
TEST(Integration, PosixLayoutWithSstableAndManifest) {
  const std::string root = ::testing::TempDir() + "/aster_integ_posix";
  ::mkdir(root.c_str(), 0755);
  PosixStorage store(root);

  std::vector<Row> rows = {
      MakeRow("a", {1.0f, 0.0f}, 1),
      MakeRow("b", {0.0f, 1.0f}, 2),
  };
  const std::string seg_rel = "segments/seg_000001.ast";
  const std::string seg_abs = root + "/" + seg_rel;
  ::mkdir((root + "/segments").c_str(), 0755);
  ASSERT_TRUE(WriteSstable(seg_abs, 1, Metric::kL2, rows).ok());

  // Store a copy of the bytes via StorageBackend API as well.
  auto bytes = store.Read("segments/seg_000001.ast");
  // PosixStorage root already contains the file written by WriteSstable.
  ASSERT_TRUE(store.Exists(seg_rel));

  Manifest m;
  m.generation = 1;
  m.segments.push_back({1, seg_rel});
  const std::string man = root + "/MANIFEST";
  ASSERT_TRUE(WriteManifest(man, m).ok());

  auto loaded = ReadManifest(man);
  ASSERT_TRUE(loaded.ok());
  auto reader = SstableReader::Open(root + "/" + loaded.value().segments[0].path);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader.value()->Get("a").has_value());
  EXPECT_TRUE(reader.value()->Get("b").has_value());
}

// Coordinator-style scatter: ring placement + MergeTopK of per-node hits.
TEST(Integration, RingScatterGatherMerge) {
  Ring ring(64);
  ring.AddNode("n1");
  ring.AddNode("n2");
  ring.AddNode("n3");

  // Simulate three vnode-local result lists.
  std::vector<std::vector<SearchHit>> per_node;
  for (const char* node : {"n1", "n2", "n3"}) {
    (void)node;
    per_node.push_back({});
  }
  for (int i = 0; i < 30; ++i) {
    const std::string id = "id-" + std::to_string(i);
    auto reps = ring.GetReplicas(id, 1);
    ASSERT_EQ(reps.size(), 1u);
    const int idx = reps[0] == "n1" ? 0 : reps[0] == "n2" ? 1 : 2;
    per_node[idx].push_back({id, static_cast<float>(i)});
  }

  auto merged = MergeTopK(per_node, 5);
  ASSERT_EQ(merged.size(), 5u);
  EXPECT_EQ(merged[0].id, "id-29");
}

// Metrics observe around a mini Db workload.
TEST(Integration, MetricsAroundSearchWorkload) {
  MetricsRegistry registry;
  Db db([&] {
    Db::Options o;
    o.dimension = 2;
    o.metric = Metric::kL2;
    return o;
  }());

  registry.Gauge("segment_count").Set(0);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Flush().ok());
  registry.Gauge("segment_count").Set(static_cast<int64_t>(db.segment_count()));
  registry.Histogram("write_latency_ms").Observe(1.2);
  registry.Histogram("read_latency_ms").Observe(0.8);

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  auto hits = db.Search(req);
  registry.Histogram("hnsw_search_latency").Observe(2.5);
  ASSERT_FALSE(hits.empty());

  const std::string text = registry.Render();
  EXPECT_NE(text.find("segment_count 1\n"), std::string::npos);
  EXPECT_NE(text.find("write_latency_ms_count 1\n"), std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_count 1\n"), std::string::npos);
}

// LWW across memtable + multiple durable segments with reopen.
TEST(Integration, LwwAcrossSegmentsAfterReopen) {
  const std::string dir = ::testing::TempDir() + "/aster_integ_lww";
  Db::Options options;
  options.dimension = 2;
  options.metric = Metric::kDot;
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;

  {
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok());
    ASSERT_TRUE(db.value()->Upsert(MakeRow("k", {1.0f, 0.0f}, 1)).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    ASSERT_TRUE(db.value()->Upsert(MakeRow("k", {0.0f, 1.0f}, 2)).ok());
    ASSERT_TRUE(db.value()->Flush().ok());
  }
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok());
  auto row = db.value()->Get("k");
  ASSERT_TRUE(row.has_value());
  EXPECT_FLOAT_EQ(row->vector[0], 0.0f);
  EXPECT_FLOAT_EQ(row->vector[1], 1.0f);
  EXPECT_FLOAT_EQ(Score(Metric::kDot, row->vector, std::vector<float>{0.0f, 1.0f}),
                  1.0f);
}

// Simulates an update/delete-heavy workload and asserts disk usage shrinks
// after compaction / drop (guards against silent storage leaks).
TEST(Integration, StorageUsageSimulationUpdateDeleteCompactDrop) {
  constexpr int kDocs = 200;
  constexpr uint32_t kDim = 32;
  const std::string root = ::testing::TempDir() + "/aster_integ_storage_sim";
  ::mkdir(root.c_str(), 0755);

  Catalog::Options cat_opt;
  cat_opt.data_dir = root;
  cat_opt.wal_sync = SyncPolicy::kNever;
  cat_opt.max_segments_before_compact = 0;  // manual compact for measurement
  auto cat = Catalog::Open(cat_opt);
  ASSERT_TRUE(cat.ok()) << cat.status().message();

  CollectionInfo info;
  info.name = "sim";
  info.dimension = kDim;
  info.metric = Metric::kL2;
  ASSERT_TRUE(cat.value()->CreateCollection(info).ok());
  const std::string col_dir = root + "/sim";

  auto MakeVec = [&](int seed) {
    std::vector<float> v(kDim);
    for (uint32_t i = 0; i < kDim; ++i) {
      v[i] = static_cast<float>((seed * 31 + static_cast<int>(i)) % 100) / 100.0f;
    }
    return v;
  };

  // Phase 1: insert + flush in batches (multiple segments / versions).
  for (int i = 0; i < kDocs; ++i) {
    ASSERT_TRUE(cat.value()
                    ->Upsert("sim", MakeRow("d" + std::to_string(i), MakeVec(i),
                                            static_cast<Timestamp>(i + 1)))
                    .ok());
    if ((i + 1) % 40 == 0) {
      ASSERT_TRUE(cat.value()->Flush("sim").ok());
    }
  }
  ASSERT_TRUE(cat.value()->Flush("sim").ok());
  const uint64_t after_insert = DirBytes(col_dir);
  const int segs_after_insert = CountFilesWithPrefix(col_dir, "seg_");
  EXPECT_GE(segs_after_insert, 2);
  EXPECT_GT(after_insert, 0u);

  // Phase 2: rewrite every doc (LWW supersedes) across new segments.
  for (int i = 0; i < kDocs; ++i) {
    ASSERT_TRUE(
        cat.value()
            ->Upsert("sim", MakeRow("d" + std::to_string(i), MakeVec(i + 1000),
                                    static_cast<Timestamp>(1000 + i)))
            .ok());
    if ((i + 1) % 40 == 0) {
      ASSERT_TRUE(cat.value()->Flush("sim").ok());
    }
  }
  ASSERT_TRUE(cat.value()->Flush("sim").ok());
  const uint64_t after_update = DirBytes(col_dir);
  EXPECT_GT(after_update, after_insert);  // old + new versions on disk

  ASSERT_TRUE(cat.value()->Compact("sim").ok());
  const uint64_t after_compact = DirBytes(col_dir);
  EXPECT_LT(after_compact, after_update);
  EXPECT_LE(CountFilesWithPrefix(col_dir, "seg_"), 1);
  // One live copy of each vector should fit well under 2× the post-insert size.
  EXPECT_LT(after_compact, after_insert * 2);

  // Phase 3: delete everything, compact to empty (no leftover SSTables).
  for (int i = 0; i < kDocs; ++i) {
    ASSERT_TRUE(cat.value()
                    ->Delete("sim", "d" + std::to_string(i),
                             static_cast<Timestamp>(5000 + i))
                    .ok());
  }
  ASSERT_TRUE(cat.value()->Flush("sim").ok());
  ASSERT_TRUE(cat.value()->Compact("sim").ok());
  EXPECT_EQ(CountFilesWithPrefix(col_dir, "seg_"), 0);
  const uint64_t after_delete = DirBytes(col_dir);
  EXPECT_LT(after_delete, after_compact);
  EXPECT_LT(after_delete, 4096u);  // WAL+MANIFEST only, roughly

  // Phase 4: drop must remove the collection directory entirely.
  ASSERT_TRUE(cat.value()->DropCollection("sim").ok());
  struct stat st {};
  EXPECT_NE(::stat(col_dir.c_str(), &st), 0);
  EXPECT_EQ(CountFilesWithPrefix(root, "seg_"), 0);
}

TEST(Integration, OpenRemovesOrphansAfterSimulatedCrashFlush) {
  const std::string dir = ::testing::TempDir() + "/aster_integ_orphan_crash";
  Db::Options options;
  options.dimension = 8;
  options.metric = Metric::kL2;
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  options.max_segments_before_compact = 0;

  {
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok());
    for (int i = 0; i < 20; ++i) {
      std::vector<float> v(8, static_cast<float>(i));
      ASSERT_TRUE(db.value()
                      ->Upsert(MakeRow("x" + std::to_string(i), v,
                                       static_cast<Timestamp>(i + 1)))
                      .ok());
    }
    ASSERT_TRUE(db.value()->Flush().ok());
  }

  // Orphan from a "crashed" compaction write that never made the manifest.
  std::vector<Row> junk = {MakeRow("orphan", std::vector<float>(8, 9.0f), 99)};
  ASSERT_TRUE(WriteSstable(dir + "/seg_000777.ast", 777, Metric::kL2, junk).ok());
  EXPECT_EQ(CountFilesWithPrefix(dir, "seg_"), 2);

  auto reopened = Db::Open(options);
  ASSERT_TRUE(reopened.ok());
  EXPECT_EQ(reopened.value()->segment_count(), 1u);
  EXPECT_EQ(CountFilesWithPrefix(dir, "seg_"), 1);
  EXPECT_TRUE(reopened.value()->Get("x0").has_value());
  EXPECT_FALSE(reopened.value()->Get("orphan").has_value());
}

}  // namespace
}  // namespace aster
