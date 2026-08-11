#pragma once

#include <cstdint>
#include <string>
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
  // Optional credentials (skeleton sends them as dummy headers; SigV4 is
  // deferred to M8-T01).
  std::string access_key = "test";
  std::string secret_key = "test";
  // Path-style addressing: PUT {endpoint}/{bucket}/{key}. LocalStack and
  // FakeS3Server expect this; real AWS often prefers virtual-hosted.
  bool path_style = true;
};

// S3-compatible StorageBackend skeleton (Put / Read / List / Remove / Exists).
//
// Speaks the subset of the S3 REST API needed for single-object CRUD and
// ListObjectsV2. Not production-complete:
//   TODO(M8-T01): multipart upload for large objects
//   TODO(M8-T01): HTTP Range GET / block cache
//   TODO(M8-T01): AWS SigV4 signing
class S3Storage final : public StorageBackend {
 public:
  explicit S3Storage(S3Config config);

  Status Put(const std::string& path, const std::string& data) override;
  Result<std::string> Read(const std::string& path) override;
  Status Remove(const std::string& path) override;
  Result<std::vector<std::string>> List(const std::string& prefix) override;
  bool Exists(const std::string& path) override;

 private:
  struct HttpResult {
    int status = 0;
    std::string body;
  };

  Result<HttpResult> Request(const std::string& method,
                             const std::string& url_path,
                             const std::string& query,
                             const std::string& body) const;
  std::string ObjectPath(const std::string& key) const;

  S3Config config_;
  std::string host_;
  uint16_t port_ = 80;
};

}  // namespace aster
