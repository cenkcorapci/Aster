#include "aster/platform/s3_storage.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <sstream>

namespace aster {
namespace {

Status ParseEndpoint(const std::string& endpoint, std::string* host,
                     uint16_t* port) {
  if (endpoint.empty()) return Status::InvalidArgument("empty S3 endpoint");
  std::string rest = endpoint;
  if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
    *port = 80;
  } else if (rest.rfind("https://", 0) == 0) {
    // TLS not implemented in this skeleton; still parse for LocalStack HTTPS.
    rest = rest.substr(8);
    *port = 443;
  } else {
    return Status::InvalidArgument("S3 endpoint must be http(s)://...");
  }
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  const auto slash = rest.find('/');
  if (slash != std::string::npos) rest = rest.substr(0, slash);
  const auto colon = rest.find(':');
  if (colon == std::string::npos) {
    *host = rest;
  } else {
    *host = rest.substr(0, colon);
    const std::string ps = rest.substr(colon + 1);
    if (ps.empty()) return Status::InvalidArgument("bad S3 endpoint port");
    int p = 0;
    for (char c : ps) {
      if (c < '0' || c > '9') {
        return Status::InvalidArgument("bad S3 endpoint port");
      }
      p = p * 10 + (c - '0');
      if (p > 65535) return Status::InvalidArgument("bad S3 endpoint port");
    }
    *port = static_cast<uint16_t>(p);
  }
  if (host->empty()) return Status::InvalidArgument("empty S3 host");
  return Status::Ok();
}

std::string UrlEncode(const std::string& s, bool encode_slash) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
        (!encode_slash && c == '/')) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

// Extract text between <Key>...</Key> tags (ListObjectsV2).
std::vector<std::string> ParseListKeys(const std::string& xml) {
  std::vector<std::string> keys;
  const std::string open = "<Key>";
  const std::string close = "</Key>";
  size_t i = 0;
  while (i < xml.size()) {
    const size_t a = xml.find(open, i);
    if (a == std::string::npos) break;
    const size_t start = a + open.size();
    const size_t b = xml.find(close, start);
    if (b == std::string::npos) break;
    keys.push_back(xml.substr(start, b - start));
    i = b + close.size();
  }
  return keys;
}

}  // namespace

S3Storage::S3Storage(S3Config config) : config_(std::move(config)) {
  auto st = ParseEndpoint(config_.endpoint, &host_, &port_);
  if (!st.ok()) {
    // Defer failure to the first Request; keep host empty as a sentinel.
    host_.clear();
  }
}

std::string S3Storage::ObjectPath(const std::string& key) const {
  if (config_.path_style) {
    return "/" + config_.bucket + "/" + UrlEncode(key, /*encode_slash=*/false);
  }
  // Virtual-hosted: Host is bucket.endpoint; path is /{key}.
  return "/" + UrlEncode(key, /*encode_slash=*/false);
}

Result<S3Storage::HttpResult> S3Storage::Request(const std::string& method,
                                                 const std::string& url_path,
                                                 const std::string& query,
                                                 const std::string& body) const {
  if (host_.empty() || config_.bucket.empty()) {
    return Status::InvalidArgument("S3 endpoint/bucket not configured");
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return Status::IoError("s3 socket failed");

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return Status::InvalidArgument("s3 host not an IPv4 address: " + host_);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return Status::IoError("s3 connect failed: " + host_ + ":" +
                           std::to_string(port_));
  }

  std::string target = url_path;
  if (!query.empty()) target += "?" + query;

  std::string host_header = host_;
  if (!config_.path_style) {
    host_header = config_.bucket + "." + host_;
  }
  if (port_ != 80 && port_ != 443) {
    host_header += ":" + std::to_string(port_);
  }

  std::ostringstream req;
  req << method << " " << target << " HTTP/1.1\r\n"
      << "Host: " << host_header << "\r\n"
      << "Connection: close\r\n"
      << "Accept: */*\r\n";
  // Skeleton credentials — not SigV4. LocalStack/FakeS3 accept these.
  if (!config_.access_key.empty()) {
    req << "Authorization: AWS " << config_.access_key << ":skeleton\r\n"
        << "x-amz-content-sha256: UNSIGNED-PAYLOAD\r\n";
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    req << "Content-Length: " << body.size() << "\r\n";
  }
  if (method == "PUT") {
    req << "Content-Type: application/octet-stream\r\n";
  }
  req << "\r\n" << body;
  const std::string bytes = req.str();
  size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n =
        ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      return Status::IoError("s3 send failed");
    }
    sent += static_cast<size_t>(n);
  }

  std::string raw;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      ::close(fd);
      return Status::IoError("s3 recv failed");
    }
    if (n == 0) break;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > 64 * 1024 * 1024) {
      ::close(fd);
      return Status::IoError("s3 response too large");
    }
  }
  ::close(fd);

  const size_t hdr_end = raw.find("\r\n\r\n");
  if (hdr_end == std::string::npos) {
    return Status::IoError("s3 malformed response");
  }
  const std::string headers = raw.substr(0, hdr_end);
  HttpResult out;
  out.body = raw.substr(hdr_end + 4);

  {
    std::istringstream hs(headers);
    std::string line;
    if (!std::getline(hs, line)) {
      return Status::IoError("s3 empty status line");
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // HTTP/1.1 200 OK
    std::istringstream sl(line);
    std::string httpver;
    sl >> httpver >> out.status;
    if (out.status <= 0) return Status::IoError("s3 bad status line");

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
        want = static_cast<size_t>(std::stoul(val));
        saw_length = true;
      }
    }
    if (saw_length && out.body.size() > want) out.body.resize(want);
  }
  return out;
}

Status S3Storage::Put(const std::string& path, const std::string& data) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  // TODO(M8-T01): multipart upload when data exceeds a size threshold.
  auto resp = Request("PUT", ObjectPath(path), "", data);
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 200 || resp.value().status == 201) {
    return Status::Ok();
  }
  return Status::IoError("s3 PutObject HTTP " +
                         std::to_string(resp.value().status));
}

Result<std::string> S3Storage::Read(const std::string& path) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  // TODO(M8-T01): Range GET for partial reads / block cache.
  auto resp = Request("GET", ObjectPath(path), "", "");
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 404) return Status::NotFound(path);
  if (resp.value().status != 200) {
    return Status::IoError("s3 GetObject HTTP " +
                           std::to_string(resp.value().status));
  }
  return resp.value().body;
}

Status S3Storage::Remove(const std::string& path) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  // S3 DeleteObject is idempotent (always 204). Match Posix/Memory by
  // returning NotFound when the key is absent.
  if (!Exists(path)) return Status::NotFound(path);
  auto resp = Request("DELETE", ObjectPath(path), "", "");
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 200 || resp.value().status == 204) {
    return Status::Ok();
  }
  return Status::IoError("s3 DeleteObject HTTP " +
                         std::to_string(resp.value().status));
}

Result<std::vector<std::string>> S3Storage::List(const std::string& prefix) {
  std::string query = "list-type=2";
  if (!prefix.empty()) {
    query += "&prefix=" + UrlEncode(prefix, /*encode_slash=*/true);
  }
  const std::string list_path =
      config_.path_style ? ("/" + config_.bucket) : "/";
  auto resp = Request("GET", list_path, query, "");
  if (!resp.ok()) return resp.status();
  if (resp.value().status != 200) {
    return Status::IoError("s3 ListObjectsV2 HTTP " +
                           std::to_string(resp.value().status));
  }
  return ParseListKeys(resp.value().body);
}

bool S3Storage::Exists(const std::string& path) {
  if (path.empty()) return false;
  auto resp = Request("HEAD", ObjectPath(path), "", "");
  if (!resp.ok()) return false;
  return resp.value().status == 200;
}

}  // namespace aster
