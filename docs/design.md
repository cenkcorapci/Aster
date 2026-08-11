# Aster design

Target architecture: Cassandra-style peer-to-peer vector database with LSM
storage and **per-segment HNSW** indexes. C++20, Bazel, CPU-only search.

**Status (pre-alpha):** single-node `Db` with memtable, WAL, SSTables, exact
search, and compaction works. Segmented HNSW, Thrift RPC, gossip, and
replication are roadmap — see [development-plan.md](development-plan.md).

Normative indexing lifecycle: [indexing.md](indexing.md) + `tla/`.  
On-disk format: [sstable-format.md](sstable-format.md).  
SaaS / S3 economics: [business-plan.md](business-plan.md).

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

Product API is **collection-centric** (dimension, metric, index settings) —
see [client-api.md](client-api.md). Wire surface: `aster/rpc/aster.thrift`.

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
Aligned contiguous vectors; optional float16 / int8 quantization later.

Detail and lifecycle states: [indexing.md](indexing.md).

## Replication & conflicts

Configurable RF. Default write/read `ONE` (BASE). Rows carry timestamp +
node id (+ version). Default conflict policy: last-write-wins. Background
repair reconciles replicas (M7; see `tla/AsterReplication.tla`).

### Distributed search

Coordinator → replica set → parallel segment HNSW → merge → filter → return.

## Memory tiers

| Tier | Keep hot |
| --- | --- |
| Hot | Upper HNSW layers, manifests, blooms, sparse indexes, tag dict |
| Warm | Lower graph / adjacency |
| Cold | Vector payloads, metadata |

SSD: mmap vectors/graph, larger segments. HDD: upper layers in RAM,
locality-aware reorder on compact, sequential vector pages.

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
locking simple, and scales with vnode ownership. SaaS cost advantage comes
from the same immutability when compute is ephemeral and S3 holds truth
([business-plan.md](business-plan.md)).
