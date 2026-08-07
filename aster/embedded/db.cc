#include "aster/embedded/db.h"

#include <algorithm>

#include "aster/index/distance.h"
#include "aster/query/topk.h"

namespace aster {
namespace embedded {
namespace {

bool HasAllTags(const Row& row, const std::set<std::string>& wanted) {
  return std::includes(row.tags.begin(), row.tags.end(), wanted.begin(),
                       wanted.end());
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
  std::vector<std::vector<SearchHit>> candidates;
  {
    std::vector<SearchHit> hits;
    for (const Row& row : memtable_.Scan()) {
      if (row.tombstone) continue;
      hits.push_back(
          {row.id, Score(options_.metric, request.vector, row.vector)});
    }
    candidates.push_back(std::move(hits));
  }

  const uint32_t fetch_k = request.top_k * 2 + 16;
  for (const auto& segment : segments_) {
    candidates.push_back(
        segment->Search(request.vector, fetch_k, request.ef_search));
  }

  std::vector<SearchHit> merged = MergeTopK(candidates, fetch_k);
  std::vector<SearchHit> results;
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
  auto rows = memtable_.Scan();
  segments_.push_back(Segment::Build(id, options_.metric, std::move(rows)));
  memtable_ = Memtable();
  return Status::Ok();
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
