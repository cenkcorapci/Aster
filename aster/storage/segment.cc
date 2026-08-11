#include "aster/storage/segment.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <utility>

#include "aster/core/features.h"
#include "aster/index/distance.h"

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
    HnswParams hnsw_params, uint64_t hnsw_rng_seed
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
  // M2-T05: rebuild-from-rows (not merge input graphs) → one READY graph.
  if (segment->TryBeginIndexBuild()) {
    segment->CompleteIndexBuild(RebuildHnswFromLiveRows(
        metric, hnsw_params, segment->rows(), hnsw_rng_seed));
  }
#endif
  return segment;
}

}  // namespace aster
