#include "aster/server/http_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

#include "aster/metrics/metrics.h"
#include "aster/server/json.h"

namespace aster {
namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;  // 16 MiB JSON body cap
constexpr uint32_t kMaxTopK = 1000;
constexpr size_t kMaxDocIdLen = 512;

bool ConstantTimeEqual(const std::string& a, const std::string& b) {
  // Length leak is acceptable for API keys; avoid early-exit on content.
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ac = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char bc = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff = static_cast<unsigned char>(diff | (ac ^ bc));
  }
  return diff == 0;
}

bool ValidDocId(const std::string& id) {
  if (id.empty() || id.size() > kMaxDocIdLen) return false;
  if (id == "." || id == "..") return false;
  for (char c : id) {
    if (c == '/' || c == '\\' || c == '\0') return false;
  }
  return true;
}

HttpResponse JsonStatus(int code, const std::string& msg) {
  HttpResponse r;
  r.status = code;
  r.body = std::string("{\"error\":\"") + json::Escape(msg) + "\"}";
  return r;
}

HttpResponse FromStatus(const Status& st) {
  int code = 500;
  switch (st.code()) {
    case StatusCode::kOk:
      code = 200;
      break;
    case StatusCode::kNotFound:
      code = 404;
      break;
    case StatusCode::kInvalidArgument:
      code = 400;
      break;
    case StatusCode::kIoError:
      code = 500;
      break;
    case StatusCode::kCorruption:
      code = 500;
      break;
    default:
      code = 500;
      break;
  }
  return JsonStatus(code, st.message().empty() ? "error" : st.message());
}

std::string QueryParam(const std::string& query, const char* key) {
  const std::string prefix = std::string(key) + "=";
  size_t i = 0;
  while (i < query.size()) {
    size_t amp = query.find('&', i);
    if (amp == std::string::npos) amp = query.size();
    const std::string part = query.substr(i, amp - i);
    if (part.rfind(prefix, 0) == 0) return part.substr(prefix.size());
    i = amp + 1;
  }
  return {};
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t j = i;
    while (j < path.size() && path[j] != '/') ++j;
    parts.push_back(path.substr(i, j - i));
    i = j;
  }
  return parts;
}

}  // namespace

ApiHandler::ApiHandler(Catalog* catalog, std::string expected_api_key)
    : catalog_(catalog), expected_api_key_(std::move(expected_api_key)) {}

HttpResponse ApiHandler::Handle(const HttpRequest& req) const {
  if (req.method == "GET" && req.path == "/health") {
    return {200, "application/json", "{\"ok\":true}"};
  }

  if (!expected_api_key_.empty() &&
      !ConstantTimeEqual(req.api_key, expected_api_key_)) {
    return JsonStatus(401, "unauthorized");
  }

  if (req.method == "GET" && req.path == "/metrics") {
    const auto u = catalog_->Usage();
    auto& reg = MetricsRegistry::Instance();
    reg.Gauge("collection_count").Set(static_cast<int64_t>(u.collections));
    reg.Gauge("vectors_estimate")
        .Set(static_cast<int64_t>(u.vectors_estimate));
    reg.Gauge("segment_count").Set(static_cast<int64_t>(u.segments));
    reg.Gauge("memtable_rows").Set(static_cast<int64_t>(u.memtable_rows));
    HttpResponse r;
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    r.body = reg.Render();
    return r;
  }
  if (req.method == "GET" && req.path == "/v1/usage") {
    const auto u = catalog_->Usage();
    std::ostringstream os;
    os << "{\"collections\":" << u.collections
       << ",\"vectors_estimate\":" << u.vectors_estimate
       << ",\"upserts\":" << u.upserts << ",\"deletes\":" << u.deletes
       << ",\"searches\":" << u.searches << ",\"gets\":" << u.gets << "}";
    return {200, "application/json", os.str()};
  }

  // Durable scale-down hook: flush every collection so WAL is sealed before
  // the pod is killed (preStop / drain). Idempotent.
  if (req.method == "POST" && req.path == "/v1/admin/drain") {
    const auto cols = catalog_->ListCollections();
    size_t flushed = 0;
    for (const auto& c : cols) {
      auto st = catalog_->Flush(c.name);
      if (!st.ok()) {
        return JsonStatus(500, "drain flush failed: " + st.message());
      }
      ++flushed;
    }
    std::ostringstream os;
    os << "{\"status\":\"drained\",\"collections_flushed\":" << flushed << "}";
    MetricsRegistry::Instance().Counter("drain_total").Inc();
    return {200, "application/json", os.str()};
  }

  const auto parts = SplitPath(req.path);
  // /v1/collections
  if (parts.size() == 2 && parts[0] == "v1" && parts[1] == "collections") {
    if (req.method == "GET") {
      std::ostringstream os;
      os << "{\"collections\":[";
      bool first = true;
      for (const auto& c : catalog_->ListCollections()) {
        if (!first) os << ',';
        first = false;
        os << "{\"name\":\"" << json::Escape(c.name)
           << "\",\"dimension\":" << c.dimension << ",\"metric\":\""
           << Catalog::MetricToString(c.metric) << "\"}";
      }
      os << "]}";
      return {200, "application/json", os.str()};
    }
    return JsonStatus(405, "method not allowed");
  }

  // /v1/collections/{name}
  if (parts.size() == 3 && parts[0] == "v1" && parts[1] == "collections") {
    const std::string& name = parts[2];
    if (req.method == "GET") {
      auto info = catalog_->GetCollection(name);
      if (!info) return JsonStatus(404, "collection not found");
      std::ostringstream os;
      os << "{\"name\":\"" << json::Escape(info->name)
         << "\",\"dimension\":" << info->dimension << ",\"metric\":\""
         << Catalog::MetricToString(info->metric) << "\"}";
      return {200, "application/json", os.str()};
    }
    if (req.method == "PUT") {
      CollectionInfo info;
      info.name = name;
      auto dim = json::GetInt(req.body, "dimension");
      if (!dim || *dim <= 0) return JsonStatus(400, "dimension required");
      if (*dim > static_cast<int64_t>(Catalog::kMaxDimension)) {
        return JsonStatus(400, "dimension too large");
      }
      info.dimension = static_cast<uint32_t>(*dim);
      auto metric_s = json::GetString(req.body, "metric");
      const std::string metric_name =
          metric_s.has_value() && !metric_s->empty() ? *metric_s : "cosine";
      auto metric = Catalog::MetricFromString(metric_name);
      if (!metric.ok()) return FromStatus(metric.status());
      info.metric = metric.value();
      auto st = catalog_->CreateCollection(info);
      if (!st.ok() && st.message().find("already exists") != std::string::npos) {
        return {200, "application/json",
                std::string("{\"name\":\"") + json::Escape(name) +
                    "\",\"status\":\"exists\"}"};
      }
      if (!st.ok()) return FromStatus(st);
      return {201, "application/json",
              std::string("{\"name\":\"") + json::Escape(name) +
                  "\",\"status\":\"created\"}"};
    }
    if (req.method == "DELETE") {
      auto st = catalog_->DropCollection(name);
      if (!st.ok()) return FromStatus(st);
      return {200, "application/json", "{\"status\":\"dropped\"}"};
    }
    return JsonStatus(405, "method not allowed");
  }

  // /v1/collections/{name}/flush|compact|search
  if (parts.size() == 4 && parts[0] == "v1" && parts[1] == "collections") {
    const std::string& name = parts[2];
    const std::string& action = parts[3];
    if (action == "flush" && req.method == "POST") {
      const auto t0 = std::chrono::steady_clock::now();
      auto st = catalog_->Flush(name);
      const auto ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();
      MetricsRegistry::Instance().Histogram("flush_latency_ms").Observe(ms);
      if (!st.ok()) return FromStatus(st);
      return {200, "application/json", "{\"status\":\"flushed\"}"};
    }
    if (action == "compact" && req.method == "POST") {
      auto st = catalog_->Compact(name);
      if (!st.ok()) return FromStatus(st);
      return {200, "application/json", "{\"status\":\"compacted\"}"};
    }
    if (action == "search" && req.method == "POST") {
      auto vec = json::GetFloatArray(req.body, "vector");
      if (!vec) return JsonStatus(400, "vector required");
      SearchRequest search;
      search.vector = std::move(*vec);
      if (auto k = json::GetInt(req.body, "top_k")) {
        if (*k <= 0 || *k > static_cast<int64_t>(kMaxTopK)) {
          return JsonStatus(400, "top_k out of range");
        }
        search.top_k = static_cast<uint32_t>(*k);
      }
      if (auto ef = json::GetInt(req.body, "ef_search")) {
        if (*ef < 0 || *ef > 100000) {
          return JsonStatus(400, "ef_search out of range");
        }
        search.ef_search = static_cast<uint32_t>(*ef);
      }
      if (auto tags = json::GetStringArray(req.body, "tags")) {
        search.tags.insert(tags->begin(), tags->end());
      }
      const auto t0 = std::chrono::steady_clock::now();
      auto hits = catalog_->Search(name, search);
      const auto ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();
      MetricsRegistry::Instance().Histogram("read_latency_ms").Observe(ms);
      MetricsRegistry::Instance()
          .Histogram("hnsw_search_latency")
          .Observe(ms);
      if (!hits.ok()) return FromStatus(hits.status());
      MetricsRegistry::Instance().Counter("searches_total").Inc();
      std::ostringstream os;
      os << "{\"hits\":[";
      bool first = true;
      for (const auto& h : hits.value()) {
        if (!first) os << ',';
        first = false;
        os << "{\"id\":\"" << json::Escape(h.id) << "\",\"score\":" << h.score
           << "}";
      }
      os << "]}";
      return {200, "application/json", os.str()};
    }
    return JsonStatus(404, "not found");
  }

  // /v1/collections/{name}/docs/{id}
  if (parts.size() == 5 && parts[0] == "v1" && parts[1] == "collections" &&
      parts[3] == "docs") {
    const std::string& name = parts[2];
    const std::string& id = parts[4];
    if (!ValidDocId(id)) return JsonStatus(400, "invalid document id");
    if (req.method == "GET") {
      auto row = catalog_->Get(name, id);
      if (!row.ok()) return FromStatus(row.status());
      if (!row.value()) return JsonStatus(404, "document not found");
      const Row& r = *row.value();
      std::ostringstream os;
      os << "{\"id\":\"" << json::Escape(r.id) << "\",\"vector\":[";
      for (size_t i = 0; i < r.vector.size(); ++i) {
        if (i) os << ',';
        os << r.vector[i];
      }
      os << "],\"tags\":[";
      bool first = true;
      for (const auto& t : r.tags) {
        if (!first) os << ',';
        first = false;
        os << '"' << json::Escape(t) << '"';
      }
      os << "],\"timestamp\":" << r.timestamp << "}";
      return {200, "application/json", os.str()};
    }
    if (req.method == "PUT") {
      Row row;
      row.id = id;
      auto vec = json::GetFloatArray(req.body, "vector");
      if (!vec) return JsonStatus(400, "vector required");
      row.vector = std::move(*vec);
      if (auto tags = json::GetStringArray(req.body, "tags")) {
        row.tags.insert(tags->begin(), tags->end());
      }
      if (auto meta = json::GetString(req.body, "metadata")) {
        row.metadata = *meta;
      }
      if (auto ts = json::GetInt(req.body, "timestamp")) {
        row.timestamp = static_cast<Timestamp>(*ts);
      } else {
        row.timestamp = static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
      }
      const auto t0 = std::chrono::steady_clock::now();
      auto st = catalog_->Upsert(name, std::move(row));
      const auto ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();
      MetricsRegistry::Instance().Histogram("write_latency_ms").Observe(ms);
      if (!st.ok()) return FromStatus(st);
      MetricsRegistry::Instance().Counter("upserts_total").Inc();
      return {200, "application/json", "{\"status\":\"ok\"}"};
    }
    if (req.method == "DELETE") {
      Timestamp ts = 0;
      if (auto q = QueryParam(req.query, "timestamp"); !q.empty()) {
        ts = static_cast<Timestamp>(std::stoull(q));
      } else if (auto body_ts = json::GetInt(req.body, "timestamp")) {
        ts = static_cast<Timestamp>(*body_ts);
      } else {
        ts = static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
      }
      const auto t0 = std::chrono::steady_clock::now();
      auto st = catalog_->Delete(name, id, ts);
      const auto ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();
      MetricsRegistry::Instance().Histogram("delete_latency_ms").Observe(ms);
      if (!st.ok()) return FromStatus(st);
      MetricsRegistry::Instance().Counter("deletes_total").Inc();
      return {200, "application/json", "{\"status\":\"deleted\"}"};
    }
    return JsonStatus(405, "method not allowed");
  }

  return JsonStatus(404, "not found");
}

HttpServer::HttpServer(Options options, ApiHandler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

Status HttpServer::Listen() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::IoError("socket failed");
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::InvalidArgument("bad host");
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::IoError("bind failed");
  }
  if (::listen(listen_fd_, 64) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::IoError("listen failed");
  }
  sockaddr_in bound {};
  socklen_t len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) ==
      0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = options_.port;
  }
  return Status::Ok();
}

void HttpServer::Stop() {
  stop_.store(true);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::HandleClient(int fd) {
  std::string raw;
  char buf[4096];
  while (raw.find("\r\n\r\n") == std::string::npos) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      ::close(fd);
      return;
    }
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > kMaxHeaderBytes) {
      ::close(fd);
      return;
    }
  }
  const size_t hdr_end = raw.find("\r\n\r\n");
  const std::string headers = raw.substr(0, hdr_end);
  std::string body = raw.substr(hdr_end + 4);

  HttpRequest req;
  {
    std::istringstream hs(headers);
    std::string line;
    if (!std::getline(hs, line)) {
      ::close(fd);
      return;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream rl(line);
    rl >> req.method;
    std::string target;
    rl >> target;
    if (target.size() > 2048) {
      ::close(fd);
      return;
    }
    const size_t q = target.find('?');
    if (q == std::string::npos) {
      req.path = target;
    } else {
      req.path = target.substr(0, q);
      req.query = target.substr(q + 1);
    }
    bool saw_length = false;
    size_t want = 0;
    while (std::getline(hs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon);
      std::string val = line.substr(colon + 1);
      while (!val.empty() && val[0] == ' ') val.erase(val.begin());
      for (char& c : key) c = static_cast<char>(std::tolower(c));
      if (key == "content-length") {
        // Reject non-digits / overflow before allocating.
        if (val.empty() || val.size() > 10) {
          ::close(fd);
          return;
        }
        for (char c : val) {
          if (c < '0' || c > '9') {
            ::close(fd);
            return;
          }
        }
        want = static_cast<size_t>(std::stoul(val));
        if (want > kMaxBodyBytes) {
          ::close(fd);
          return;
        }
        saw_length = true;
      } else if (key == "x-api-key") {
        if (val.size() > 512) {
          ::close(fd);
          return;
        }
        req.api_key = val;
      } else if (key == "authorization" && val.rfind("Bearer ", 0) == 0) {
        req.api_key = val.substr(7);
        if (req.api_key.size() > 512) {
          ::close(fd);
          return;
        }
      }
    }
    if (saw_length) {
      while (body.size() < want) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
        if (body.size() > kMaxBodyBytes) {
          ::close(fd);
          return;
        }
      }
      if (body.size() > want) body.resize(want);
      if (body.size() != want) {
        ::close(fd);
        return;
      }
    } else if (body.size() > kMaxBodyBytes) {
      ::close(fd);
      return;
    }
  }
  req.body = std::move(body);

  const HttpResponse resp = handler_.Handle(req);
  std::ostringstream out;
  out << "HTTP/1.1 " << resp.status;
  if (resp.status == 200) out << " OK";
  else if (resp.status == 201) out << " Created";
  else if (resp.status == 400) out << " Bad Request";
  else if (resp.status == 401) out << " Unauthorized";
  else if (resp.status == 404) out << " Not Found";
  else if (resp.status == 405) out << " Method Not Allowed";
  else out << " Error";
  out << "\r\n"
      << "Content-Type: " << resp.content_type << "\r\n"
      << "Content-Length: " << resp.body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << resp.body;
  const std::string bytes = out.str();
  ::send(fd, bytes.data(), bytes.size(), 0);
  ::close(fd);
}

void HttpServer::Serve() {
  while (!stop_.load() && listen_fd_ >= 0) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_.load()) break;
      continue;
    }
    HandleClient(fd);
  }
}

}  // namespace aster
