#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"
#include "aster/storage/wal.h"

namespace aster {

// Single-node, single-collection engine: memtable + immutable indexed
// segments, searched together and merged (LSM-style, docs/design.md).
//
// When Options::data_dir is set, Flush persists SSTables + a manifest and
// Upsert/Delete append to a WAL so Open() can recover after a crash
// (tla/AsterLsmIndex.tla WalTruncationSafe / SearchCompleteness).
//
// A background thread flushes the memtable when size (memtable_flush_bytes)
// and/or time (memtable_flush_ms) triggers are hit.
class Db {
 public:
  struct Options {
    uint32_t dimension = 0;
    Metric metric = Metric::kCosine;
    size_t memtable_flush_bytes = 64 << 20;
    // Periodic flush while the memtable is non-empty. 0 disables the timer.
    uint64_t memtable_flush_ms = 0;
    std::string data_dir;  // empty = in-memory only
    SyncPolicy wal_sync = SyncPolicy::kAlways;
    // After Flush, compact when the segment count reaches this (keeps RAM
    // and search fan-out bounded without under-utilizing memory).
    size_t max_segments_before_compact = 8;
  };

  explicit Db(Options options);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Opens (or creates) a durable database under options.data_dir.
  // Requires non-empty data_dir. Loads manifest segments and replays WAL.
  static Result<std::unique_ptr<Db>> Open(Options options);

  Status Upsert(Row row);
  Status Delete(const RowId& id, Timestamp timestamp);

  std::optional<Row> Get(const RowId& id) const;
  std::vector<SearchHit> Search(const SearchRequest& request) const;

  // Seals the memtable into a new immutable segment. When durable, also
  // writes an SSTable, publishes the manifest, and truncates the WAL.
  Status Flush();

  // Full compaction: merges all segments, LWW, purges tombstones.
  // Rewrites even a single segment when it still contains tombstones;
  // removes all SSTables when nothing live remains.
  Status Compact();

  size_t segment_count() const;
  size_t memtable_rows() const;
  // Memtable + all segment rows (includes tombstones until compacted).
  size_t approximate_row_count() const;
  const std::string& data_dir() const { return options_.data_dir; }

 private:
  struct DeferFlushThread {};
  Db(Options options, DeferFlushThread);

  void StartFlushThread();
  void StopFlushThread();
  void BackgroundFlushLoop();
  bool ShouldFlushLocked() const;
  void RequestFlushLocked();
  Status FlushLocked();
  Status CompactLocked();

  Row Reconcile(const RowId& id) const;
  Status AppendWal(const Row& row);
  Status PublishManifest();
  Status MaybeCompact();
  // Deletes seg_*.ast / *.tmp files not referenced by the live segment set.
  void GarbageCollectOrphans();
  std::string SegmentPath(uint64_t id) const;
  std::string ManifestPath() const;
  std::string WalPath() const;

  Options options_;
  Memtable memtable_;
  std::vector<std::shared_ptr<const Segment>> segments_;  // oldest first
  uint64_t next_segment_id_ = 1;
  uint64_t manifest_generation_ = 0;
  std::optional<WalWriter> wal_;

  mutable std::mutex mu_;
  std::condition_variable flush_cv_;
  std::thread flush_thread_;
  bool stop_flush_thread_ = false;
  bool flush_requested_ = false;
  std::chrono::steady_clock::time_point memtable_live_since_{};
};

}  // namespace aster
