// M2-T10: HNSW recall@10 evaluation vs exact (SIFT/GloVe-like subsets).

#include "aster/qa/recall_gate_lib.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "aster/core/features.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace recall_gate {
namespace {

struct CorpusConfig {
  const char* name;
  Metric metric;
  uint32_t dim;
  uint32_t n_base;
  uint32_t n_query;
  uint64_t seed;
};

struct ScaleConfig {
  const char* name;
  CorpusConfig sift;   // SIFT1M-like: L2, 128-d
  CorpusConfig glove;  // GloVe-like: cosine, 100-d
};

// PR: small deterministic subsets (seconds). Nightly: larger stand-ins for
// full SIFT1M / GloVe (optional download not required for the gate).
constexpr ScaleConfig kPrScale = {
    "pr",
    {"sift_subset", Metric::kL2, 128, 2048, 64, 0x51F70001ull},
    {"glove_subset", Metric::kCosine, 100, 1536, 48, 0x610FE001ull},
};

constexpr ScaleConfig kNightlyScale = {
    "nightly",
    {"sift_subset", Metric::kL2, 128, 16384, 256, 0x51F70001ull},
    {"glove_subset", Metric::kCosine, 100, 12288, 192, 0x610FE001ull},
};

constexpr uint32_t kTopK = 10;
constexpr uint32_t kEfSearch = 128;
constexpr float kRecallFloor = 0.95f;

const ScaleConfig& ResolveScale(std::string_view scale) {
  if (scale == "nightly" || scale == "large") {
    return kNightlyScale;
  }
  return kPrScale;
}

// Deterministic unit-ish vectors with mild cluster structure so HNSW at
// ef=128 is a meaningful gate (not a trivial grid, not pure noise).
std::vector<std::vector<float>> MakeCorpus(const CorpusConfig& cfg,
                                           uint32_t count, uint64_t salt) {
  std::mt19937_64 rng(cfg.seed ^ salt);
  std::normal_distribution<float> noise(0.0f, 0.15f);
  std::uniform_int_distribution<int> cluster(0, 31);

  std::vector<std::vector<float>> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const int c = cluster(rng);
    std::vector<float> v(cfg.dim);
    float norm_sq = 0.0f;
    for (uint32_t d = 0; d < cfg.dim; ++d) {
      const float center =
          ((d + static_cast<uint32_t>(c) * 3u) % cfg.dim < cfg.dim / 4u)
              ? 1.0f
              : 0.0f;
      v[d] = center + noise(rng);
      norm_sq += v[d] * v[d];
    }
    if (cfg.metric == Metric::kCosine || cfg.metric == Metric::kDot) {
      const float inv = norm_sq > 0.0f ? 1.0f / std::sqrt(norm_sq) : 1.0f;
      for (float& x : v) x *= inv;
    }
    out.push_back(std::move(v));
  }
  return out;
}

float RecallAtK(const std::vector<SearchHit>& approx,
                const std::vector<SearchHit>& truth) {
  if (truth.empty()) return 1.0f;
  std::unordered_set<std::string> want;
  want.reserve(truth.size());
  for (const auto& h : truth) want.insert(h.id);
  size_t hits = 0;
  for (const auto& h : approx) {
    if (want.count(h.id) != 0) ++hits;
  }
  return static_cast<float>(hits) / static_cast<float>(truth.size());
}

struct CorpusResult {
  const char* name;
  float mean_recall;
  uint32_t n_query;
  uint32_t n_base;
};

#if ASTER_ENABLE_HNSW
CorpusResult EvaluateCorpus(const CorpusConfig& cfg) {
  auto base = MakeCorpus(cfg, cfg.n_base, /*salt=*/0);
  auto queries = MakeCorpus(cfg, cfg.n_query, /*salt=*/0x9E3779B97F4A7C15ull);

  std::vector<IndexEntry> exact_entries;
  std::vector<IndexEntry> hnsw_entries;
  exact_entries.reserve(base.size());
  hnsw_entries.reserve(base.size());
  std::vector<std::string> ids;
  ids.reserve(base.size());
  for (size_t i = 0; i < base.size(); ++i) {
    ids.push_back("b" + std::to_string(i));
  }
  for (size_t i = 0; i < base.size(); ++i) {
    exact_entries.push_back({ids[i], base[i]});
    hnsw_entries.push_back({ids[i], base[i]});
  }

  HnswParams params;
  params.m = 16;
  params.ef_construction = 128;
  params.ef_search_default = kEfSearch;
  params.max_layers = 16;

  auto exact = BuildExactIndex(cfg.metric, std::move(exact_entries));
  auto hnsw = BuildHnswIndex(cfg.metric, params, std::move(hnsw_entries),
                             /*rng_seed=*/7);

  float sum = 0.0f;
  for (uint32_t q = 0; q < cfg.n_query; ++q) {
    auto truth = exact->Search(queries[q], kTopK, /*ef_search=*/0);
    auto approx = hnsw->Search(queries[q], kTopK, kEfSearch);
    sum += RecallAtK(approx, truth);
  }
  return CorpusResult{cfg.name, sum / static_cast<float>(cfg.n_query),
                      cfg.n_query, cfg.n_base};
}
#endif  // ASTER_ENABLE_HNSW

bool RunScale(const ScaleConfig& scale, bool print) {
#if !ASTER_ENABLE_HNSW
  (void)scale;
  if (print) {
    std::fprintf(stderr, "recall_gate: HNSW disabled (Tiny profile)\n");
  }
  return false;
#else
  const CorpusResult sift = EvaluateCorpus(scale.sift);
  const CorpusResult glove = EvaluateCorpus(scale.glove);
  if (print) {
    std::printf(
        "recall_gate scale=%s corpus=%s n_base=%u n_query=%u "
        "recall@%u@ef=%u=%.4f (floor=%.2f)\n",
        scale.name, sift.name, sift.n_base, sift.n_query, kTopK, kEfSearch,
        sift.mean_recall, kRecallFloor);
    std::printf(
        "recall_gate scale=%s corpus=%s n_base=%u n_query=%u "
        "recall@%u@ef=%u=%.4f (floor=%.2f)\n",
        scale.name, glove.name, glove.n_base, glove.n_query, kTopK, kEfSearch,
        glove.mean_recall, kRecallFloor);
  }
  return sift.mean_recall >= kRecallFloor && glove.mean_recall >= kRecallFloor;
#endif
}

}  // namespace

std::string_view ScaleFromEnvOrDefault(const char* fallback) {
  if (const char* env = std::getenv("ASTER_RECALL_SCALE")) {
    if (env[0] != '\0') return env;
  }
  return fallback != nullptr ? fallback : "pr";
}

bool Run(std::string_view scale, bool print) {
  return RunScale(ResolveScale(scale), print);
}

}  // namespace recall_gate
}  // namespace aster
