#pragma once

#include "aster/core/features.h"

#if ASTER_ENABLE_HNSW

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

#include "aster/index/distance.h"
#include "aster/index/hnsw_graph.h"

namespace aster {
namespace hnsw_internal {

// Bitmap visited set — O(1) test/set, no hash allocations on the hot path.
class VisitBitmap {
 public:
  explicit VisitBitmap(uint32_t node_count)
      : words_((static_cast<size_t>(node_count) + 63) / 64, 0) {}

  [[nodiscard]] bool TestAndSet(uint32_t id) noexcept {
    const size_t word = static_cast<size_t>(id >> 6);
    if (word >= words_.size()) return false;
    const uint64_t mask = 1ull << (id & 63);
    uint64_t& w = words_[word];
    if (w & mask) return false;
    w |= mask;
    return true;
  }

 private:
  std::vector<uint64_t> words_;
};

struct ScoredNode {
  float score = 0.0f;  // higher is better
  uint32_t id = 0;
};

struct BetterFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const noexcept {
    if (a.score != b.score) return a.score < b.score;
    return a.id > b.id;
  }
};

struct WorseFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const noexcept {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
  }
};

struct VectorStore {
  const std::vector<std::vector<float>>* vectors = nullptr;
  const std::vector<float>* norms = nullptr;  // parallel to vectors when cosine
  Metric metric = Metric::kCosine;
};

inline float QueryNorm(Metric metric, VectorView query) noexcept {
  if (metric != Metric::kCosine) return 0.0f;
  float n2 = 0.0f;
  for (float x : query) n2 += x * x;
  return std::sqrt(n2);
}

inline bool HasNormCache(const VectorStore& store) noexcept {
  return store.metric == Metric::kCosine && store.norms != nullptr &&
         store.vectors != nullptr &&
         store.norms->size() == store.vectors->size();
}

inline float NodeScore(const VectorStore& store, VectorView query,
                       float qnorm, uint32_t id) noexcept {
  const VectorView vec{store.vectors->at(id)};
  if (HasNormCache(store)) {
    return CosineSimilarityPreNorm(query, qnorm, vec, store.norms->at(id));
  }
  return Score(store.metric, query, vec);
}

inline std::vector<float> BuildNormCache(
    Metric metric, const std::vector<std::vector<float>>& vectors) {
  if (metric != Metric::kCosine) return {};
  std::vector<float> norms;
  norms.reserve(vectors.size());
  for (const auto& v : vectors) {
    float n2 = 0.0f;
    for (float x : v) n2 += x * x;
    norms.push_back(std::sqrt(n2));
  }
  return norms;
}

// Brute-force top-k when the graph is tiny (cheaper than HNSW overhead).
inline constexpr uint32_t kBruteForceThreshold = 128;

inline std::vector<ScoredNode> BruteForceTopK(
    const VectorStore& store, VectorView query, float qnorm, uint32_t top_k) {
  if (top_k == 0 || store.vectors == nullptr || store.vectors->empty()) {
    return {};
  }
  using Node = std::pair<float, uint32_t>;
  auto worse = [](const Node& a, const Node& b) noexcept {
    if (a.first != b.first) return a.first > b.first;
    return a.second < b.second;  // tie-break: keep lower id (matches SearchLayer)
  };
  std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);

  const uint32_t n = static_cast<uint32_t>(store.vectors->size());
  for (uint32_t id = 0; id < n; ++id) {
    const float s = NodeScore(store, query, qnorm, id);
    if (heap.size() < top_k) {
      heap.emplace(s, id);
    } else if (s > heap.top().first) {
      heap.pop();
      heap.emplace(s, id);
    }
  }

  std::vector<ScoredNode> out(heap.size());
  for (size_t i = out.size(); i > 0; --i) {
    const auto [score, id] = heap.top();
    heap.pop();
    out[i - 1] = {score, id};
  }
  return out;
}

template <typename NeighborFn>
std::vector<ScoredNode> SearchLayer(
    const VectorStore& store, VectorView query, float qnorm, NeighborFn neighbors,
    const std::vector<uint32_t>& entry_points, uint32_t ef, uint16_t layer) {
  if (entry_points.empty() || ef == 0) return {};

  VisitBitmap visited(static_cast<uint32_t>(store.vectors->size()));

  std::priority_queue<ScoredNode, std::vector<ScoredNode>, BetterFirst>
      candidates;
  std::priority_queue<ScoredNode, std::vector<ScoredNode>, WorseFirst> w;

  for (uint32_t ep : entry_points) {
    if (!visited.TestAndSet(ep)) continue;
    const float s = NodeScore(store, query, qnorm, ep);
    candidates.push({s, ep});
    w.push({s, ep});
  }
  if (w.empty()) return {};

  while (w.size() > ef) w.pop();

  while (!candidates.empty()) {
    const ScoredNode c = candidates.top();
    candidates.pop();
    if (c.score < w.top().score) break;

    for (uint32_t e : neighbors(c.id, layer)) {
      if (!visited.TestAndSet(e)) continue;
      const float s = NodeScore(store, query, qnorm, e);
      if (w.size() < ef || s > w.top().score) {
        candidates.push({s, e});
        w.push({s, e});
        if (w.size() > ef) w.pop();
      }
    }
  }

  std::vector<ScoredNode> result;
  result.reserve(w.size());
  while (!w.empty()) {
    result.push_back(w.top());
    w.pop();
  }
  std::sort(result.begin(), result.end(),
            [](const ScoredNode& a, const ScoredNode& b) noexcept {
              if (a.score != b.score) return a.score > b.score;
              return a.id < b.id;
            });
  return result;
}

}  // namespace hnsw_internal
}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
