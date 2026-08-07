#include <gtest/gtest.h>

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

}  // namespace
}  // namespace aster
