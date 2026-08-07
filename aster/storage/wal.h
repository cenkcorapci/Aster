#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"

namespace aster {

// Append-only write-ahead log. A write is acknowledged to the client only
// after its WAL record is durable (policy-dependent, see SyncPolicy).
//
// Record framing (little-endian), per docs/code-structure.md:
//   [u32 magic][u32 crc32][u32 length][payload bytes]
// Replay stops at the first torn or corrupt record, which is the correct
// crash-recovery behavior for an append-only log.

enum class SyncPolicy {
  kAlways,   // fsync every record
  kEveryMs,  // group commit (server default)
  kNever,    // rely on OS flush (embedded profiles)
};

class WalWriter {
 public:
  ~WalWriter();

  static Result<WalWriter> Open(const std::string& path,
                                SyncPolicy policy = SyncPolicy::kAlways);

  WalWriter(WalWriter&& other) noexcept;
  WalWriter& operator=(WalWriter&& other) noexcept;
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Append(const std::string& payload);

  // Makes buffered group-commit data durable when policy is kEveryMs.
  Status Sync();

  // Truncates the WAL file to zero length (call after a successful flush
  // that has persisted all prior records into a segment).
  Status Truncate();

 private:
  WalWriter(int fd, SyncPolicy policy) : fd_(fd), policy_(policy) {}
  int fd_ = -1;
  SyncPolicy policy_;
  uint64_t last_sync_ms_ = 0;
};

// Replays every intact record in order. Corrupt/torn tail records are
// silently dropped (they were never acknowledged).
Result<std::vector<std::string>> ReplayWal(const std::string& path);

// Truncate an existing WAL path to empty (utility for recovery tooling).
Status TruncateWalFile(const std::string& path);

// CRC-32 (IEEE 802.3 polynomial), exposed for tests and the SSTable footer.
uint32_t Crc32(const void* data, size_t size);

}  // namespace aster
