# Tutorial: database management

Manage a single-node Aster database with the C++ `aster::Db` API (and the
demo CLI / BusyBox image). This is the supported path today: one collection
per `Db`, exact (brute-force) search, durable SSTables + WAL when
`data_dir` is set.

Remote multi-collection RPC is milestone M4/M5 — see
[client libraries](client-libraries.md).

## Prerequisites

- Bazel (see `.bazelversion`) and a C++20 toolchain
- Optional: Docker for the BusyBox image

```bash
bazel test //aster/...
bazel run //aster/cli:aster -- --data-dir /tmp/aster-demo
```

## Concepts

| Piece | Role |
| --- | --- |
| Memtable | In-memory write buffer (LWW per id) |
| Segment | Immutable flushed rows + vector index |
| WAL | Crash recovery for unflushed writes (durable mode) |
| Manifest | Lists live SSTable segments |
| Flush | Seal memtable → new segment (+ SSTable on disk) |
| Compact | Merge segments, drop tombstones (full compact) |

Deletes are tombstones. They disappear only after a full compaction that
covers every segment that might still hold an older version.

## Open a database

**In-memory** (no durability — fine for tests):

```cpp
#include "aster/db/db.h"

aster::Db::Options opt;
opt.dimension = 384;
opt.metric = aster::Metric::kCosine;
aster::Db db(opt);
```

**Durable** (directory must exist or be creatable):

```cpp
aster::Db::Options opt;
opt.dimension = 384;
opt.metric = aster::Metric::kCosine;
opt.data_dir = "/var/lib/aster/demo";
opt.wal_sync = aster::SyncPolicy::kAlways;           // or kEveryMs / kNever
opt.memtable_flush_bytes = 64 << 20;                 // ~64 MiB
opt.max_segments_before_compact = 8;                 // auto-compact threshold

auto opened = aster::Db::Open(opt);
if (!opened.ok()) { /* handle opened.status() */ }
std::unique_ptr<aster::Db> db = std::move(opened.value());
```

`Open` loads the manifest, opens each SSTable, then replays the WAL.

CLI equivalent:

```bash
bazel run //aster/cli:aster -- --data-dir /tmp/aster-demo
# or: ASTER_DATA_DIR=/tmp/aster-demo bazel run //aster/cli:aster
```

## Write, read, search

```cpp
aster::Row row;
row.id = "doc-1";
row.vector = /* size == opt.dimension */;
row.timestamp = 1;                    // LWW: higher timestamp wins
row.tags = {"electronics", "sale"};
row.metadata = /* opaque bytes for now */;

if (!db->Upsert(std::move(row)).ok()) { /* … */ }

auto got = db->Get("doc-1");          // nullopt if missing or tombstoned

aster::SearchRequest req;
req.vector = query;
req.top_k = 10;
req.tags = {"electronics"};           // must have *all* listed tags
auto hits = db->Search(req);          // higher score is always better
```

Metrics: `kL2` (score = −distance²), `kDot`, `kCosine`.

```cpp
db->Delete("doc-1", /*timestamp=*/2); // tombstone; needs newer timestamp
```

## Flush and compact

```cpp
db->Flush();     // memtable → segment; durable: write SSTable, truncate WAL
db->Compact();   // merge all segments; drop tombstones
```

Notes:

- Upserts also auto-flush when `memtable_flush_bytes` is exceeded.
- After flush, if `segment_count() >= max_segments_before_compact`, Aster
  auto-compacts (set `0` to disable).
- Prefer periodic flush under write load so crash recovery stays short.

Inspect:

```cpp
db->segment_count();
db->memtable_rows();
db->data_dir();
```

## Docker (BusyBox)

```bash
./scripts/docker-build.sh
docker run --rm -v aster-data:/data aster:local
```

The image runs the demo CLI with `ASTER_DATA_DIR=/data`. Mount a volume so
SSTables and WAL survive restarts. Do not run as root without the image
entrypoint’s `su-exec` path.

## Embedded / MCU

Tiny profile (no POSIX disk):

```bash
bazel build --config=arduino //aster/embedded
# → bazel-bin/aster/embedded/libembedded.a
```

API: `aster::embedded::Db` — same upsert/search/flush/compact ideas, flush
triggered by row count (`memtable_flush_rows`), in-memory only.

## Ops checklist

1. Pick `dimension` and `metric` once; they are fixed for that DB directory.
2. Use durable mode + `kAlways` (or group-commit later) for production data.
3. Flush regularly; keep `max_segments_before_compact` modest (default 8).
4. Compact after heavy delete/update churn to reclaim space.
5. Back up `data_dir` (manifest + `seg_*.ast` + `WAL`) as a consistent set
   after a flush (or while the process is stopped).

## What’s not here yet

- Multi-collection server / Thrift RPC (M4)
- HNSW ANN (M2) — search is exact today
- Background flush/compaction threads (planned in M1)
- S3 primary storage (M8)

Next: [client libraries](client-libraries.md).
