#include "aster/storage/memtable.h"

#include <utility>

namespace aster {

namespace {
size_t RowBytes(const Row& row) {
  size_t bytes = row.id.size() + row.vector.size() * sizeof(float) +
                 row.metadata.size() + sizeof(Row);
  for (const auto& tag : row.tags) bytes += tag.size();
  return bytes;
}
}  // namespace

bool Memtable::Apply(Row row) {
  auto it = rows_.find(row.id);
  if (it != rows_.end()) {
    if (!NewerThan(row, it->second)) return false;
    approximate_bytes_ -= RowBytes(it->second);
    approximate_bytes_ += RowBytes(row);
    it->second = std::move(row);
    return true;
  }
  approximate_bytes_ += RowBytes(row);
  rows_.emplace(row.id, std::move(row));
  return true;
}

std::optional<Row> Memtable::Get(const RowId& id) const {
  auto it = rows_.find(id);
  if (it == rows_.end()) return std::nullopt;
  return it->second;
}

std::vector<Row> Memtable::Scan() const {
  std::vector<Row> out;
  out.reserve(rows_.size());
  for (const auto& [_, row] : rows_) out.push_back(row);
  return out;
}

std::vector<Row> Memtable::Take() {
  std::map<RowId, Row> local;
  local.swap(rows_);
  approximate_bytes_ = 0;
  std::vector<Row> out;
  out.reserve(local.size());
  for (auto& [_, row] : local) out.push_back(std::move(row));
  return out;
}

}  // namespace aster
