#include "aster/index/hnsw_search.h"

#if ASTER_ENABLE_HNSW

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "aster/index/distance.h"
#include "aster/index/hnsw_build.h"
#include "aster/index/hnsw_search_layer.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

using hnsw_internal::BruteForceTopK;
using hnsw_internal::BuildNormCache;
using hnsw_internal::kBruteForceThreshold;
using hnsw_internal::QueryNorm;
using hnsw_internal::ScoredNode;
using hnsw_internal::SearchLayer;
using hnsw_internal::VectorStore;

std::vector<std::pair<uint32_t, float>> HnswSearchPacked(
    Metric metric, const HnswGraph& graph, const CompactVectorStore& packed,
    const std::vector<uint32_t>& node_slots, const std::vector<float>& norms,
    VectorView query, uint32_t top_k, uint32_t ef_search) {
  if (graph.node_count() == 0 || top_k == 0 || query.empty()) return {};
  if (node_slots.size() != graph.node_count()) return {};
  if (graph.entry_point() == HnswGraph::kNoEntry) return {};

  const VectorStore store{nullptr,
                          &packed,
                          &node_slots,
                          metric == Metric::kCosine && !norms.empty() ? &norms
                                                                     : nullptr,
                          metric,
                          graph.node_count()};
  const float qnorm = QueryNorm(metric, query);

  if (graph.node_count() <= kBruteForceThreshold) {
    auto found = BruteForceTopK(store, query, qnorm, top_k);
    std::vector<std::pair<uint32_t, float>> out;
    out.reserve(found.size());
    for (const ScoredNode& sn : found) out.emplace_back(sn.id, sn.score);
    return out;
  }

  uint32_t ef = ef_search;
  if (ef == 0) ef = graph.params().ef_search_default;
  if (ef < top_k) ef = top_k;

  const auto graph_neighbors = [&graph](uint32_t node, uint16_t layer) {
    return graph.Neighbors(node, layer);
  };

  uint32_t ep = graph.entry_point();
  const uint16_t top = graph.max_level();
  for (uint16_t lc = top; lc > 0; --lc) {
    auto nearest =
        SearchLayer(store, query, qnorm, graph_neighbors, {ep}, /*ef=*/1, lc);
    if (!nearest.empty()) ep = nearest.front().id;
  }

  auto found =
      SearchLayer(store, query, qnorm, graph_neighbors, {ep}, ef, /*layer=*/0);
  if (found.size() > top_k) found.resize(top_k);

  std::vector<std::pair<uint32_t, float>> out;
  out.reserve(found.size());
  for (const ScoredNode& sn : found) out.emplace_back(sn.id, sn.score);
  return out;
}

class HnswIndex final : public VectorIndex {
 public:
  HnswIndex(Metric metric, HnswGraph graph,
            std::vector<std::vector<float>> vectors, std::vector<RowId> ids)
      : metric_(metric),
        graph_(std::move(graph)),
        vectors_(std::move(vectors)),
        ids_(std::move(ids)),
        norms_(BuildNormCache(metric_, vectors_)) {}

  HnswIndex(Metric metric, HnswGraph graph,
            std::shared_ptr<const CompactVectorStore> packed,
            std::vector<uint32_t> node_slots, std::vector<RowId> ids,
            std::vector<float> norms)
      : metric_(metric),
        graph_(std::move(graph)),
        packed_(std::move(packed)),
        node_slots_(std::move(node_slots)),
        ids_(std::move(ids)),
        norms_(std::move(norms)) {}

  size_t size() const override {
    return packed_ ? ids_.size() : vectors_.size();
  }

  std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                uint32_t ef_search) const override {
    std::vector<std::pair<uint32_t, float>> nodes;
    if (packed_) {
      nodes = HnswSearchPacked(metric_, graph_, *packed_, node_slots_, norms_,
                               query, top_k, ef_search);
    } else {
      nodes = HnswSearch(metric_, graph_, vectors_, norms_, query, top_k,
                         ef_search);
    }
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
  std::shared_ptr<const CompactVectorStore> packed_;
  std::vector<uint32_t> node_slots_;
  std::vector<RowId> ids_;
  std::vector<float> norms_;
};

}  // namespace

std::unique_ptr<VectorIndex> BuildHnswIndexFromGraph(
    Metric metric, HnswGraph graph, std::vector<std::vector<float>> vectors,
    std::vector<RowId> ids) {
  return std::make_unique<HnswIndex>(metric, std::move(graph),
                                      std::move(vectors), std::move(ids));
}

std::unique_ptr<VectorIndex> LoadHnswIndexFromGraph(
    Metric metric, HnswGraph graph, const std::vector<Row>& rows,
    std::shared_ptr<const CompactVectorStore> packed,
    std::shared_ptr<const std::vector<uint32_t>> live_slots) {
  const uint32_t n = graph.node_count();
  if (n == 0) {
    return BuildHnswIndexFromGraph(metric, std::move(graph), {}, {});
  }

  if (packed && live_slots) {
    std::vector<uint32_t> node_slots;
    std::vector<RowId> ids;
    node_slots.reserve(n);
    ids.reserve(n);
    std::vector<float> norms;
    if (metric == Metric::kCosine) norms.reserve(n);
    thread_local std::vector<float> scratch;
    scratch.resize(packed->dimension());
    for (uint32_t node = 0; node < n; ++node) {
      const uint32_t ord = graph.RowOrdinal(node);
      if (ord >= rows.size() || ord >= live_slots->size()) return nullptr;
      const Row& row = rows[ord];
      if (row.tombstone) return nullptr;
      const uint32_t slot = (*live_slots)[ord];
      if (slot == 0xFFFFFFFFu) return nullptr;
      node_slots.push_back(slot);
      ids.push_back(row.id);
      if (metric == Metric::kCosine) {
        if (!packed->Decode(slot, scratch).ok()) return nullptr;
        float n2 = 0.0f;
        for (float x : scratch) n2 += x * x;
        norms.push_back(std::sqrt(n2));
      }
    }
    return std::make_unique<HnswIndex>(metric, std::move(graph),
                                       std::move(packed), std::move(node_slots),
                                       std::move(ids), std::move(norms));
  }

  std::vector<std::vector<float>> vectors;
  std::vector<RowId> ids;
  vectors.reserve(n);
  ids.reserve(n);
  for (uint32_t node = 0; node < n; ++node) {
    const uint32_t ord = graph.RowOrdinal(node);
    if (ord >= rows.size()) return nullptr;
    const Row& row = rows[ord];
    if (row.tombstone || row.vector.empty()) return nullptr;
    vectors.emplace_back(row.vector.begin(), row.vector.end());
    ids.push_back(row.id);
  }
  return BuildHnswIndexFromGraph(metric, std::move(graph), std::move(vectors),
                                  std::move(ids));
}

std::vector<std::pair<uint32_t, float>> HnswSearch(
    Metric metric, const HnswGraph& graph,
    const std::vector<std::vector<float>>& vectors,
    const std::vector<float>& norms, VectorView query, uint32_t top_k,
    uint32_t ef_search) {
  if (graph.node_count() == 0 || top_k == 0 || query.empty()) return {};
  if (vectors.size() != graph.node_count()) return {};
  if (graph.entry_point() == HnswGraph::kNoEntry) return {};
  if (metric == Metric::kCosine && !norms.empty() &&
      norms.size() != vectors.size()) {
    return {};
  }

  const VectorStore store{&vectors,
                          nullptr,
                          nullptr,
                          metric == Metric::kCosine && !norms.empty() ? &norms
                                                                      : nullptr,
                          metric,
                          graph.node_count()};
  const float qnorm = QueryNorm(metric, query);

  if (graph.node_count() <= kBruteForceThreshold) {
    auto found = BruteForceTopK(store, query, qnorm, top_k);
    std::vector<std::pair<uint32_t, float>> out;
    out.reserve(found.size());
    for (const ScoredNode& sn : found) {
      out.emplace_back(sn.id, sn.score);
    }
    return out;
  }

  uint32_t ef = ef_search;
  if (ef == 0) ef = graph.params().ef_search_default;
  if (ef < top_k) ef = top_k;

  const auto graph_neighbors = [&graph](uint32_t node, uint16_t layer) {
    return graph.Neighbors(node, layer);
  };

  uint32_t ep = graph.entry_point();
  const uint16_t top = graph.max_level();
  for (uint16_t lc = top; lc > 0; --lc) {
    auto nearest =
        SearchLayer(store, query, qnorm, graph_neighbors, {ep}, /*ef=*/1, lc);
    if (!nearest.empty()) ep = nearest.front().id;
  }

  auto found =
      SearchLayer(store, query, qnorm, graph_neighbors, {ep}, ef, /*layer=*/0);
  if (found.size() > top_k) found.resize(top_k);

  std::vector<std::pair<uint32_t, float>> out;
  out.reserve(found.size());
  for (const ScoredNode& sn : found) {
    out.emplace_back(sn.id, sn.score);
  }
  return out;
}

std::vector<std::pair<uint32_t, float>> HnswSearch(
    Metric metric, const HnswGraph& graph,
    const std::vector<std::vector<float>>& vectors, VectorView query,
    uint32_t top_k, uint32_t ef_search) {
  const std::vector<float> norms =
      metric == Metric::kCosine ? BuildNormCache(metric, vectors)
                                  : std::vector<float>{};
  return HnswSearch(metric, graph, vectors, norms, query, top_k, ef_search);
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

  if (vectors.empty()) {
    return std::make_unique<HnswIndex>(metric, HnswGraph(params),
                                       std::vector<std::vector<float>>{},
                                       std::vector<RowId>{});
  }

  HnswBuilder builder(metric, params, rng_seed);
  auto built = builder.Build(vectors);
  if (!built.ok()) {
    return nullptr;
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
