#include "aster/storage/segment.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <queue>
#include <utility>
#include <unordered_set>

#include "aster/core/features.h"
#include "aster/core/status.h"
#include "aster/index/distance.h"
#include "aster/index/vector_encoding.h"

#if ASTER_ENABLE_HNSW
#include <random>

#include "aster/index/hnsw_build.h"
#include "aster/index/hnsw_graph.h"
#include "aster/index/hnsw_search_layer.h"
#include "aster/index/vector_index.h"
#endif

namespace aster {
namespace {

// Bitmap-driven exact scan over matching ordinals (filtered search fallback
// and the exact path when a tag predicate is present).
std::vector<SearchHit> ScoreMatchingOrdinals(
    Metric metric, const std::vector<Row>& rows, const TagBitmap& matching,
    VectorView query, uint32_t top_k,
    const CompactVectorStore* packed,
    const std::vector<uint32_t>* live_slots) {
  if (matching.empty() || top_k == 0 || query.empty()) return {};

  float qnorm = 0.0f;
  if (metric == Metric::kCosine) {
    for (float x : query) qnorm += x * x;
    qnorm = std::sqrt(qnorm);
  }

  using Node = std::pair<float, uint32_t>;
  auto worse = [](const Node& a, const Node& b) { return a.first > b.first; };
  std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);
  thread_local std::vector<float> scratch;

  for (uint32_t ord : matching.ordinals()) {
    if (ord >= rows.size()) continue;
    const Row& row = rows[ord];
    if (row.tombstone) continue;

    VectorView vec = row.vector;
    if (vec.empty() && packed && live_slots && ord < live_slots->size() &&
        (*live_slots)[ord] != 0xFFFFFFFFu) {
      scratch.resize(packed->dimension());
      if (!packed->Decode((*live_slots)[ord], scratch).ok()) continue;
      vec = scratch;
    }
    if (vec.empty()) continue;

    float score;
    if (metric == Metric::kCosine) {
      float n2 = 0.0f;
      for (float x : vec) n2 += x * x;
      score = CosineSimilarityPreNorm(query, qnorm, vec, std::sqrt(n2));
    } else {
      score = Score(metric, query, vec);
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
  auto built = Build(id, metric, std::move(rows), VectorEncoding::kFloat32);
  // float32 Build always succeeds.
  return built.ok() ? std::move(built.value()) : nullptr;
}

std::shared_ptr<const Segment> Segment::Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows) {
  return Build(id, metric,
               std::make_shared<std::vector<Row>>(std::move(rows)));
}

Result<std::shared_ptr<const Segment>> Segment::Build(
    uint64_t id, Metric metric, std::shared_ptr<std::vector<Row>> rows,
    VectorEncoding encoding) {
  if (!rows) return Status::InvalidArgument("rows required");
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(encoding))) {
    return Status::InvalidArgument("unknown vector encoding");
  }

  TagIndex tags = TagIndex::Build(*rows);
  std::shared_ptr<const CompactVectorStore> packed;
  std::shared_ptr<const std::vector<uint32_t>> live_slots;
  std::unique_ptr<VectorIndex> index;

  if (encoding == VectorEncoding::kFloat32) {
    std::shared_ptr<const std::vector<Row>> shared = std::move(rows);
    index = BuildExactIndex(metric, shared);
    return std::shared_ptr<const Segment>(new Segment(
        id, metric, std::move(shared), std::move(index), std::move(tags),
        encoding, nullptr, nullptr));
  }

  uint32_t dim = 0;
  for (const Row& row : *rows) {
    if (!row.tombstone && !row.vector.empty()) {
      dim = static_cast<uint32_t>(row.vector.size());
      break;
    }
  }
  auto encoded = CompactVectorStore::Encode(encoding, dim, *rows);
  if (!encoded.ok()) return encoded.status();
  packed = std::make_shared<const CompactVectorStore>(std::move(encoded.value()));

  auto slots = std::make_shared<std::vector<uint32_t>>(rows->size(), 0xFFFFFFFFu);
  uint32_t slot = 0;
  for (size_t i = 0; i < rows->size(); ++i) {
    if ((*rows)[i].tombstone) continue;
    (*slots)[i] = slot++;
    (*rows)[i].vector.clear();
    (*rows)[i].vector.shrink_to_fit();
  }
  live_slots = std::move(slots);

  std::shared_ptr<const std::vector<Row>> shared = std::move(rows);
  index = BuildExactIndex(metric, shared, packed, live_slots);
  return std::shared_ptr<const Segment>(new Segment(
      id, metric, std::move(shared), std::move(index), std::move(tags),
      encoding, std::move(packed), std::move(live_slots)));
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

Status Segment::DecodeRowVector(size_t ordinal, std::vector<float>* out) const {
  if (!out) return Status::InvalidArgument("out required");
  if (!rows_ || ordinal >= rows_->size()) {
    return Status::InvalidArgument("row ordinal OOB");
  }
  const Row& row = (*rows_)[ordinal];
  if (!row.vector.empty()) {
    *out = row.vector;
    return Status::Ok();
  }
  if (!packed_ || !live_slots_ || ordinal >= live_slots_->size()) {
    out->clear();
    return Status::Ok();
  }
  const uint32_t slot = (*live_slots_)[ordinal];
  if (slot == 0xFFFFFFFFu) {
    out->clear();
    return Status::Ok();
  }
  out->assign(packed_->dimension(), 0.0f);
  return packed_->Decode(slot, *out);
}

uint64_t Segment::VectorBytesUsed() const {
  if (packed_) return packed_->BytesUsed();
  uint64_t bytes = 0;
  if (!rows_) return 0;
  for (const Row& row : *rows_) {
    if (row.tombstone) continue;
    bytes += row.vector.size() * sizeof(float);
  }
  return bytes;
}

std::vector<Row> Segment::MaterializedRows() const {
  std::vector<Row> out;
  if (!rows_) return out;
  out.reserve(rows_->size());
  for (size_t i = 0; i < rows_->size(); ++i) {
    Row row = (*rows_)[i];
    if (!row.tombstone && row.vector.empty()) {
      (void)DecodeRowVector(i, &row.vector);
    }
    out.push_back(std::move(row));
  }
  return out;
}

std::optional<Row> Segment::Get(const RowId& row_id) const {
  const auto& rows = *rows_;
  auto it = std::lower_bound(
      rows.begin(), rows.end(), row_id,
      [](const Row& row, const RowId& id) { return row.id < id; });
  if (it == rows.end() || it->id != row_id) return std::nullopt;
  Row row = *it;
  if (!row.tombstone && row.vector.empty()) {
    const size_t ordinal = static_cast<size_t>(std::distance(rows.begin(), it));
    (void)DecodeRowVector(ordinal, &row.vector);
  }
  return row;
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
                               query, top_k, packed_.get(),
                               live_slots_ ? live_slots_.get() : nullptr);
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
  // Materialize packed vectors so the merged segment always owns float32
  // (re-packing is the caller's choice via Segment::Build(..., encoding)).
  std::map<RowId, Row> merged;
  for (const auto& segment : inputs) {
    const auto rows = segment->MaterializedRows();
    for (const Row& row : rows) {
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
    const auto materialized = segment->MaterializedRows();
    for (const Row& row : materialized) {
      if (row.tombstone || row.vector.empty()) continue;
      live_output_ids.insert(row.id);
    }

    auto Rebuild = [&]() {
      return RebuildHnswFromLiveRows(metric, hnsw_params, materialized,
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

      using hnsw_internal::BuildNormCache;
      using hnsw_internal::QueryNorm;
      using hnsw_internal::ScoredNode;
      using hnsw_internal::SearchLayer;
      using hnsw_internal::VectorStore;

      auto AppendVector = [&](std::vector<std::vector<float>>& vectors,
                              std::vector<float>& norms,
                              const std::vector<float>& vec) {
        vectors.push_back(vec);
        if (metric == Metric::kCosine) {
          float n2 = 0.0f;
          for (float x : vec) n2 += x * x;
          norms.push_back(std::sqrt(n2));
        }
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
        std::vector<float> norms = BuildNormCache(metric, vectors);

        std::mt19937_64 rng(hnsw_rng_seed + 1);

        const auto graph_neighbors = [&graph](uint32_t node, uint16_t layer) {
          return graph.Neighbors(node, layer);
        };

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
            AppendVector(vectors, norms, vec);
            ids.push_back(id);

            const uint32_t node = added.value();
            if (node == 0) continue;

            VectorStore store{&vectors,
                              metric == Metric::kCosine ? &norms : nullptr,
                              metric};
            VectorView query{vectors[node]};
            const float qnorm = QueryNorm(metric, query);
            uint32_t ep = old_entry;

            // Greedy descent from previous top down to level+1.
            if (old_max_level > level) {
              for (uint16_t lc = old_max_level; lc > level; --lc) {
                auto nearest = SearchLayer(store, query, qnorm, graph_neighbors,
                                           {ep}, /*ef=*/1, lc);
                if (!nearest.empty()) ep = nearest.front().id;
              }
            }

            const uint16_t insert_top =
                static_cast<uint16_t>(std::min<uint32_t>(old_max_level, level));

            for (int lc = static_cast<int>(insert_top); lc >= 0; --lc) {
              const uint16_t layer = static_cast<uint16_t>(lc);
              auto found = SearchLayer(store, query, qnorm, graph_neighbors,
                                       {ep}, hnsw_params.ef_construction, layer);

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
