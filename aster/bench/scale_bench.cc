// Elastic membership bench: continuously scale a virtual cluster between
// --min-nodes and --max-nodes (default 15..50) with RF replication.
//
// Guarantees under test:
//   * no data loss — every acked key remains Get-able on ≥1 live replica
//   * accuracy stable — scatter-gather top-k matches offline ground truth
//     after every scale event
//
// Backends:
//   --backend local   per-node durable directories under --data-dir
//   --backend memory  in-memory Dbs (fast correctness)
//
//   bazel run //aster/bench:scale-bench -- --profile smoke
//   bazel run //aster/bench:scale-bench -- --min-nodes 15 --max-nodes 50

#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aster/db/db.h"
#include "aster/distributed/rebalance.h"
#include "aster/distributed/ring.h"

namespace {

using Clock = std::chrono::steady_clock;
using aster::Db;
using aster::Migration;
using aster::NodeId;
using aster::RebalancePlan;
using aster::Ring;
using aster::Row;
using aster::RowId;
using aster::SearchHit;
using aster::SearchRequest;

struct Config {
  std::string data_dir = "/tmp/aster-scale-bench";
  std::string object_dir = "/tmp/aster-scale-objects";  // MinIO stand-in
  std::string backend = "local";  // local | memory | minio
  std::string out_json;
  int min_nodes = 15;
  int max_nodes = 50;
  int start_nodes = 15;
  int scale_events = 40;
  uint32_t rf = 2;
  uint32_t dimension = 64;
  uint64_t initial_rows = 2000;
  uint64_t writes_per_event = 50;
  int queries_per_event = 16;
  uint32_t top_k = 10;
  uint32_t vnodes = 64;
  std::string profile;  // smoke overrides sizes
};

struct NodeStore {
  NodeId id;
  std::unique_ptr<Db> db;
  std::string dir;
};

struct GroundTruth {
  std::unordered_map<RowId, std::vector<float>> vectors;
};

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na <= 0 || nb <= 0) return 0;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<float> UnitVec(uint32_t dim, std::mt19937& rng) {
  std::normal_distribution<float> dist(0.f, 1.f);
  std::vector<float> v(dim);
  double n2 = 0;
  for (float& x : v) {
    x = dist(rng);
    n2 += static_cast<double>(x) * x;
  }
  const float inv = n2 > 0 ? static_cast<float>(1.0 / std::sqrt(n2)) : 1.f;
  for (float& x : v) x *= inv;
  return v;
}

std::vector<SearchHit> ExactTopK(const GroundTruth& gt,
                                 const std::vector<float>& query, uint32_t k) {
  std::vector<std::pair<float, RowId>> scored;
  scored.reserve(gt.vectors.size());
  for (const auto& [id, vec] : gt.vectors) {
    scored.emplace_back(static_cast<float>(Cosine(query, vec)), id);
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) {
              if (a.first != b.first) return a.first > b.first;
              return a.second < b.second;
            });
  if (scored.size() > k) scored.resize(k);
  std::vector<SearchHit> hits;
  for (const auto& [score, id] : scored) hits.push_back({id, score});
  return hits;
}

bool SameRanking(const std::vector<SearchHit>& a,
                 const std::vector<SearchHit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id) return false;
  }
  return true;
}

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [--profile smoke|full] [options]\n"
               "  --min-nodes N --max-nodes N --start-nodes N\n"
               "  --scale-events N --rf N --dimension N --rows N\n"
               "  --backend local|memory|minio --data-dir PATH\n"
               "  --object-dir PATH   (minio stand-in bucket root)\n"
               "  --out-json PATH\n",
               argv0);
}

bool Parse(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* f) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", f);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (!std::strcmp(argv[i], "--profile")) {
      cfg->profile = need("--profile");
    } else if (!std::strcmp(argv[i], "--min-nodes")) {
      cfg->min_nodes = std::atoi(need("--min-nodes"));
    } else if (!std::strcmp(argv[i], "--max-nodes")) {
      cfg->max_nodes = std::atoi(need("--max-nodes"));
    } else if (!std::strcmp(argv[i], "--start-nodes")) {
      cfg->start_nodes = std::atoi(need("--start-nodes"));
    } else if (!std::strcmp(argv[i], "--scale-events")) {
      cfg->scale_events = std::atoi(need("--scale-events"));
    } else if (!std::strcmp(argv[i], "--rf")) {
      cfg->rf = static_cast<uint32_t>(std::atoi(need("--rf")));
    } else if (!std::strcmp(argv[i], "--dimension")) {
      cfg->dimension = static_cast<uint32_t>(std::atoi(need("--dimension")));
    } else if (!std::strcmp(argv[i], "--rows")) {
      cfg->initial_rows = static_cast<uint64_t>(std::atoll(need("--rows")));
    } else if (!std::strcmp(argv[i], "--writes-per-event")) {
      cfg->writes_per_event =
          static_cast<uint64_t>(std::atoll(need("--writes-per-event")));
    } else if (!std::strcmp(argv[i], "--queries-per-event")) {
      cfg->queries_per_event = std::atoi(need("--queries-per-event"));
    } else if (!std::strcmp(argv[i], "--top-k")) {
      cfg->top_k = static_cast<uint32_t>(std::atoi(need("--top-k")));
    } else if (!std::strcmp(argv[i], "--backend")) {
      cfg->backend = need("--backend");
    } else if (!std::strcmp(argv[i], "--data-dir")) {
      cfg->data_dir = need("--data-dir");
    } else if (!std::strcmp(argv[i], "--object-dir")) {
      cfg->object_dir = need("--object-dir");
    } else if (!std::strcmp(argv[i], "--out-json")) {
      cfg->out_json = need("--out-json");
    } else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      return false;
    }
  }
  if (cfg->profile == "smoke") {
    cfg->min_nodes = 3;
    cfg->max_nodes = 8;
    cfg->start_nodes = 3;
    cfg->scale_events = 12;
    cfg->initial_rows = 400;
    cfg->writes_per_event = 20;
    cfg->dimension = 32;
    cfg->queries_per_event = 8;
  } else if (cfg->profile == "full" || cfg->profile.empty()) {
    // keep defaults / CLI overrides; "full" = 15..50
    if (cfg->profile == "full") {
      cfg->min_nodes = 15;
      cfg->max_nodes = 50;
      cfg->start_nodes = 15;
    }
  } else {
    std::fprintf(stderr, "unknown profile %s\n", cfg->profile.c_str());
    return false;
  }
  if (cfg->min_nodes < 1 || cfg->max_nodes < cfg->min_nodes) return false;
  if (cfg->start_nodes < cfg->min_nodes || cfg->start_nodes > cfg->max_nodes)
    cfg->start_nodes = cfg->min_nodes;
  if (cfg->rf < 1) cfg->rf = 1;
  if (cfg->backend != "local" && cfg->backend != "memory" &&
      cfg->backend != "minio") {
    std::fprintf(stderr, "unknown backend %s\n", cfg->backend.c_str());
    return false;
  }
  return true;
}

bool MkDir(const std::string& path) {
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// Mirror node durable files into the object-store stand-in (MinIO bucket).
bool MirrorNodeToObject(const Config& cfg, const NodeId& id,
                        const std::string& local_dir) {
  if (cfg.backend != "minio") return true;
  const std::string dst = cfg.object_dir + "/nodes/" + id;
  MkDir(cfg.object_dir);
  MkDir(cfg.object_dir + "/nodes");
  MkDir(dst);
  // Replace prior object snapshot atomically-ish: wipe then copy.
  const std::string wipe = "rm -rf '" + dst + "'/*";
  const std::string copy =
      "cp -a '" + local_dir + "/.' '" + dst + "/' 2>/dev/null";
  if (std::system(wipe.c_str()) != 0) { /* empty ok */
  }
  return std::system(copy.c_str()) == 0;
}

bool RestoreNodeFromObject(const Config& cfg, const NodeId& id,
                           const std::string& local_dir) {
  if (cfg.backend != "minio") return true;
  const std::string src = cfg.object_dir + "/nodes/" + id;
  struct stat st {};
  if (::stat(src.c_str(), &st) != 0) return true;  // nothing to restore
  MkDir(local_dir);
  const std::string copy = "cp -a '" + src + "/.' '" + local_dir + "/'";
  return std::system(copy.c_str()) == 0;
}

NodeId NodeName(int i) { return "n" + std::to_string(i); }

std::unique_ptr<Db> OpenNode(const Config& cfg, const NodeId& id) {
  Db::Options opt;
  opt.dimension = cfg.dimension;
  opt.metric = aster::Metric::kCosine;
  opt.wal_sync = aster::SyncPolicy::kEveryMs;
  opt.memtable_flush_bytes = 4 << 20;
  if (cfg.backend == "memory") {
    return std::make_unique<Db>(opt);
  }
  opt.data_dir = cfg.data_dir + "/" + id;
  MkDir(cfg.data_dir);
  MkDir(opt.data_dir);
  if (cfg.backend == "minio") {
    if (!RestoreNodeFromObject(cfg, id, opt.data_dir)) {
      std::fprintf(stderr, "restore from object store failed for %s\n",
                   id.c_str());
      return nullptr;
    }
  }
  auto opened = Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "open %s failed: %s\n", id.c_str(),
                 opened.status().message().c_str());
    return nullptr;
  }
  return std::move(opened.value());
}

NodeStore* Find(std::vector<NodeStore>& nodes, const NodeId& id) {
  for (auto& n : nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

bool UpsertRf(std::vector<NodeStore>& nodes, Ring& ring, uint32_t rf, Row row) {
  auto reps = ring.GetReplicas(row.id, rf);
  if (reps.empty()) return false;
  for (const auto& r : reps) {
    auto* n = Find(nodes, r);
    if (!n || !n->db) return false;
    Row copy = row;
    if (!n->db->Upsert(std::move(copy)).ok()) return false;
  }
  return true;
}

bool GetAny(std::vector<NodeStore>& nodes, Ring& ring, uint32_t rf,
            const RowId& id, Row* out) {
  for (const auto& r : ring.GetReplicas(id, rf)) {
    auto* n = Find(nodes, r);
    if (!n || !n->db) continue;
    auto got = n->db->Get(id);
    if (got.has_value()) {
      *out = *got;
      return true;
    }
  }
  return false;
}

std::vector<SearchHit> ScatterGather(std::vector<NodeStore>& nodes,
                                     const std::vector<float>& query,
                                     uint32_t top_k) {
  std::unordered_map<RowId, float> best;
  SearchRequest req;
  req.vector = query;
  req.top_k = top_k;
  for (auto& n : nodes) {
    if (!n.db) continue;
    auto hits = n.db->Search(req);
    for (const auto& h : hits) {
      auto it = best.find(h.id);
      if (it == best.end() || h.score > it->second) best[h.id] = h.score;
    }
  }
  std::vector<std::pair<float, RowId>> scored;
  for (const auto& [id, score] : best) scored.emplace_back(score, id);
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second < b.second;
  });
  if (scored.size() > top_k) scored.resize(top_k);
  std::vector<SearchHit> out;
  for (const auto& [score, id] : scored) out.push_back({id, score});
  return out;
}

bool ApplyMigrations(std::vector<NodeStore>& nodes, const RebalancePlan& plan) {
  for (const auto& m : plan.migrations) {
    auto* src = Find(nodes, m.from);
    auto* dst = Find(nodes, m.to);
    if (!dst || !dst->db) return false;
    if (!src || !src->db) return false;
    auto got = src->db->Get(m.key);
    if (!got.has_value()) return false;
    Row row = *got;
    row.timestamp += 1;  // LWW bump for repair
    if (!dst->db->Upsert(std::move(row)).ok()) return false;
  }
  return true;
}

bool DrainAll(std::vector<NodeStore>& nodes, const Config& cfg) {
  for (auto& n : nodes) {
    if (!n.db) continue;
    if (!n.db->Flush().ok()) return false;
    if (!MirrorNodeToObject(cfg, n.id, n.dir.empty() ? n.db->data_dir() : n.dir))
      return false;
  }
  return true;
}

bool VerifyNoLoss(std::vector<NodeStore>& nodes, Ring& ring, uint32_t rf,
                  const GroundTruth& gt, size_t* missing) {
  *missing = 0;
  for (const auto& [id, _] : gt.vectors) {
    Row row;
    if (!GetAny(nodes, ring, rf, id, &row)) {
      ++(*missing);
    }
  }
  return *missing == 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!Parse(argc, argv, &cfg)) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::printf(
      "{\"phase\":\"start\",\"min\":%d,\"max\":%d,\"start\":%d,\"rf\":%u,"
      "\"backend\":\"%s\",\"rows\":%llu,\"events\":%d,\"dim\":%u}\n",
      cfg.min_nodes, cfg.max_nodes, cfg.start_nodes, cfg.rf, cfg.backend.c_str(),
      static_cast<unsigned long long>(cfg.initial_rows), cfg.scale_events,
      cfg.dimension);
  std::fflush(stdout);

  ::mkdir(cfg.data_dir.c_str(), 0755);

  Ring ring(cfg.vnodes);
  std::vector<NodeStore> nodes;
  int next_id = 0;

  auto add_node = [&]() -> bool {
    NodeStore ns;
    ns.id = NodeName(next_id++);
    ns.db = OpenNode(cfg, ns.id);
    if (!ns.db) return false;
    ns.dir = cfg.data_dir + "/" + ns.id;
    nodes.push_back(std::move(ns));
    ring.AddNode(nodes.back().id);
    return true;
  };

  for (int i = 0; i < cfg.start_nodes; ++i) {
    if (!add_node()) return 1;
  }

  std::mt19937 rng(42);
  GroundTruth gt;
  uint64_t seq = 0;

  auto write_one = [&](const RowId& id) -> bool {
    Row row;
    row.id = id;
    row.vector = UnitVec(cfg.dimension, rng);
    row.timestamp = static_cast<aster::Timestamp>(++seq);
    gt.vectors[id] = row.vector;
    return UpsertRf(nodes, ring, cfg.rf, std::move(row));
  };

  for (uint64_t i = 0; i < cfg.initial_rows; ++i) {
    if (!write_one("doc-" + std::to_string(i))) {
      std::fprintf(stderr, "initial write failed\n");
      return 1;
    }
  }
  if (!DrainAll(nodes, cfg)) return 1;

  size_t missing = 0;
  if (!VerifyNoLoss(nodes, ring, cfg.rf, gt, &missing)) {
    std::fprintf(stderr, "data loss after initial load: missing=%zu\n", missing);
    return 1;
  }

  // Fixed query set for stability checks.
  std::vector<std::vector<float>> queries;
  for (int i = 0; i < cfg.queries_per_event; ++i) {
    queries.push_back(UnitVec(cfg.dimension, rng));
  }
  std::vector<std::vector<SearchHit>> baselines;
  for (const auto& q : queries) {
    baselines.push_back(ExactTopK(gt, q, cfg.top_k));
  }

  int accuracy_fails = 0;
  int loss_events = 0;
  int scale_ups = 0;
  int scale_downs = 0;
  std::vector<double> search_ms;
  std::vector<double> event_ms;
  bool climbing = true;

  const auto wall0 = Clock::now();

  for (int ev = 0; ev < cfg.scale_events; ++ev) {
    const auto t0 = Clock::now();
    const int n = static_cast<int>(nodes.size());
    if (n <= cfg.min_nodes) climbing = true;
    if (n >= cfg.max_nodes) climbing = false;
    const bool do_up = climbing;

    if (do_up) {
      // Capture keys before add.
      std::vector<RowId> keys;
      keys.reserve(gt.vectors.size());
      for (const auto& [id, _] : gt.vectors) keys.push_back(id);

      NodeStore ns;
      ns.id = NodeName(next_id++);
      ns.db = OpenNode(cfg, ns.id);
      if (!ns.db) return 1;
      ns.dir = cfg.data_dir + "/" + ns.id;
      // Plan against ring BEFORE add, then add, then migrate.
      auto plan = aster::PlanAddNode(ring, ns.id, cfg.rf, keys);
      nodes.push_back(std::move(ns));
      ring.AddNode(nodes.back().id);
      if (!ApplyMigrations(nodes, plan)) {
        std::fprintf(stderr, "scale-up migration failed at event %d\n", ev);
        return 1;
      }
      if (!nodes.back().db->Flush().ok()) return 1;
      if (!MirrorNodeToObject(cfg, nodes.back().id, nodes.back().dir)) return 1;
      ++scale_ups;
    } else {
      // Remove last node (deterministic). With RF>=2, survivors cover.
      const NodeId leaving = nodes.back().id;
      std::vector<RowId> keys;
      for (const auto& [id, _] : gt.vectors) keys.push_back(id);
      auto plan = aster::PlanRemoveNode(ring, leaving, cfg.rf, keys);
      if (cfg.rf >= 2 && !plan.durable_without_source) {
        std::fprintf(stderr,
                     "refusing scale-down: RF survivors do not cover all keys\n");
        return 1;
      }
      // Drain leaving to local + object store, migrate, then drop.
      auto* leaving_node = Find(nodes, leaving);
      if (!leaving_node || !leaving_node->db->Flush().ok()) return 1;
      if (!MirrorNodeToObject(cfg, leaving, leaving_node->dir)) return 1;
      if (!ApplyMigrations(nodes, plan)) {
        std::fprintf(stderr, "scale-down migration failed at event %d\n", ev);
        return 1;
      }
      // Flush survivors that received repairs.
      for (const auto& m : plan.migrations) {
        auto* dst = Find(nodes, m.to);
        if (dst && dst->db) {
          if (!dst->db->Flush().ok()) return 1;
          if (!MirrorNodeToObject(cfg, dst->id, dst->dir)) return 1;
        }
      }
      ring.RemoveNode(leaving);
      // Destroy local node storage (pod gone); object snapshot retained.
      if (cfg.backend != "memory") {
        const std::string wipe = "rm -rf '" + nodes.back().dir + "'";
        std::system(wipe.c_str());
      }
      nodes.pop_back();
      ++scale_downs;
    }

    // Interleaved writes during elasticity.
    for (uint64_t w = 0; w < cfg.writes_per_event; ++w) {
      const RowId id =
          "doc-" + std::to_string(cfg.initial_rows +
                                  static_cast<uint64_t>(ev) * cfg.writes_per_event +
                                  w);
      if (!write_one(id)) {
        std::fprintf(stderr, "write failed during event %d\n", ev);
        return 1;
      }
    }
    // Refresh baselines for new rows (accuracy = match exact GT, not stale).
    baselines.clear();
    for (const auto& q : queries) {
      baselines.push_back(ExactTopK(gt, q, cfg.top_k));
    }

    if (!VerifyNoLoss(nodes, ring, cfg.rf, gt, &missing)) {
      ++loss_events;
      std::fprintf(stderr,
                   "{\"phase\":\"DATA_LOSS\",\"event\":%d,\"missing\":%zu,"
                   "\"nodes\":%zu}\n",
                   ev, missing, nodes.size());
      return 1;
    }

    int q_fail = 0;
    for (size_t qi = 0; qi < queries.size(); ++qi) {
      const auto s0 = Clock::now();
      auto got = ScatterGather(nodes, queries[qi], cfg.top_k);
      search_ms.push_back(
          std::chrono::duration<double, std::milli>(Clock::now() - s0).count());
      if (!SameRanking(got, baselines[qi])) ++q_fail;
    }
    if (q_fail) {
      accuracy_fails += q_fail;
      std::fprintf(stderr,
                   "{\"phase\":\"ACCURACY_FAIL\",\"event\":%d,\"failed_queries\":%d,"
                   "\"nodes\":%zu}\n",
                   ev, q_fail, nodes.size());
      return 1;
    }

    const double ems =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    event_ms.push_back(ems);
    std::printf(
        "{\"phase\":\"event\",\"i\":%d,\"nodes\":%zu,\"op\":\"%s\","
        "\"ms\":%.2f,\"keys\":%zu}\n",
        ev, nodes.size(), do_up ? "up" : "down", ems, gt.vectors.size());
    std::fflush(stdout);
  }

  const double wall_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - wall0).count();

  auto pct = [](std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double k = (v.size() - 1) * (p / 100.0);
    const size_t f = static_cast<size_t>(std::floor(k));
    const size_t c = std::min(f + 1, v.size() - 1);
    if (f == c) return v[f];
    return v[f] * (c - k) + v[c] * (k - f);
  };

  double search_sum = 0;
  for (double x : search_ms) search_sum += x;

  std::printf("\n======== Scale bench summary ========\n");
  std::printf("nodes final=%zu  events=%d  up=%d down=%d  keys=%zu\n",
              nodes.size(), cfg.scale_events, scale_ups, scale_downs,
              gt.vectors.size());
  std::printf("data_loss_events=%d  accuracy_fails=%d\n", loss_events,
              accuracy_fails);
  std::printf("search_ms avg=%.3f p50=%.3f p95=%.3f p99=%.3f\n",
              search_ms.empty() ? 0.0 : search_sum / search_ms.size(),
              pct(search_ms, 50), pct(search_ms, 95), pct(search_ms, 99));
  std::printf("event_ms  p50=%.1f p95=%.1f wall_ms=%.1f\n", pct(event_ms, 50),
              pct(event_ms, 95), wall_ms);
  std::printf("backend=%s rf=%u range=%d..%d\n", cfg.backend.c_str(), cfg.rf,
              cfg.min_nodes, cfg.max_nodes);
  std::printf("=====================================\n");

  std::string report = "{";
  report += "\"min_nodes\":" + std::to_string(cfg.min_nodes) + ",";
  report += "\"max_nodes\":" + std::to_string(cfg.max_nodes) + ",";
  report += "\"rf\":" + std::to_string(cfg.rf) + ",";
  report += "\"backend\":\"" + cfg.backend + "\",";
  report += "\"scale_events\":" + std::to_string(cfg.scale_events) + ",";
  report += "\"scale_ups\":" + std::to_string(scale_ups) + ",";
  report += "\"scale_downs\":" + std::to_string(scale_downs) + ",";
  report += "\"keys\":" + std::to_string(gt.vectors.size()) + ",";
  report += "\"data_loss_events\":" + std::to_string(loss_events) + ",";
  report += "\"accuracy_fails\":" + std::to_string(accuracy_fails) + ",";
  report += "\"search_avg_ms\":" +
            std::to_string(search_ms.empty() ? 0.0
                                             : search_sum / search_ms.size()) +
            ",";
  report += "\"search_p50_ms\":" + std::to_string(pct(search_ms, 50)) + ",";
  report += "\"search_p95_ms\":" + std::to_string(pct(search_ms, 95)) + ",";
  report += "\"search_p99_ms\":" + std::to_string(pct(search_ms, 99)) + ",";
  report += "\"event_p50_ms\":" + std::to_string(pct(event_ms, 50)) + ",";
  report += "\"wall_ms\":" + std::to_string(wall_ms) + ",";
  report += "\"ok\":" +
            std::string(loss_events == 0 && accuracy_fails == 0 ? "true"
                                                               : "false");
  report += "}\n";

  if (!cfg.out_json.empty()) {
    FILE* f = std::fopen(cfg.out_json.c_str(), "w");
    if (!f) return 1;
    std::fputs(report.c_str(), f);
    std::fclose(f);
    std::printf("wrote %s\n", cfg.out_json.c_str());
  } else {
    std::fputs(report.c_str(), stdout);
  }

  return (loss_events == 0 && accuracy_fails == 0) ? 0 : 1;
}
