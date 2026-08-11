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

class S3Storage;

struct CollectionInfo {
  std::string name;
  uint32_t dimension = 0;
  Metric metric = Metric::kCosine;
  StorageMode storage_mode = StorageMode::kHot;
};

struct UsageStats {
  uint64_t upserts = 0;
  uint64_t deletes = 0;
  uint64_t searches = 0;
  uint64_t gets = 0;
  size_t collections = 0;
  size_t vectors_estimate = 0;
  size_t segments = 0;
  size_t memtable_rows = 0;
};

// Multi-collection facade over aster::Db for the single-node SaaS kernel.
// Layout under data_dir:
//   CATALOG          — one line per collection:
//                      name\tdimension\tmetric[\tstorage_mode]
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
    size_t compaction_tier_threshold = 4;
    size_t max_segments_before_compact = 8;
    // Shared object store for WARM/COLD collections (optional; required when
    // any collection uses a non-HOT storage mode).
    std::shared_ptr<S3Storage> object_store;
  };

  static Result<std::unique_ptr<Catalog>> Open(Options options);

  static std::string MetricToString(Metric m);
  static Result<Metric> MetricFromString(const std::string& s);

  // Lifecycle phase 1: durable placeholder (not searchable/writeable yet).
  Status CreateCollectionCreated(const std::string& name);
  // Lifecycle phase 2: provides vector config and opens the underlying Db.
  Status ConfigureCollection(const CollectionInfo& info);

  // Backwards compatible: single-call create+configure (used by HTTP API).
  Status CreateCollection(const CollectionInfo& info);
  // Removes the collection from the catalog and deletes data_dir/<name>/.
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
