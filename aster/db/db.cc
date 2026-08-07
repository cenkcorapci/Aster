#include "aster/db/db.h"

#include <algorithm>
#include <utility>

#include "aster/index/distance.h"
#include "aster/query/topk.h"

namespace aster {

namespace {
bool HasAllTags(const Row& row, const std::set<std::string>& wanted) {
  return std::includes(row.tags.begin(), row.tags.end(), wanted.begin(),
                       wanted.end());
}
}  // namespace

Status Db::Upsert(Row row) {
  if (options_.dimension != 0 && row.vector.size() != options_.dimension) {
    return Status::InvalidArgument("vector dimension mismatch");
  }
  memtable_.Apply(std::move(row));
  if (memtable_.approximate_bytes() >= options_.memtable_flush_bytes) {
    Flush();
  }
  return Status::Ok();
}

Status Db::Delete(const RowId& id, Timestamp timestamp) {
  Row tombstone;
  tombstone.id = id;
  tombstone.timestamp = timestamp;
  tombstone.tombstone = true;
  memtable_.Apply(std::move(tombstone));
  return Status::Ok();
}

Row Db::Reconcile(const RowId& id) const {
  // Newest version across memtable and all segments wins (LWW).
  Row newest;  // timestamp 0: any real row beats it
  bool found = false;
  if (auto row = memtable_.Get(id)) {
    newest = *row;
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
  if (!found) newest.tombstone = true;  // "not found" behaves like deleted
  return newest;
}

std::optional<Row> Db::Get(const RowId& id) const {
  Row row = Reconcile(id);
  if (row.tombstone) return std::nullopt;
  return row;
}

std::vector<SearchHit> Db::Search(const SearchRequest& request) const {
  std::vector<std::vector<SearchHit>> candidates;

  // Memtable candidates: brute-force over live rows.
  {
    std::vector<SearchHit> hits;
    for (const Row& row : memtable_.Scan()) {
      if (row.tombstone) continue;
      hits.push_back(
          {row.id, Score(options_.metric, request.vector, row.vector)});
    }
    candidates.push_back(std::move(hits));
  }

  // Per-segment ANN searches. Over-fetch to survive post-filtering.
  const uint32_t fetch_k = request.top_k * 2 + 16;
  for (const auto& segment : segments_) {
    candidates.push_back(
        segment->Search(request.vector, fetch_k, request.ef_search));
  }

  // Merge, then validate each candidate against the reconciled (LWW) row:
  // a hit from an old segment must not surface if the row was since
  // deleted or updated, and tag filters apply to the latest version.
  std::vector<SearchHit> merged = MergeTopK(candidates, fetch_k);
  std::vector<SearchHit> results;
  for (const SearchHit& hit : merged) {
    if (results.size() >= request.top_k) break;
    const Row row = Reconcile(hit.id);
    if (row.tombstone) continue;
    if (!request.tags.empty() && !HasAllTags(row, request.tags)) continue;
    // Exact rerank against the authoritative version of the vector.
    results.push_back(
        {hit.id, Score(options_.metric, request.vector, row.vector)});
  }
  std::sort(results.begin(), results.end(),
            [](const SearchHit& a, const SearchHit& b) {
              return a.score > b.score;
            });
  return results;
}

void Db::Flush() {
  if (memtable_.empty()) return;
  segments_.push_back(Segment::Build(next_segment_id_++, options_.metric,
                                     memtable_.Scan()));
  memtable_ = Memtable();
}

void Db::Compact() {
  if (segments_.size() < 2) return;
  // Full compaction covers every segment, so tombstones can be purged
  // safely (no older version can survive anywhere else on this node).
  auto compacted = CompactSegments(next_segment_id_++, options_.metric,
                                   segments_, /*drop_tombstones=*/true);
  segments_.clear();
  segments_.push_back(std::move(compacted));
}

}  // namespace aster
