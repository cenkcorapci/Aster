#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "aster/core/status.h"
#include "aster/server/catalog.h"

namespace aster {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  std::string api_key;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

// Routes Firebase-style REST over Catalog.
class ApiHandler {
 public:
  explicit ApiHandler(Catalog* catalog, std::string expected_api_key = {});

  HttpResponse Handle(const HttpRequest& req) const;

 private:
  Catalog* catalog_;
  std::string expected_api_key_;
};

// Blocking single-threaded accept loop (good enough for local SaaS MVP).
class HttpServer {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
  };

  HttpServer(Options options, ApiHandler handler);

  // Binds and listens. Returns error if bind fails.
  Status Listen();

  // Serves until Stop() is called from another thread, or forever.
  void Serve();
  void Stop();

  uint16_t port() const { return bound_port_; }

 private:
  void HandleClient(int fd);

  Options options_;
  ApiHandler handler_;
  int listen_fd_ = -1;
  uint16_t bound_port_ = 0;
  bool stop_ = false;
};

}  // namespace aster
