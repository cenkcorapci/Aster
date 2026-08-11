#include "aster/index/hnsw_build.h"

#if ASTER_ENABLE_HNSW

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>

#include "aster/index/distance.h"

namespace aster {
namespace {

struct ScoredNode {
  float score = 0.0f;  // higher is better
  uint32_t id = 0;
};

// Max-heap by score (best candidate first).
struct BetterFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const {
    if (a.score != b.score) return a.score < b.score;
    return a.id > b.id;
  }
};

// Min-heap by score (worst of the current best at top).
struct WorseFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
  }
};

float NodeScore(Metric metric, VectorView query,
                const std::vector<std::vector<float>>& vectors, uint32_t id) {
  return Score(metric, query, vectors[id]);
}

// Beam search on one layer. Returns up to `ef` nodes, best-first by score.
std::vector<ScoredNode> SearchLayer(
    Metric metric, VectorView query,
    const std::vector<std::vector<float>>& vectors, const HnswGraph& graph,
    const std::vector<uint32_t>& entry_points, uint32_t ef, uint16_t layer) {
  if (entry_points.empty() || ef == 0) return {};

  std::unordered_set<uint32_t> visited;
  visited.reserve(ef * 4);

  std::priority_queue<ScoredNode, std::vector<ScoredNode>, BetterFirst>
      candidates;
  std::priority_queue<ScoredNode, std::vector<ScoredNode>, WorseFirst> w;

  for (uint32_t ep : entry_points) {
    if (!visited.insert(ep).second) continue;
    const float s = NodeScore(metric, query, vectors, ep);
    candidates.push({s, ep});
    w.push({s, ep});
  }
  if (w.empty()) return {};

  while (w.size() > ef) w.pop();

  while (!candidates.empty()) {
    const ScoredNode c = candidates.top();
    candidates.pop();
    if (c.score < w.top().score) break;

    for (uint32_t e : graph.Neighbors(c.id, layer)) {
      if (!visited.insert(e).second) continue;
      const float s = NodeScore(metric, query, vectors, e);
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
  // Best first.
  std::sort(result.begin(), result.end(),
            [](const ScoredNode& a, const ScoredNode& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.id < b.id;
            });
  return result;
}

uint32_t MaxDegree(const HnswParams& params, uint16_t layer) {
  return layer == 0 ? HnswLayer0MaxDegree(params) : params.m;
}

Status ConnectBidirectional(
    Metric metric, HnswGraph& graph,
    const std::vector<std::vector<float>>& vectors, uint32_t new_node,
    uint16_t layer, const std::vector<uint32_t>& neighbors) {
  const uint32_t limit = MaxDegree(graph.params(), layer);
  auto st = graph.SetNeighbors(new_node, layer, neighbors);
  if (!st.ok()) return st;

  for (uint32_t n : neighbors) {
    std::vector<uint32_t> links = graph.Neighbors(n, layer);
    if (std::find(links.begin(), links.end(), new_node) == links.end()) {
      links.push_back(new_node);
    }
    if (links.size() > limit) {
      std::vector<std::pair<uint32_t, VectorView>> cands;
      cands.reserve(links.size());
      for (uint32_t id : links) {
        cands.emplace_back(id, VectorView{vectors[id]});
      }
      links = HnswBuilder::SelectNeighborsHeuristic(
          metric, vectors[n], cands, limit);
    }
    st = graph.SetNeighbors(n, layer, std::move(links));
    if (!st.ok()) return st;
  }
  return Status::Ok();
}

}  // namespace

HnswBuilder::HnswBuilder(Metric metric, HnswParams params, uint64_t rng_seed)
    : metric_(metric), params_(params), rng_(rng_seed) {}

uint8_t HnswBuilder::SampleLevel() {
  if (params_.max_layers == 0) return 0;
  if (params_.m <= 1) return 0;

  std::uniform_real_distribution<double> uni(
      std::numeric_limits<double>::min(), 1.0);
  const double u = uni(rng_);
  const double ml = 1.0 / std::log(static_cast<double>(params_.m));
  auto level = static_cast<uint32_t>(std::floor(-std::log(u) * ml));
  const uint32_t cap = params_.max_layers - 1;
  if (level > cap) level = cap;
  return static_cast<uint8_t>(level);
}

std::vector<uint32_t> HnswBuilder::SelectNeighborsHeuristic(
    Metric metric, VectorView base,
    const std::vector<std::pair<uint32_t, VectorView>>& candidates,
    uint32_t max_keep) {
  if (max_keep == 0 || candidates.empty()) return {};

  struct Cand {
    uint32_t id;
    VectorView vec;
    float score_to_base;
  };
  std::vector<Cand> ordered;
  ordered.reserve(candidates.size());
  for (const auto& [id, vec] : candidates) {
    ordered.push_back({id, vec, Score(metric, base, vec)});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const Cand& a, const Cand& b) {
              if (a.score_to_base != b.score_to_base) {
                return a.score_to_base > b.score_to_base;
              }
              return a.id < b.id;
            });

  std::vector<uint32_t> selected;
  std::vector<VectorView> selected_vecs;
  selected.reserve(max_keep);
  selected_vecs.reserve(max_keep);

  for (const Cand& c : ordered) {
    if (selected.size() >= max_keep) break;
    bool keep = true;
    for (size_t i = 0; i < selected.size(); ++i) {
      // Closer to an already-kept neighbor than to base → reject (clustered).
      if (Score(metric, c.vec, selected_vecs[i]) > c.score_to_base) {
        keep = false;
        break;
      }
    }
    if (keep) {
      selected.push_back(c.id);
      selected_vecs.push_back(c.vec);
    }
  }
  return selected;
}

Result<HnswGraph> HnswBuilder::Build(
    const std::vector<std::vector<float>>& vectors) {
  std::vector<uint32_t> ordinals(vectors.size());
  for (uint32_t i = 0; i < ordinals.size(); ++i) ordinals[i] = i;
  return Build(vectors, ordinals);
}

Result<HnswGraph> HnswBuilder::Build(
    const std::vector<std::vector<float>>& vectors,
    const std::vector<uint32_t>& row_ordinals) {
  if (vectors.size() != row_ordinals.size()) {
    return Status::InvalidArgument("hnsw build: row_ordinals size mismatch");
  }
  if (params_.m == 0) {
    return Status::InvalidArgument("hnsw build: m must be > 0");
  }
  if (params_.ef_construction == 0) {
    return Status::InvalidArgument("hnsw build: ef_construction must be > 0");
  }
  if (params_.max_layers == 0 || params_.max_layers > 255) {
    return Status::InvalidArgument("hnsw build: max_layers out of range");
  }
  for (size_t i = 0; i < vectors.size(); ++i) {
    if (vectors[i].empty()) {
      return Status::InvalidArgument("hnsw build: empty vector");
    }
    if (i > 0 && vectors[i].size() != vectors[0].size()) {
      return Status::InvalidArgument("hnsw build: dimension mismatch");
    }
  }

  HnswGraph graph(params_);
  if (vectors.empty()) return graph;

  for (uint32_t i = 0; i < static_cast<uint32_t>(vectors.size()); ++i) {
    const uint8_t level = SampleLevel();
    const uint16_t old_max_level = graph.max_level();
    const uint32_t old_entry = graph.entry_point();

    auto added = graph.AddNode(row_ordinals[i], level);
    if (!added.ok()) return added.status();
    const uint32_t node = added.value();

    if (node == 0) {
      // First point: isolated entry; higher layers stay empty.
      continue;
    }

    VectorView query{vectors[i]};
    uint32_t ep = old_entry;

    // Greedy descent from the previous top down to level+1.
    if (old_max_level > level) {
      for (uint16_t lc = old_max_level; lc > level; --lc) {
        auto nearest =
            SearchLayer(metric_, query, vectors, graph, {ep}, /*ef=*/1, lc);
        if (!nearest.empty()) ep = nearest.front().id;
      }
    }

    const uint16_t insert_top =
        static_cast<uint16_t>(std::min<uint32_t>(old_max_level, level));
    for (int lc = static_cast<int>(insert_top); lc >= 0; --lc) {
      const auto layer = static_cast<uint16_t>(lc);
      auto found = SearchLayer(metric_, query, vectors, graph, {ep},
                               params_.ef_construction, layer);
      std::vector<std::pair<uint32_t, VectorView>> cands;
      cands.reserve(found.size());
      for (const ScoredNode& sn : found) {
        if (sn.id == node) continue;
        cands.emplace_back(sn.id, VectorView{vectors[sn.id]});
      }
      auto neighbors = SelectNeighborsHeuristic(
          metric_, query, cands, MaxDegree(params_, layer));
      auto st = ConnectBidirectional(metric_, graph, vectors, node, layer,
                                     neighbors);
      if (!st.ok()) return st;
      if (!found.empty()) ep = found.front().id;
    }
  }

  return graph;
}

}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
