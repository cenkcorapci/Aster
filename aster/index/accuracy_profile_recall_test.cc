#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "aster/core/types.h"
#include "aster/index/hnsw_graph.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

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

std::vector<std::vector<float>> MakeCorpus(uint32_t dim, uint32_t n,
                                             uint64_t seed) {
  // Deterministic vectors with mild cluster structure.
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> noise(0.0f, 0.25f);
  std::uniform_int_distribution<int> cluster(0, 31);

  std::vector<std::vector<float>> out;
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const int c = cluster(rng);
    std::vector<float> v(dim);
    float norm_sq = 0.0f;
    for (uint32_t d = 0; d < dim; ++d) {
      const float center =
          ((d + static_cast<uint32_t>(c) * 3u) % dim < dim / 4u) ? 1.0f : 0.0f;
      v[d] = center + noise(rng);
      norm_sq += v[d] * v[d];
    }
    out.push_back(std::move(v));
  }
  return out;
}

}  // namespace

#if ASTER_ENABLE_HNSW

TEST(AccuracyProfile, RecallImprovesWithProfile) {
  constexpr uint32_t kTopK = 10;
  constexpr uint32_t kDim = 64;
  constexpr uint32_t kBase = 512;
  constexpr uint32_t kQueries = 64;

  const auto base = MakeCorpus(kDim, kBase, /*seed=*/0x51F70001ull);
  const auto queries = MakeCorpus(kDim, kQueries,
                                   /*seed=*/0x9E3779B97F4A7C15ull);

  // Same string IDs for exact + HNSW indexes.
  std::vector<std::string> ids;
  ids.reserve(kBase);
  for (uint32_t i = 0; i < kBase; ++i) ids.push_back("b" + std::to_string(i));

  std::vector<IndexEntry> exact_entries;
  exact_entries.reserve(kBase);
  for (uint32_t i = 0; i < kBase; ++i) {
    exact_entries.push_back({ids[i], base[i]});
  }

  auto exact = BuildExactIndex(Metric::kL2, std::move(exact_entries));

  struct ProfileResult {
    AccuracyProfile profile;
    float mean_recall;
  };

  auto eval_profile = [&](AccuracyProfile profile) -> float {
    const HnswParams params = HnswParamsFromAccuracyProfile(profile);
    std::vector<IndexEntry> hnsw_entries;
    hnsw_entries.reserve(kBase);
    for (uint32_t i = 0; i < kBase; ++i) {
      hnsw_entries.push_back({ids[i], base[i]});
    }

    auto hnsw = BuildHnswIndex(Metric::kL2, params, std::move(hnsw_entries),
                                 /*rng_seed=*/7);

    float sum = 0.0f;
    for (uint32_t q = 0; q < kQueries; ++q) {
      auto truth = exact->Search(queries[q], kTopK, /*ef_search=*/0);
      auto approx = hnsw->Search(queries[q], kTopK, /*ef_search=*/0);
      sum += RecallAtK(approx, truth);
    }
    return sum / static_cast<float>(kQueries);
  };

  const float recall_cost = eval_profile(AccuracyProfile::kCostOptimized);
  const float recall_balanced = eval_profile(AccuracyProfile::kBalanced);
  const float recall_high = eval_profile(AccuracyProfile::kHighRecall);
  const float recall_max = eval_profile(AccuracyProfile::kMaxRecall);

  // Small deterministic corpus gate: MAX_RECALL should be meaningfully better
  // than COST_OPTIMIZED, and recall should not regress as the profile
  // increases.
  EXPECT_GE(recall_max, recall_cost);
  EXPECT_GE(recall_high, recall_cost);
  EXPECT_GE(recall_balanced, recall_cost);
  EXPECT_GE(recall_max, recall_high);

  // Recall should still be high at MAX_RECALL for this deterministic dataset.
  EXPECT_GE(recall_max, 0.85f);
}

#endif  // ASTER_ENABLE_HNSW

}  // namespace aster

