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

#include "aster/core/features.h"
#include "aster/core/memory.h"
#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/core/version.h"
#include "aster/db/storage_mode.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"
#include "aster/storage/wal.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_graph.h"
#endif

namespace aster {

class S3Storage;

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
    // Hard cap on write-path memory (memtable + write arena). 0 = unlimited.
    // Upsert/Delete that would exceed the budget return ResourceExhausted
    // after attempting a flush to reclaim the memtable.
    size_t memory_budget_bytes = 0;
    // HOT / WARM / COLD cache layout (docs/client-api.md). Safe to change
    // online via SetStorageMode(). Default HOT keeps the historical local path.
    StorageMode storage_mode = StorageMode::kHot;
    // Optional S3-compatible object store used by WARM/COLD mirroring and
    // HNSW upper-layer pins. Required when storage_mode != HOT.
    std::shared_ptr<S3Storage> object_store;
#if ASTER_ENABLE_HNSW
    // Background PENDING→BUILDING→READY HNSW builds (docs/indexing.md §4.3).
    // When false, Flush still leaves segments PENDING (exact search) until
    // BuildPendingIndexes() or the index thread is re-enabled.
    bool background_index_build = true;
    HnswParams hnsw_params{};
    uint64_t hnsw_rng_seed = 1;
    // Optional accuracy preset that deterministically maps onto HNSW params
    // (M8-T04). When set, it overrides `hnsw_params` for newly built READY
    // graphs. (Already-built durable graphs still use their persisted params.)
    std::optional<AccuracyProfile> accuracy_profile = std::nullopt;
    // Optional graph-merge optimization (docs/indexing.md §6.2).
    // When enabled, some compactions try to reuse the largest input graph
    // and insert the other inputs' live rows into it. If too many deleted
    // ids become "ghost" traversal nodes, we fall back to rebuild-from-rows.
    bool hnsw_compaction_insert_into_largest = false;
    double hnsw_compaction_staleness_debt_threshold = 0.3;
#endif
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
  // Memtable approximate_bytes + write-arena usage (write-path footprint).
  size_t approximate_write_memory_bytes() const;

  // Per-segment index build states (oldest first). Empty when no segments.
  std::vector<SegState> segment_index_states() const;
  // True when that segment's Search path uses the HNSW graph.
  std::vector<bool> segment_uses_hnsw() const;

  // Number of vectors/nodes in the installed HNSW index for each segment.
  // Returns 0 when the segment has no installed READY HNSW graph.
  std::vector<size_t> segment_hnsw_index_sizes() const;

  // Synchronously run PENDING→BUILDING→READY for every live segment that is
  // still PENDING (and finish in-flight BUILDING). Used by tests; also safe
  // for callers that want READY before returning from Flush-heavy workloads.
  Status BuildPendingIndexes();

  // Current storage cache mode (HOT / WARM / COLD).
  StorageMode storage_mode() const;
  // Online mode switch (docs/client-api.md: cache mode is safe online).
  // WARM/COLD require Options::object_store. Switching into WARM/COLD mirrors
  // live segment/index objects and (re)pins HNSW upper layers.
  Status SetStorageMode(StorageMode mode);

  // Immutable after construction; safe to read without calling other methods.
  const std::string& data_dir() const { return options_.data_dir; }

 private:
  struct DeferFlushThread {};
  Db(Options options, DeferFlushThread);

  void StartFlushThread();
  void StopFlushThread();
  void BackgroundFlushLoop();
  void StartIndexThread();
  void StopIndexThread();
  void BackgroundIndexLoop();
  void RequestIndexBuildLocked();
  bool ShouldFlushLocked() const;
  void RequestFlushLocked();
  Status FlushLocked();
  Status CompactLocked();
  size_t ApproximateWriteMemoryLocked() const;
  // Projected memtable bytes if `row` were applied (LWW-aware).
  size_t ProjectedMemtableBytesLocked(const Row& row) const;
  // Enforces Options::memory_budget_bytes; may FlushLocked to reclaim.
  Status EnsureWriteMemoryLocked(const Row& row);
  // Merges the segments at `indices` (into segments_). Full-overlap merges
  // (all live segments) purge tombstones; partial merges keep them.
  Status CompactSelectedLocked(const std::vector<size_t>& indices);

  // Builds HNSW for `segment` outside mu_. On success installs READY under
  // mu_ if the segment is still live and BUILDING. Persists .hnsw when
  // durable. Returns false if the segment was dropped or aborted.
  bool BuildOneSegmentIndex(std::shared_ptr<const Segment> segment);

  Row Reconcile(const RowId& id) const;
  Status AppendWal(const Row& row);
  Status PublishManifest();
  Status MaybeCompact();
  // Deletes seg_*.ast / *.tmp / orphan .hnsw files not in the live set.
  void GarbageCollectOrphans();
  std::string SegmentPath(uint64_t id) const;
  std::string HnswPath(uint64_t id) const;
  std::string HnswRelativePath(uint64_t id) const;
  std::string ManifestPath() const;
  std::string WalPath() const;
  // Mirror a local durable object into Options::object_store (WARM/COLD).
  Status MirrorObjectLocked(const std::string& relative_key,
                            const std::string& absolute_path);
  // Pin HNSW upper-layer ranges for a READY segment into the object store.
  Status PinHnswUpperLayersLocked(uint64_t segment_id);
  // Apply WARM/COLD side effects for every live durable segment.
  Status ApplyObjectStorePolicyLocked();

  Options options_;
  Memtable memtable_;
  Arena write_arena_;  // WAL encode scratch; reset after append / flush
  std::vector<std::shared_ptr<const Segment>> segments_;  // oldest first
  uint64_t next_segment_id_ = 1;
  uint64_t manifest_generation_ = 0;
  std::optional<WalWriter> wal_;

  mutable std::mutex mu_;
  std::condition_variable flush_cv_;
  std::thread flush_thread_;
  bool stop_flush_thread_ = false;
  bool flush_requested_ = false;
  std::condition_variable index_cv_;
  std::thread index_thread_;
  bool stop_index_thread_ = false;
  bool index_build_requested_ = false;
  std::chrono::steady_clock::time_point memtable_live_since_{};
};

}  // namespace aster
