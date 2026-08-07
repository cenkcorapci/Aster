#include "aster/storage/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <utility>

namespace aster {

namespace {

constexpr uint32_t kRecordMagic = 0x41535452;  // "ASTR"

std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

bool WriteAll(int fd, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t n = ::write(fd, p, size);
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

uint32_t Crc32(const void* data, size_t size) {
  static const std::array<uint32_t, 256> kTable = MakeCrcTable();
  uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) {
    c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

WalWriter::WalWriter(WalWriter&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), policy_(other.policy_) {}

WalWriter& WalWriter::operator=(WalWriter&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    policy_ = other.policy_;
  }
  return *this;
}

Result<WalWriter> WalWriter::Open(const std::string& path, SyncPolicy policy) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return Status::IoError("open failed: " + path);
  return WalWriter(fd, policy);
}

Status WalWriter::Append(const std::string& payload) {
  const uint32_t crc = Crc32(payload.data(), payload.size());
  const uint32_t length = static_cast<uint32_t>(payload.size());

  std::string record;
  record.reserve(12 + payload.size());
  record.append(reinterpret_cast<const char*>(&kRecordMagic), 4);
  record.append(reinterpret_cast<const char*>(&crc), 4);
  record.append(reinterpret_cast<const char*>(&length), 4);
  record.append(payload);

  if (!WriteAll(fd_, record.data(), record.size())) {
    return Status::IoError("wal append failed");
  }
  if (policy_ == SyncPolicy::kAlways) {
    if (::fsync(fd_) != 0) return Status::IoError("wal fsync failed");
  }
  return Status::Ok();
}

Result<std::vector<std::string>> ReplayWal(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return Status::IoError("open failed: " + path);

  std::string contents;
  char buf[64 * 1024];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
    contents.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  if (n < 0) return Status::IoError("read failed: " + path);

  std::vector<std::string> records;
  size_t off = 0;
  while (off + 12 <= contents.size()) {
    uint32_t magic, crc, length;
    std::memcpy(&magic, contents.data() + off, 4);
    std::memcpy(&crc, contents.data() + off + 4, 4);
    std::memcpy(&length, contents.data() + off + 8, 4);
    if (magic != kRecordMagic) break;                    // corrupt header
    if (off + 12 + length > contents.size()) break;      // torn record
    const char* payload = contents.data() + off + 12;
    if (Crc32(payload, length) != crc) break;            // corrupt payload
    records.emplace_back(payload, length);
    off += 12 + length;
  }
  return records;
}

}  // namespace aster
