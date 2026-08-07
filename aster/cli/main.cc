// Aster demo binary: exercises the single-node engine end to end.
// The real server (Thrift RPC, gossip, config file) is milestone M4.
//
//   bazel run //aster/cli:aster

#include <cstdio>
#include <random>
#include <string>

#include "aster/db/db.h"
#include "aster/distributed/ring.h"

namespace {
constexpr const char* kVersion = "0.1.0-dev";
constexpr uint32_t kDim = 16;
constexpr int kRows = 1000;
}  // namespace

int main() {
  std::printf("aster %s (single-node demo)\n\n", kVersion);

  aster::Db::Options options;
  options.dimension = kDim;
  options.metric = aster::Metric::kCosine;
  aster::Db db(options);

  std::mt19937 rng(42);
  std::normal_distribution<float> dist(0.0f, 1.0f);

  for (int i = 0; i < kRows; ++i) {
    aster::Row row;
    row.id = "doc-" + std::to_string(i);
    row.vector.resize(kDim);
    for (auto& x : row.vector) x = dist(rng);
    row.timestamp = static_cast<aster::Timestamp>(i + 1);
    row.tags.insert(i % 2 == 0 ? "even" : "odd");
    if (!db.Upsert(std::move(row)).ok()) return 1;
    if ((i + 1) % 250 == 0) {
      if (!db.Flush().ok()) return 1;  // spread rows across segments
    }
  }
  std::printf("inserted %d rows across %zu segments (+ memtable rows: %zu)\n",
              kRows, db.segment_count(), db.memtable_rows());

  aster::SearchRequest req;
  req.vector.resize(kDim);
  for (auto& x : req.vector) x = dist(rng);
  req.top_k = 5;
  req.tags = {"even"};

  auto hits = db.Search(req);
  std::printf("top-%u results (tag filter: even):\n", req.top_k);
  for (const auto& hit : hits) {
    std::printf("  %-10s score=%.4f\n", hit.id.c_str(), hit.score);
  }

  if (!db.Compact().ok()) return 1;
  std::printf("\nafter full compaction: %zu segment(s)\n", db.segment_count());

  // Ring placement preview: where would these rows live in a cluster?
  aster::Ring ring(64);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");
  auto replicas = ring.GetReplicas(hits.empty() ? "doc-0" : hits[0].id, 2);
  std::printf("ring placement (RF=2): %s -> [%s, %s]\n",
              hits.empty() ? "doc-0" : hits[0].id.c_str(),
              replicas[0].c_str(), replicas[1].c_str());
  return 0;
}
