#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "aster/core/status.h"

namespace aster {

// Minimal in-process S3-compatible HTTP server for unit/integration tests.
// Supports path-style PutObject / GetObject / HeadObject / DeleteObject and
// ListObjectsV2. No auth, no multipart, no range GET.
class FakeS3Server {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 0;  // ephemeral
    std::string bucket = "aster-test";
  };

  FakeS3Server() : FakeS3Server(Options{}) {}
  explicit FakeS3Server(Options options);
  ~FakeS3Server();

  FakeS3Server(const FakeS3Server&) = delete;
  FakeS3Server& operator=(const FakeS3Server&) = delete;

  Status Start();
  void Stop();

  uint16_t port() const { return bound_port_; }
  const std::string& bucket() const { return options_.bucket; }
  std::string endpoint() const;

 private:
  void ServeLoop();
  void HandleClient(int fd);

  Options options_;
  int listen_fd_ = -1;
  uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::map<std::string, std::string> objects_;
};

}  // namespace aster
