// Single-node and 3-node (RF=3 Coordinator) RAM stress for cold-mmap sealed
// vectors. Prints a machine-readable summary block for docs.
//
//   bazel run //aster/bench:ram_stress_bench -- \
//     --single-n 50000 --cluster-n 30000 --dim 384 --encoding int8_sq

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/distributed/replication.h"
#include "aster/index/vector_encoding.h"
#include "aster/storage/sstable.h"

namespace {

using Clock = std::chrono::steady_clock;

double RssMb() {
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

struct Lat {
  std::vector<double> ms;
  void Add(double v) { ms.push_back(v); }
  double Pct(double p) const {
    if (ms.empty()) return 0;
    std::vector<double> s = ms;
    std::sort(s.begin(), s.end());
    const double k = (s.size() - 1) * (p / 100.0);
    const size_t f = static_cast<size_t>(std::floor(k));
    const size_t c = static_cast<size_t>(std::ceil(k));
    if (f == c) return s[f];
    return s[f] * (c - k) + s[c] * (k - f);
  }
  double Avg() const {
    if (ms.empty()) return 0;
    double sum = 0;
    for (double x : ms) sum += x;
    return sum / static_cast<double>(ms.size());
  }
};

aster::VectorEncoding ParseEnc(const char* s) {
  if (std::strcmp(s, "float32") == 0) return aster::VectorEncoding::kFloat32;
  if (std::strcmp(s, "float16") == 0) return aster::VectorEncoding::kFloat16;
  if (std::strcmp(s, "binary_rabitq") == 0) {
    return aster::VectorEncoding::kBinaryRabitq;
  }
  return aster::VectorEncoding::kInt8Sq;
}

void FillVec(std::mt19937& rng, std::vector<float>& v) {
  std::normal_distribution<float> dist(0.f, 1.f);
  double n2 = 0;
  for (float& x : v) {
    x = dist(rng);
    n2 += static_cast<double>(x) * x;
  }
  const float inv = n2 > 0 ? static_cast<float>(1.0 / std::sqrt(n2)) : 1.f;
  for (float& x : v) x *= inv;
}

uint64_t HeapFromAst(const std::string& path,
                     const aster::StoragePolicy& policy) {
  auto reader = aster::SstableReader::Open(path, policy);
  if (!reader.ok() || !reader.value()->packed_vectors()) return 0;
  return reader.value()->packed_vectors()->HeapBytesUsed();
}

uint64_t LogicalFromAst(const std::string& path,
                        const aster::StoragePolicy& policy) {
  auto reader = aster::SstableReader::Open(path, policy);
  if (!reader.ok() || !reader.value()->packed_vectors()) return 0;
  return reader.value()->packed_vectors()->BytesUsed();
}

std::string FirstAst(const std::string& dir) {
  if (!std::filesystem::exists(dir)) return {};
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const auto name = e.path().filename().string();
    if (name.rfind("seg_", 0) == 0 && name.size() > 4 &&
        name.substr(name.size() - 4) == ".ast") {
      return e.path().string();
    }
  }
  return {};
}

struct RunResult {
  const char* setup = "";
  size_t n = 0;
  uint32_t dim = 0;
  const char* encoding = "";
  uint64_t raw_f32 = 0;
  uint64_t logical = 0;
  uint64_t anon_heap = 0;
  double load_sec = 0;
  double reopen_sec = 0;
  double search_qps = 0;
  double search_p50_ms = 0;
  double search_p95_ms = 0;
  double search_p99_ms = 0;
  double write_qps = 0;
  double write_p95_ms = 0;
  double rss_mb = 0;
  uint64_t searches = 0;
  uint64_t errors = 0;
  bool file_backed = false;
};

void PrintResult(const RunResult& r) {
  std::printf(
      "### %s\n\n"
      "| Metric | Value |\n"
      "| --- | ---: |\n"
      "| Vectors | %zu |\n"
      "| Dimension | %u |\n"
      "| Encoding | `%s` |\n"
      "| Raw float32 payload | %.2f MiB |\n"
      "| Logical packed payload | %.2f MiB |\n"
      "| Anonymous vector heap | %llu B (%s) |\n"
      "| Load + flush | %.2f s (%.0f upserts/s) |\n"
      "| Reopen | %.3f s |\n"
      "| Search QPS (warm) | %.1f |\n"
      "| Search p50 / p95 / p99 | %.3f / %.3f / %.3f ms |\n"
      "| Write p95 (ingest) | %.3f ms |\n"
      "| Process RSS max | %.1f MiB |\n"
      "| Searches / errors | %llu / %llu |\n\n",
      r.setup, r.n, r.dim, r.encoding,
      static_cast<double>(r.raw_f32) / (1024.0 * 1024.0),
      static_cast<double>(r.logical) / (1024.0 * 1024.0),
      static_cast<unsigned long long>(r.anon_heap),
      r.file_backed ? "file-backed" : "heap", r.load_sec, r.write_qps,
      r.reopen_sec, r.search_qps, r.search_p50_ms, r.search_p95_ms,
      r.search_p99_ms, r.write_p95_ms, r.rss_mb,
      static_cast<unsigned long long>(r.searches),
      static_cast<unsigned long long>(r.errors));
}

aster::Db::Options BaseOpt(uint32_t dim, aster::VectorEncoding enc,
                            const std::string& dir) {
  aster::Db::Options opt;
  opt.dimension = dim;
  opt.metric = aster::Metric::kL2;
  opt.vector_encoding = enc;
  opt.data_dir = dir;
  opt.storage_policy.vectors = aster::Residency::kCold;
  opt.wal_sync = aster::SyncPolicy::kNever;
  opt.memtable_flush_bytes = 32ull << 20;
  opt.compaction_tier_threshold = 0;
  opt.max_segments_before_compact = 0;
#if ASTER_ENABLE_HNSW
  opt.background_index_build = false;
#endif
  return opt;
}

RunResult RunSingle(size_t n, uint32_t dim, aster::VectorEncoding enc,
                    const char* enc_name, int search_queries,
                    const std::string& root) {
  RunResult r;
  r.setup = "Single-node durable (cold mmap)";
  r.n = n;
  r.dim = dim;
  r.encoding = enc_name;
  r.raw_f32 = static_cast<uint64_t>(n) * dim * 4ull;

  const std::string dir = root + "/single";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto opt = BaseOpt(dim, enc, dir);
  auto opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "single open: %s\n", opened.status().message().c_str());
    ++r.errors;
    return r;
  }
  auto db = std::move(opened.value());

  std::mt19937 rng(42);
  std::vector<float> vec(dim);
  Lat write_lat;
  const auto t0 = Clock::now();
  for (size_t i = 0; i < n; ++i) {
    FillVec(rng, vec);
    aster::Row row;
    char id[32];
    std::snprintf(id, sizeof(id), "s%08zu", i);
    row.id = id;
    row.vector = vec;
    row.timestamp = static_cast<aster::Timestamp>(i + 1);
    const auto a = Clock::now();
    if (!db->Upsert(std::move(row)).ok()) ++r.errors;
    write_lat.Add(std::chrono::duration<double, std::milli>(Clock::now() - a)
                      .count());
  }
  if (!db->Flush().ok()) ++r.errors;
  r.load_sec = std::chrono::duration<double>(Clock::now() - t0).count();
  r.write_qps = r.load_sec > 0 ? static_cast<double>(n) / r.load_sec : 0;
  r.write_p95_ms = write_lat.Pct(95);

  db.reset();
  const auto t_re = Clock::now();
  opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "single reopen: %s\n",
                 opened.status().message().c_str());
    ++r.errors;
    return r;
  }
  db = std::move(opened.value());
  r.reopen_sec = std::chrono::duration<double>(Clock::now() - t_re).count();

  const std::string ast = FirstAst(dir);
  r.anon_heap = HeapFromAst(ast, opt.storage_policy);
  r.logical = LogicalFromAst(ast, opt.storage_policy);
  r.file_backed = (r.anon_heap == 0 && r.logical > 0);

  Lat search_lat;
  std::vector<float> q(dim);
  const auto ts = Clock::now();
  for (int i = 0; i < search_queries; ++i) {
    FillVec(rng, q);
    aster::SearchRequest req;
    req.vector = q;
    req.top_k = 10;
    const auto a = Clock::now();
    auto hits = db->Search(req);
    search_lat.Add(
        std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    ++r.searches;
    if (hits.empty()) ++r.errors;
  }
  const double search_sec =
      std::chrono::duration<double>(Clock::now() - ts).count();
  r.search_qps =
      search_sec > 0 ? static_cast<double>(search_queries) / search_sec : 0;
  r.search_p50_ms = search_lat.Pct(50);
  r.search_p95_ms = search_lat.Pct(95);
  r.search_p99_ms = search_lat.Pct(99);
  r.rss_mb = RssMb();
  return r;
}

RunResult RunCluster3(size_t n, uint32_t dim, aster::VectorEncoding enc,
                      const char* enc_name, int search_queries,
                      const std::string& root) {
  RunResult r;
  r.setup = "3-node RF=3 Coordinator (cold mmap per replica)";
  r.n = n;
  r.dim = dim;
  r.encoding = enc_name;
  r.raw_f32 = static_cast<uint64_t>(n) * dim * 4ull;

  const std::string base = root + "/cluster3";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);

  std::map<std::string, std::unique_ptr<aster::Db>> dbs;
  for (const char* node : {"a", "b", "c"}) {
    auto opt = BaseOpt(dim, enc, base + "/" + node);
    auto opened = aster::Db::Open(opt);
    if (!opened.ok()) {
      std::fprintf(stderr, "node %s open: %s\n", node,
                   opened.status().message().c_str());
      ++r.errors;
      return r;
    }
    dbs[node] = std::move(opened.value());
  }

  aster::CoordinatorOptions copts;
  copts.replication_factor = 3;
  copts.vnodes_per_node = 64;
  copts.hint_directory = base + "/hints";
  auto copen = aster::Coordinator::Open(copts);
  if (!copen.ok()) {
    std::fprintf(stderr, "coordinator: %s\n", copen.status().message().c_str());
    ++r.errors;
    return r;
  }
  auto coord = std::move(copen.value());
  for (auto& [id, db] : dbs) {
    if (!coord->AddReplica(id, db.get()).ok()) ++r.errors;
  }

  std::mt19937 rng(99);
  std::vector<float> vec(dim);
  Lat write_lat;
  const auto t0 = Clock::now();
  for (size_t i = 0; i < n; ++i) {
    FillVec(rng, vec);
    aster::Row row;
    char id[32];
    std::snprintf(id, sizeof(id), "c%08zu", i);
    row.id = id;
    row.vector = vec;
    row.timestamp = static_cast<aster::Timestamp>(i + 1);
    const auto a = Clock::now();
    if (!coord->Write(row, aster::ConsistencyLevel::kQuorum).ok()) ++r.errors;
    write_lat.Add(std::chrono::duration<double, std::milli>(Clock::now() - a)
                      .count());
  }
  for (auto& [_, db] : dbs) {
    if (!db->Flush().ok()) ++r.errors;
  }
  r.load_sec = std::chrono::duration<double>(Clock::now() - t0).count();
  r.write_qps = r.load_sec > 0 ? static_cast<double>(n) / r.load_sec : 0;
  r.write_p95_ms = write_lat.Pct(95);

  coord.reset();
  dbs.clear();

  const auto t_re = Clock::now();
  uint64_t heap_sum = 0;
  uint64_t logical_max = 0;
  for (const char* node : {"a", "b", "c"}) {
    auto opt = BaseOpt(dim, enc, base + "/" + node);
    auto opened = aster::Db::Open(opt);
    if (!opened.ok()) {
      std::fprintf(stderr, "reopen %s: %s\n", node,
                   opened.status().message().c_str());
      ++r.errors;
      continue;
    }
    dbs[node] = std::move(opened.value());
    const std::string ast = FirstAst(base + "/" + node);
    heap_sum += HeapFromAst(ast, opt.storage_policy);
    logical_max =
        std::max(logical_max, LogicalFromAst(ast, opt.storage_policy));
  }
  r.reopen_sec = std::chrono::duration<double>(Clock::now() - t_re).count();
  r.anon_heap = heap_sum;  // sum across 3 replicas' sealed vector heaps
  r.logical = logical_max;
  r.file_backed = (heap_sum == 0 && logical_max > 0);

  copen = aster::Coordinator::Open(copts);
  if (!copen.ok()) {
    ++r.errors;
    return r;
  }
  coord = std::move(copen.value());
  for (auto& [id, db] : dbs) {
    (void)coord->AddReplica(id, db.get());
  }

  // Quorum point reads + local ANN search on node a.
  Lat search_lat;
  std::vector<float> q(dim);
  const auto ts = Clock::now();
  for (int i = 0; i < search_queries; ++i) {
    char id[32];
    std::snprintf(id, sizeof(id), "c%08d", i % static_cast<int>(n));
    const auto a = Clock::now();
    auto got = coord->Read(id, aster::ConsistencyLevel::kQuorum);
    const double read_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - a).count();
    if (!got.ok() || !got.value().has_value()) ++r.errors;

    FillVec(rng, q);
    aster::SearchRequest req;
    req.vector = q;
    req.top_k = 10;
    const auto b = Clock::now();
    auto hits = dbs["a"]->Search(req);
    const double ann_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - b).count();
    search_lat.Add(read_ms + ann_ms);
    ++r.searches;
    if (hits.empty()) ++r.errors;
  }
  const double search_sec =
      std::chrono::duration<double>(Clock::now() - ts).count();
  r.search_qps =
      search_sec > 0 ? static_cast<double>(search_queries) / search_sec : 0;
  r.search_p50_ms = search_lat.Pct(50);
  r.search_p95_ms = search_lat.Pct(95);
  r.search_p99_ms = search_lat.Pct(99);
  r.rss_mb = RssMb();
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  size_t single_n = 50000;
  size_t cluster_n = 30000;
  uint32_t dim = 384;
  const char* encoding = "int8_sq";
  int searches = 500;
  std::string root = "/tmp/aster_ram_stress";
  bool run_single = true;
  bool run_cluster = true;

  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--single-n") == 0) {
      single_n = static_cast<size_t>(std::strtoull(need("--single-n"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--cluster-n") == 0) {
      cluster_n =
          static_cast<size_t>(std::strtoull(need("--cluster-n"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--dim") == 0) {
      dim = static_cast<uint32_t>(std::strtoul(need("--dim"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--encoding") == 0) {
      encoding = need("--encoding");
    } else if (std::strcmp(argv[i], "--searches") == 0) {
      searches = static_cast<int>(std::strtol(need("--searches"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--root") == 0) {
      root = need("--root");
    } else if (std::strcmp(argv[i], "--single-only") == 0) {
      run_cluster = false;
    } else if (std::strcmp(argv[i], "--cluster-only") == 0) {
      run_single = false;
    }
  }

  const auto enc = ParseEnc(encoding);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  char host[256] = "unknown";
#if defined(__APPLE__)
  std::snprintf(host, sizeof(host), "darwin");
#else
  std::snprintf(host, sizeof(host), "linux");
#endif

  std::printf(
      "## Stress results\n\n"
      "Host: `%s`, cold mmap (`StoragePolicy::vectors = kCold`), "
      "WAL sync=never, HNSW background build off (exact search). "
      "Anonymous vector heap is measured from sealed `.ast` packed stores "
      "after reopen (file-backed ⇒ 0).\n\n",
      host);

  int rc = 0;
  if (run_single) {
    auto s = RunSingle(single_n, dim, enc, encoding, searches, root);
    PrintResult(s);
    if (s.anon_heap != 0 || s.errors > 0) rc = 2;
  }
  if (run_cluster) {
    auto c = RunCluster3(cluster_n, dim, enc, encoding, searches, root);
    PrintResult(c);
    if (c.anon_heap != 0 || c.errors > 0) rc = 2;
  }
  return rc;
}
