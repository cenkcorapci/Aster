# Aster design

Target architecture: Cassandra-style peer-to-peer vector database with LSM
storage and **per-segment HNSW** indexes. C++20, Bazel, CPU-only search.

For milestone status and delivery gates, see the
[development plan](development-plan.md).

Normative indexing lifecycle: [indexing.md](indexing.md) + `tla/`.  
On-disk format: [sstable-format.md](sstable-format.md).  
Sealed vector packing / mmap residency: [vector-encoding.md](vector-encoding.md).

## Goals

- Decentralized cluster: all nodes equal, consistent hashing + vnodes
- BASE consistency, tunable replication (`ONE` / `QUORUM` / `ALL`)
- High write throughput via LSM; ANN via segmented HNSW
- Low footprint: MCU → edge → multi-node from one codebase
- Few runtime deps; Prometheus metrics; Thrift client protocol

## Non-goals

- ACID transactions, joins, distributed SQL
- GPU acceleration
- Conflict-free multi-master beyond LWW (optional vector clocks later)

## Cluster

```
Client → any coordinator node
              ↓
     consistent-hash ring (vnodes)
              ↓
   per-vnode: WAL → memtable → SSTable segments (+ HNSW)
              ↓
   background: gossip · compaction · repair · metrics
```

- Hash: XXH3 or Murmur3 128-bit; default ~256 vnodes per physical node
- Partition key = hash(document id); RF clockwise on the ring
- Gossip: liveness, token ownership, load, index/compaction generations
- Failure detection: Phi Accrual

Every node can coordinate. No master.

### C++

`Coordinator` executes the replication protocol over independently durable
`Db` instances. A transport registers local or remote replica endpoints with
the same interface; this example uses three already-open durable instances.

```cpp
#include "aster/distributed/replication.h"

aster::CoordinatorOptions options;
options.replication_factor = 3;
options.hint_directory = "/var/lib/aster/hints";
auto opened = aster::Coordinator::Open(options);
if (!opened.ok()) { /* handle opened.status() */ }
std::unique_ptr<aster::Coordinator> coordinator = std::move(opened.value());

coordinator->AddReplica("node-a", node_a.get());
coordinator->AddReplica("node-b", node_b.get());
coordinator->AddReplica("node-c", node_c.get());

aster::Row row;
row.id = "doc-42";
row.vector = {0.1f, 0.2f};
row.timestamp = 42;
coordinator->Write(row, aster::ConsistencyLevel::kQuorum);

auto read = coordinator->Read("doc-42", aster::ConsistencyLevel::kQuorum);
```

Each acknowledged replica has synchronously appended the row to its WAL.
When a replica is unreachable, the coordinator fsyncs a coalesced hint before
returning; call `DrainHints("node-c")` after that replica rejoins. Quorum
reads merge the newest row and repair older reachable copies, including
tombstones.

## Storage (LSM)

### Write path

1. Append WAL  
2. Insert memtable  
3. Flush → immutable segment  
4. Build (or schedule) per-segment HNSW  
5. Replicate per consistency level  

Ack when durability + replication requirements are met.

### Segment contents

Each SSTable-style segment holds: sorted ID index, vector blob, compressed
metadata, tag bitmaps, bloom filter, timestamps, and (M2+) HNSW graph.

Immutability enables lock-free reads, sequential writes, concurrent indexing,
and safe compaction. This is the core alternative to one mutable global graph.

### Primary-key lookup

Per segment: bloom → sparse/ID index → binary search → offset read.  
Global: segment manifest (+ optional in-memory directory).  
Usually O(log n) over 1–2 segments after compaction.

### Metadata

Prefer CBOR + Zstd (LZ4 optional for embedded). Optional tree/trie columns
for hierarchical attributes.

## Data model

Logical row: `id`, float32 vector, optional metadata, optional tags,
timestamps / version for LWW.

Product API is **collection-centric** (dimension, metric, index settings).
Wire surface: `aster/rpc/aster.thrift`. HTTP JSON: [tutorials/http-api.md](tutorials/http-api.md).

## Segmented HNSW

One mutable global HNSW fights writes (locking, degradation). Aster treats
vectors like an LSM: each immutable segment owns its own graph; compaction
merges graphs by rebuilding the result segment.

| Path | Behavior |
| --- | --- |
| Write | No global graph mutation — only the new segment’s index |
| Search | Parallel per-segment ANN → merge top-k → tag filter → exact rerank |
| Compact | Merge rows + rebuild HNSW; drop old graphs |

### Parameters

| Param | Role |
| --- | --- |
| `M` | Graph degree |
| `ef_construction` | Build quality |
| `ef_search` | Per-query recall/latency |
| `max_layers` | Hierarchy depth |

### Tag filter

Per-segment `tag → roaring bitmap`. Flow: over-fetch ANN → bitmap AND →
optional metadata predicates → rerank. Filter-aware entry points are optional
later.

### CPU

SIMD distance kernels (AVX2/512, NEON, scalar). Metrics: L2, dot, cosine.
Aligned contiguous vectors; compact encodings (`float16` / `int8_sq` /
`binary_rabitq`) via `VectorEncoding` — see [`vector-encoding.md`](vector-encoding.md).

Detail and lifecycle states: [indexing.md](indexing.md).

## Replication & conflicts

Configurable RF. Default write/read `ONE` (BASE). Rows carry timestamp +
node id (+ version). Default conflict policy: last-write-wins. Background
repair reconciles replicas (M7; see `tla/AsterReplication.tla`).

### Distributed search

Coordinator → replica set → parallel segment HNSW → merge → filter → return.

## Memory tiers

| Tier | Keep hot | Default residency |
| --- | --- | --- |
| Hot | Upper HNSW layers, manifests, blooms, sparse indexes, tag dict | pinned |
| Warm | Lower graph / adjacency (cached when beneficial) | cold / cached |
| Cold | Vector payloads, metadata | **mmap cold** (shipped for sealed vectors) |

Sealed SSTable vector blocks use `MappedFile` + `StoragePolicy`
([vector-encoding.md](vector-encoding.md)): cold demand-pages, cached
prefaults, pinned heap-copies. Design rationale and deferred milestones:
[ram-efficiency.md](ram-efficiency.md). SSD: mmap vectors, larger segments.
HDD / object store: upper layers pinned; locality-aware reorder on compact.

## Networking & ops

- **RPC:** Apache Thrift over framed TCP (+ TLS later). HTTP JSON API already
  exists for single-node (`aster serve`) — see tutorials.
- **Metrics:** Prometheus `/metrics` (latency, recall samples, segments,
  compaction backlog, gossip, replication lag, vnode load).
- **Threads:** pools for network, WAL, memtable, search, compaction, index
  build, repair.

## Profiles

Compile-time feature profiles (`tiny` / `edge` / `server`) — MCU without
HNSW/gossip through full server. See [code-structure.md](code-structure.md).

Memory targets (aspirational): <64 MB minimal node; smaller without ANN.

## Ideas (non-normative)

Worth exploring after the baseline works:

1. **VNode-local graphs** — natural scale-out, simpler rebalance  
2. **Micrograph for recent writes** — near-real-time ANN before flush  
3. **Connectivity-aware compaction** — physical locality for disk  
4. **Adaptive `M`** — denser/sparser regions  
5. **Replica-aware ANN** — bump `ef_search` or second replica if recall dips  

## Performance targets (384-d, aspirational)

| Metric | Target |
| --- | --- |
| Writes / node | 100k+/s |
| ID lookup | <1 ms |
| ANN @ 1M / 10M | 1–5 ms / 5–20 ms |
| Crash recovery | WAL replay |

## Stack

| Piece | Choice |
| --- | --- |
| Language / build | C++20 / Bazel |
| RPC | Apache Thrift |
| Compression | Zstd + LZ4 |
| Hash | XXH3 / Murmur3 |
| Metrics | prometheus-cpp (optional) |
| I/O | mmap + optional direct I/O |

## Summary

Differentiator: **Cassandra-style LSM + immutable per-segment HNSW**, not a
globally mutable distributed graph. That preserves write throughput, keeps
locking simple, and scales with vnode ownership. The same immutability lets
sealed vectors stay mmap/file-backed so corpus size need not track worker RAM.
