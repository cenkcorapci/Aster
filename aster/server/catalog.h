#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/db/db.h"

namespace aster {

struct CollectionInfo {
  std::string name;
  uint32_t dimension = 0;
  Metric metric = Metric::kCosine;
};

struct UsageStats {
  uint64_t upserts = 0;
  uint64_t deletes = 0;
  uint64_t searches = 0;
  uint64_t gets = 0;
  size_t collections = 0;
  size_t vectors_estimate = 0;
};

// Multi-collection facade over aster::Db for the single-node SaaS kernel.
// Layout under data_dir:
//   CATALOG          — one line per collection: name\tdimension\tmetric
//   <name>/          — Db durable directory (MANIFEST, WAL, seg_*.ast)
//
// Db instances are shared_ptr so search/get can run without holding mu_
// for the whole call (DropCollection cannot free a Db still in use).
class Catalog {
 public:
  static constexpr uint32_t kMaxDimension = 8192;

  struct Options {
    std::string data_dir;  // required (durable)
    SyncPolicy wal_sync = SyncPolicy::kEveryMs;
    size_t memtable_flush_bytes = 64 << 20;
    size_t max_segments_before_compact = 8;
  };

  static Result<std::unique_ptr<Catalog>> Open(Options options);

  static std::string MetricToString(Metric m);
  static Result<Metric> MetricFromString(const std::string& s);

  Status CreateCollection(const CollectionInfo& info);
  // Removes the collection from the catalog. On-disk files under
  // data_dir/<name>/ are left in place (manual cleanup / future GC).
  Status DropCollection(const std::string& name);
  std::vector<CollectionInfo> ListCollections() const;
  std::optional<CollectionInfo> GetCollection(const std::string& name) const;

  Status Upsert(const std::string& collection, Row row);
  Status Delete(const std::string& collection, const RowId& id,
                Timestamp timestamp);
  Result<std::optional<Row>> Get(const std::string& collection,
                                 const RowId& id) const;
  Result<std::vector<SearchHit>> Search(const std::string& collection,
                                        const SearchRequest& request) const;
  Status Flush(const std::string& collection);
  Status Compact(const std::string& collection);

  UsageStats Usage() const;

 private:
  explicit Catalog(Options options);

  Status Load();
  Status PersistCatalog() const;
  Result<std::shared_ptr<Db>> LookupDb(const std::string& name) const;
  static Status ValidateName(const std::string& name);
  std::string CollectionDir(const std::string& name) const;

  Options options_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, CollectionInfo> infos_;
  std::unordered_map<std::string, std::shared_ptr<Db>> dbs_;
  mutable UsageStats usage_;
};

}  // namespace aster
