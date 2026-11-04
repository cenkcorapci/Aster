#include "aster/server/catalog.h"

#include <dirent.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace aster {
namespace {

#if ASTER_ENABLE_HNSW
void ApplyHnswParams(const CollectionInfo& info, Db::Options& db_opt) {
  if (info.hnsw_m > 0) db_opt.hnsw_params.m = info.hnsw_m;
  if (info.hnsw_ef_construction > 0) {
    db_opt.hnsw_params.ef_construction = info.hnsw_ef_construction;
  }
  if (info.hnsw_ef_search_default > 0) {
    db_opt.hnsw_params.ef_search_default = info.hnsw_ef_search_default;
  }
}
#endif

bool EnsureDir(const std::string& path) {
  if (path.empty()) return false;
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  return ::mkdir(path.c_str(), 0755) == 0;
}

// Best-effort recursive delete of a collection data directory.
void RemoveTree(const std::string& path) {
  DIR* dir = ::opendir(path.c_str());
  if (!dir) {
    ::remove(path.c_str());
    return;
  }
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    const std::string child = path + "/" + name;
    struct stat st {};
    if (::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RemoveTree(child);
    } else {
      ::remove(child.c_str());
    }
  }
  ::closedir(dir);
  ::rmdir(path.c_str());
}

}  // namespace

Catalog::Catalog(Options options) : options_(std::move(options)) {}

Result<std::unique_ptr<Catalog>> Catalog::Open(Options options) {
  if (options.data_dir.empty()) {
    return Status::InvalidArgument("Catalog requires data_dir");
  }
  if (!EnsureDir(options.data_dir)) {
    return Status::IoError("cannot create data_dir");
  }
  auto catalog = std::unique_ptr<Catalog>(new Catalog(std::move(options)));
  if (auto st = catalog->Load(); !st.ok()) return st;
  return catalog;
}

Status Catalog::ValidateName(const std::string& name) {
  if (name.empty() || name.size() > 128) {
    return Status::InvalidArgument("invalid collection name");
  }
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return Status::InvalidArgument("invalid collection name");
  }
  if (name == "CATALOG" || name == "." || name == "..") {
    return Status::InvalidArgument("reserved collection name");
  }
  return Status::Ok();
}

std::string Catalog::MetricToString(Metric m) {
  switch (m) {
    case Metric::kL2:
      return "l2";
    case Metric::kDot:
      return "dot";
    case Metric::kCosine:
      return "cosine";
  }
  return "cosine";
}

Result<Metric> Catalog::MetricFromString(const std::string& s) {
  if (s == "l2" || s == "L2") return Metric::kL2;
  if (s == "dot" || s == "DOT") return Metric::kDot;
  if (s == "cosine" || s == "COSINE") return Metric::kCosine;
  return Status::InvalidArgument("unknown metric");
}

std::string Catalog::CollectionDir(const std::string& name) const {
  return options_.data_dir + "/" + name;
}

Status Catalog::Load() {
  const std::string path = options_.data_dir + "/CATALOG";
  std::ifstream in(path);
  if (!in) return Status::Ok();

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const size_t t1 = line.find('\t');
    const size_t t2 =
        t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos) {
      return Status::Corruption("bad CATALOG line");
    }
    const size_t t3 = line.find('\t', t2 + 1);
    CollectionInfo info;
    info.name = line.substr(0, t1);
    info.dimension =
        static_cast<uint32_t>(std::stoul(line.substr(t1 + 1, t2 - t1 - 1)));
    const std::string metric_s =
        t3 == std::string::npos ? line.substr(t2 + 1)
                                : line.substr(t2 + 1, t3 - t2 - 1);
    auto metric = MetricFromString(metric_s);
    if (!metric.ok()) return metric.status();
    info.metric = metric.value();
    if (t3 != std::string::npos) {
      // Optional: storage_mode [\t max_vectors \t memory \t max_qps
      //                        \t storage_quota \t isolation]
      std::string rest = line.substr(t3 + 1);
      const size_t lim0 = rest.find('\t');
      const std::string mode_s =
          lim0 == std::string::npos ? rest : rest.substr(0, lim0);
      auto mode = StorageModeFromString(mode_s);
      if (!mode.has_value()) {
        return Status::Corruption("bad CATALOG storage mode");
      }
      info.storage_mode = *mode;
      if (lim0 != std::string::npos) {
        // Parse five resource-limit fields.
        std::string lim = rest.substr(lim0 + 1);
        std::string fields[5];
        size_t start = 0;
        for (int i = 0; i < 5; ++i) {
          const size_t tab = lim.find('\t', start);
          fields[i] = (tab == std::string::npos)
                          ? lim.substr(start)
                          : lim.substr(start, tab - start);
          if (tab == std::string::npos) {
            start = lim.size();
            if (i < 4) {
              return Status::Corruption("bad CATALOG resource limits");
            }
            break;
          }
          start = tab + 1;
        }
        try {
          info.resource_limits.max_vectors =
              static_cast<uint64_t>(std::stoull(fields[0]));
          info.resource_limits.memory_budget_bytes =
              static_cast<size_t>(std::stoull(fields[1]));
          info.resource_limits.max_qps =
              static_cast<uint32_t>(std::stoul(fields[2]));
          info.resource_limits.storage_quota_bytes =
              static_cast<uint64_t>(std::stoull(fields[3]));
        } catch (...) {
          return Status::Corruption("bad CATALOG resource limits");
        }
        auto iso = IsolationLevelFromString(fields[4]);
        if (!iso.has_value()) {
          return Status::Corruption("bad CATALOG isolation level");
        }
        info.resource_limits.isolation = *iso;
#if ASTER_ENABLE_HNSW
        if (start < lim.size()) {
          std::string hnsw_rest = lim.substr(start);
          std::string hnsw_fields[3];
          size_t hs = 0;
          for (int i = 0; i < 3; ++i) {
            const size_t tab =
                (i == 2) ? std::string::npos : hnsw_rest.find('\t', hs);
            hnsw_fields[i] = (tab == std::string::npos)
                                 ? hnsw_rest.substr(hs)
                                 : hnsw_rest.substr(hs, tab - hs);
            if (tab == std::string::npos && i < 2) break;
            hs = tab + 1;
          }
          try {
            if (!hnsw_fields[0].empty()) {
              info.hnsw_m = static_cast<uint32_t>(std::stoul(hnsw_fields[0]));
            }
            if (!hnsw_fields[1].empty()) {
              info.hnsw_ef_construction =
                  static_cast<uint32_t>(std::stoul(hnsw_fields[1]));
            }
            if (!hnsw_fields[2].empty()) {
              info.hnsw_ef_search_default =
                  static_cast<uint32_t>(std::stoul(hnsw_fields[2]));
            }
          } catch (...) {
            return Status::Corruption("bad CATALOG hnsw params");
          }
        }
#endif
      }
    }

    if (auto st = ValidateName(info.name); !st.ok()) return st;
    if (info.dimension > kMaxDimension) {
      return Status::Corruption("catalog dimension out of range");
    }

    // Lifecycle phase 1: created-but-not-configured placeholders are durable,
    // but their underlying Db is opened only during ConfigureCollection().
    if (info.dimension == 0) {
      infos_[info.name] = info;
      continue;
    }

    Db::Options db_opt;
    db_opt.dimension = info.dimension;
    db_opt.metric = info.metric;
    db_opt.data_dir = CollectionDir(info.name);
    db_opt.wal_sync = options_.wal_sync;
    db_opt.memtable_flush_bytes = options_.memtable_flush_bytes;
    db_opt.compaction_tier_threshold = options_.compaction_tier_threshold;
    db_opt.max_segments_before_compact = options_.max_segments_before_compact;
    db_opt.storage_mode = info.storage_mode;
    db_opt.object_store = options_.object_store;
    db_opt.resource_limits = info.resource_limits;
    db_opt.memory_budget_bytes = info.resource_limits.memory_budget_bytes;
#if ASTER_ENABLE_HNSW
    ApplyHnswParams(info, db_opt);
#endif
    auto db = Db::Open(db_opt);
    if (!db.ok()) return db.status();
    infos_[info.name] = info;
    dbs_[info.name] = std::shared_ptr<Db>(std::move(db.value()));
  }
  return Status::Ok();
}

Status Catalog::PersistCatalog() const {
  const std::string path = options_.data_dir + "/CATALOG";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Status::IoError("cannot write CATALOG.tmp");
    for (const auto& [_, info] : infos_) {
      out << info.name << '\t' << info.dimension << '\t'
          << MetricToString(info.metric) << '\t' << ToString(info.storage_mode)
          << '\t' << info.resource_limits.max_vectors << '\t'
          << info.resource_limits.memory_budget_bytes << '\t'
          << info.resource_limits.max_qps << '\t'
          << info.resource_limits.storage_quota_bytes << '\t'
          << ToString(info.resource_limits.isolation)
#if ASTER_ENABLE_HNSW
          << '\t' << info.hnsw_m << '\t' << info.hnsw_ef_construction << '\t'
          << info.hnsw_ef_search_default
#endif
          << '\n';
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IoError("cannot publish CATALOG");
  }
  return Status::Ok();
}

Status Catalog::CreateCollectionCreated(const std::string& name) {
  if (auto st = ValidateName(name); !st.ok()) return st;

  std::lock_guard lock(mu_);
  if (infos_.count(name)) {
    return Status::InvalidArgument("collection already exists");
  }
  if (!EnsureDir(CollectionDir(name))) {
    return Status::IoError("cannot create collection directory");
  }

  // Default metric is persisted so Load() can validate/parse the catalog
  // format. Dimension==0 is the authoritative lifecycle marker.
  CollectionInfo info;
  info.name = name;
  info.dimension = 0;
  info.metric = Metric::kCosine;
  infos_[name] = info;

  if (auto st = PersistCatalog(); !st.ok()) {
    infos_.erase(name);
    return st;
  }
  return Status::Ok();
}

Status Catalog::ConfigureCollection(const CollectionInfo& info) {
  if (auto st = ValidateName(info.name); !st.ok()) return st;
  if (info.dimension == 0) {
    return Status::InvalidArgument("dimension must be > 0");
  }
  if (info.dimension > kMaxDimension) {
    return Status::InvalidArgument("dimension too large");
  }

  std::lock_guard lock(mu_);
  auto it = infos_.find(info.name);
  if (it == infos_.end()) {
    return Status::NotFound("collection not found");
  }
  if (it->second.dimension != 0) {
    return Status::InvalidArgument("collection already configured");
  }

  const CollectionInfo prev = it->second;

  if (!EnsureDir(CollectionDir(info.name))) {
    return Status::IoError("cannot create collection directory");
  }

  // Persist first so recovery after a crash sees the configured dimension
  // and opens the Db during Catalog::Load().
  infos_[info.name] = info;
  if (auto st = PersistCatalog(); !st.ok()) {
    infos_[info.name] = prev;
    return st;
  }

  Db::Options db_opt;
  db_opt.dimension = info.dimension;
  db_opt.metric = info.metric;
  db_opt.data_dir = CollectionDir(info.name);
  db_opt.wal_sync = options_.wal_sync;
  db_opt.memtable_flush_bytes = options_.memtable_flush_bytes;
  db_opt.compaction_tier_threshold = options_.compaction_tier_threshold;
  db_opt.max_segments_before_compact = options_.max_segments_before_compact;
  db_opt.storage_mode = info.storage_mode;
  db_opt.object_store = options_.object_store;
  db_opt.resource_limits = info.resource_limits;
  db_opt.memory_budget_bytes = info.resource_limits.memory_budget_bytes;
#if ASTER_ENABLE_HNSW
  ApplyHnswParams(info, db_opt);
#endif
  auto db = Db::Open(db_opt);
  if (!db.ok()) {
    // Best-effort rollback to the previous lifecycle phase.
    infos_[info.name] = prev;
    PersistCatalog();
    return db.status();
  }

  dbs_[info.name] = std::shared_ptr<Db>(std::move(db.value()));
  return Status::Ok();
}

Status Catalog::CreateCollection(const CollectionInfo& info) {
  // HTTP API historically used CreateCollection(info) as a single call.
  // Keep that contract by implementing it as create+configure.
  auto st = CreateCollectionCreated(info.name);
  if (!st.ok()) {
    // If the collection already exists, it may be in phase 1 (dimension=0),
    // in which case ConfigureCollection() should succeed.
    if (st.message().find("already exists") == std::string::npos) return st;
  } else {
    // created successfully, now configure
  }
  return ConfigureCollection(info);
}

Status Catalog::DropCollection(const std::string& name) {
  std::string dir;
  {
    std::lock_guard lock(mu_);
    if (!infos_.count(name)) {
      return Status::NotFound("collection not found");
    }
    dir = CollectionDir(name);
    // Drop the Db first so WAL fds close before we unlink files.
    dbs_.erase(name);
    infos_.erase(name);
    if (auto st = PersistCatalog(); !st.ok()) return st;
  }
  RemoveTree(dir);
  return Status::Ok();
}

std::vector<CollectionInfo> Catalog::ListCollections() const {
  std::lock_guard lock(mu_);
  std::vector<CollectionInfo> out;
  out.reserve(infos_.size());
  for (const auto& [_, info] : infos_) out.push_back(info);
  return out;
}

std::optional<CollectionInfo> Catalog::GetCollection(
    const std::string& name) const {
  std::lock_guard lock(mu_);
  auto it = infos_.find(name);
  if (it == infos_.end()) return std::nullopt;
  return it->second;
}

Result<std::shared_ptr<Db>> Catalog::LookupDb(const std::string& name) const {
  auto it = dbs_.find(name);
  if (it == dbs_.end()) {
    auto info_it = infos_.find(name);
    if (info_it == infos_.end()) return Status::NotFound("collection not found");
    if (info_it->second.dimension == 0) {
      return Status(StatusCode::kUnavailable, "collection not configured");
    }
    return Status::NotFound("collection not found");
  }
  return it->second;
}

Status Catalog::Upsert(const std::string& collection, Row row) {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
  }
  auto st = db->Upsert(std::move(row));
  if (st.ok()) {
    std::lock_guard lock(mu_);
    ++usage_.upserts;
  }
  return st;
}

Status Catalog::Delete(const std::string& collection, const RowId& id,
                       Timestamp timestamp) {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
  }
  auto st = db->Delete(id, timestamp);
  if (st.ok()) {
    std::lock_guard lock(mu_);
    ++usage_.deletes;
  }
  return st;
}

Result<std::optional<Row>> Catalog::Get(const std::string& collection,
                                        const RowId& id) const {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
    ++usage_.gets;
  }
  return db->Get(id);
}

Result<std::vector<SearchHit>> Catalog::Search(
    const std::string& collection, const SearchRequest& request) const {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
    ++usage_.searches;
  }
  return db->TrySearch(request);
}

Status Catalog::Flush(const std::string& collection,
                      CatalogFlushStats* stats) {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto st = db->Flush();
  const auto t1 = std::chrono::steady_clock::now();
  if (!st.ok()) return st;
#if ASTER_ENABLE_HNSW
  st = db->BuildPendingIndexes();
#endif
  const auto t2 = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->memtable_flush_seconds =
        std::chrono::duration<double>(t1 - t0).count();
    stats->index_build_seconds =
        std::chrono::duration<double>(t2 - t1).count();
  }
  return st;
}

Status Catalog::Compact(const std::string& collection) {
  std::shared_ptr<Db> db;
  {
    std::lock_guard lock(mu_);
    auto got = LookupDb(collection);
    if (!got.ok()) return got.status();
    db = got.value();
  }
  return db->Compact();
}

UsageStats Catalog::Usage() const {
  std::lock_guard lock(mu_);
  UsageStats u = usage_;
  u.collections = infos_.size();
  size_t vectors = 0;
  size_t segments = 0;
  size_t mem_rows = 0;
  size_t backlog = 0;
  for (const auto& [_, db] : dbs_) {
    vectors += db->approximate_row_count();
    segments += db->segment_count();
    mem_rows += db->memtable_rows();
    const size_t seg = db->segment_count();
    if (seg > options_.compaction_tier_threshold) {
      backlog += seg - options_.compaction_tier_threshold;
    }
  }
  u.vectors_estimate = vectors;
  u.segments = segments;
  u.memtable_rows = mem_rows;
  u.compaction_backlog = backlog;
  return u;
}

std::optional<CollectionStats> Catalog::Stats(
    const std::string& collection) const {
  std::shared_ptr<Db> db;
  CollectionInfo info;
  {
    std::lock_guard lock(mu_);
    auto info_it = infos_.find(collection);
    auto db_it = dbs_.find(collection);
    if (info_it == infos_.end() || db_it == dbs_.end()) return std::nullopt;
    info = info_it->second;
    db = db_it->second;
  }
  CollectionStats s;
  s.dimension = info.dimension;
  s.metric = info.metric;
  s.vectors_estimate = db->approximate_row_count();
  s.segments = db->segment_count();
  s.memtable_rows = db->memtable_rows();
  s.write_memory_bytes = db->approximate_write_memory_bytes();
  if (s.segments > options_.compaction_tier_threshold) {
    s.compaction_backlog = s.segments - options_.compaction_tier_threshold;
  }
  return s;
}

}  // namespace aster
