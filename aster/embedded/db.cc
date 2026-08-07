#include "aster/embedded/db.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "aster/index/distance.h"
#include "aster/query/topk.h"

namespace aster {
namespace embedded {
namespace {

bool HasAllTags(const Row& row, const std::set<std::string>& wanted) {
  return std::includes(row.tags.begin(), row.tags.end(), wanted.begin(),
                       wanted.end());
}

std::vector<SearchHit> MemtableTopK(const Memtable& memtable, Metric metric,
                                    VectorView query, uint32_t top_k) {
  if (top_k == 0 || query.empty() || memtable.empty()) return {};

  float qnorm = 0.0f;
  if (metric == Metric::kCosine) {
    for (float x : query) qnorm += x * x;
    qnorm = std::sqrt(qnorm);
  }

  using Node = std::pair<float, const Row*>;
  auto worse = [](const Node& a, const Node& b) { return a.first > b.first; };
  std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);

  memtable.ForEach([&](const Row& row) {
    if (row.tombstone || row.vector.empty()) return;
    float score;
    if (metric == Metric::kCosine) {
      float n2 = 0.0f;
      for (float x : row.vector) n2 += x * x;
      score = CosineSimilarityPreNorm(query, qnorm, row.vector, std::sqrt(n2));
    } else {
      score = Score(metric, query, row.vector);
    }
    if (heap.size() < top_k) {
      heap.emplace(score, &row);
    } else if (score > heap.top().first) {
      heap.pop();
      heap.emplace(score, &row);
    }
  });

  std::vector<SearchHit> hits(heap.size());
  for (size_t i = hits.size(); i > 0; --i) {
    const auto [score, row] = heap.top();
    heap.pop();
    hits[i - 1] = {row->id, score};
  }
  return hits;
}

}  // namespace

Db::Db(Options options) : options_(std::move(options)) {}

Status Db::Upsert(Row row) {
  if (options_.dimension == 0) {
    return Status::InvalidArgument("dimension must be > 0");
  }
  if (!row.tombstone && row.vector.size() != options_.dimension) {
    return Status::InvalidArgument("vector dimension mismatch");
  }
  memtable_.Apply(std::move(row));
  if (memtable_.row_count() >= options_.memtable_flush_rows) {
    return Flush();
  }
  return Status::Ok();
}

Status Db::Delete(const RowId& id, Timestamp timestamp) {
  Row row;
  row.id = id;
  row.timestamp = timestamp;
  row.tombstone = true;
  return Upsert(std::move(row));
}

Row Db::Reconcile(const RowId& id) const {
  bool found = false;
  Row newest;
  if (auto m = memtable_.Get(id)) {
    newest = *m;
    found = true;
  }
  for (const auto& segment : segments_) {
    if (auto row = segment->Get(id)) {
      if (!found || NewerThan(*row, newest)) {
        newest = *row;
        found = true;
      }
    }
  }
  if (!found) {
    Row missing;
    missing.id = id;
    missing.tombstone = true;
    return missing;
  }
  return newest;
}

std::optional<Row> Db::Get(const RowId& id) const {
  Row row = Reconcile(id);
  if (row.tombstone) return std::nullopt;
  return row;
}

std::vector<SearchHit> Db::Search(const SearchRequest& request) const {
  const uint32_t fetch_k = request.top_k * 2 + 16;
  std::vector<std::vector<SearchHit>> candidates;
  candidates.reserve(1 + segments_.size());
  candidates.push_back(
      MemtableTopK(memtable_, options_.metric, request.vector, fetch_k));
  for (const auto& segment : segments_) {
    candidates.push_back(
        segment->Search(request.vector, fetch_k, request.ef_search));
  }

  std::vector<SearchHit> merged = MergeTopK(candidates, fetch_k);
  std::vector<SearchHit> results;
  results.reserve(request.top_k);
  for (const SearchHit& hit : merged) {
    if (results.size() >= request.top_k) break;
    const Row row = Reconcile(hit.id);
    if (row.tombstone) continue;
    if (!request.tags.empty() && !HasAllTags(row, request.tags)) continue;
    results.push_back(
        {hit.id, Score(options_.metric, request.vector, row.vector)});
  }
  std::sort(results.begin(), results.end(),
            [](const SearchHit& a, const SearchHit& b) {
              return a.score > b.score;
            });
  return results;
}

Status Db::Flush() {
  if (memtable_.empty()) return Status::Ok();
  const uint64_t id = next_segment_id_++;
  auto rows = std::make_shared<std::vector<Row>>(memtable_.Take());
  segments_.push_back(Segment::Build(id, options_.metric, std::move(rows)));
  return MaybeCompact();
}

Status Db::MaybeCompact() {
  if (options_.max_segments_before_compact == 0) return Status::Ok();
  if (segments_.size() < options_.max_segments_before_compact) {
    return Status::Ok();
  }
  return Compact();
}

Status Db::Compact() {
  if (segments_.size() < 2) return Status::Ok();
  const uint64_t id = next_segment_id_++;
  auto compacted = CompactSegments(id, options_.metric, segments_,
                                   /*drop_tombstones=*/true);
  segments_.clear();
  segments_.push_back(std::move(compacted));
  return Status::Ok();
}

}  // namespace embedded
}  // namespace aster
