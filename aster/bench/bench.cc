// Aster soak / throughput bench: continuous write, update, and search.
//
//   bazel run //aster/bench:aster-bench -- --help
//
// Emits a JSON metrics line on stdout every --report-every seconds and a
// final summary object when finished (or on SIGTERM).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "aster/db/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  std::string data_dir;
  std::string node_id = "0";
  uint32_t dimension = 256;
  uint64_t vectors_per_node = 100000;  // scaled by orchestrator
  uint64_t id_base = 0;               // global id offset for this shard
  int duration_sec = 120;
  int report_every_sec = 5;
  uint32_t top_k = 10;
  int flush_every = 500;
  double write_weight = 0.50;
  double update_weight = 0.20;
  double search_weight = 0.30;
  size_t memtable_flush_bytes = 8 << 20;
  bool durable = true;
};

struct Stats {
  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> updates{0};
  std::atomic<uint64_t> searches{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> write_ns{0};
  std::atomic<uint64_t> update_ns{0};
  std::atomic<uint64_t> search_ns{0};
  std::atomic<uint64_t> search_hits{0};
};

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "\n"
               "  --data-dir PATH          Durable directory (required unless --memory)\n"
               "  --memory                 In-memory only\n"
               "  --node-id ID             Shard label (default 0)\n"
               "  --id-base N              Starting vector id (default 0)\n"
               "  --vectors N              Working-set size for this node\n"
               "  --dimension D            Vector dimension (default 256; try 256/2048/4096)\n"
               "  --duration SEC           Run length (default 120)\n"
               "  --report-every SEC       Metrics cadence (default 5)\n"
               "  --top-k K                Search top-k (default 10)\n"
               "  --flush-every N          Upserts between Flush() (default 500)\n"
               "  --write-weight W         Mix weight (default 0.50)\n"
               "  --update-weight W        Mix weight (default 0.20)\n"
               "  --search-weight W        Mix weight (default 0.30)\n"
               "  --help\n",
               argv0);
}

Config ParseArgs(int argc, char** argv) {
  Config c;
  bool memory = false;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      std::exit(0);
    }
    if (std::strcmp(argv[i], "--data-dir") == 0) {
      c.data_dir = need("--data-dir");
    } else if (std::strcmp(argv[i], "--memory") == 0) {
      memory = true;
    } else if (std::strcmp(argv[i], "--node-id") == 0) {
      c.node_id = need("--node-id");
    } else if (std::strcmp(argv[i], "--id-base") == 0) {
      c.id_base = std::strtoull(need("--id-base"), nullptr, 10);
    } else if (std::strcmp(argv[i], "--vectors") == 0) {
      c.vectors_per_node = std::strtoull(need("--vectors"), nullptr, 10);
    } else if (std::strcmp(argv[i], "--dimension") == 0) {
      c.dimension = static_cast<uint32_t>(std::strtoul(need("--dimension"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--duration") == 0) {
      c.duration_sec = std::atoi(need("--duration"));
    } else if (std::strcmp(argv[i], "--report-every") == 0) {
      c.report_every_sec = std::atoi(need("--report-every"));
    } else if (std::strcmp(argv[i], "--top-k") == 0) {
      c.top_k = static_cast<uint32_t>(std::strtoul(need("--top-k"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--flush-every") == 0) {
      c.flush_every = std::atoi(need("--flush-every"));
    } else if (std::strcmp(argv[i], "--write-weight") == 0) {
      c.write_weight = std::atof(need("--write-weight"));
    } else if (std::strcmp(argv[i], "--update-weight") == 0) {
      c.update_weight = std::atof(need("--update-weight"));
    } else if (std::strcmp(argv[i], "--search-weight") == 0) {
      c.search_weight = std::atof(need("--search-weight"));
    } else {
      std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
      PrintUsage(argv[0]);
      std::exit(2);
    }
  }
  if (const char* env = std::getenv("ASTER_DATA_DIR"); env && c.data_dir.empty()) {
    c.data_dir = env;
  }
  if (memory) c.data_dir.clear();
  c.durable = !c.data_dir.empty();
  if (c.vectors_per_node == 0) c.vectors_per_node = 1;
  if (c.dimension == 0) c.dimension = 256;
  if (c.dimension > 8192) {
    std::fprintf(stderr, "error: --dimension must be <= 8192\n");
    std::exit(2);
  }
  if (c.duration_sec <= 0) c.duration_sec = 60;
  return c;
}

void FillVector(std::mt19937& rng, std::vector<float>& v) {
  std::normal_distribution<float> dist(0.0f, 1.0f);
  for (auto& x : v) x = dist(rng);
}

std::string MakeId(uint64_t id_base, uint64_t local) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "v-%llu",
                static_cast<unsigned long long>(id_base + local));
  return buf;
}

void EmitReport(const Config& cfg, const Stats& s, double elapsed_sec,
                const char* phase, size_t segments, size_t mem_rows) {
  const double w = static_cast<double>(s.writes.load());
  const double u = static_cast<double>(s.updates.load());
  const double q = static_cast<double>(s.searches.load());
  auto avg_ms = [](uint64_t total_ns, uint64_t n) -> double {
    if (n == 0) return 0.0;
    return (static_cast<double>(total_ns) / static_cast<double>(n)) / 1e6;
  };
  std::printf(
      "{\"phase\":\"%s\",\"node\":\"%s\",\"elapsed_sec\":%.3f,"
      "\"writes\":%llu,\"updates\":%llu,\"searches\":%llu,\"errors\":%llu,"
      "\"write_ops_sec\":%.2f,\"update_ops_sec\":%.2f,\"search_ops_sec\":%.2f,"
      "\"write_avg_ms\":%.4f,\"update_avg_ms\":%.4f,\"search_avg_ms\":%.4f,"
      "\"search_hits\":%llu,\"segments\":%zu,\"memtable_rows\":%zu,"
      "\"vectors_per_node\":%llu,\"dimension\":%u,\"durable\":%s}\n",
      phase, cfg.node_id.c_str(), elapsed_sec,
      static_cast<unsigned long long>(s.writes.load()),
      static_cast<unsigned long long>(s.updates.load()),
      static_cast<unsigned long long>(s.searches.load()),
      static_cast<unsigned long long>(s.errors.load()),
      elapsed_sec > 0 ? w / elapsed_sec : 0.0,
      elapsed_sec > 0 ? u / elapsed_sec : 0.0,
      elapsed_sec > 0 ? q / elapsed_sec : 0.0,
      avg_ms(s.write_ns.load(), s.writes.load()),
      avg_ms(s.update_ns.load(), s.updates.load()),
      avg_ms(s.search_ns.load(), s.searches.load()),
      static_cast<unsigned long long>(s.search_hits.load()), segments, mem_rows,
      static_cast<unsigned long long>(cfg.vectors_per_node), cfg.dimension,
      cfg.durable ? "true" : "false");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const Config cfg = ParseArgs(argc, argv);

  aster::Db::Options options;
  options.dimension = cfg.dimension;
  options.metric = aster::Metric::kCosine;
  options.data_dir = cfg.data_dir;
  options.memtable_flush_bytes = cfg.memtable_flush_bytes;
  options.wal_sync = aster::SyncPolicy::kEveryMs;  // group commit for throughput

  std::unique_ptr<aster::Db> owned;
  aster::Db* db = nullptr;
  if (cfg.durable) {
    auto opened = aster::Db::Open(options);
    if (!opened.ok()) {
      std::fprintf(stderr, "error: open failed: %s\n",
                   opened.status().message().c_str());
      return 1;
    }
    owned = std::move(opened.value());
    db = owned.get();
  } else {
    owned = std::make_unique<aster::Db>(options);
    db = owned.get();
  }

  Stats stats;
  std::mt19937 rng(static_cast<uint32_t>(cfg.id_base + 42));
  std::vector<float> vec(cfg.dimension);
  std::uniform_real_distribution<double> pick(0.0, 1.0);
  std::uniform_int_distribution<uint64_t> id_dist(0, cfg.vectors_per_node - 1);

  const double w_sum =
      cfg.write_weight + cfg.update_weight + cfg.search_weight;
  const double w_write = cfg.write_weight / w_sum;
  const double w_update = cfg.update_weight / w_sum;

  // Bootstrap: insert a fraction so search has something to hit quickly.
  const uint64_t bootstrap =
      std::min<uint64_t>(cfg.vectors_per_node, 2000);
  for (uint64_t i = 0; i < bootstrap; ++i) {
    FillVector(rng, vec);
    aster::Row row;
    row.id = MakeId(cfg.id_base, i);
    row.vector = vec;
    row.timestamp = i + 1;
    if (!db->Upsert(std::move(row)).ok()) ++stats.errors;
    if ((i + 1) % static_cast<uint64_t>(cfg.flush_every) == 0) {
      if (!db->Flush().ok()) ++stats.errors;
    }
  }
  if (!db->Flush().ok()) ++stats.errors;

  const auto t0 = Clock::now();
  auto last_report = t0;
  uint64_t since_flush = 0;
  uint64_t next_new = bootstrap;
  aster::Timestamp ts = bootstrap + 1;

  std::printf(
      "{\"phase\":\"start\",\"node\":\"%s\",\"vectors_per_node\":%llu,"
      "\"dimension\":%u,\"duration_sec\":%d,\"durable\":%s,\"data_dir\":\"%s\"}\n",
      cfg.node_id.c_str(),
      static_cast<unsigned long long>(cfg.vectors_per_node), cfg.dimension,
      cfg.duration_sec, cfg.durable ? "true" : "false", cfg.data_dir.c_str());
  std::fflush(stdout);

  while (true) {
    const auto now = Clock::now();
    const double elapsed =
        std::chrono::duration<double>(now - t0).count();
    if (elapsed >= cfg.duration_sec) break;

    if (std::chrono::duration<double>(now - last_report).count() >=
        cfg.report_every_sec) {
      EmitReport(cfg, stats, elapsed, "progress", db->segment_count(),
                 db->memtable_rows());
      last_report = now;
    }

    const double r = pick(rng);
    if (r < w_write) {
      const uint64_t local =
          next_new < cfg.vectors_per_node ? next_new++ : id_dist(rng);
      FillVector(rng, vec);
      aster::Row row;
      row.id = MakeId(cfg.id_base, local);
      row.vector = vec;
      row.timestamp = ++ts;
      const auto a = Clock::now();
      const bool ok = db->Upsert(std::move(row)).ok();
      const auto b = Clock::now();
      stats.write_ns += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      if (ok) {
        ++stats.writes;
        ++since_flush;
      } else {
        ++stats.errors;
      }
    } else if (r < w_write + w_update) {
      const uint64_t local = id_dist(rng);
      FillVector(rng, vec);
      aster::Row row;
      row.id = MakeId(cfg.id_base, local);
      row.vector = vec;
      row.timestamp = ++ts;
      const auto a = Clock::now();
      const bool ok = db->Upsert(std::move(row)).ok();
      const auto b = Clock::now();
      stats.update_ns += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      if (ok) {
        ++stats.updates;
        ++since_flush;
      } else {
        ++stats.errors;
      }
    } else {
      FillVector(rng, vec);
      aster::SearchRequest req;
      req.vector = vec;
      req.top_k = cfg.top_k;
      const auto a = Clock::now();
      auto hits = db->Search(req);
      const auto b = Clock::now();
      stats.search_ns += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      ++stats.searches;
      stats.search_hits += hits.size();
    }

    if (since_flush >= static_cast<uint64_t>(cfg.flush_every)) {
      if (!db->Flush().ok()) ++stats.errors;
      since_flush = 0;
    }
  }

  if (!db->Flush().ok()) ++stats.errors;
  // Occasional compact keeps segment count bounded on long soaks.
  if (db->segment_count() > 8) {
    if (!db->Compact().ok()) ++stats.errors;
  }

  const double total =
      std::chrono::duration<double>(Clock::now() - t0).count();
  EmitReport(cfg, stats, total, "final", db->segment_count(),
             db->memtable_rows());
  return stats.errors.load() == 0 ? 0 : 1;
}
