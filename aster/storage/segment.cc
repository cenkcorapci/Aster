#include "aster/storage/segment.h"

#include <algorithm>
#include <map>
#include <utility>

namespace aster {

std::shared_ptr<const Segment> Segment::Build(
    uint64_t id, Metric metric, std::shared_ptr<std::vector<Row>> rows) {
  std::shared_ptr<const std::vector<Row>> shared = std::move(rows);
  auto index = BuildExactIndex(metric, shared);
  return std::shared_ptr<const Segment>(
      new Segment(id, std::move(shared), std::move(index)));
}

std::shared_ptr<const Segment> Segment::Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows) {
  return Build(id, metric,
               std::make_shared<std::vector<Row>>(std::move(rows)));
}

std::optional<Row> Segment::Get(const RowId& row_id) const {
  const auto& rows = *rows_;
  auto it = std::lower_bound(
      rows.begin(), rows.end(), row_id,
      [](const Row& row, const RowId& id) { return row.id < id; });
  if (it == rows.end() || it->id != row_id) return std::nullopt;
  return *it;
}

std::vector<SearchHit> Segment::Search(VectorView query, uint32_t top_k,
                                       uint32_t ef_search) const {
  return index_->Search(query, top_k, ef_search);
}

std::shared_ptr<const Segment> CompactSegments(
    uint64_t new_id, Metric metric,
    const std::vector<std::shared_ptr<const Segment>>& inputs,
    bool drop_tombstones) {
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
  return Segment::Build(new_id, metric, std::move(rows));
}

}  // namespace aster
