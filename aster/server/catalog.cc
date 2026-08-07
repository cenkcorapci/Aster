#include "aster/server/catalog.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include "aster/metrics/metrics.h"

namespace aster {
namespace {

bool EnsureDir(const std::string& path) {
  if (path.empty()) return false;
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  return ::mkdir(path.c_str(), 0755) == 0;
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
    CollectionInfo info;
    info.name = line.substr(0, t1);
    info.dimension =
        static_cast<uint32_t>(std::stoul(line.substr(t1 + 1, t2 - t1 - 1)));
    auto metric = MetricFromString(line.substr(t2 + 1));
    if (!metric.ok()) return metric.status();
    info.metric = metric.value();

    if (auto st = ValidateName(info.name); !st.ok()) return st;

    Db::Options db_opt;
    db_opt.dimension = info.dimension;
    db_opt.metric = info.metric;
    db_opt.data_dir = CollectionDir(info.name);
    db_opt.wal_sync = options_.wal_sync;
    db_opt.memtable_flush_bytes = options_.memtable_flush_bytes;
    db_opt.max_segments_before_compact = options_.max_segments_before_compact;
    auto db = Db::Open(db_opt);
    if (!db.ok()) return db.status();
    infos_[info.name] = info;
    dbs_[info.name] = std::move(db.value());
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
          << MetricToString(info.metric) << '\n';
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IoError("cannot publish CATALOG");
  }
  return Status::Ok();
}

Status Catalog::CreateCollection(const CollectionInfo& info) {
  if (auto st = ValidateName(info.name); !st.ok()) return st;
  if (info.dimension == 0) {
    return Status::InvalidArgument("dimension must be > 0");
  }
  if (info.dimension > 8192) {
    return Status::InvalidArgument("dimension too large");
  }

  std::lock_guard lock(mu_);
  if (infos_.count(info.name)) {
    return Status::InvalidArgument("collection already exists");
  }
  if (!EnsureDir(CollectionDir(info.name))) {
    return Status::IoError("cannot create collection directory");
  }

  Db::Options db_opt;
  db_opt.dimension = info.dimension;
  db_opt.metric = info.metric;
  db_opt.data_dir = CollectionDir(info.name);
  db_opt.wal_sync = options_.wal_sync;
  db_opt.memtable_flush_bytes = options_.memtable_flush_bytes;
  db_opt.max_segments_before_compact = options_.max_segments_before_compact;
  auto db = Db::Open(db_opt);
  if (!db.ok()) return db.status();

  infos_[info.name] = info;
  dbs_[info.name] = std::move(db.value());
  if (auto st = PersistCatalog(); !st.ok()) {
    infos_.erase(info.name);
    dbs_.erase(info.name);
    return st;
  }
  return Status::Ok();
}

Status Catalog::DropCollection(const std::string& name) {
  std::lock_guard lock(mu_);
  if (!infos_.count(name)) {
    return Status::NotFound("collection not found");
  }
  dbs_.erase(name);
  infos_.erase(name);
  return PersistCatalog();
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

Result<Db*> Catalog::MutableDb(const std::string& name) {
  auto it = dbs_.find(name);
  if (it == dbs_.end()) return Status::NotFound("collection not found");
  return it->second.get();
}

Result<const Db*> Catalog::ConstDb(const std::string& name) const {
  auto it = dbs_.find(name);
  if (it == dbs_.end()) return Status::NotFound("collection not found");
  return it->second.get();
}

Status Catalog::Upsert(const std::string& collection, Row row) {
  std::lock_guard lock(mu_);
  auto db = MutableDb(collection);
  if (!db.ok()) return db.status();
  auto st = db.value()->Upsert(std::move(row));
  if (st.ok()) ++usage_.upserts;
  return st;
}

Status Catalog::Delete(const std::string& collection, const RowId& id,
                       Timestamp timestamp) {
  std::lock_guard lock(mu_);
  auto db = MutableDb(collection);
  if (!db.ok()) return db.status();
  auto st = db.value()->Delete(id, timestamp);
  if (st.ok()) ++usage_.deletes;
  return st;
}

Result<std::optional<Row>> Catalog::Get(const std::string& collection,
                                        const RowId& id) const {
  std::lock_guard lock(mu_);
  auto db = ConstDb(collection);
  if (!db.ok()) return db.status();
  ++usage_.gets;
  return db.value()->Get(id);
}

Result<std::vector<SearchHit>> Catalog::Search(
    const std::string& collection, const SearchRequest& request) const {
  std::lock_guard lock(mu_);
  auto db = ConstDb(collection);
  if (!db.ok()) return db.status();
  ++usage_.searches;
  return db.value()->Search(request);
}

Status Catalog::Flush(const std::string& collection) {
  std::lock_guard lock(mu_);
  auto db = MutableDb(collection);
  if (!db.ok()) return db.status();
  return db.value()->Flush();
}

Status Catalog::Compact(const std::string& collection) {
  std::lock_guard lock(mu_);
  auto db = MutableDb(collection);
  if (!db.ok()) return db.status();
  return db.value()->Compact();
}

UsageStats Catalog::Usage() const {
  std::lock_guard lock(mu_);
  UsageStats u = usage_;
  u.collections = infos_.size();
  size_t vectors = 0;
  for (const auto& [_, db] : dbs_) {
    vectors += db->approximate_row_count();
  }
  u.vectors_estimate = vectors;
  return u;
}

}  // namespace aster
