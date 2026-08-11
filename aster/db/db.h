#pragma once

// Public embedded / in-process API contract for Aster.
//
// This header is the stable surface for linking the installable engine into a
// process (single collection, LSM memtable + segments). Prefer Bazel dep
// `//aster:embedded_lib` (re-exports `//aster/db`). Downstream code should
// depend only on the public declarations below — not on private members or
// .cc details.

//
// Versioning
// ----------
// Product / API version: repo-root VERSION, mirrored by ASTER_VERSION_* in
// aster/core/version.h (kept in sync by scripts/bump-version.sh).
// Breaking changes to this contract require a MAJOR bump, a CHANGELOG.md
// entry, and a note in docs/versioning.md. Additive / clarifying changes
// are MINOR or PATCH. See docs/versioning.md § Embedded C++ API.
//
// Thread safety
// -------------
// A single Db instance is safe for concurrent use from multiple threads:
// Upsert, Delete, Get, Search, Flush, Compact, and the size inspectors
// serialize on an internal mutex. Open() / construction must complete on
// one thread before other threads call methods. Destroying a Db while
// other threads still use it is undefined. Distinct Db instances do not
// share locks (do not open the same data_dir from two live Db objects).
//
// Tutorial: docs/tutorials/database-management.md

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
#include "aster/core/version.h"
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
// and/or time (memtable_flush_ms) triggers are hit. After flush, size-tiered
// compaction may merge similar-sized segments (compaction_tier_threshold).
class Db {
 public:
  // Construction / open options. Fix dimension and metric for the life of
  // the Db (and for a durable data_dir across restarts).
  struct Options {
    uint32_t dimension = 0;  // 0 = do not enforce on Upsert
    Metric metric = Metric::kCosine;
    // Soft size trigger: background flush when memtable reaches this many
    // approximate bytes. Also checked on the write path.
    size_t memtable_flush_bytes = 64 << 20;
    // Periodic flush while the memtable is non-empty. 0 disables the timer.
    uint64_t memtable_flush_ms = 0;
    // Empty = in-memory only (constructor). Non-empty = durable (prefer Open).
    std::string data_dir;
    SyncPolicy wal_sync = SyncPolicy::kAlways;
    // Size-tiered compaction: merge when a size bucket has this many
    // similar-sized segments (Cassandra-style; see docs/indexing.md §6.1).
    // 0 disables size-tiered auto-compaction.
    size_t compaction_tier_threshold = 4;
    // Size bucket growth factor between tiers (default 4 → L0, ~4×, ~16×…).
    size_t compaction_bucket_ratio = 4;
    // Optional hard cap: full-compact when total segment count reaches this.
    // 0 disables the cap. Kept as a safety net beside size-tiered policy.
    size_t max_segments_before_compact = 8;
  };

  // In-memory or caller-managed durable setup. Starts the background flush
  // thread. For crash-safe open of an existing data_dir, use Open().
  explicit Db(Options options);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Opens (or creates) a durable database under options.data_dir.
  // Requires non-empty data_dir. Loads manifest segments and replays WAL.
  static Result<std::unique_ptr<Db>> Open(Options options);

  // Insert or replace by id (LWW via Row::timestamp / Row::version).
  // Rejects non-tombstone vectors whose size != Options::dimension when
  // dimension != 0. Durable mode appends to the WAL before applying.
  Status Upsert(Row row);

  // Tombstone delete; needs a timestamp newer than the live row to win LWW.
  Status Delete(const RowId& id, Timestamp timestamp);

  // Point lookup after reconciling memtable + segments. nullopt if missing
  // or the newest version is a tombstone.
  std::optional<Row> Get(const RowId& id) const;

  // Exact (brute-force) top-k over memtable + segments; higher score is
  // always better. request.tags is an AND post-filter when non-empty.
  std::vector<SearchHit> Search(const SearchRequest& request) const;

  // Seals the memtable into a new immutable segment. When durable, also
  // writes an SSTable, publishes the manifest, and truncates the WAL.
  // May trigger auto-compaction per Options.
  Status Flush();

  // Full compaction: merges all segments, LWW, purges tombstones.
  // Rewrites even a single segment when it still contains tombstones;
  // removes all SSTables when nothing live remains.
  Status Compact();

  size_t segment_count() const;
  size_t memtable_rows() const;
  // Memtable + all segment rows (includes tombstones until compacted).
  size_t approximate_row_count() const;

  // Immutable after construction; safe to read without calling other methods.
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
  // Merges the segments at `indices` (into segments_). Full-overlap merges
  // (all live segments) purge tombstones; partial merges keep them.
  Status CompactSelectedLocked(const std::vector<size_t>& indices);

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
