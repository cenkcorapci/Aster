#include "aster/db/db.h"

#include <dirent.h>
#include <errno.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <queue>
#include <set>
#include <utility>

#include "aster/index/distance.h"
#include "aster/query/topk.h"
#include "aster/storage/compaction.h"
#include "aster/storage/manifest.h"
#include "aster/storage/sstable.h"

namespace aster {
namespace {

bool HasAllTags(const Row& row, const std::set<std::string>& wanted) {
  return std::includes(row.tags.begin(), row.tags.end(), wanted.begin(),
                       wanted.end());
}

bool SegmentHasTombstone(
    const std::vector<std::shared_ptr<const Segment>>& segments) {
  for (const auto& seg : segments) {
    for (const Row& row : seg->rows()) {
      if (row.tombstone) return true;
    }
  }
  return false;
}

// Bounded top-k over the memtable without materializing an O(n) hit list.
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
  const size_t total = b.size();
  if (total < 4) return Status::Corruption("wal row truncated");
  size_t o = 0;

  auto NeedBytes = [&](size_t n) -> bool {
    return o <= total && n <= total - o;
  };

  Row row;
  const uint32_t id_len = GetU32(b, o);
  if (!NeedBytes(id_len)) return Status::Corruption("wal id truncated");
  row.id = b.substr(o, id_len);
  o += id_len;

  if (!NeedBytes(8 + 8 + 1))
    return Status::Corruption("wal row header truncated");
  row.timestamp = GetU64(b, o);
  row.version = GetU64(b, o);
  row.tombstone = b[o++] != 0;

  if (!NeedBytes(4)) return Status::Corruption("wal vector dim truncated");
  const uint32_t dim = GetU32(b, o);
  if (dim > (total - o) / 4) return Status::Corruption("wal vector truncated");
  row.vector.resize(dim);
  for (uint32_t i = 0; i < dim; ++i) {
    const uint32_t u = GetU32(b, o);
    std::memcpy(&row.vector[i], &u, 4);
  }

  if (!NeedBytes(4)) return Status::Corruption("wal metadata len truncated");
  const uint32_t meta_len = GetU32(b, o);
  if (!NeedBytes(meta_len)) return Status::Corruption("wal metadata truncated");
  row.metadata = b.substr(o, meta_len);
  o += meta_len;

  if (!NeedBytes(4)) return Status::Corruption("wal tags count truncated");
  const uint32_t ntags = GetU32(b, o);
  for (uint32_t i = 0; i < ntags; ++i) {
    if (!NeedBytes(4)) return Status::Corruption("wal tag len truncated");
    const uint32_t tlen = GetU32(b, o);
    if (!NeedBytes(tlen)) return Status::Corruption("wal tag truncated");
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

bool IsSegmentFileName(const std::string& name) {
  // seg_NNNNNN.ast or leftover seg_*.ast.tmp from a crashed write.
  if (name.rfind("seg_", 0) != 0) return false;
  return name.find(".ast") != std::string::npos;
}

}  // namespace

Db::Db(Options options) : Db(std::move(options), DeferFlushThread{}) {
  StartFlushThread();
}

Db::Db(Options options, DeferFlushThread) : options_(std::move(options)) {}

Db::~Db() { StopFlushThread(); }

void Db::StartFlushThread() {
  stop_flush_thread_ = false;
  flush_thread_ = std::thread([this] { BackgroundFlushLoop(); });
}

void Db::StopFlushThread() {
  {
    std::lock_guard lock(mu_);
    stop_flush_thread_ = true;
  }
  flush_cv_.notify_all();
  if (flush_thread_.joinable()) flush_thread_.join();
}

bool Db::ShouldFlushLocked() const {
  if (memtable_.empty()) return false;
  if (memtable_.approximate_bytes() >= options_.memtable_flush_bytes) {
    return true;
  }
  if (options_.memtable_flush_ms == 0) return false;
  const auto age = std::chrono::steady_clock::now() - memtable_live_since_;
  return age >= std::chrono::milliseconds(options_.memtable_flush_ms);
}

void Db::RequestFlushLocked() {
  flush_requested_ = true;
  flush_cv_.notify_one();
}

void Db::BackgroundFlushLoop() {
  std::unique_lock lock(mu_);
  while (!stop_flush_thread_) {
    if (options_.memtable_flush_ms > 0) {
      flush_cv_.wait_for(lock,
                         std::chrono::milliseconds(options_.memtable_flush_ms),
                         [this] {
                           return stop_flush_thread_ || flush_requested_ ||
                                  ShouldFlushLocked();
                         });
    } else {
      flush_cv_.wait(lock, [this] {
        return stop_flush_thread_ || flush_requested_ || ShouldFlushLocked();
      });
    }
    if (stop_flush_thread_) break;
    flush_requested_ = false;
    if (!ShouldFlushLocked()) continue;
    // FlushLocked may compact; keep holding mu_ so readers/writers stay
    // consistent with the sealed memtable and segment list.
    (void)FlushLocked();
  }
}

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

void Db::GarbageCollectOrphans() {
  if (options_.data_dir.empty()) return;

  std::set<std::string> live;
  for (const auto& seg : segments_) {
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(seg->id()));
    live.insert(name);
  }

  DIR* dir = ::opendir(options_.data_dir.c_str());
  if (!dir) return;
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (name == "MANIFEST.tmp") {
      ::remove(JoinPath(options_.data_dir, name).c_str());
      continue;
    }
    // seg_*.ast not in the manifest, or any seg_*.ast.tmp from a crashed write.
    if (!IsSegmentFileName(name)) continue;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      ::remove(JoinPath(options_.data_dir, name).c_str());
      continue;
    }
    if (live.count(name) == 0) {
      ::remove(JoinPath(options_.data_dir, name).c_str());
    }
  }
  ::closedir(dir);
}

Result<std::unique_ptr<Db>> Db::Open(Options options) {
  if (options.data_dir.empty()) {
    return Status::InvalidArgument("Open requires data_dir");
  }
  if (auto st = EnsureDir(options.data_dir); !st.ok()) return st;

  // Load without the background flusher racing against recovery.
  auto db = std::unique_ptr<Db>(new Db(std::move(options), DeferFlushThread{}));

  const std::string manifest_path = db->ManifestPath();
  if (FileExists(manifest_path)) {
    auto manifest = ReadManifest(manifest_path);
    if (!manifest.ok()) return manifest.status();
    db->manifest_generation_ = manifest.value().generation;
    for (const auto& entry : manifest.value().segments) {
      const std::string path =
          entry.path.empty() ? db->SegmentPath(entry.segment_id)
                             : JoinPath(db->options_.data_dir, entry.path);
      auto reader = SstableReader::Open(path);
      if (!reader.ok()) return reader.status();
      auto rows = reader.value()->TakeAll();
      db->segments_.push_back(Segment::Build(
          entry.segment_id, db->options_.metric, std::move(rows)));
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
  if (!db->memtable_.empty()) {
    db->memtable_live_since_ = std::chrono::steady_clock::now();
  }

  auto wal = WalWriter::Open(db->WalPath(), db->options_.wal_sync);
  if (!wal.ok()) return wal.status();
  db->wal_ = std::move(wal.value());

  db->GarbageCollectOrphans();
  if (db->ShouldFlushLocked()) db->flush_requested_ = true;
  db->StartFlushThread();
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
  std::lock_guard lock(mu_);
  if (options_.dimension != 0 && !row.tombstone &&
      row.vector.size() != options_.dimension) {
    return Status::InvalidArgument("vector dimension mismatch");
  }
  if (auto st = AppendWal(row); !st.ok()) return st;
  const bool was_empty = memtable_.empty();
  memtable_.Apply(std::move(row));
  if (was_empty) {
    memtable_live_since_ = std::chrono::steady_clock::now();
  }
  if (ShouldFlushLocked()) {
    RequestFlushLocked();
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
  std::lock_guard lock(mu_);
  Row row = Reconcile(id);
  if (row.tombstone) return std::nullopt;
  return row;
}

std::vector<SearchHit> Db::Search(const SearchRequest& request) const {
  std::lock_guard lock(mu_);
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
  std::lock_guard lock(mu_);
  flush_requested_ = false;
  return FlushLocked();
}

Status Db::FlushLocked() {
  if (memtable_.empty()) return Status::Ok();
  const uint64_t id = next_segment_id_++;
  auto rows = std::make_shared<std::vector<Row>>(memtable_.Take());
  auto segment = Segment::Build(id, options_.metric, rows);

  if (!options_.data_dir.empty()) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric, *rows);
        !st.ok()) {
      for (Row& row : *rows) memtable_.Apply(std::move(row));
      --next_segment_id_;
      memtable_live_since_ = std::chrono::steady_clock::now();
      return st;
    }
  }

  segments_.push_back(std::move(segment));

  if (!options_.data_dir.empty()) {
    if (auto st = PublishManifest(); !st.ok()) return st;
    if (wal_.has_value()) {
      if (auto st = wal_->Truncate(); !st.ok()) return st;
    }
  }
  return MaybeCompact();
}

Status Db::MaybeCompact() {
  // Cascade size-tiered merges: a merged segment may fill the next tier.
  constexpr int kMaxCascades = 64;
  for (int i = 0; i < kMaxCascades; ++i) {
    if (options_.compaction_tier_threshold > 0) {
      std::vector<size_t> sizes;
      sizes.reserve(segments_.size());
      for (const auto& seg : segments_) {
        sizes.push_back(seg->row_count());
      }
      auto pick = SelectSizeTieredCompaction(
          sizes, options_.compaction_tier_threshold,
          options_.compaction_bucket_ratio);
      if (pick.has_value()) {
        if (auto st = CompactSelectedLocked(pick->input_indices); !st.ok()) {
          return st;
        }
        continue;
      }
    }
    if (options_.max_segments_before_compact > 0 &&
        segments_.size() >= options_.max_segments_before_compact) {
      return CompactLocked();
    }
    return Status::Ok();
  }
  return Status::Ok();
}

Status Db::Compact() {
  std::lock_guard lock(mu_);
  return CompactLocked();
}

Status Db::CompactLocked() {
  if (segments_.empty()) return Status::Ok();
  if (segments_.size() < 2 && !SegmentHasTombstone(segments_)) {
    return Status::Ok();
  }
  std::vector<size_t> all(segments_.size());
  for (size_t i = 0; i < all.size(); ++i) all[i] = i;
  return CompactSelectedLocked(all);
}

Status Db::CompactSelectedLocked(const std::vector<size_t>& indices) {
  if (indices.empty()) return Status::Ok();

  std::vector<std::shared_ptr<const Segment>> inputs;
  inputs.reserve(indices.size());
  for (size_t idx : indices) {
    if (idx >= segments_.size()) {
      return Status::InvalidArgument("compaction index out of range");
    }
    inputs.push_back(segments_[idx]);
  }

  const bool full_overlap = indices.size() == segments_.size();
  if (!full_overlap && inputs.size() < 2) return Status::Ok();
  if (full_overlap && inputs.size() < 2 && !SegmentHasTombstone(inputs)) {
    return Status::Ok();
  }

  const uint64_t id = next_segment_id_++;

  std::vector<std::string> old_paths;
  if (!options_.data_dir.empty()) {
    old_paths.reserve(inputs.size());
    for (const auto& seg : inputs) {
      old_paths.push_back(SegmentPath(seg->id()));
    }
  }

  // Tombstones may only be dropped on full-overlap compaction
  // (tla/AsterLsmIndex.tla NoResurrection / M1-T11).
  auto compacted = CompactSegments(id, options_.metric, inputs,
                                   /*drop_tombstones=*/full_overlap);
  const bool omit_empty = full_overlap && compacted->row_count() == 0;

  if (!options_.data_dir.empty() && !omit_empty) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric,
                               compacted->rows());
        !st.ok()) {
      return st;
    }
  }

  std::vector<bool> drop(segments_.size(), false);
  for (size_t idx : indices) drop[idx] = true;

  std::vector<std::shared_ptr<const Segment>> next;
  next.reserve(segments_.size() - inputs.size() + (omit_empty ? 0 : 1));
  const size_t insert_at = *std::min_element(indices.begin(), indices.end());
  bool inserted = false;
  for (size_t i = 0; i < segments_.size(); ++i) {
    if (i == insert_at && !inserted) {
      if (!omit_empty) next.push_back(compacted);
      inserted = true;
    }
    if (!drop[i]) next.push_back(segments_[i]);
  }
  if (!inserted && !omit_empty) next.push_back(compacted);

  segments_ = std::move(next);

  if (!options_.data_dir.empty()) {
    if (auto st = PublishManifest(); !st.ok()) return st;
    for (const auto& path : old_paths) {
      ::remove(path.c_str());
    }
    GarbageCollectOrphans();
  }
  return Status::Ok();
}

size_t Db::segment_count() const {
  std::lock_guard lock(mu_);
  return segments_.size();
}

size_t Db::memtable_rows() const {
  std::lock_guard lock(mu_);
  return memtable_.row_count();
}

size_t Db::approximate_row_count() const {
  std::lock_guard lock(mu_);
  size_t n = memtable_.row_count();
  for (const auto& segment : segments_) n += segment->row_count();
  return n;
}

}  // namespace aster
