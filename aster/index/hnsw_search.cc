#include "aster/index/hnsw_search.h"

#if ASTER_ENABLE_HNSW

#include <algorithm>
#include <memory>
#include <queue>
#include <unordered_set>
#include <utility>

#include "aster/index/distance.h"
#include "aster/index/hnsw_build.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

struct ScoredNode {
  float score = 0.0f;  // higher is better
  uint32_t id = 0;
};

struct BetterFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const {
    if (a.score != b.score) return a.score < b.score;
    return a.id > b.id;
  }
};

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
  std::sort(result.begin(), result.end(),
            [](const ScoredNode& a, const ScoredNode& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.id < b.id;
            });
  return result;
}

class HnswIndex final : public VectorIndex {
 public:
  HnswIndex(Metric metric, HnswGraph graph,
            std::vector<std::vector<float>> vectors, std::vector<RowId> ids)
      : metric_(metric),
        graph_(std::move(graph)),
        vectors_(std::move(vectors)),
        ids_(std::move(ids)) {}

  size_t size() const override { return vectors_.size(); }

  std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                uint32_t ef_search) const override {
    auto nodes =
        HnswSearch(metric_, graph_, vectors_, query, top_k, ef_search);
    std::vector<SearchHit> hits;
    hits.reserve(nodes.size());
    for (const auto& [node, score] : nodes) {
      hits.push_back({ids_[node], score});
    }
    return hits;
  }

 private:
  Metric metric_;
  HnswGraph graph_;
  std::vector<std::vector<float>> vectors_;
  std::vector<RowId> ids_;
};

}  // namespace

std::vector<std::pair<uint32_t, float>> HnswSearch(
    Metric metric, const HnswGraph& graph,
    const std::vector<std::vector<float>>& vectors, VectorView query,
    uint32_t top_k, uint32_t ef_search) {
  if (graph.node_count() == 0 || top_k == 0 || query.empty()) return {};
  if (vectors.size() != graph.node_count()) return {};
  if (graph.entry_point() == HnswGraph::kNoEntry) return {};

  uint32_t ef = ef_search;
  if (ef == 0) ef = graph.params().ef_search_default;
  if (ef < top_k) ef = top_k;

  uint32_t ep = graph.entry_point();
  const uint16_t top = graph.max_level();
  for (uint16_t lc = top; lc > 0; --lc) {
    auto nearest = SearchLayer(metric, query, vectors, graph, {ep},
                               /*ef=*/1, lc);
    if (!nearest.empty()) ep = nearest.front().id;
  }

  auto found =
      SearchLayer(metric, query, vectors, graph, {ep}, ef, /*layer=*/0);
  if (found.size() > top_k) found.resize(top_k);

  std::vector<std::pair<uint32_t, float>> out;
  out.reserve(found.size());
  for (const ScoredNode& sn : found) {
    out.emplace_back(sn.id, sn.score);
  }
  return out;
}

std::unique_ptr<VectorIndex> BuildHnswIndex(Metric metric, HnswParams params,
                                            std::vector<IndexEntry> entries,
                                            uint64_t rng_seed) {
  std::vector<std::vector<float>> vectors;
  std::vector<RowId> ids;
  vectors.reserve(entries.size());
  ids.reserve(entries.size());
  for (auto& e : entries) {
    ids.push_back(std::move(e.id));
    vectors.emplace_back(e.vector.begin(), e.vector.end());
  }

  HnswBuilder builder(metric, params, rng_seed);
  auto built = builder.Build(vectors);
  if (!built.ok()) {
    // Degenerate: empty searchable index on build failure.
    return std::make_unique<HnswIndex>(metric, HnswGraph(params),
                                       std::vector<std::vector<float>>{},
                                       std::vector<RowId>{});
  }
  return std::make_unique<HnswIndex>(metric, std::move(built).value(),
                                     std::move(vectors), std::move(ids));
}

std::unique_ptr<VectorIndex> RebuildHnswFromLiveRows(
    Metric metric, HnswParams params, const std::vector<Row>& rows,
    uint64_t rng_seed) {
  std::vector<IndexEntry> entries;
  entries.reserve(rows.size());
  for (const Row& row : rows) {
    if (row.tombstone || row.vector.empty()) continue;
    entries.push_back({row.id, row.vector});
  }
  return BuildHnswIndex(metric, params, std::move(entries), rng_seed);
}

}  // namespace aster

#else  // !ASTER_ENABLE_HNSW

namespace aster {
namespace {
[[maybe_unused]] constexpr int kHnswSearchDisabledStub = 0;
}  // namespace
}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
