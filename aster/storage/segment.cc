#include "aster/storage/segment.h"

#include <algorithm>
#include <map>
#include <utility>

namespace aster {

std::shared_ptr<const Segment> Segment::Build(uint64_t id, Metric metric,
                                              std::vector<Row> rows) {
  std::vector<IndexEntry> entries;
  entries.reserve(rows.size());
  for (const Row& row : rows) {
    if (row.tombstone) continue;
    entries.push_back({row.id, row.vector});
  }
  auto index = BuildExactIndex(metric, std::move(entries));
  return std::shared_ptr<const Segment>(
      new Segment(id, std::move(rows), std::move(index)));
}

std::optional<Row> Segment::Get(const RowId& row_id) const {
  auto it = std::lower_bound(
      rows_.begin(), rows_.end(), row_id,
      [](const Row& row, const RowId& id) { return row.id < id; });
  if (it == rows_.end() || it->id != row_id) return std::nullopt;
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

  std::vector<Row> rows;
  rows.reserve(merged.size());
  for (auto& [_, row] : merged) {
    if (drop_tombstones && row.tombstone) continue;
    rows.push_back(std::move(row));
  }
  return Segment::Build(new_id, metric, std::move(rows));
}

}  // namespace aster
