// Multi-tenant Catalog bench: many tenants × indexes with mixed dims/row counts.
//
//   bazel run //aster/bench:multi-tenant-bench -- --help
//   bazel run //aster/bench:multi-tenant-bench -- --profile smoke --data-dir /tmp/mt
//
// Emits progress + a final JSON report on stdout (and optionally --out-json PATH).

#include <sys/resource.h>
#include <sys/stat.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aster/server/catalog.h"

namespace {

using Clock = std::chrono::steady_clock;

struct IndexSpec {
  uint32_t dimension = 0;
  uint64_t rows = 0;
};

struct Config {
  std::string data_dir;
  std::string profile = "default";
  std::string out_json;
  int tenants = 0;  // 0 → profile default
  int top_k = 10;
  int queries_per_index = 8;
  int concurrent_tenants = 0;  // 0 → min(tenants, hardware)
  bool durable = true;
  bool skip_compact = false;
};

struct IndexResult {
  std::string tenant;
  std::string collection;
  uint32_t dimension = 0;
  uint64_t rows = 0;
  double create_ms = 0;
  double upsert_ms = 0;
  double flush_ms = 0;
  double compact_ms = 0;
  double upsert_vps = 0;
  double search_avg_ms = 0;
  double search_p50_ms = 0;
  double search_p95_ms = 0;
  double search_p99_ms = 0;
  int queries = 0;
  int target_hits = 0;  // how often planted id is rank-0
  bool ok = false;
  std::string error;
};

struct TenantResult {
  std::string tenant;
  std::vector<IndexResult> indexes;
  double wall_ms = 0;
};

double Percentile(std::vector<double> s, double p) {
  if (s.empty()) return 0;
  std::sort(s.begin(), s.end());
  const double k = (s.size() - 1) * (p / 100.0);
  const size_t f = static_cast<size_t>(std::floor(k));
  const size_t c = static_cast<size_t>(std::ceil(k));
  if (f == c) return s[f];
  return s[f] * (c - k) + s[c] * (k - f);
}

std::vector<float> MakeUnitVector(uint32_t dim, std::mt19937& rng) {
  std::normal_distribution<float> dist(0.0f, 1.0f);
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

std::vector<IndexSpec> ProfileSpecs(const std::string& profile) {
  // Mixed vector sizes (dims) and corpus sizes (rows) per index.
  if (profile == "smoke") {
    return {
        {64, 64},
        {256, 128},
        {512, 48},
        {2048, 32},
    };
  }
  if (profile == "large") {
    return {
        {64, 50000},
        {128, 30000},
        {256, 20000},
        {384, 15000},
        {768, 8000},
        {1536, 4000},
        {2048, 2000},
        {4096, 500},
    };
  }
  // default
  return {
      {64, 5000},
      {128, 3000},
      {256, 8000},
      {384, 4000},
      {768, 2000},
      {1024, 1500},
      {1536, 1000},
      {2048, 500},
      {4096, 128},
  };
}

int ProfileTenants(const std::string& profile) {
  if (profile == "smoke") return 3;
  if (profile == "large") return 12;
  return 8;
}

void PrintUsage(const char* argv0) {
  std::fprintf(
      stderr,
      "Usage: %s --data-dir PATH [options]\n"
      "\n"
      "  --profile smoke|default|large   Index dim×rows matrix (default: default)\n"
      "  --tenants N                     Override tenant count\n"
      "  --concurrent N                  Parallel tenant workers (default: auto)\n"
      "  --queries N                     Searches per index (default: 8)\n"
      "  --top-k N                       Search top_k (default: 10)\n"
      "  --out-json PATH                 Write final report JSON\n"
      "  --no-durable                    Faster WAL (kNever)\n"
      "  --skip-compact                  Skip per-index compact\n"
      "\n"
      "Tenants map to Catalog collections: t{i}_d{dim}_n{rows}.\n",
      argv0);
}

bool ParseArgs(int argc, char** argv, Config* cfg) {
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
    } else if (std::strcmp(argv[i], "--data-dir") == 0) {
      cfg->data_dir = need("--data-dir");
    } else if (std::strcmp(argv[i], "--profile") == 0) {
      cfg->profile = need("--profile");
    } else if (std::strcmp(argv[i], "--tenants") == 0) {
      cfg->tenants = std::atoi(need("--tenants"));
    } else if (std::strcmp(argv[i], "--concurrent") == 0) {
      cfg->concurrent_tenants = std::atoi(need("--concurrent"));
    } else if (std::strcmp(argv[i], "--queries") == 0) {
      cfg->queries_per_index = std::atoi(need("--queries"));
    } else if (std::strcmp(argv[i], "--top-k") == 0) {
      cfg->top_k = std::atoi(need("--top-k"));
    } else if (std::strcmp(argv[i], "--out-json") == 0) {
      cfg->out_json = need("--out-json");
    } else if (std::strcmp(argv[i], "--no-durable") == 0) {
      cfg->durable = false;
    } else if (std::strcmp(argv[i], "--skip-compact") == 0) {
      cfg->skip_compact = true;
    } else {
      std::fprintf(stderr, "error: unknown arg %s\n", argv[i]);
      return false;
    }
  }
  if (cfg->data_dir.empty()) {
    std::fprintf(stderr, "error: --data-dir required\n");
    return false;
  }
  if (cfg->profile != "smoke" && cfg->profile != "default" &&
      cfg->profile != "large") {
    std::fprintf(stderr, "error: unknown profile %s\n", cfg->profile.c_str());
    return false;
  }
  return true;
}

IndexResult RunIndex(aster::Catalog* catalog, const std::string& tenant,
                     const IndexSpec& spec, const Config& cfg, uint32_t seed) {
  IndexResult r;
  r.tenant = tenant;
  r.dimension = spec.dimension;
  r.rows = spec.rows;
  r.collection = tenant + "_d" + std::to_string(spec.dimension) + "_n" +
                 std::to_string(spec.rows);
  r.queries = cfg.queries_per_index;

  std::mt19937 rng(seed);

  aster::CollectionInfo info;
  info.name = r.collection;
  info.dimension = spec.dimension;
  info.metric = aster::Metric::kCosine;

  auto t0 = Clock::now();
  auto st = catalog->CreateCollection(info);
  r.create_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (!st.ok()) {
    r.error = st.message();
    return r;
  }

  // Plant an exact-match target for accuracy probes.
  auto target = MakeUnitVector(spec.dimension, rng);
  {
    aster::Row row;
    row.id = "target";
    row.vector = target;
    row.timestamp = 1;
    row.tags.insert(tenant);
    if (!catalog->Upsert(r.collection, std::move(row)).ok()) {
      r.error = "target upsert failed";
      return r;
    }
  }

  t0 = Clock::now();
  for (uint64_t i = 0; i < spec.rows; ++i) {
    aster::Row row;
    row.id = "doc-" + std::to_string(i);
    row.vector = MakeUnitVector(spec.dimension, rng);
    row.timestamp = static_cast<aster::Timestamp>(i + 2);
    row.tags.insert(tenant);
    if (!catalog->Upsert(r.collection, std::move(row)).ok()) {
      r.error = "upsert failed at " + std::to_string(i);
      return r;
    }
  }
  r.upsert_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  r.upsert_vps =
      r.upsert_ms > 0 ? (1000.0 * static_cast<double>(spec.rows) / r.upsert_ms)
                      : 0.0;

  t0 = Clock::now();
  if (!catalog->Flush(r.collection).ok()) {
    r.error = "flush failed";
    return r;
  }
  r.flush_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

  if (!cfg.skip_compact) {
    t0 = Clock::now();
    if (!catalog->Compact(r.collection).ok()) {
      r.error = "compact failed";
      return r;
    }
    r.compact_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  }

  std::vector<double> search_ms;
  search_ms.reserve(static_cast<size_t>(cfg.queries_per_index));
  for (int q = 0; q < cfg.queries_per_index; ++q) {
    aster::SearchRequest req;
    // First query uses planted target; rest are random.
    req.vector = (q == 0) ? target : MakeUnitVector(spec.dimension, rng);
    req.top_k = static_cast<uint32_t>(cfg.top_k);
    t0 = Clock::now();
    auto hits = catalog->Search(r.collection, req);
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    search_ms.push_back(ms);
    if (!hits.ok()) {
      r.error = "search failed: " + hits.status().message();
      return r;
    }
    if (q == 0) {
      if (!hits.value().empty() && hits.value()[0].id == "target") {
        ++r.target_hits;
      }
    }
  }
  double sum = 0;
  for (double x : search_ms) sum += x;
  r.search_avg_ms = search_ms.empty() ? 0 : sum / search_ms.size();
  r.search_p50_ms = Percentile(search_ms, 50);
  r.search_p95_ms = Percentile(search_ms, 95);
  r.search_p99_ms = Percentile(search_ms, 99);
  r.ok = true;
  return r;
}

TenantResult RunTenant(aster::Catalog* catalog, int tenant_id,
                       const std::vector<IndexSpec>& specs, const Config& cfg) {
  TenantResult tr;
  tr.tenant = "t" + std::to_string(tenant_id);
  const auto wall0 = Clock::now();

  // Rotate which specs each tenant starts with so load is mixed across dims.
  for (size_t i = 0; i < specs.size(); ++i) {
    const size_t idx = (static_cast<size_t>(tenant_id) + i) % specs.size();
    const uint32_t seed = static_cast<uint32_t>(
        10007u * static_cast<uint32_t>(tenant_id) + 17u * static_cast<uint32_t>(i) +
        specs[idx].dimension);
    auto ir = RunIndex(catalog, tr.tenant, specs[idx], cfg, seed);
    std::printf(
        "{\"phase\":\"index_done\",\"tenant\":\"%s\",\"collection\":\"%s\","
        "\"dim\":%u,\"rows\":%llu,\"ok\":%s,\"upsert_vps\":%.1f,"
        "\"search_p50_ms\":%.3f}\n",
        ir.tenant.c_str(), ir.collection.c_str(), ir.dimension,
        static_cast<unsigned long long>(ir.rows), ir.ok ? "true" : "false",
        ir.upsert_vps, ir.search_p50_ms);
    std::fflush(stdout);
    tr.indexes.push_back(std::move(ir));
  }
  tr.wall_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - wall0).count();
  return tr;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o.push_back('\\');
      o.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      o += buf;
    } else {
      o.push_back(c);
    }
  }
  return o;
}

std::string BuildReportJson(const Config& cfg,
                            const std::vector<IndexSpec>& specs,
                            const std::vector<TenantResult>& tenants,
                            double wall_ms, size_t rss_kb) {
  uint64_t total_rows = 0;
  size_t ok_indexes = 0;
  size_t fail_indexes = 0;
  size_t target_ok = 0;
  size_t target_n = 0;
  for (const auto& t : tenants) {
    for (const auto& ix : t.indexes) {
      total_rows += ix.rows;
      if (ix.ok) {
        ++ok_indexes;
        target_ok += static_cast<size_t>(ix.target_hits);
        ++target_n;
      } else {
        ++fail_indexes;
      }
    }
  }

  std::string out;
  out += "{\n";
  out += "  \"profile\": \"" + JsonEscape(cfg.profile) + "\",\n";
  out += "  \"tenants\": " + std::to_string(tenants.size()) + ",\n";
  out += "  \"indexes_per_tenant\": " + std::to_string(specs.size()) + ",\n";
  out += "  \"total_collections\": " +
         std::to_string(tenants.size() * specs.size()) + ",\n";
  out += "  \"total_rows\": " + std::to_string(total_rows) + ",\n";
  out += "  \"ok_indexes\": " + std::to_string(ok_indexes) + ",\n";
  out += "  \"failed_indexes\": " + std::to_string(fail_indexes) + ",\n";
  out += "  \"target_recall_rate\": " +
         std::to_string(target_n ? static_cast<double>(target_ok) / target_n
                                 : 0.0) +
         ",\n";
  out += "  \"wall_ms\": " + std::to_string(wall_ms) + ",\n";
  out += "  \"rss_kb\": " + std::to_string(rss_kb) + ",\n";
  out += "  \"matrix\": [";
  for (size_t i = 0; i < specs.size(); ++i) {
    if (i) out += ", ";
    out += "{\"dimension\":" + std::to_string(specs[i].dimension) +
           ",\"rows\":" + std::to_string(specs[i].rows) + "}";
  }
  out += "],\n  \"tenant_results\": [\n";
  for (size_t ti = 0; ti < tenants.size(); ++ti) {
    const auto& t = tenants[ti];
    if (ti) out += ",\n";
    out += "    {\"tenant\":\"" + JsonEscape(t.tenant) +
           "\",\"wall_ms\":" + std::to_string(t.wall_ms) + ",\"indexes\":[\n";
    for (size_t ii = 0; ii < t.indexes.size(); ++ii) {
      const auto& ix = t.indexes[ii];
      if (ii) out += ",\n";
      out += "      {\"collection\":\"" + JsonEscape(ix.collection) +
             "\",\"dimension\":" + std::to_string(ix.dimension) +
             ",\"rows\":" + std::to_string(ix.rows) +
             ",\"ok\":" + (ix.ok ? "true" : "false") +
             ",\"create_ms\":" + std::to_string(ix.create_ms) +
             ",\"upsert_ms\":" + std::to_string(ix.upsert_ms) +
             ",\"flush_ms\":" + std::to_string(ix.flush_ms) +
             ",\"compact_ms\":" + std::to_string(ix.compact_ms) +
             ",\"upsert_vps\":" + std::to_string(ix.upsert_vps) +
             ",\"search_avg_ms\":" + std::to_string(ix.search_avg_ms) +
             ",\"search_p50_ms\":" + std::to_string(ix.search_p50_ms) +
             ",\"search_p95_ms\":" + std::to_string(ix.search_p95_ms) +
             ",\"search_p99_ms\":" + std::to_string(ix.search_p99_ms) +
             ",\"target_hits\":" + std::to_string(ix.target_hits) +
             ",\"error\":\"" + JsonEscape(ix.error) + "\"}";
    }
    out += "\n    ]}";
  }
  out += "\n  ]\n}\n";
  return out;
}

void PrintCliSummary(const std::vector<TenantResult>& tenants,
                     const std::vector<IndexSpec>& specs, double wall_ms) {
  std::printf("\n======== Multi-tenant bench summary ========\n");
  std::printf("tenants=%zu  indexes/tenant=%zu  collections=%zu  wall=%.1fms\n",
              tenants.size(), specs.size(), tenants.size() * specs.size(),
              wall_ms);
  std::printf("%-10s %-8s %-10s %10s %10s %10s %8s\n", "tenant", "dim", "rows",
              "upsert_vps", "p50_ms", "p95_ms", "ok");
  for (const auto& t : tenants) {
    for (const auto& ix : t.indexes) {
      std::printf("%-10s %-8u %-10llu %10.1f %10.3f %10.3f %8s\n",
                  t.tenant.c_str(), ix.dimension,
                  static_cast<unsigned long long>(ix.rows), ix.upsert_vps,
                  ix.search_p50_ms, ix.search_p95_ms, ix.ok ? "yes" : "NO");
    }
  }
  std::printf("============================================\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!ParseArgs(argc, argv, &cfg)) {
    PrintUsage(argv[0]);
    return 2;
  }

  const auto specs = ProfileSpecs(cfg.profile);
  if (cfg.tenants <= 0) cfg.tenants = ProfileTenants(cfg.profile);
  if (cfg.concurrent_tenants <= 0) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    cfg.concurrent_tenants = std::max(1, std::min(cfg.tenants, hw > 0 ? hw : 4));
  }

  if (::mkdir(cfg.data_dir.c_str(), 0755) != 0 && errno != EEXIST) {
    std::fprintf(stderr, "error: cannot create data-dir %s\n",
                 cfg.data_dir.c_str());
    return 1;
  }

  aster::Catalog::Options opt;
  opt.data_dir = cfg.data_dir;
  opt.wal_sync =
      cfg.durable ? aster::SyncPolicy::kEveryMs : aster::SyncPolicy::kNever;
  opt.memtable_flush_bytes = 8 << 20;
  auto catalog = aster::Catalog::Open(opt);
  if (!catalog.ok()) {
    std::fprintf(stderr, "error: catalog open: %s\n",
                 catalog.status().message().c_str());
    return 1;
  }

  std::printf(
      "{\"phase\":\"start\",\"profile\":\"%s\",\"tenants\":%d,"
      "\"indexes_per_tenant\":%zu,\"concurrent\":%d}\n",
      cfg.profile.c_str(), cfg.tenants, specs.size(), cfg.concurrent_tenants);
  std::fflush(stdout);

  std::vector<TenantResult> results(static_cast<size_t>(cfg.tenants));
  std::atomic<int> next_tenant{0};
  std::mutex err_mu;
  std::string fatal;

  const auto wall0 = Clock::now();
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(cfg.concurrent_tenants));
  for (int w = 0; w < cfg.concurrent_tenants; ++w) {
    workers.emplace_back([&] {
      while (true) {
        const int tid = next_tenant.fetch_add(1);
        if (tid >= cfg.tenants) break;
        try {
          results[static_cast<size_t>(tid)] =
              RunTenant(catalog.value().get(), tid, specs, cfg);
        } catch (const std::exception& ex) {
          std::lock_guard<std::mutex> lock(err_mu);
          fatal = ex.what();
        }
      }
    });
  }
  for (auto& th : workers) th.join();

  if (!fatal.empty()) {
    std::fprintf(stderr, "error: %s\n", fatal.c_str());
    return 1;
  }

  const double wall_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - wall0).count();

  size_t rss_kb = 0;
#if defined(__APPLE__) || defined(__linux__)
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
    rss_kb = static_cast<size_t>(ru.ru_maxrss / 1024);  // Apple: bytes
#else
    rss_kb = static_cast<size_t>(ru.ru_maxrss);  // Linux: KiB
#endif
  }
#endif

  // Isolation smoke: search one collection, ensure hits belong to it only
  // (ids are namespaced by construction; verify list count matches).
  const auto listed = catalog.value()->ListCollections();
  const size_t expected = static_cast<size_t>(cfg.tenants) * specs.size();
  if (listed.size() != expected) {
    std::fprintf(stderr,
                 "error: catalog list size %zu != expected collections %zu\n",
                 listed.size(), expected);
    return 1;
  }

  const auto usage = catalog.value()->Usage();
  PrintCliSummary(results, specs, wall_ms);

  const std::string report =
      BuildReportJson(cfg, specs, results, wall_ms, rss_kb);
  // Append usage into a thin envelope line for log scrapers.
  std::printf(
      "{\"phase\":\"done\",\"collections\":%zu,\"vectors_estimate\":%zu,"
      "\"upserts\":%llu,\"searches\":%llu,\"wall_ms\":%.1f,\"rss_kb\":%zu}\n",
      usage.collections, usage.vectors_estimate,
      static_cast<unsigned long long>(usage.upserts),
      static_cast<unsigned long long>(usage.searches), wall_ms, rss_kb);

  if (!cfg.out_json.empty()) {
    FILE* f = std::fopen(cfg.out_json.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "error: cannot write %s\n", cfg.out_json.c_str());
      return 1;
    }
    std::fputs(report.c_str(), f);
    std::fclose(f);
    std::printf("wrote %s\n", cfg.out_json.c_str());
  } else {
    std::fputs(report.c_str(), stdout);
  }

  for (const auto& t : results) {
    for (const auto& ix : t.indexes) {
      if (!ix.ok) return 1;
      if (ix.target_hits < 1) return 1;
    }
  }
  return 0;
}
