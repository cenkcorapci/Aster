#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aster/platform/storage_backend.h"

namespace aster {

// Configuration for an S3-compatible object store (AWS, LocalStack, minio,
// or the in-process FakeS3Server used in tests).
struct S3Config {
  // Base URL without trailing slash, e.g. "http://127.0.0.1:4566".
  std::string endpoint;
  std::string bucket;
  std::string region = "us-east-1";
  // Optional credentials (skeleton Authorization header; not SigV4).
  std::string access_key = "test";
  std::string secret_key = "test";
  // Path-style addressing: PUT {endpoint}/{bucket}/{key}. LocalStack and
  // FakeS3Server expect this; real AWS often prefers virtual-hosted.
  bool path_style = true;

  // Objects at or above this size use multipart upload (tests may lower it).
  size_t multipart_threshold = 8 * 1024 * 1024;
  // Part size for multipart Put (except possibly the last part).
  size_t multipart_part_size = 5 * 1024 * 1024;

  // Range-GET block cache: each miss fetches [block_size] bytes via HTTP Range.
  size_t block_cache_block_size = 64 * 1024;
  // Maximum number of cached blocks (LRU eviction).
  size_t block_cache_max_blocks = 64;
};

// S3-compatible StorageBackend with multipart Put, Range GET, and a simple
// LRU block cache. Speaks the S3 REST subset needed for CRUD, ListObjectsV2,
// and multipart upload. SigV4 signing remains deferred.
class S3Storage final : public StorageBackend {
 public:
  explicit S3Storage(S3Config config);

  Status Put(const std::string& path, const std::string& data) override;
  Result<std::string> Read(const std::string& path) override;
  Status Remove(const std::string& path) override;
  Result<std::vector<std::string>> List(const std::string& prefix) override;
  bool Exists(const std::string& path) override;

  // Test / observability hooks for the block cache.
  uint64_t cache_hits() const;
  uint64_t cache_misses() const;
  void ClearCache();

  // Non-evictable pin cache for HNSW upper layers (and similar hot metadata).
  // Pinned bytes survive ClearCache() / LRU eviction and are consulted before
  // the block cache on Read/ReadRange. See docs/indexing.md §10.3.1.
  Status PinRange(const std::string& path, size_t start, size_t end_exclusive,
                  std::string data);
  // Fetch [start, end) via Range GET (or pin hit) and pin it.
  Status PinRangeFromStore(const std::string& path, size_t start,
                           size_t end_exclusive);
  void ClearPins();
  void Unpin(const std::string& path);
  bool HasPinned(const std::string& path, size_t start,
                 size_t end_exclusive) const;
  size_t pinned_bytes() const;
  uint64_t pin_hits() const;
  uint64_t range_gets() const;

  // Byte-range read: returns file[start, end). Uses pin cache, then block
  // cache / Range GET. end_exclusive may be past EOF (truncated).
  Result<std::string> ReadRange(const std::string& path, size_t start,
                                size_t end_exclusive);

 private:
  struct HttpResult {
    int status = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;  // lower-case keys
  };

  struct CacheKey {
    std::string object_key;
    size_t block_index = 0;

    bool operator==(const CacheKey& o) const {
      return block_index == o.block_index && object_key == o.object_key;
    }
  };

  struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const {
      return std::hash<std::string>{}(k.object_key) ^
             (std::hash<size_t>{}(k.block_index) << 1);
    }
  };

  Result<HttpResult> Request(
      const std::string& method, const std::string& url_path,
      const std::string& query, const std::string& body,
      const std::vector<std::pair<std::string, std::string>>& extra_headers =
          {}) const;
  std::string ObjectPath(const std::string& key) const;

  Status PutSingle(const std::string& path, const std::string& data);
  Status PutMultipart(const std::string& path, const std::string& data);
  Result<size_t> HeadSize(const std::string& path);
  Result<std::string> RangeGet(const std::string& path, size_t start,
                               size_t end_inclusive);
  Result<std::string> ReadBlock(const std::string& path, size_t block_index,
                                size_t object_size);
  void InvalidateCache(const std::string& path);
  void CachePut(const CacheKey& key, std::string data);
  bool CacheGet(const CacheKey& key, std::string* out);

  struct PinKey {
    std::string object_key;
    size_t start = 0;
    size_t end = 0;  // exclusive

    bool operator==(const PinKey& o) const {
      return start == o.start && end == o.end && object_key == o.object_key;
    }
  };

  struct PinKeyHash {
    size_t operator()(const PinKey& k) const {
      return std::hash<std::string>{}(k.object_key) ^
             (std::hash<size_t>{}(k.start) << 1) ^
             (std::hash<size_t>{}(k.end) << 2);
    }
  };

  bool PinGet(const std::string& path, size_t start, size_t end_exclusive,
              std::string* out) const;

  S3Config config_;
  std::string host_;
  uint16_t port_ = 80;

  mutable std::mutex cache_mu_;
  using LruList = std::list<CacheKey>;
  LruList lru_;
  std::unordered_map<CacheKey,
                     std::pair<std::string, LruList::iterator>, CacheKeyHash>
      cache_;
  uint64_t cache_hits_ = 0;
  uint64_t cache_misses_ = 0;

  std::unordered_map<PinKey, std::string, PinKeyHash> pins_;
  uint64_t pin_hits_ = 0;
  uint64_t range_gets_ = 0;
};

}  // namespace aster
