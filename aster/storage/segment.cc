#include "aster/storage/segment.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <utility>
#include <unordered_set>

#include "aster/core/features.h"
#include "aster/index/distance.h"

#if ASTER_ENABLE_HNSW
#include <random>

#include "aster/index/hnsw_build.h"
#include "aster/index/hnsw_graph.h"
#include "aster/index/vector_index.h"
#endif

namespace aster {
namespace {

// Bitmap-driven exact scan over matching ordinals (filtered search fallback
// and the exact path when a tag predicate is present).
std::vector<SearchHit> ScoreMatchingOrdinals(
    Metric metric, const std::vector<Row>& rows, const TagBitmap& matching,
    VectorView query, uint32_t top_k) {
  if (matching.empty() || top_k == 0 || query.empty()) return {};

  float qnorm = 0.0f;
  if (metric == Metric::kCosine) {
    for (float x : query) qnorm += x * x;
    qnorm = std::sqrt(qnorm);
  }

  using Node = std::pair<float, uint32_t>;
  auto worse = [](const Node& a, const Node& b) { return a.first > b.first; };
  std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);

  for (uint32_t ord : matching.ordinals()) {
    if (ord >= rows.size()) continue;
    const Row& row = rows[ord];
    if (row.tombstone || row.vector.empty()) continue;
    float score;
    if (metric == Metric::kCosine) {
      float n2 = 0.0f;
      for (float x : row.vector) n2 += x * x;
      score = CosineSimilarityPreNorm(query, qnorm, row.vector, std::sqrt(n2));
    } else {
      score = Score(metric, query, row.vector);
    }
    if (heap.size() < top_k) {
      heap.emplace(score, ord);
    } else if (score > heap.top().first) {
      heap.pop();
      heap.emplace(score, ord);
    }
  }

  std::vector<SearchHit> hits(heap.size());
  for (size_t i = hits.size(); i > 0; --i) {
    const auto [score, ord] = heap.top();
    heap.pop();
    hits[i - 1] = {rows[ord].id, score};
  }
  return hits;
}

}  // namespace

std::shared_ptr<const Segment> Segment::Build(
    uint64_t id, Metric metric, std::shared_ptr<std::vector<Row>> rows) {
  std::shared_ptr<const std::vector<Row>> shared = std::move(rows);
  TagIndex tags = TagIndex::Build(*shared);
  auto index = BuildExactIndex(metric, shared);
  return std::shared_ptr<const Segment>(new Segment(
      id, metric, std::move(shared), std::move(index), std::move(tags)));
}

std::shared_ptr<const Segment> Segment::Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows) {
  return Build(id, metric,
               std::make_shared<std::vector<Row>>(std::move(rows)));
}

bool Segment::TryBeginIndexBuild() const {
  if (index_state_ != SegState::kPending) return false;
  index_state_ = SegState::kBuilding;
  return true;
}

void Segment::CompleteIndexBuild(std::unique_ptr<VectorIndex> hnsw) const {
  if (index_state_ != SegState::kBuilding) return;
  hnsw_index_ = std::move(hnsw);
  index_state_ = SegState::kReady;
}

void Segment::AbortIndexBuild() const {
  if (index_state_ != SegState::kBuilding) return;
  hnsw_index_.reset();
  index_state_ = SegState::kPending;
}

std::optional<Row> Segment::Get(const RowId& row_id) const {
  const auto& rows = *rows_;
  auto it = std::lower_bound(
      rows.begin(), rows.end(), row_id,
      [](const Row& row, const RowId& id) { return row.id < id; });
  if (it == rows.end() || it->id != row_id) return std::nullopt;
  return *it;
}

std::vector<SearchHit> Segment::Search(
    VectorView query, uint32_t top_k, uint32_t ef_search,
    const std::set<std::string>& tags) const {
  if (tags.empty()) {
    // docs/indexing.md §4.2: READY → HNSW; otherwise exact scan.
    if (index_state_ == SegState::kReady && hnsw_index_ != nullptr) {
      return hnsw_index_->Search(query, top_k, ef_search);
    }
    return index_->Search(query, top_k, ef_search);
  }
  // Tag predicate: score only bitmap-matching ordinals so non-matches never
  // consume fetch_k slots (docs/indexing.md §7).
  return ScoreMatchingOrdinals(metric_, *rows_, tag_index_.Matching(tags),
                               query, top_k);
}

std::shared_ptr<const Segment> CompactSegments(
    uint64_t new_id, Metric metric,
    const std::vector<std::shared_ptr<const Segment>>& inputs,
    bool drop_tombstones
#if ASTER_ENABLE_HNSW
    ,
    HnswParams hnsw_params, uint64_t hnsw_rng_seed,
    bool enable_insert_into_largest, double insert_largest_staleness_debt_threshold
#endif
) {
  // LWW merge: for each id keep the newest version across all inputs.
  std::map<RowId, Row> merged;
  for (const auto& segment : inputs) {
    for (const Row& row : segment->rows()) {
      auto it = merged.find(row.id);
      if (it == merged.end()) {
        merged.emplace(row.id, row);
      } else if (NewerThan(row, it->second)) {
        it->second = row;
      }
    }
  }

  auto rows = std::make_shared<std::vector<Row>>();
  rows->reserve(merged.size());
  for (auto& [_, row] : merged) {
    if (drop_tombstones && row.tombstone) continue;
    rows->push_back(std::move(row));
  }
  auto segment = Segment::Build(new_id, metric, std::move(rows));
#if ASTER_ENABLE_HNSW
  if (segment->TryBeginIndexBuild()) {
    // Output live id set: used for staleness-debt estimation (deleted rows
    // become "ghosts" when we reuse an input graph).
    std::unordered_set<RowId> live_output_ids;
    live_output_ids.reserve(segment->row_count());
    for (const Row& row : segment->rows()) {
      if (row.tombstone || row.vector.empty()) continue;
      live_output_ids.insert(row.id);
    }

    auto Rebuild = [&]() {
      return RebuildHnswFromLiveRows(metric, hnsw_params, segment->rows(),
                                     hnsw_rng_seed);
    };

    if (!enable_insert_into_largest || inputs.size() < 2 ||
        live_output_ids.empty()) {
      segment->CompleteIndexBuild(Rebuild());
    } else {
      struct LiveInput {
        std::vector<std::vector<float>> vectors;
        std::vector<RowId> ids;
      };
      std::vector<LiveInput> live_inputs(inputs.size());
      size_t total_live_nodes = 0;
      size_t ghost_nodes = 0;
      size_t largest_idx = 0;
      size_t largest_live_nodes = 0;

      for (size_t si = 0; si < inputs.size(); ++si) {
        for (const Row& row : inputs[si]->rows()) {
          if (row.tombstone || row.vector.empty()) continue;
          live_inputs[si].vectors.push_back(row.vector);
          live_inputs[si].ids.push_back(row.id);
          ++total_live_nodes;
          if (live_output_ids.count(row.id) == 0) ++ghost_nodes;
        }
        if (live_inputs[si].vectors.size() > largest_live_nodes) {
          largest_live_nodes = live_inputs[si].vectors.size();
          largest_idx = si;
        }
      }

      const double ghost_fraction =
          total_live_nodes == 0
              ? 0.0
              : static_cast<double>(ghost_nodes) /
                    static_cast<double>(total_live_nodes);

      auto SampleLevel = [&](std::mt19937_64& rng) -> uint8_t {
        if (hnsw_params.max_layers == 0) return 0;
        if (hnsw_params.m <= 1) return 0;

        std::uniform_real_distribution<double> uni(
            std::numeric_limits<double>::min(), 1.0);
        const double u = uni(rng);
        const double ml = 1.0 / std::log(static_cast<double>(hnsw_params.m));
        auto level =
            static_cast<uint32_t>(std::floor(-std::log(u) * ml));
        const uint32_t cap = hnsw_params.max_layers - 1;
        if (level > cap) level = cap;
        return static_cast<uint8_t>(level);
      };

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

      auto NodeScore = [&](VectorView query,
                           const std::vector<std::vector<float>>& vectors,
                           uint32_t id) -> float {
        return Score(metric, query, vectors[id]);
      };

      auto SearchLayer = [&](VectorView query, const std::vector<uint32_t>& eps,
                              uint32_t ef, uint16_t layer,
                              const HnswGraph& graph,
                              const std::vector<std::vector<float>>& vectors)
          -> std::vector<ScoredNode> {
        if (eps.empty() || ef == 0) return {};
        std::unordered_set<uint32_t> visited;
        visited.reserve(ef * 4);

        std::priority_queue<ScoredNode, std::vector<ScoredNode>, BetterFirst>
            candidates;
        std::priority_queue<ScoredNode, std::vector<ScoredNode>, WorseFirst>
            w;

        for (uint32_t ep : eps) {
          if (!visited.insert(ep).second) continue;
          const float s = NodeScore(query, vectors, ep);
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
            const float s = NodeScore(query, vectors, e);
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
      };

      auto MaxDegree = [&](uint16_t layer) -> uint32_t {
        return layer == 0 ? HnswLayer0MaxDegree(hnsw_params) : hnsw_params.m;
      };

      auto ConnectBidirectional = [&](HnswGraph& graph,
                                       const std::vector<std::vector<float>>& vectors,
                                       uint32_t new_node, uint16_t layer,
                                       const std::vector<uint32_t>& neighbors)
          -> Status {
        const uint32_t limit = MaxDegree(layer);
        if (auto st = graph.SetNeighbors(new_node, layer, neighbors);
            !st.ok()) {
          return st;
        }

        for (uint32_t n : neighbors) {
          std::vector<uint32_t> links = graph.Neighbors(n, layer);
          if (std::find(links.begin(), links.end(), new_node) ==
              links.end()) {
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
          auto st = graph.SetNeighbors(n, layer, std::move(links));
          if (!st.ok()) return st;
        }
        return Status::Ok();
      };

      auto GhostyGraph = [&]() -> std::unique_ptr<VectorIndex> {
        // Build base graph from the largest input only, then insert all other
        // inputs' live vectors into it (standard HNSW sequential inserts).
        HnswBuilder builder(metric, hnsw_params, hnsw_rng_seed);
        auto built = builder.Build(live_inputs[largest_idx].vectors);
        if (!built.ok()) return Rebuild();

        HnswGraph graph = std::move(built).value();
        graph.set_segment_id(new_id);

        std::vector<std::vector<float>> vectors =
            live_inputs[largest_idx].vectors;
        std::vector<RowId> ids = live_inputs[largest_idx].ids;

        std::mt19937_64 rng(hnsw_rng_seed + 1);

        for (size_t si = 0; si < inputs.size(); ++si) {
          if (si == largest_idx) continue;
          for (size_t vi = 0; vi < live_inputs[si].vectors.size(); ++vi) {
            const std::vector<float>& vec = live_inputs[si].vectors[vi];
            const RowId& id = live_inputs[si].ids[vi];

            const uint16_t old_max_level = graph.max_level();
            const uint32_t old_entry = graph.entry_point();
            const uint8_t level = SampleLevel(rng);

            const uint32_t row_ordinal = static_cast<uint32_t>(vectors.size());
            auto added = graph.AddNode(row_ordinal, level);
            if (!added.ok()) return Rebuild();
            vectors.push_back(vec);
            ids.push_back(id);

            const uint32_t node = added.value();
            if (node == 0) continue;

            VectorView query{vectors[node]};
            uint32_t ep = old_entry;

            // Greedy descent from previous top down to level+1.
            if (old_max_level > level) {
              for (uint16_t lc = old_max_level; lc > level; --lc) {
                auto nearest =
                    SearchLayer(query, {ep}, /*ef=*/1, lc, graph, vectors);
                if (!nearest.empty()) ep = nearest.front().id;
              }
            }

            const uint16_t insert_top =
                static_cast<uint16_t>(std::min<uint32_t>(old_max_level, level));

            for (int lc = static_cast<int>(insert_top); lc >= 0; --lc) {
              const uint16_t layer = static_cast<uint16_t>(lc);
              auto found = SearchLayer(query, {ep}, hnsw_params.ef_construction,
                                        layer, graph, vectors);

              std::vector<std::pair<uint32_t, VectorView>> cands;
              cands.reserve(found.size());
              for (const ScoredNode& sn : found) {
                if (sn.id == node) continue;
                cands.emplace_back(sn.id, VectorView{vectors[sn.id]});
              }

              auto neighbors =
                  HnswBuilder::SelectNeighborsHeuristic(metric, query, cands,
                                                         MaxDegree(layer));

              auto st = ConnectBidirectional(graph, vectors, node, layer,
                                              neighbors);
              if (!st.ok()) return Rebuild();
              if (!found.empty()) ep = found.front().id;
            }
          }
        }

        return BuildHnswIndexFromGraph(metric, std::move(graph), std::move(vectors),
                                        std::move(ids));
      };

      if (ghost_fraction <= insert_largest_staleness_debt_threshold &&
          largest_live_nodes > 0) {
        segment->CompleteIndexBuild(GhostyGraph());
      } else {
        segment->CompleteIndexBuild(Rebuild());
      }
    }
  }
#endif
  return segment;
}

}  // namespace aster
