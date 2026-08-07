#include "aster/db/db.h"

#include <errno.h>
#include <cstdio>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "aster/index/distance.h"
#include "aster/query/topk.h"
#include "aster/storage/manifest.h"
#include "aster/storage/sstable.h"

namespace aster {
namespace {

bool HasAllTags(const Row& row, const std::set<std::string>& wanted) {
  return std::includes(row.tags.begin(), row.tags.end(), wanted.begin(),
                       wanted.end());
}

void PutU32(std::string& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}
void PutU64(std::string& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}
uint32_t GetU32(const std::string& b, size_t& o) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(b[o++])) << (8 * i);
  }
  return v;
}
uint64_t GetU64(const std::string& b, size_t& o) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(b[o++])) << (8 * i);
  }
  return v;
}

std::string EncodeRow(const Row& row) {
  std::string b;
  PutU32(b, static_cast<uint32_t>(row.id.size()));
  b.append(row.id);
  PutU64(b, row.timestamp);
  PutU64(b, row.version);
  b.push_back(row.tombstone ? 1 : 0);
  PutU32(b, static_cast<uint32_t>(row.vector.size()));
  for (float f : row.vector) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    PutU32(b, u);
  }
  PutU32(b, static_cast<uint32_t>(row.metadata.size()));
  b.append(row.metadata);
  PutU32(b, static_cast<uint32_t>(row.tags.size()));
  for (const auto& tag : row.tags) {
    PutU32(b, static_cast<uint32_t>(tag.size()));
    b.append(tag);
  }
  return b;
}

Result<Row> DecodeRow(const std::string& b) {
  if (b.size() < 4) return Status::Corruption("wal row truncated");
  size_t o = 0;
  Row row;
  const uint32_t id_len = GetU32(b, o);
  if (o + id_len > b.size()) return Status::Corruption("wal id truncated");
  row.id = b.substr(o, id_len);
  o += id_len;
  row.timestamp = GetU64(b, o);
  row.version = GetU64(b, o);
  row.tombstone = b[o++] != 0;
  const uint32_t dim = GetU32(b, o);
  row.vector.resize(dim);
  for (uint32_t i = 0; i < dim; ++i) {
    const uint32_t u = GetU32(b, o);
    std::memcpy(&row.vector[i], &u, 4);
  }
  const uint32_t meta_len = GetU32(b, o);
  row.metadata = b.substr(o, meta_len);
  o += meta_len;
  const uint32_t ntags = GetU32(b, o);
  for (uint32_t i = 0; i < ntags; ++i) {
    const uint32_t tlen = GetU32(b, o);
    row.tags.insert(b.substr(o, tlen));
    o += tlen;
  }
  return row;
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

Status EnsureDir(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::IoError("mkdir failed: " + dir);
  }
  return Status::Ok();
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

}  // namespace

Db::Db(Options options) : options_(std::move(options)) {}

std::string Db::SegmentPath(uint64_t id) const {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "seg_%06llu.ast",
                static_cast<unsigned long long>(id));
  return JoinPath(options_.data_dir, buf);
}

std::string Db::ManifestPath() const {
  return JoinPath(options_.data_dir, "MANIFEST");
}

std::string Db::WalPath() const {
  return JoinPath(options_.data_dir, "WAL");
}

Result<std::unique_ptr<Db>> Db::Open(Options options) {
  if (options.data_dir.empty()) {
    return Status::InvalidArgument("Open requires data_dir");
  }
  if (auto st = EnsureDir(options.data_dir); !st.ok()) return st;

  auto db = std::make_unique<Db>(options);

  const std::string manifest_path = db->ManifestPath();
  if (FileExists(manifest_path)) {
    auto manifest = ReadManifest(manifest_path);
    if (!manifest.ok()) return manifest.status();
    db->manifest_generation_ = manifest.value().generation;
    for (const auto& entry : manifest.value().segments) {
      const std::string path =
          entry.path.empty() ? db->SegmentPath(entry.segment_id)
                             : JoinPath(options.data_dir, entry.path);
      auto reader = SstableReader::Open(path);
      if (!reader.ok()) return reader.status();
      auto rows = reader.value()->LoadAll();
      db->segments_.push_back(
          Segment::Build(entry.segment_id, options.metric, std::move(rows)));
      db->next_segment_id_ =
          std::max(db->next_segment_id_, entry.segment_id + 1);
    }
  }

  auto records = ReplayWal(db->WalPath());
  if (!records.ok()) {
    if (FileExists(db->WalPath())) return records.status();
  } else {
    for (const auto& payload : records.value()) {
      auto row = DecodeRow(payload);
      if (!row.ok()) return row.status();
      db->memtable_.Apply(std::move(row.value()));
    }
  }

  auto wal = WalWriter::Open(db->WalPath(), options.wal_sync);
  if (!wal.ok()) return wal.status();
  db->wal_ = std::move(wal.value());
  return db;
}

Status Db::AppendWal(const Row& row) {
  if (!wal_.has_value()) return Status::Ok();
  return wal_->Append(EncodeRow(row));
}

Status Db::PublishManifest() {
  Manifest m;
  m.generation = ++manifest_generation_;
  for (const auto& seg : segments_) {
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(seg->id()));
    m.segments.push_back({seg->id(), name});
  }
  return WriteManifest(ManifestPath(), m);
}

Status Db::Upsert(Row row) {
  if (options_.dimension != 0 && !row.tombstone &&
      row.vector.size() != options_.dimension) {
    return Status::InvalidArgument("vector dimension mismatch");
  }
  if (auto st = AppendWal(row); !st.ok()) return st;
  memtable_.Apply(std::move(row));
  if (memtable_.approximate_bytes() >= options_.memtable_flush_bytes) {
    return Flush();
  }
  return Status::Ok();
}

Status Db::Delete(const RowId& id, Timestamp timestamp) {
  Row tombstone;
  tombstone.id = id;
  tombstone.timestamp = timestamp;
  tombstone.tombstone = true;
  return Upsert(std::move(tombstone));
}

Row Db::Reconcile(const RowId& id) const {
  Row newest;
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
  if (!found) newest.tombstone = true;
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
  auto segment = Segment::Build(id, options_.metric, rows);

  if (!options_.data_dir.empty()) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric, rows);
        !st.ok()) {
      return st;
    }
  }

  segments_.push_back(std::move(segment));
  memtable_ = Memtable();

  if (!options_.data_dir.empty()) {
    if (auto st = PublishManifest(); !st.ok()) return st;
    if (wal_.has_value()) {
      if (auto st = wal_->Truncate(); !st.ok()) return st;
    }
  }
  return Status::Ok();
}

Status Db::Compact() {
  if (segments_.size() < 2) return Status::Ok();
  const uint64_t id = next_segment_id_++;
  auto compacted = CompactSegments(id, options_.metric, segments_,
                                   /*drop_tombstones=*/true);
  if (!options_.data_dir.empty()) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric,
                               compacted->rows());
        !st.ok()) {
      return st;
    }
  }
  segments_.clear();
  segments_.push_back(std::move(compacted));
  if (!options_.data_dir.empty()) {
    return PublishManifest();
  }
  return Status::Ok();
}

}  // namespace aster
