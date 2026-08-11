#include "aster/platform/s3_storage.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
    // TLS not implemented; still parse for LocalStack HTTPS.
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

std::string XmlTagValue(const std::string& xml, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const size_t a = xml.find(open);
  if (a == std::string::npos) return {};
  const size_t start = a + open.size();
  const size_t b = xml.find(close, start);
  if (b == std::string::npos) return {};
  return xml.substr(start, b - start);
}

std::string HeaderValue(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& key) {
  auto it = headers.find(key);
  if (it == headers.end()) return {};
  return it->second;
}

}  // namespace

S3Storage::S3Storage(S3Config config) : config_(std::move(config)) {
  if (config_.multipart_part_size == 0) {
    config_.multipart_part_size = 5 * 1024 * 1024;
  }
  if (config_.block_cache_block_size == 0) {
    config_.block_cache_block_size = 64 * 1024;
  }
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

Result<S3Storage::HttpResult> S3Storage::Request(
    const std::string& method, const std::string& url_path,
    const std::string& query, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& extra_headers)
    const {
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
  for (const auto& h : extra_headers) {
    req << h.first << ": " << h.second << "\r\n";
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    req << "Content-Length: " << body.size() << "\r\n";
  }
  if (method == "PUT") {
    req << "Content-Type: application/octet-stream\r\n";
  } else if (method == "POST" && !body.empty()) {
    req << "Content-Type: application/xml\r\n";
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
      out.headers[key] = val;
      if (key == "content-length") {
        want = static_cast<size_t>(std::stoul(val));
        saw_length = true;
      }
    }
    if (saw_length && out.body.size() > want) out.body.resize(want);
  }
  return out;
}

uint64_t S3Storage::cache_hits() const {
  std::lock_guard<std::mutex> lock(cache_mu_);
  return cache_hits_;
}

uint64_t S3Storage::cache_misses() const {
  std::lock_guard<std::mutex> lock(cache_mu_);
  return cache_misses_;
}

void S3Storage::ClearCache() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  cache_.clear();
  lru_.clear();
  cache_hits_ = 0;
  cache_misses_ = 0;
}

void S3Storage::InvalidateCache(const std::string& path) {
  std::lock_guard<std::mutex> lock(cache_mu_);
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->first.object_key == path) {
      lru_.erase(it->second.second);
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

void S3Storage::CachePut(const CacheKey& key, std::string data) {
  std::lock_guard<std::mutex> lock(cache_mu_);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    lru_.erase(it->second.second);
    cache_.erase(it);
  }
  lru_.push_front(key);
  cache_.emplace(key, std::make_pair(std::move(data), lru_.begin()));
  while (cache_.size() > config_.block_cache_max_blocks && !lru_.empty()) {
    const CacheKey victim = lru_.back();
    lru_.pop_back();
    cache_.erase(victim);
  }
}

bool S3Storage::CacheGet(const CacheKey& key, std::string* out) {
  std::lock_guard<std::mutex> lock(cache_mu_);
  auto it = cache_.find(key);
  if (it == cache_.end()) {
    ++cache_misses_;
    return false;
  }
  ++cache_hits_;
  lru_.erase(it->second.second);
  lru_.push_front(key);
  it->second.second = lru_.begin();
  *out = it->second.first;
  return true;
}

Status S3Storage::PutSingle(const std::string& path, const std::string& data) {
  auto resp = Request("PUT", ObjectPath(path), "", data);
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 200 || resp.value().status == 201) {
    return Status::Ok();
  }
  return Status::IoError("s3 PutObject HTTP " +
                         std::to_string(resp.value().status));
}

Status S3Storage::PutMultipart(const std::string& path,
                               const std::string& data) {
  // 1) Initiate
  auto init = Request("POST", ObjectPath(path), "uploads", "");
  if (!init.ok()) return init.status();
  if (init.value().status != 200) {
    return Status::IoError("s3 CreateMultipartUpload HTTP " +
                           std::to_string(init.value().status));
  }
  const std::string upload_id = XmlTagValue(init.value().body, "UploadId");
  if (upload_id.empty()) {
    return Status::IoError("s3 CreateMultipartUpload missing UploadId");
  }

  const std::string encoded_id = UrlEncode(upload_id, /*encode_slash=*/true);
  std::vector<std::pair<int, std::string>> etags;  // partNumber, ETag

  auto abort = [&]() {
    (void)Request("DELETE", ObjectPath(path), "uploadId=" + encoded_id, "");
  };

  // 2) Upload parts
  const size_t part_size = config_.multipart_part_size;
  int part_number = 1;
  for (size_t offset = 0; offset < data.size(); offset += part_size) {
    const size_t n = std::min(part_size, data.size() - offset);
    const std::string part = data.substr(offset, n);
    const std::string query = "partNumber=" + std::to_string(part_number) +
                              "&uploadId=" + encoded_id;
    auto part_resp = Request("PUT", ObjectPath(path), query, part);
    if (!part_resp.ok()) {
      abort();
      return part_resp.status();
    }
    if (part_resp.value().status != 200) {
      abort();
      return Status::IoError("s3 UploadPart HTTP " +
                             std::to_string(part_resp.value().status));
    }
    std::string etag = HeaderValue(part_resp.value().headers, "etag");
    if (etag.empty()) etag = "\"part" + std::to_string(part_number) + "\"";
    etags.emplace_back(part_number, etag);
    ++part_number;
  }

  // 3) Complete
  std::ostringstream xml;
  xml << "<CompleteMultipartUpload>";
  for (const auto& pe : etags) {
    xml << "<Part><PartNumber>" << pe.first << "</PartNumber><ETag>"
        << pe.second << "</ETag></Part>";
  }
  xml << "</CompleteMultipartUpload>";
  auto complete =
      Request("POST", ObjectPath(path), "uploadId=" + encoded_id, xml.str());
  if (!complete.ok()) {
    abort();
    return complete.status();
  }
  if (complete.value().status != 200) {
    abort();
    return Status::IoError("s3 CompleteMultipartUpload HTTP " +
                           std::to_string(complete.value().status));
  }
  return Status::Ok();
}

Status S3Storage::Put(const std::string& path, const std::string& data) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  Status st;
  if (data.size() >= config_.multipart_threshold) {
    st = PutMultipart(path, data);
  } else {
    st = PutSingle(path, data);
  }
  if (st.ok()) InvalidateCache(path);
  return st;
}

Result<size_t> S3Storage::HeadSize(const std::string& path) {
  auto resp = Request("HEAD", ObjectPath(path), "", "");
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 404) return Status::NotFound(path);
  if (resp.value().status != 200) {
    return Status::IoError("s3 HeadObject HTTP " +
                           std::to_string(resp.value().status));
  }
  const std::string cl = HeaderValue(resp.value().headers, "content-length");
  if (cl.empty()) return Status::IoError("s3 HeadObject missing Content-Length");
  size_t n = 0;
  for (char c : cl) {
    if (c < '0' || c > '9') {
      return Status::IoError("s3 HeadObject bad Content-Length");
    }
    n = n * 10 + static_cast<size_t>(c - '0');
  }
  return n;
}

Result<std::string> S3Storage::RangeGet(const std::string& path, size_t start,
                                        size_t end_inclusive) {
  std::ostringstream range;
  range << "bytes=" << start << "-" << end_inclusive;
  auto resp = Request("GET", ObjectPath(path), "", "",
                      {{"Range", range.str()}});
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 404) return Status::NotFound(path);
  if (resp.value().status != 206 && resp.value().status != 200) {
    return Status::IoError("s3 GetObject Range HTTP " +
                           std::to_string(resp.value().status));
  }
  return resp.value().body;
}

Result<std::string> S3Storage::ReadBlock(const std::string& path,
                                         size_t block_index,
                                         size_t object_size) {
  CacheKey key{path, block_index};
  std::string cached;
  if (CacheGet(key, &cached)) return cached;

  const size_t bs = config_.block_cache_block_size;
  const size_t start = block_index * bs;
  if (start >= object_size) {
    return Status::InvalidArgument("block past end of object");
  }
  size_t end_inclusive = start + bs - 1;
  if (end_inclusive >= object_size) end_inclusive = object_size - 1;
  auto got = RangeGet(path, start, end_inclusive);
  if (!got.ok()) return got.status();
  CachePut(key, got.value());
  return got.value();
}

Result<std::string> S3Storage::Read(const std::string& path) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  auto size_r = HeadSize(path);
  if (!size_r.ok()) return size_r.status();
  const size_t object_size = size_r.value();
  if (object_size == 0) return std::string();

  const size_t bs = config_.block_cache_block_size;
  const size_t n_blocks = (object_size + bs - 1) / bs;
  std::string out;
  out.reserve(object_size);
  for (size_t i = 0; i < n_blocks; ++i) {
    auto block = ReadBlock(path, i, object_size);
    if (!block.ok()) return block.status();
    out += block.value();
  }
  if (out.size() != object_size) {
    return Status::IoError("s3 Read assembled size mismatch");
  }
  return out;
}

Status S3Storage::Remove(const std::string& path) {
  if (path.empty()) return Status::InvalidArgument("empty object key");
  // S3 DeleteObject is idempotent (always 204). Match Posix/Memory by
  // returning NotFound when the key is absent.
  if (!Exists(path)) return Status::NotFound(path);
  auto resp = Request("DELETE", ObjectPath(path), "", "");
  if (!resp.ok()) return resp.status();
  if (resp.value().status == 200 || resp.value().status == 204) {
    InvalidateCache(path);
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
