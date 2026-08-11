#include "aster/platform/s3_fake.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace aster {
namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;

std::string XmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string QueryParam(const std::string& query, const char* key) {
  const std::string prefix = std::string(key) + "=";
  size_t i = 0;
  while (i < query.size()) {
    size_t amp = query.find('&', i);
    if (amp == std::string::npos) amp = query.size();
    const std::string part = query.substr(i, amp - i);
    if (part == key || part.rfind(prefix, 0) == 0) {
      if (part == key) return "";
      std::string val = part.substr(prefix.size());
      // Minimal percent-decode for tests.
      std::string decoded;
      for (size_t j = 0; j < val.size(); ++j) {
        if (val[j] == '%' && j + 2 < val.size()) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          const int hi = hex(val[j + 1]);
          const int lo = hex(val[j + 2]);
          if (hi >= 0 && lo >= 0) {
            decoded.push_back(static_cast<char>((hi << 4) | lo));
            j += 2;
            continue;
          }
        }
        if (val[j] == '+') {
          decoded.push_back(' ');
        } else {
          decoded.push_back(val[j]);
        }
      }
      return decoded;
    }
    i = amp + 1;
  }
  return {};
}

bool QueryHasKey(const std::string& query, const char* key) {
  size_t i = 0;
  const std::string needle = key;
  while (i < query.size()) {
    size_t amp = query.find('&', i);
    if (amp == std::string::npos) amp = query.size();
    const std::string part = query.substr(i, amp - i);
    if (part == needle || part.rfind(needle + "=", 0) == 0) return true;
    i = amp + 1;
  }
  return false;
}

void SendResponse(int fd, int status, const std::string& content_type,
                  const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>&
                      extra_headers = {}) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status;
  if (status == 200) out << " OK";
  else if (status == 204) out << " No Content";
  else if (status == 206) out << " Partial Content";
  else if (status == 404) out << " Not Found";
  else out << " Error";
  out << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n";
  for (const auto& h : extra_headers) {
    out << h.first << ": " << h.second << "\r\n";
  }
  out << "Connection: close\r\n\r\n" << body;
  const std::string bytes = out.str();
  ::send(fd, bytes.data(), bytes.size(), 0);
}

// Parse "bytes=start-end" (end optional). Returns false if absent/invalid.
bool ParseByteRange(const std::string& range_hdr, size_t object_size,
                    size_t* start, size_t* end_inclusive) {
  const std::string prefix = "bytes=";
  if (range_hdr.rfind(prefix, 0) != 0) return false;
  const std::string spec = range_hdr.substr(prefix.size());
  const auto dash = spec.find('-');
  if (dash == std::string::npos) return false;
  const std::string a = spec.substr(0, dash);
  const std::string b = spec.substr(dash + 1);
  if (a.empty()) return false;  // suffix ranges not needed for tests
  size_t s = 0;
  for (char c : a) {
    if (c < '0' || c > '9') return false;
    s = s * 10 + static_cast<size_t>(c - '0');
  }
  if (s >= object_size) return false;
  size_t e = object_size - 1;
  if (!b.empty()) {
    e = 0;
    for (char c : b) {
      if (c < '0' || c > '9') return false;
      e = e * 10 + static_cast<size_t>(c - '0');
    }
    if (e >= object_size) e = object_size - 1;
    if (e < s) return false;
  }
  *start = s;
  *end_inclusive = e;
  return true;
}

std::string MakeEtag(const std::string& data) {
  // Deterministic fake ETag (not MD5) — good enough for CompleteMultipart.
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ull;
  }
  std::ostringstream os;
  os << '"' << std::hex << h << '"';
  return os.str();
}

}  // namespace

FakeS3Server::FakeS3Server(Options options) : options_(std::move(options)) {}

FakeS3Server::~FakeS3Server() { Stop(); }

std::string FakeS3Server::endpoint() const {
  return "http://" + options_.host + ":" + std::to_string(bound_port_);
}

size_t FakeS3Server::ObjectCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return objects_.size();
}

bool FakeS3Server::HasObject(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  return objects_.count(key) > 0;
}

Status FakeS3Server::Start() {
  if (listen_fd_ >= 0) return Status::Ok();
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::IoError("fake s3 socket failed");
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::InvalidArgument("fake s3 bad host");
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::IoError("fake s3 bind failed");
  }
  if (::listen(listen_fd_, 64) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::IoError("fake s3 listen failed");
  }
  sockaddr_in bound {};
  socklen_t len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) ==
      0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = options_.port;
  }
  stop_.store(false);
  thread_ = std::thread([this] { ServeLoop(); });
  return Status::Ok();
}

void FakeS3Server::Stop() {
  stop_.store(true);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
}

void FakeS3Server::ServeLoop() {
  while (!stop_.load() && listen_fd_ >= 0) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_.load()) break;
      continue;
    }
    HandleClient(fd);
  }
}

void FakeS3Server::HandleClient(int fd) {
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

  std::string method;
  std::string path;
  std::string query;
  std::string range_hdr;
  {
    std::istringstream hs(headers);
    std::string line;
    if (!std::getline(hs, line)) {
      ::close(fd);
      return;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream rl(line);
    rl >> method;
    std::string target;
    rl >> target;
    const size_t q = target.find('?');
    if (q == std::string::npos) {
      path = target;
    } else {
      path = target.substr(0, q);
      query = target.substr(q + 1);
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
        want = static_cast<size_t>(std::stoul(val));
        if (want > kMaxBodyBytes) {
          ::close(fd);
          return;
        }
        saw_length = true;
      } else if (key == "range") {
        range_hdr = val;
      }
    }
    if (saw_length) {
      while (body.size() < want) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
      }
      if (body.size() > want) body.resize(want);
    }
  }

  // Path-style: /{bucket} or /{bucket}/{key...}
  std::string bucket;
  std::string key;
  if (!path.empty() && path[0] == '/') {
    const size_t slash = path.find('/', 1);
    if (slash == std::string::npos) {
      bucket = path.substr(1);
    } else {
      bucket = path.substr(1, slash - 1);
      key = path.substr(slash + 1);
    }
  }

  if (bucket != options_.bucket) {
    SendResponse(fd, 404, "application/xml",
                 "<Error><Code>NoSuchBucket</Code></Error>");
    ::close(fd);
    return;
  }

  // ListObjectsV2: GET /{bucket}?list-type=2&prefix=...
  if (method == "GET" && key.empty() &&
      query.find("list-type=2") != std::string::npos) {
    const std::string prefix = QueryParam(query, "prefix");
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        << "<Name>" << XmlEscape(options_.bucket) << "</Name>"
        << "<Prefix>" << XmlEscape(prefix) << "</Prefix>"
        << "<IsTruncated>false</IsTruncated>";
    size_t count = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (const auto& kv : objects_) {
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        xml << "<Contents><Key>" << XmlEscape(kv.first) << "</Key><Size>"
            << kv.second.size() << "</Size></Contents>";
        ++count;
      }
    }
    xml << "<KeyCount>" << count << "</KeyCount></ListBucketResult>";
    SendResponse(fd, 200, "application/xml", xml.str());
    ::close(fd);
    return;
  }

  if (key.empty()) {
    SendResponse(fd, 400, "application/xml",
                 "<Error><Code>InvalidRequest</Code></Error>");
    ::close(fd);
    return;
  }

  // InitiateMultipartUpload: POST /{bucket}/{key}?uploads
  if (method == "POST" && QueryHasKey(query, "uploads")) {
    std::string upload_id;
    {
      std::lock_guard<std::mutex> lock(mu_);
      upload_id = "upload-" + std::to_string(next_upload_id_++);
      MultipartUpload up;
      up.key = key;
      uploads_[upload_id] = std::move(up);
    }
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<InitiateMultipartUploadResult>"
        << "<Bucket>" << XmlEscape(options_.bucket) << "</Bucket>"
        << "<Key>" << XmlEscape(key) << "</Key>"
        << "<UploadId>" << XmlEscape(upload_id) << "</UploadId>"
        << "</InitiateMultipartUploadResult>";
    SendResponse(fd, 200, "application/xml", xml.str());
    ::close(fd);
    return;
  }

  // UploadPart: PUT /{bucket}/{key}?partNumber=N&uploadId=ID
  if (method == "PUT" && QueryHasKey(query, "uploadId") &&
      QueryHasKey(query, "partNumber")) {
    const std::string upload_id = QueryParam(query, "uploadId");
    const std::string pn_s = QueryParam(query, "partNumber");
    int part_number = 0;
    for (char c : pn_s) {
      if (c < '0' || c > '9') {
        part_number = 0;
        break;
      }
      part_number = part_number * 10 + (c - '0');
    }
    if (part_number < 1) {
      SendResponse(fd, 400, "application/xml",
                   "<Error><Code>InvalidPart</Code></Error>");
      ::close(fd);
      return;
    }
    std::string etag;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = uploads_.find(upload_id);
      if (it == uploads_.end() || it->second.key != key) {
        SendResponse(fd, 404, "application/xml",
                     "<Error><Code>NoSuchUpload</Code></Error>");
        ::close(fd);
        return;
      }
      etag = MakeEtag(body);
      it->second.parts[part_number] = body;
    }
    SendResponse(fd, 200, "application/xml", "", {{"ETag", etag}});
    ::close(fd);
    return;
  }

  // CompleteMultipartUpload: POST /{bucket}/{key}?uploadId=ID
  if (method == "POST" && QueryHasKey(query, "uploadId") &&
      !QueryHasKey(query, "uploads")) {
    const std::string upload_id = QueryParam(query, "uploadId");
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = uploads_.find(upload_id);
      if (it == uploads_.end() || it->second.key != key) {
        SendResponse(fd, 404, "application/xml",
                     "<Error><Code>NoSuchUpload</Code></Error>");
        ::close(fd);
        return;
      }
      // Assemble parts in ascending partNumber order (ignore XML order for
      // simplicity; client always sends contiguous 1..N).
      std::string assembled;
      for (const auto& part : it->second.parts) {
        assembled += part.second;
      }
      objects_[key] = std::move(assembled);
      uploads_.erase(it);
    }
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<CompleteMultipartUploadResult>"
        << "<Bucket>" << XmlEscape(options_.bucket) << "</Bucket>"
        << "<Key>" << XmlEscape(key) << "</Key>"
        << "<ETag>\"multipart\"</ETag>"
        << "</CompleteMultipartUploadResult>";
    SendResponse(fd, 200, "application/xml", xml.str());
    ::close(fd);
    return;
  }

  // AbortMultipartUpload: DELETE /{bucket}/{key}?uploadId=ID
  if (method == "DELETE" && QueryHasKey(query, "uploadId")) {
    const std::string upload_id = QueryParam(query, "uploadId");
    {
      std::lock_guard<std::mutex> lock(mu_);
      uploads_.erase(upload_id);
    }
    SendResponse(fd, 204, "application/xml", "");
    ::close(fd);
    return;
  }

  if (method == "PUT") {
    {
      std::lock_guard<std::mutex> lock(mu_);
      objects_[key] = body;
    }
    SendResponse(fd, 200, "application/xml", "");
    ::close(fd);
    return;
  }

  if (method == "GET") {
    std::string data;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = objects_.find(key);
      if (it != objects_.end()) {
        data = it->second;
        found = true;
      }
    }
    if (!found) {
      SendResponse(fd, 404, "application/xml",
                   "<Error><Code>NoSuchKey</Code></Error>");
      ::close(fd);
      return;
    }
    if (!range_hdr.empty()) {
      size_t start = 0;
      size_t end_inclusive = 0;
      if (!ParseByteRange(range_hdr, data.size(), &start, &end_inclusive)) {
        SendResponse(fd, 416, "application/xml",
                     "<Error><Code>InvalidRange</Code></Error>");
        ::close(fd);
        return;
      }
      const std::string slice =
          data.substr(start, end_inclusive - start + 1);
      std::ostringstream cr;
      cr << "bytes " << start << "-" << end_inclusive << "/" << data.size();
      SendResponse(fd, 206, "application/octet-stream", slice,
                   {{"Content-Range", cr.str()},
                    {"Accept-Ranges", "bytes"}});
    } else {
      SendResponse(fd, 200, "application/octet-stream", data);
    }
    ::close(fd);
    return;
  }

  if (method == "HEAD") {
    bool found = false;
    size_t size = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = objects_.find(key);
      if (it != objects_.end()) {
        found = true;
        size = it->second.size();
      }
    }
    if (!found) {
      SendResponse(fd, 404, "application/xml", "");
    } else {
      std::ostringstream out;
      out << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: application/octet-stream\r\n"
          << "Content-Length: " << size << "\r\n"
          << "Accept-Ranges: bytes\r\n"
          << "Connection: close\r\n\r\n";
      const std::string bytes = out.str();
      ::send(fd, bytes.data(), bytes.size(), 0);
    }
    ::close(fd);
    return;
  }

  if (method == "DELETE") {
    {
      std::lock_guard<std::mutex> lock(mu_);
      objects_.erase(key);
    }
    // Real S3 DeleteObject is idempotent.
    SendResponse(fd, 204, "application/xml", "");
    ::close(fd);
    return;
  }

  SendResponse(fd, 405, "application/xml",
               "<Error><Code>MethodNotAllowed</Code></Error>");
  ::close(fd);
}

}  // namespace aster
