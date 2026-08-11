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
#include <string_view>
#include <utility>

#include "aster/core/memory.h"
#include "aster/index/distance.h"
#include "aster/index/tags.h"
#include "aster/index/vector_index.h"
#include "aster/platform/s3_storage.h"
#include "aster/query/topk.h"
#include "aster/storage/compaction.h"
#include "aster/storage/manifest.h"
#include "aster/storage/sstable.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_build.h"
#include "aster/index/hnsw_graph.h"
#include "aster/index/hnsw_pin.h"
#endif

#include <fstream>

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
// When `tags` is non-empty, only rows that contain all tags are considered
// so selective filters cannot be starved by unfiltered over-fetch.
std::vector<SearchHit> MemtableTopK(const Memtable& memtable, Metric metric,
                                    VectorView query, uint32_t top_k,
                                    const std::set<std::string>& tags) {
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
    if (!tags.empty() && !HasAllTags(row, tags)) return;
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

// Estimate filter selectivity σ across live segments (+ memtable tag scan).
double EstimateFilterSelectivity(
    const Memtable& memtable,
    const std::vector<std::shared_ptr<const Segment>>& segments,
    const std::set<std::string>& tags) {
  if (tags.empty()) return 1.0;
  uint64_t match = 0;
  uint64_t total = 0;
  for (const auto& segment : segments) {
    total += segment->row_count();
    match += segment->tag_index().MatchCount(tags);
  }
  memtable.ForEach([&](const Row& row) {
    if (row.tombstone) return;
    ++total;
    if (HasAllTags(row, tags)) ++match;
  });
  if (total == 0) return 1.0;
  return static_cast<double>(match) / static_cast<double>(total);
}

// Bound the WAL payload size before allocating the encode buffer.
size_t EncodedRowSize(const Row& row) {
  size_t n = 4 + row.id.size() + 8 + 8 + 1 + 4 + row.vector.size() * 4 + 4 +
             row.metadata.size() + 4;
  for (const auto& tag : row.tags) n += 4 + tag.size();
  return n;
}

void PutU32At(char* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
void PutU64At(char* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

// Encodes `row` into a contiguous buffer allocated from `arena` (write path).
// Returns a view over arena memory; valid until arena Reset().
std::string_view EncodeRowToArena(const Row& row, Arena& arena) {
  const size_t need = EncodedRowSize(row);
  char* buf = static_cast<char*>(arena.Allocate(need));
  char* p = buf;

  PutU32At(p, static_cast<uint32_t>(row.id.size()));
  p += 4;
  std::memcpy(p, row.id.data(), row.id.size());
  p += row.id.size();

  PutU64At(p, row.timestamp);
  p += 8;
  PutU64At(p, row.version);
  p += 8;
  *p++ = row.tombstone ? 1 : 0;

  PutU32At(p, static_cast<uint32_t>(row.vector.size()));
  p += 4;
  for (float f : row.vector) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    PutU32At(p, u);
    p += 4;
  }

  PutU32At(p, static_cast<uint32_t>(row.metadata.size()));
  p += 4;
  std::memcpy(p, row.metadata.data(), row.metadata.size());
  p += row.metadata.size();

  PutU32At(p, static_cast<uint32_t>(row.tags.size()));
  p += 4;
  for (const auto& tag : row.tags) {
    PutU32At(p, static_cast<uint32_t>(tag.size()));
    p += 4;
    std::memcpy(p, tag.data(), tag.size());
    p += tag.size();
  }
  return std::string_view(buf, static_cast<size_t>(p - buf));
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

bool IsHnswFileName(const std::string& name) {
  // seg_NNNNNN.hnsw or leftover .hnsw.tmp
  if (name.rfind("seg_", 0) != 0) return false;
  return name.find(".hnsw") != std::string::npos;
}

Result<std::string> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::IoError("open failed: " + path);
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) {
    return Status::IoError("read failed: " + path);
  }
  return data;
}

Status ValidateStorageOptions(const Db::Options& options) {
  if (options.storage_mode != StorageMode::kHot && !options.object_store) {
    return Status::InvalidArgument(
        "WARM/COLD storage modes require Options::object_store");
  }
  return Status::Ok();
}

#if ASTER_ENABLE_HNSW
std::unique_ptr<VectorIndex> BuildSegmentHnsw(Metric metric,
                                              const HnswParams& params,
                                              uint64_t rng_seed,
                                              const std::vector<Row>& rows) {
  std::vector<IndexEntry> entries;
  entries.reserve(rows.size());
  for (const Row& row : rows) {
    if (row.tombstone || row.vector.empty()) continue;
    entries.push_back({row.id, row.vector});
  }
  return BuildHnswIndex(metric, params, std::move(entries), rng_seed);
}

Status WriteHnswAtomic(const std::string& path, Metric metric,
                       const HnswParams& params, uint64_t rng_seed,
                       uint64_t segment_id, const std::vector<Row>& rows) {
  std::vector<std::vector<float>> vectors;
  std::vector<uint32_t> ordinals;
  vectors.reserve(rows.size());
  ordinals.reserve(rows.size());
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    if (row.tombstone || row.vector.empty()) continue;
    ordinals.push_back(i);
    vectors.emplace_back(row.vector.begin(), row.vector.end());
  }
  HnswBuilder builder(metric, params, rng_seed);
  auto built = builder.Build(vectors, ordinals);
  if (!built.ok()) return built.status();
  built.value().set_segment_id(segment_id);

  const std::string tmp = path + ".tmp";
  if (auto st = built.value().WriteToFile(tmp); !st.ok()) {
    ::remove(tmp.c_str());
    return st;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::remove(tmp.c_str());
    return Status::IoError("hnsw rename failed: " + path);
  }
  return Status::Ok();
}
#endif  // ASTER_ENABLE_HNSW

}  // namespace

Db::Db(Options options) : Db(std::move(options), DeferFlushThread{}) {
  StartFlushThread();
  StartIndexThread();
}

Db::Db(Options options, DeferFlushThread) : options_(std::move(options)) {
#if ASTER_ENABLE_HNSW
  // Accuracy profiles map onto concrete HNSW params deterministically.
  if (options_.accuracy_profile.has_value()) {
    options_.hnsw_params =
        HnswParamsFromAccuracyProfile(*options_.accuracy_profile);
  }
#endif
  // Keep memory_budget_bytes and resource_limits.memory_budget_bytes aligned.
  // Prefer an explicit Options::memory_budget_bytes when limits left it at 0
  // (existing callers / tests); otherwise the nested limits win.
  if (options_.resource_limits.memory_budget_bytes == 0 &&
      options_.memory_budget_bytes > 0) {
    options_.resource_limits.memory_budget_bytes = options_.memory_budget_bytes;
  } else if (options_.resource_limits.memory_budget_bytes > 0) {
    options_.memory_budget_bytes = options_.resource_limits.memory_budget_bytes;
  }
  qps_window_start_ = std::chrono::steady_clock::now();
}

Db::~Db() {
  StopIndexThread();
  StopFlushThread();
}

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

void Db::StartIndexThread() {
#if ASTER_ENABLE_HNSW
  if (!options_.background_index_build) return;
  stop_index_thread_ = false;
  index_thread_ = std::thread([this] { BackgroundIndexLoop(); });
#else
  (void)0;
#endif
}

void Db::StopIndexThread() {
  {
    std::lock_guard lock(mu_);
    stop_index_thread_ = true;
  }
  index_cv_.notify_all();
  if (index_thread_.joinable()) index_thread_.join();
}

void Db::RequestIndexBuildLocked() {
#if ASTER_ENABLE_HNSW
  if (!options_.background_index_build) return;
  index_build_requested_ = true;
  index_cv_.notify_one();
#else
  (void)0;
#endif
}

void Db::BackgroundIndexLoop() {
#if ASTER_ENABLE_HNSW
  std::unique_lock lock(mu_);
  while (!stop_index_thread_) {
    index_cv_.wait(lock, [this] {
      return stop_index_thread_ || index_build_requested_;
    });
    if (stop_index_thread_) break;
    index_build_requested_ = false;

    while (!stop_index_thread_) {
      std::shared_ptr<const Segment> target;
      for (const auto& seg : segments_) {
        if (seg->index_state() == SegState::kPending &&
            seg->TryBeginIndexBuild()) {
          target = seg;
          break;
        }
      }
      if (!target) break;
      lock.unlock();
      (void)BuildOneSegmentIndex(std::move(target));
      lock.lock();
    }
  }
#else
  (void)0;
#endif
}

bool Db::ShouldFlushLocked() const {
  if (memtable_.empty()) return false;
  if (memtable_.approximate_bytes() >= options_.memtable_flush_bytes) {
    return true;
  }
  if (options_.memory_budget_bytes > 0 &&
      memtable_.approximate_bytes() >= options_.memory_budget_bytes) {
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

std::string Db::HnswRelativePath(uint64_t id) const {
  char buf[80];
  std::snprintf(buf, sizeof(buf), "index/seg_%06llu.hnsw",
                static_cast<unsigned long long>(id));
  return buf;
}

std::string Db::HnswPath(uint64_t id) const {
  return JoinPath(options_.data_dir, HnswRelativePath(id));
}

std::string Db::ManifestPath() const {
  return JoinPath(options_.data_dir, "MANIFEST");
}

std::string Db::WalPath() const {
  return JoinPath(options_.data_dir, "WAL");
}

void Db::GarbageCollectOrphans() {
  if (options_.data_dir.empty()) return;

  std::set<std::string> live_ast;
  std::set<std::string> live_hnsw;
  for (const auto& seg : segments_) {
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(seg->id()));
    live_ast.insert(name);
    if (seg->index_state() == SegState::kReady) {
      live_hnsw.insert(HnswRelativePath(seg->id()));
    }
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
    if (IsSegmentFileName(name)) {
      if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
        ::remove(JoinPath(options_.data_dir, name).c_str());
        continue;
      }
      if (live_ast.count(name) == 0) {
        ::remove(JoinPath(options_.data_dir, name).c_str());
      }
      continue;
    }
  }
  ::closedir(dir);

  // GC index/ orphans (READY graphs for dropped segments).
  const std::string index_dir = JoinPath(options_.data_dir, "index");
  DIR* idir = ::opendir(index_dir.c_str());
  if (!idir) return;
  while (dirent* ent = ::readdir(idir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (!IsHnswFileName(name)) continue;
    const std::string rel = std::string("index/") + name;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      ::remove(JoinPath(index_dir, name).c_str());
      continue;
    }
    if (live_hnsw.count(rel) == 0) {
      ::remove(JoinPath(index_dir, name).c_str());
    }
  }
  ::closedir(idir);
}

Result<std::unique_ptr<Db>> Db::Open(Options options) {
  if (options.data_dir.empty()) {
    return Status::InvalidArgument("Open requires data_dir");
  }
  if (auto st = ValidateStorageOptions(options); !st.ok()) return st;
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
      auto segment = Segment::Build(entry.segment_id, db->options_.metric,
                                    std::move(rows));
#if ASTER_ENABLE_HNSW
      // READY graphs are derived data: if the manifest names a .hnsw and the
      // file exists, rebuild the in-memory graph and mark READY. Missing or
      // partial graphs stay PENDING (TLA: crash during BUILDING → PENDING).
      const std::string hnsw_path =
          entry.hnsw_path.empty()
              ? db->HnswPath(entry.segment_id)
              : JoinPath(db->options_.data_dir, entry.hnsw_path);
      if (!entry.hnsw_path.empty() && FileExists(hnsw_path)) {
        if (segment->TryBeginIndexBuild()) {
          auto hnsw =
              BuildSegmentHnsw(db->options_.metric, db->options_.hnsw_params,
                               db->options_.hnsw_rng_seed, segment->rows());
          segment->CompleteIndexBuild(std::move(hnsw));
        }
      }
#endif
      db->segments_.push_back(std::move(segment));
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
  if (auto st = db->ApplyObjectStorePolicyLocked(); !st.ok()) return st;
  if (db->ShouldFlushLocked()) db->flush_requested_ = true;
  db->StartFlushThread();
  db->StartIndexThread();
  {
    std::lock_guard lock(db->mu_);
    db->RequestIndexBuildLocked();
  }
  return db;
}

Status Db::AppendWal(const Row& row) {
  if (!wal_.has_value()) return Status::Ok();
  // Encode into the write arena, then copy into a std::string for WalWriter.
  // Arena bytes are reclaimed on Reset after a successful append.
  const std::string_view encoded = EncodeRowToArena(row, write_arena_);
  Status st = wal_->Append(std::string(encoded));
  write_arena_.Reset();
  return st;
}

size_t Db::ApproximateWriteMemoryLocked() const {
  return memtable_.approximate_bytes() + write_arena_.MemoryUsage();
}

size_t Db::ProjectedMemtableBytesLocked(const Row& row) const {
  const size_t incoming = EstimateRowBytes(row);
  if (auto existing = memtable_.Get(row.id)) {
    if (!NewerThan(row, *existing)) {
      return memtable_.approximate_bytes();
    }
    const size_t old_bytes = EstimateRowBytes(*existing);
    return memtable_.approximate_bytes() - old_bytes + incoming;
  }
  return memtable_.approximate_bytes() + incoming;
}

Status Db::EnsureWriteMemoryLocked(const Row& row) {
  if (options_.memory_budget_bytes == 0) return Status::Ok();

  const size_t incoming = EstimateRowBytes(row);
  if (incoming > options_.memory_budget_bytes) {
    return Status::ResourceExhausted("memory budget exceeded");
  }

  auto fits = [&]() {
    const size_t projected =
        ProjectedMemtableBytesLocked(row) + write_arena_.MemoryUsage();
    return projected <= options_.memory_budget_bytes;
  };

  if (fits()) return Status::Ok();

  // Reclaim memtable via flush before rejecting the write.
  if (!memtable_.empty()) {
    if (auto st = FlushLocked(); !st.ok()) return st;
    write_arena_.Reset();
  }
  if (fits()) return Status::Ok();

  return Status::ResourceExhausted("memory budget exceeded");
}

size_t Db::LiveRowCountLocked() const {
  // Segment / memtable rows may be superseded; reconcile unique ids.
  std::set<RowId> seen;
  memtable_.ForEach([&](const Row& row) { seen.insert(row.id); });
  for (const auto& segment : segments_) {
    for (const Row& row : segment->rows()) seen.insert(row.id);
  }
  size_t n = 0;
  for (const RowId& id : seen) {
    const Row row = Reconcile(id);
    if (!row.tombstone) ++n;
  }
  return n;
}

uint64_t Db::EstimatedStorageBytesLocked() const {
  const uint32_t dim = options_.dimension;
  if (dim == 0) return 0;
  return static_cast<uint64_t>(LiveRowCountLocked()) *
         static_cast<uint64_t>(dim) * sizeof(float);
}

Status Db::EnsureResourceLimitsLocked(const Row& row) {
  if (row.tombstone) return Status::Ok();
  const ResourceLimits& lim = options_.resource_limits;
  if (lim.max_vectors == 0 && lim.storage_quota_bytes == 0) {
    return Status::Ok();
  }

  // Updates to an existing live id do not consume an additional vector slot
  // or extra storage quota beyond the (already counted) live row.
  const Row existing = Reconcile(row.id);
  const bool is_new_live = existing.tombstone;

  if (lim.max_vectors > 0 && is_new_live) {
    if (LiveRowCountLocked() >= lim.max_vectors) {
      return Status::ResourceExhausted("max vectors exceeded");
    }
  }

  if (lim.storage_quota_bytes > 0 && is_new_live) {
    const uint32_t dim =
        options_.dimension != 0 ? options_.dimension
                                : static_cast<uint32_t>(row.vector.size());
    const uint64_t add =
        static_cast<uint64_t>(dim) * static_cast<uint64_t>(sizeof(float));
    if (EstimatedStorageBytesLocked() + add > lim.storage_quota_bytes) {
      return Status::ResourceExhausted("storage quota exceeded");
    }
  }
  return Status::Ok();
}

Status Db::AdmitSearchLocked() const {
  const uint32_t max_qps = options_.resource_limits.max_qps;
  if (max_qps == 0) return Status::Ok();

  const auto now = std::chrono::steady_clock::now();
  if (now - qps_window_start_ >= std::chrono::seconds(1)) {
    qps_window_start_ = now;
    qps_window_count_ = 0;
  }
  if (qps_window_count_ >= max_qps) {
    return Status::ResourceExhausted("max QPS exceeded");
  }
  ++qps_window_count_;
  return Status::Ok();
}

Status Db::PublishManifest() {
  Manifest m;
  m.generation = ++manifest_generation_;
  for (const auto& seg : segments_) {
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(seg->id()));
    ManifestEntry entry;
    entry.segment_id = seg->id();
    entry.path = name;
    if (seg->index_state() == SegState::kReady) {
      entry.hnsw_path = HnswRelativePath(seg->id());
    }
    m.segments.push_back(std::move(entry));
  }
  return WriteManifest(ManifestPath(), m);
}

Status Db::Upsert(Row row) {
  std::lock_guard lock(mu_);
  if (options_.dimension != 0 && !row.tombstone &&
      row.vector.size() != options_.dimension) {
    return Status::InvalidArgument("vector dimension mismatch");
  }
  if (auto st = EnsureResourceLimitsLocked(row); !st.ok()) return st;
  if (auto st = EnsureWriteMemoryLocked(row); !st.ok()) return st;
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

std::vector<SearchHit> Db::SearchLocked(const SearchRequest& request) const {
  // COLD workers drop the LRU block cache before each search; non-evictable
  // upper-layer pins remain (docs/indexing.md §10.3.1 / client-api.md).
  if (ClearsBlockCacheOnSearch(options_.storage_mode) && options_.object_store) {
    options_.object_store->ClearCache();
  }
  const double sigma =
      EstimateFilterSelectivity(memtable_, segments_, request.tags);
  // `SearchRequest::ef_search==0` means "use the index default" (HNSW uses
  // `HnswParams::ef_search_default`). We need that same effective value for
  // filtered over-fetch selection so COST_OPTIMIZED..MAX_RECALL affects fetch
  // deterministically too.
#if ASTER_ENABLE_HNSW
  const uint32_t ef_search_effective =
      request.ef_search == 0 ? options_.hnsw_params.ef_search_default
                             : request.ef_search;
#else
  const uint32_t ef_search_effective = request.ef_search;
#endif
  const uint32_t fetch_k =
      request.tags.empty()
          ? BaseFetchK(request.top_k)
          : AdaptiveFetchK(request.top_k, ef_search_effective, sigma);
  std::vector<std::vector<SearchHit>> candidates;
  candidates.reserve(1 + segments_.size());
  candidates.push_back(MemtableTopK(memtable_, options_.metric, request.vector,
                                    fetch_k, request.tags));
  for (const auto& segment : segments_) {
    candidates.push_back(segment->Search(request.vector, fetch_k,
                                         request.ef_search, request.tags));
  }

  std::vector<SearchHit> merged = MergeTopK(candidates, fetch_k);
  std::vector<SearchHit> results;
  results.reserve(request.top_k);
  for (const SearchHit& hit : merged) {
    if (results.size() >= request.top_k) break;
    const Row row = Reconcile(hit.id);
    if (row.tombstone) continue;
    // Defense-in-depth post-filter after LWW reconcile (segment bitmaps are
    // per-segment; a newer memtable/segment version may drop tags).
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

Result<std::vector<SearchHit>> Db::TrySearch(
    const SearchRequest& request) const {
  std::lock_guard lock(mu_);
  if (auto st = AdmitSearchLocked(); !st.ok()) return st;
  return SearchLocked(request);
}

std::vector<SearchHit> Db::Search(const SearchRequest& request) const {
  auto got = TrySearch(request);
  if (!got.ok()) return {};
  return std::move(got.value());
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
  write_arena_.Reset();

  if (!options_.data_dir.empty()) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric, *rows);
        !st.ok()) {
      for (Row& row : *rows) memtable_.Apply(std::move(row));
      --next_segment_id_;
      memtable_live_since_ = std::chrono::steady_clock::now();
      return st;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(id));
    if (auto st = MirrorObjectLocked(name, SegmentPath(id)); !st.ok()) {
      ::remove(SegmentPath(id).c_str());
      for (Row& row : *rows) memtable_.Apply(std::move(row));
      --next_segment_id_;
      memtable_live_since_ = std::chrono::steady_clock::now();
      return st;
    }
  }

  segments_.push_back(std::move(segment));
  RequestIndexBuildLocked();

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
                                   /*drop_tombstones=*/full_overlap
#if ASTER_ENABLE_HNSW
                                   ,
                                   options_.hnsw_params, options_.hnsw_rng_seed,
                                   options_.hnsw_compaction_insert_into_largest,
                                   options_.hnsw_compaction_staleness_debt_threshold
#endif
  );
  const bool omit_empty = full_overlap && compacted->row_count() == 0;

  if (!options_.data_dir.empty() && !omit_empty) {
    if (auto st = WriteSstable(SegmentPath(id), id, options_.metric,
                               compacted->rows());
        !st.ok()) {
      return st;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(id));
    if (auto st = MirrorObjectLocked(name, SegmentPath(id)); !st.ok()) {
      ::remove(SegmentPath(id).c_str());
      return st;
    }
#if ASTER_ENABLE_HNSW
    // Persist the rebuilt READY graph before publishing the manifest so
    // reopen sees hnsw_path (docs/indexing.md §6.2 atomic swap).
    if (compacted->search_uses_hnsw()) {
      if (auto st = EnsureDir(JoinPath(options_.data_dir, "index")); !st.ok()) {
        ::remove(SegmentPath(id).c_str());
        return st;
      }
      if (auto st = WriteHnswAtomic(HnswPath(id), options_.metric,
                                    options_.hnsw_params,
                                    options_.hnsw_rng_seed, id,
                                    compacted->rows());
          !st.ok()) {
        ::remove(SegmentPath(id).c_str());
        return st;
      }
      if (auto st =
              MirrorObjectLocked(HnswRelativePath(id), HnswPath(id));
          !st.ok()) {
        ::remove(SegmentPath(id).c_str());
        ::remove(HnswPath(id).c_str());
        return st;
      }
      if (auto st = PinHnswUpperLayersLocked(id); !st.ok()) {
        ::remove(SegmentPath(id).c_str());
        ::remove(HnswPath(id).c_str());
        return st;
      }
    }
#endif
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
  // Compacted output is already READY (rebuild-from-rows). Wake the index
  // thread only for any remaining PENDING flush segments.
  RequestIndexBuildLocked();

  if (!options_.data_dir.empty()) {
    if (auto st = PublishManifest(); !st.ok()) return st;
    for (const auto& path : old_paths) {
      ::remove(path.c_str());
    }
    GarbageCollectOrphans();
  }
  return Status::Ok();
}

bool Db::BuildOneSegmentIndex(std::shared_ptr<const Segment> segment) {
#if ASTER_ENABLE_HNSW
  if (!segment) return false;
  if (segment->index_state() != SegState::kBuilding) return false;

  std::unique_ptr<VectorIndex> hnsw =
      BuildSegmentHnsw(options_.metric, options_.hnsw_params,
                       options_.hnsw_rng_seed, segment->rows());

  if (!options_.data_dir.empty()) {
    if (auto st = EnsureDir(JoinPath(options_.data_dir, "index")); !st.ok()) {
      std::lock_guard lock(mu_);
      if (segment->index_state() == SegState::kBuilding) {
        segment->AbortIndexBuild();
      }
      return false;
    }
    if (auto st = WriteHnswAtomic(HnswPath(segment->id()), options_.metric,
                                  options_.hnsw_params, options_.hnsw_rng_seed,
                                  segment->id(), segment->rows());
        !st.ok()) {
      std::lock_guard lock(mu_);
      if (segment->index_state() == SegState::kBuilding) {
        segment->AbortIndexBuild();
      }
      return false;
    }
    if (auto st = MirrorObjectLocked(HnswRelativePath(segment->id()),
                                     HnswPath(segment->id()));
        !st.ok()) {
      std::lock_guard lock(mu_);
      if (segment->index_state() == SegState::kBuilding) {
        segment->AbortIndexBuild();
      }
      return false;
    }
    if (auto st = PinHnswUpperLayersLocked(segment->id()); !st.ok()) {
      std::lock_guard lock(mu_);
      if (segment->index_state() == SegState::kBuilding) {
        segment->AbortIndexBuild();
      }
      return false;
    }
  }

  std::lock_guard lock(mu_);
  bool live = false;
  for (const auto& seg : segments_) {
    if (seg.get() == segment.get()) {
      live = true;
      break;
    }
  }
  if (!live || segment->index_state() != SegState::kBuilding) {
    // Compacted away or aborted; leave any on-disk .hnsw for GC.
    return false;
  }
  segment->CompleteIndexBuild(std::move(hnsw));
  if (!options_.data_dir.empty()) {
    (void)PublishManifest();
  }
  return true;
#else
  (void)segment;
  return false;
#endif
}

Status Db::BuildPendingIndexes() {
#if ASTER_ENABLE_HNSW
  for (;;) {
    std::shared_ptr<const Segment> target;
    {
      std::lock_guard lock(mu_);
      for (const auto& seg : segments_) {
        if (seg->index_state() == SegState::kPending &&
            seg->TryBeginIndexBuild()) {
          target = seg;
          break;
        }
      }
    }
    if (!target) return Status::Ok();
    if (!BuildOneSegmentIndex(std::move(target))) {
      // Aborted or compacted; continue draining remaining PENDING.
      continue;
    }
  }
#else
  return Status::Ok();
#endif
}

std::vector<SegState> Db::segment_index_states() const {
  std::lock_guard lock(mu_);
  std::vector<SegState> out;
  out.reserve(segments_.size());
  for (const auto& seg : segments_) {
    out.push_back(seg->index_state());
  }
  return out;
}

std::vector<bool> Db::segment_uses_hnsw() const {
  std::lock_guard lock(mu_);
  std::vector<bool> out;
  out.reserve(segments_.size());
  for (const auto& seg : segments_) {
    out.push_back(seg->search_uses_hnsw());
  }
  return out;
}

std::vector<size_t> Db::segment_hnsw_index_sizes() const {
  std::lock_guard lock(mu_);
  std::vector<size_t> out;
  out.reserve(segments_.size());
  for (const auto& seg : segments_) {
    out.push_back(seg->hnsw_index_size());
  }
  return out;
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

size_t Db::approximate_write_memory_bytes() const {
  std::lock_guard lock(mu_);
  return ApproximateWriteMemoryLocked();
}

StorageMode Db::storage_mode() const {
  std::lock_guard lock(mu_);
  return options_.storage_mode;
}

Status Db::SetStorageMode(StorageMode mode) {
  std::lock_guard lock(mu_);
  if (mode == options_.storage_mode) return Status::Ok();
  if (mode != StorageMode::kHot && !options_.object_store) {
    return Status::InvalidArgument(
        "WARM/COLD storage modes require Options::object_store");
  }
  options_.storage_mode = mode;
  return ApplyObjectStorePolicyLocked();
}

ResourceLimits Db::resource_limits() const {
  std::lock_guard lock(mu_);
  return options_.resource_limits;
}

Status Db::SetResourceLimits(ResourceLimits limits) {
  std::lock_guard lock(mu_);
  options_.resource_limits = limits;
  options_.memory_budget_bytes = limits.memory_budget_bytes;
  // Reset the QPS window so a newly raised/lowered cap takes effect cleanly.
  qps_window_start_ = std::chrono::steady_clock::now();
  qps_window_count_ = 0;
  return Status::Ok();
}

Status Db::MirrorObjectLocked(const std::string& relative_key,
                              const std::string& absolute_path) {
  if (!MirrorsToObjectStore(options_.storage_mode)) return Status::Ok();
  if (!options_.object_store) {
    return Status::InvalidArgument(
        "WARM/COLD storage modes require Options::object_store");
  }
  auto bytes = ReadFileBytes(absolute_path);
  if (!bytes.ok()) return bytes.status();
  return options_.object_store->Put(relative_key, bytes.value());
}

Status Db::PinHnswUpperLayersLocked(uint64_t segment_id) {
#if ASTER_ENABLE_HNSW
  if (!PinsHnswUpperLayers(options_.storage_mode)) return Status::Ok();
  if (!options_.object_store) {
    return Status::InvalidArgument(
        "WARM/COLD storage modes require Options::object_store");
  }
  const std::string rel = HnswRelativePath(segment_id);
  const std::string abs = HnswPath(segment_id);
  if (!FileExists(abs)) return Status::Ok();
  auto bytes = ReadFileBytes(abs);
  if (!bytes.ok()) return bytes.status();
  // Ensure the object exists remotely before pinning ranges against it.
  if (auto st = options_.object_store->Put(rel, bytes.value()); !st.ok()) {
    return st;
  }
  auto pin = HnswUpperLayerPin::FromSerialized(bytes.value());
  if (!pin.ok()) return pin.status();
  for (const HnswPinRange& range : pin.value().PinRanges()) {
    if (range.end <= range.start) continue;
    if (auto st = options_.object_store->PinRange(
            rel, range.start, range.end,
            bytes.value().substr(range.start, range.end - range.start));
        !st.ok()) {
      return st;
    }
  }
  return Status::Ok();
#else
  (void)segment_id;
  return Status::Ok();
#endif
}

Status Db::ApplyObjectStorePolicyLocked() {
  if (options_.storage_mode == StorageMode::kHot) return Status::Ok();
  if (!options_.object_store) {
    return Status::InvalidArgument(
        "WARM/COLD storage modes require Options::object_store");
  }
  if (options_.data_dir.empty()) return Status::Ok();

  for (const auto& seg : segments_) {
    char name[64];
    std::snprintf(name, sizeof(name), "seg_%06llu.ast",
                  static_cast<unsigned long long>(seg->id()));
    const std::string abs = SegmentPath(seg->id());
    if (FileExists(abs)) {
      if (auto st = MirrorObjectLocked(name, abs); !st.ok()) return st;
    }
#if ASTER_ENABLE_HNSW
    if (seg->index_state() == SegState::kReady) {
      if (auto st = PinHnswUpperLayersLocked(seg->id()); !st.ok()) return st;
    }
#endif
  }
  return Status::Ok();
}

}  // namespace aster
