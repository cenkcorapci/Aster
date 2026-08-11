#include "aster/qa/latency_bench_lib.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "aster/core/features.h"
#include "aster/db/db.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_graph.h"
#endif

namespace aster {
namespace latency_bench {
namespace {

using Clock = std::chrono::steady_clock;

struct ScaleConfig {
  const char* name;
  Metric metric;
  uint32_t dim;
  uint64_t n_base;
  uint32_t n_query;
  uint32_t warmup;
  uint32_t top_k;
  uint32_t ef_search;
  uint64_t seed;
  double p50_threshold_ms;
};

// CI gate target (M2-T11):
//   1M×384d search p50 < 5ms
constexpr ScaleConfig kCi = {
    "ci",
    Metric::kL2,
    /*dim=*/384,
    /*n_base=*/1000000ull,
    /*n_query=*/2000u,
    /*warmup=*/200u,
    /*top_k=*/10u,
    /*ef_search=*/128u,
    /*seed=*/0x51F70001ull,
    /*p50_threshold_ms=*/5.0,
};

// Default quick smoke configuration: exercises harness end-to-end without
// requiring large memory / time.
constexpr ScaleConfig kSmoke = {
    "smoke",
    Metric::kL2,
    /*dim=*/64,
    /*n_base=*/20000ull,
    /*n_query=*/200u,
    /*warmup=*/20u,
    /*top_k=*/10u,
    /*ef_search=*/32u,
    /*seed=*/0x51F70001ull,
    /*p50_threshold_ms=*/1000.0,
};

const ScaleConfig& Resolve(std::string_view scale) {
  if (scale == "ci") return kCi;
  return kSmoke;
}

double PercentileMs(std::vector<double>& ms, double p) {
  if (ms.empty()) return 0.0;
  if (ms.size() == 1) return ms[0];
  std::sort(ms.begin(), ms.end());
  const double k = (static_cast<double>(ms.size()) - 1.0) * (p / 100.0);
  const size_t f = static_cast<size_t>(k);
  const size_t c = std::min(ms.size() - 1, f + 1);
  if (f == c) return ms[f];
  const double w = k - static_cast<double>(f);
  return ms[f] * (1.0 - w) + ms[c] * w;
}

// Clustered deterministic vectors (fast + stable). We mimic recall_gate's
// mild clustering but tune for the larger dimensionality.
std::vector<float> MakeVector(uint32_t dim, int cluster_id, std::mt19937_64& rng,
                               std::normal_distribution<float>& noise) {
  std::vector<float> v(dim);
  for (uint32_t d = 0; d < dim; ++d) {
    // Create sparse-ish “on” dimensions with a rotating pattern so the
    // distribution is not pure noise.
    const uint32_t period = 32u;
    const bool on =
        ((d + static_cast<uint32_t>(cluster_id) * 3u) % period) <
        (period / 4u);
    const float center = on ? 1.0f : 0.0f;
    v[d] = center + noise(rng);
  }
  return v;
}

#if ASTER_ENABLE_HNSW
void EnsureHnswReady(aster::Db& db) {
  // Build pending READY graphs synchronously to avoid background thread
  // scheduling jitter in the timed loop.
  const auto st = db.BuildPendingIndexes();
  if (!st.ok()) {
    std::fprintf(stderr, "latency_bench: BuildPendingIndexes failed: %s\n",
                 st.message().c_str());
    std::exit(2);
  }
}
#else
void EnsureHnswReady(aster::Db&) {
  std::fprintf(stderr, "latency_bench: HNSW disabled (Tiny profile)\n");
  std::exit(2);
}
#endif

std::string MakeId(uint64_t i) {
  // Fixed-width hex IDs keep string comparisons cache-friendly.
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

}  // namespace

bool Run(std::string_view scale, bool print, double p50_threshold_ms,
         std::string_view out_json_path, Result* out) {
  if (out == nullptr) return false;
  const ScaleConfig& cfg = Resolve(scale);

  // CI might override thresholds, but defaults keep docs/tasks expectations.
  const double gate_ms = p50_threshold_ms > 0.0 ? p50_threshold_ms
                                                   : cfg.p50_threshold_ms;

  // Build exactly one READY segment so the timed loop measures one ANN
  // search path without segment fan-out / reconciliation noise.
  aster::Db::Options opt;
  opt.dimension = cfg.dim;
  opt.metric = cfg.metric;
  opt.memtable_flush_bytes = 1ull << 60;
  opt.memtable_flush_ms = 0;
  opt.data_dir.clear();  // in-memory only
  opt.wal_sync = aster::SyncPolicy::kNever;
  opt.compaction_tier_threshold = 0;
  opt.max_segments_before_compact = 1;

#if ASTER_ENABLE_HNSW
  opt.background_index_build = false;
  opt.hnsw_params = aster::HnswParams{};
  opt.hnsw_params.m = 16;
  opt.hnsw_params.ef_construction = 128;
  opt.hnsw_params.ef_search_default = cfg.ef_search;
  opt.hnsw_params.max_layers = 16;
  opt.hnsw_rng_seed = cfg.seed;
#else
  opt.background_index_build = false;
#endif

  auto opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "latency_bench: Db::Open failed: %s\n",
                 opened.status().message().c_str());
    return false;
  }
  auto& db = *opened.value();

  // Deterministic clustered corpus generation.
  std::mt19937_64 rng(cfg.seed);
  std::uniform_int_distribution<int> cluster(0, 31);
  std::normal_distribution<float> noise(0.0f, 0.15f);

  // Upsert all base vectors.
  for (uint64_t i = 0; i < cfg.n_base; ++i) {
    aster::Row row;
    row.id = MakeId(i);
    row.vector = MakeVector(cfg.dim, cluster(rng), rng, noise);
    row.timestamp = i + 1;
    row.version = 1;
    row.tombstone = false;
    if (!db.Upsert(std::move(row)).ok()) {
      std::fprintf(stderr, "latency_bench: Upsert failed at i=%llu\n",
                   static_cast<unsigned long long>(i));
      return false;
    }
  }

  // Seal into one segment.
  if (!db.Flush().ok()) {
    std::fprintf(stderr, "latency_bench: Flush failed\n");
    return false;
  }

  EnsureHnswReady(db);

  // Pre-generate queries (so the timed loop only measures Search()).
  std::mt19937_64 qrng(cfg.seed ^ 0x9E3779B97F4A7C15ull);
  std::uniform_int_distribution<int> qcluster(0, 31);
  std::vector<aster::SearchRequest> reqs;
  reqs.reserve(cfg.n_query);
  for (uint32_t qi = 0; qi < cfg.n_query; ++qi) {
    aster::SearchRequest r;
    r.top_k = cfg.top_k;
    r.ef_search = cfg.ef_search;
    // tags empty => unfiltered path => READY uses HNSW.
    r.tags.clear();
    r.vector = MakeVector(cfg.dim, qcluster(qrng), qrng, noise);
    reqs.push_back(std::move(r));
  }

  // Warm up (not included in stats).
  for (uint32_t i = 0; i < cfg.warmup && i < cfg.n_query; ++i) {
    (void)db.Search(reqs[i]);
  }

  std::vector<double> lat_ms;
  lat_ms.reserve(cfg.n_query > cfg.warmup ? cfg.n_query - cfg.warmup : 0);

  volatile double sink = 0.0;  // prevent dead-code elimination
  const uint32_t start = cfg.warmup;
  const uint32_t end = cfg.n_query;
  for (uint32_t i = start; i < end; ++i) {
    const auto t0 = Clock::now();
    auto hits = db.Search(reqs[i]);
    const auto t1 = Clock::now();
    if (!hits.empty()) sink += hits[0].score;
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    lat_ms.push_back(ms);
  }
  (void)sink;

  std::vector<double> lat_copy = lat_ms;  // Percentile sorts in-place.
  const double p50 = PercentileMs(lat_copy, 50.0);
  std::vector<double> lat_copy2 = lat_ms;
  const double p95 = PercentileMs(lat_copy2, 95.0);
  double sum = 0.0;
  for (double x : lat_ms) sum += x;
  const double avg = lat_ms.empty() ? 0.0 : sum / static_cast<double>(lat_ms.size());

  Result r;
  r.scale = std::string(cfg.name);
  r.dim = cfg.dim;
  r.n_base = cfg.n_base;
  r.n_query = cfg.n_query;
  r.top_k = cfg.top_k;
  r.ef_search = cfg.ef_search;
  r.warmup = cfg.warmup;
  r.measured = static_cast<uint32_t>(lat_ms.size());
  r.metric = cfg.metric;
  r.p50_ms = p50;
  r.p95_ms = p95;
  r.avg_ms = avg;
  r.p50_threshold_ms = gate_ms;
  r.pass = p50 < gate_ms;
  if (print) {
    std::printf(
        "latency_bench scale=%s dim=%u n_base=%llu n_query=%u top_k=%u "
        "ef_search=%u warmup=%u measured=%u p50_ms=%.4f p95_ms=%.4f "
        "avg_ms=%.4f gate_p50_ms=%.2f %s\n",
        r.scale.c_str(), r.dim,
        static_cast<unsigned long long>(r.n_base), r.n_query, r.top_k,
        r.ef_search, r.warmup, r.measured, r.p50_ms, r.p95_ms, r.avg_ms,
        r.p50_threshold_ms, r.pass ? "PASS" : "FAIL");
  }

  if (!out_json_path.empty()) {
    // Minimal JSON serialization with stable key ordering.
    char json[8192];
    std::snprintf(
        json, sizeof(json),
        "{"
        "\"scale\":\"%s\","
        "\"dim\":%u,"
        "\"n_base\":%llu,"
        "\"n_query\":%u,"
        "\"top_k\":%u,"
        "\"ef_search\":%u,"
        "\"warmup\":%u,"
        "\"measured\":%u,"
        "\"metric\":\"%s\","
        "\"p50_ms\":%.6f,"
        "\"p95_ms\":%.6f,"
        "\"avg_ms\":%.6f,"
        "\"p50_threshold_ms\":%.6f,"
        "\"pass\":%s"
        "}\n",
        r.scale.c_str(), r.dim,
        static_cast<unsigned long long>(r.n_base), r.n_query, r.top_k,
        r.ef_search, r.warmup, r.measured,
        r.metric == Metric::kL2   ? "l2"
        : r.metric == Metric::kDot ? "dot"
                                    : "cosine",
        r.p50_ms, r.p95_ms, r.avg_ms, r.p50_threshold_ms,
        r.pass ? "true" : "false");

    FILE* f = std::fopen(std::string(out_json_path).c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "latency_bench: fopen failed for %.*s\n",
                   static_cast<int>(out_json_path.size()),
                   std::string_view(out_json_path).data());
      return false;
    }
    std::fputs(json, f);
    std::fclose(f);
  }

  *out = r;
  return r.pass;
}

}  // namespace latency_bench
}  // namespace aster

