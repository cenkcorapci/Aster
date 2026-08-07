# Aster design

Architecture for a Cassandra-style, peer-to-peer vector database with
LSM storage and per-segment HNSW indexes.

**Implementation status (pre-alpha):** single-node `Db` with memtable,
WAL, SSTables, exact search, and compaction is usable. Segmented HNSW,
Thrift server, gossip, and replication are still roadmap items — see
[development-plan.md](development-plan.md) and
[tutorials](tutorials/README.md).

This document is the long-form target architecture. Normative indexing
lifecycle: [indexing.md](indexing.md) + `tla/`.

---

This architecture is feasible and, in my view, has a strong differentiator: a Cassandra-style decentralized storage engine with an HNSW-based vector layer that is designed for dynamic writes and low-resource deployments. The key challenge is not HNSW search itself; it is maintaining high write throughput while preserving graph quality over time. Recent work on dynamic HNSW and LSM-integrated vector indexes suggests a better approach than a single mutable graph: treat vectors like an LSM tree, where writes land in immutable segments that each own an HNSW graph, and background compaction merges graphs incrementally. That preserves Cassandra-like write characteristics while avoiding expensive global graph mutations. HNSW supports incremental inserts well, but long-running update-heavy workloads can degrade graph connectivity unless maintenance is performed. LSM-style graph organization and connectivity-aware graph maintenance significantly improve update throughput and long-term recall.

blog.vectorchord.ai +3

### Architecture document: Project Aster

### Goals

Build a minimal, embeddable, peer-to-peer vector database in modern C++ that combines:

* Cassandra-style decentralized architecture (all nodes equal)

* Consistent hashing with virtual nodes

* BASE consistency

* Tunable replication factor

* HNSW-based approximate nearest neighbor search

* High write throughput

* Low memory footprint

* SSD and HDD optimized storage

* Efficient CPU-only vector search

* Prometheus/Grafana observability

* Thrift-compatible client protocol

* Zero or very few external runtime dependencies

### Non-goals

* Strong ACID transactions

* Cross-row joins

* Distributed SQL

* GPU acceleration

* Multi-master conflict-free updates beyond last-write-wins / vector-clock reconciliation

### High-level architecture

### Cluster architecture

ClientCoordinator node

Any node can coordinate

VNode AVNode BVNode CVNode D

Consistent hashing ring with virtual nodes; every node can coordinate reads and writes.

Storage engine per vnode

WALMemtableSSTable segmentsHNSW segment indexes

LSM-style writes, immutable vector segments, and background compaction.

Background services

GossipRepairCompactionMetrics

Membership, replication repair, segment merging, and Prometheus-compatible monitoring.

Every node is identical. Any node can coordinate reads or writes. Data ownership is determined through consistent hashing with virtual nodes (vnodes).

### Cluster topology

### Consistent hashing ring

* Murmur3 or XXH3 128-bit hash

* Configurable number of virtual nodes per physical node (default 256)

* Partition key = hash(unique_id)

* Replication factor RF configurable per table

Example:

| VNode     | Owner  |
| --------- | ------ |
| 0-1023    | Node A |
| 1024-2047 | Node B |
| 2048-3071 | Node C |
| 3072-4095 | Node A |

Replication follows clockwise vnode placement, identical to Cassandra.

### Gossip protocol

Use a lightweight gossip protocol inspired by Cassandra.

State propagated:

* node liveness

* token ownership

* load statistics

* index version

* compaction progress

* repair generation

Failure detection:

* Phi Accrual Failure Detector

Benefits:

* no master node

* automatic membership

* low bandwidth

* embedded-friendly

### Storage engine

Use an LSM-tree architecture.

### Write path

Client writeWAL

WALMemtable

Memtable flushImmutable segment

Immutable segmentBackground HNSW build

Writes are acknowledged once durability and replication requirements are satisfied.

### Segment layout

Each flushed segment contains:

* sorted ID index

* vector blob

* compressed metadata blob

* tag bitmap index

* HNSW graph

* bloom filter

* min/max timestamps

Immutable segments enable:

* lock-free reads

* sequential disk writes

* efficient compaction

* safe concurrent indexing

This is the key improvement over a single global HNSW graph.

### Data model

### Table schema

Example:

C++

```
table products {
    id        UUID PRIMARY KEY,
    vector    VECTOR<384, float32>,
    metadata  JSON,
    tags      SET<STRING>,
    attrs     TREE
}
```

### Row structure

C++

```
struct Row {
    UUID id;

    Vector vector;

    JsonBlob metadata;

    TagSet tags;

    TreeNode attrs;

    Timestamp created_at;

    Timestamp updated_at;

    Version version;
};
```

### Tree columns

Tree columns are stored as:

* compact binary trie

* variable-length encoded nodes

* path-compressed representation

Useful for hierarchical attributes.

### Metadata compression

Store JSON in compressed binary form.

Recommended:

* CBOR encoding

* followed by Zstd level 1-3

Reasons:

* excellent compression

* fast decompression

* deterministic binary layout

* minimal CPU cost

For embedded deployments:

* LZ4 optional

* configurable per table

### Primary key access

Requirement: every row must be retrievable by ID without scanning.

Solution:

Per-segment:

* sorted ID array

* sparse index every N rows

* bloom filter

* binary search

Global:

* segment manifest

* optional in-memory hash directory

Lookup:

1. consult manifest

2. bloom filter

3. binary search

4. direct offset read

Complexity:

* O(log n) per segment

* usually 1-2 segments after compaction

### Vector indexing architecture

This is the most important part.

### Problem

A single mutable HNSW graph suffers from:

* expensive updates

* locking

* graph degradation

* poor write scalability

Research on dynamic HNSW shows long-running update-heavy workloads can reduce connectivity and recall. LSM-integrated graph indexes avoid this by performing out-of-place updates and merging graph structures during compaction.

arXiv +1

### Proposed design: Segmented HNSW

Instead of one graph:

```
Level 0:
  Segment A (HNSW)
  Segment B (HNSW)

Level 1:
  Segment C (HNSW)

Level 2:
  Segment D (HNSW)
```

Every immutable segment owns an independent HNSW graph.

### Write

Insert:

1. append to WAL

2. insert into memtable

3. flush

4. build HNSW for that segment only

No global graph mutation.

### Search

Query executes across relevant segments:

1. search HNSW in parallel

2. collect top K from each

3. merge results

4. apply tag filtering

5. rerank exact distances

### Compaction

When segments merge:

* merge row sets

* rebuild a new HNSW graph

* discard old graphs

This gives:

* high write throughput

* stable graph quality

* simple locking model

* excellent concurrency

### HNSW tuning

Expose HNSW parameters directly.

| Parameter    | Purpose         |
| ------------ | --------------- |
| M            | graph degree    |
| ef_construct | build quality   |
| ef_search    | query accuracy  |
| max_layers   | hierarchy depth |

Expose ef_search per query, allowing tunable recall/latency.

Example:

C++

```
SearchRequest {
    vector,
    top_k = 20,
    ef_search = 128,
    tags = ["electronics", "available"]
}
```

Higher ef_search gives higher recall at higher CPU cost. HNSW naturally supports this tradeoff and is one of its strongest production advantages.

Qdrant +2

### CPU optimization

Avoid GPU dependency.

Distance kernels:

* AVX2

* AVX-512

* ARM NEON

* scalar fallback

Supported metrics:

* cosine

* dot product

* L2

Store vectors:

* 32-byte aligned

* contiguous arrays

* optional float16

* optional int8 quantized

### Tag filtering

Requirement: post-filter results by tags.

### Tag index

Per segment:

```
tag -> roaring bitmap
```

Search flow:

1. HNSW top N

2. bitmap intersection

3. metadata predicate

4. rerank

This avoids embedding filter logic into HNSW while remaining fast.

Future enhancement:

* filter-aware HNSW entry points

* tag-partitioned graphs

### Replication

Replication factor configurable.

Example:

```
CREATE TABLE products
WITH replication_factor = 3;
```

Write consistency levels:

* ONE

* LOCAL_ONE

* QUORUM

* ALL

Read consistency levels:

* ONE

* QUORUM

* ALL

Default:

* write = ONE

* read = ONE

BASE semantics.

### Conflict resolution

Each row carries:

* timestamp

* node id

* version counter

Use:

* Last Write Wins by default

* optional vector clock for advanced deployments

Background repair reconciles replicas.

### Read path

### ID lookup

```
Client
  -> coordinator
  -> owner vnode
  -> segment lookup
  -> row read
  -> response
```

### Vector search

```
Coordinator
    |
Determine replica set
    |
Parallel HNSW search
    |
Merge top K
    |
Tag filter
    |
Metadata filter
    |
Exact rerank
    |
Return results
```

Parallelism:

* per segment

* per replica

* SIMD distance computation

### HDD / SSD optimization

HNSW is memory-centric and random-access heavy, so naïvely placing the graph on disk performs poorly. A better design keeps the graph topology hot while storing vectors separately and using memory-mapped access for vector data.

Hacker News +1

### SSD mode

* mmap vector blobs

* mmap graph pages

* aggressive read-ahead

* larger segment size

### HDD mode

* keep upper HNSW layers fully in RAM

* keep graph adjacency compressed

* sequential vector pages

* connectivity-aware graph ordering during compaction

Additional optimization:

* reorder graph nodes by traversal locality

* cluster neighboring vectors physically

This significantly reduces random I/O.

### Memory layout

### Hot data

* HNSW upper layers

* segment manifests

* bloom filters

* sparse indexes

* tag dictionary

### Warm data

* HNSW lower layers

* adjacency lists

### Cold data

* vector payloads

* metadata blobs

* tree columns

Memory target:

* 1-2 bytes overhead per vector dimension beyond graph structures

* configurable graph density

### Networking protocol

Use Apache Thrift IDL.

Reasons:

* mature

* lightweight

* code generation

* embedded friendly

* multiple language support

Example:

thrift

```
struct SearchRequest {
  1: binary vector,
  2: i32 topK,
  3: i32 efSearch,
  4: list<string> tags
}

struct SearchResult {
  1: string id,
  2: double score,
  3: binary metadata
}

service VectorDB {
  SearchResponse search(1: SearchRequest req),
  void upsert(1: Row row),
  Row get(1: string id),
  void delete(1: string id)
}
```

Transport:

* framed TCP

* optional TLS

* optional QUIC in future

### Monitoring

Built-in Prometheus endpoint.

`/metrics`

Metrics:

| Metric               | Description         |
| -------------------- | ------------------- |
| read_latency_ms      | read latency        |
| write_latency_ms     | write latency       |
| hnsw_search_latency  | ANN latency         |
| hnsw_recall_estimate | sampled recall      |
| segment_count        | LSM segments        |
| compaction_backlog   | pending compaction  |
| gossip_messages      | cluster traffic     |
| replication_lag      | replica delay       |
| vnode_load           | per vnode ownership |

Grafana dashboards shipped by default.

### Threading model

Dedicated thread pools.

| Pool        | Purpose            |
| ----------- | ------------------ |
| Network     | RPC handling       |
| WAL         | durability         |
| Memtable    | writes             |
| Search      | HNSW queries       |
| Compaction  | segment merge      |
| Index Build | HNSW construction  |
| Repair      | replication repair |

Use lock-free queues between stages.

### Embedded deployment profile

Compile-time feature flags.

Example:

```
-DENABLE_TLS=OFF
-DENABLE_PROMETHEUS=ON
-DENABLE_QUIC=OFF
-DENABLE_ZSTD=OFF
-DENABLE_LZ4=ON
```

Target:

* ARM Cortex-A

* Raspberry Pi

* edge devices

* industrial gateways

Runtime memory target:

* <64 MB minimal node

* <16 MB possible without vector indexing

### Innovative vector index ideas

### 1. VNode-local HNSW

Each vnode owns independent HNSW graphs.

Benefits:

* no cross-partition locking

* natural horizontal scaling

* replication simplicity

* easier rebalancing

### 2. Hierarchical segmented graph

Keep:

* mutable micrograph for recent writes

* immutable HNSW segments

* merged macrograph

Search:

micrograph + segments

Compaction folds micrograph into segments.

This gives near real-time indexing.

### 3. Connectivity-aware compaction

During segment merge:

* reorder nodes by graph traversal locality

* cluster neighbors physically

* optimize adjacency page locality

Improves HDD and SSD performance.

### 4. Adaptive graph density

Instead of fixed M:

* dense regions - lower M

* sparse regions - higher M

Reduces memory while maintaining recall.

### 5. Replica-aware ANN

Coordinator queries multiple replicas.

If recall appears low:

* increase ef_search

* or query second replica

Dynamic recall improvement without rebuilding indexes.

### Expected performance

Assuming 384-dimensional vectors.

| Metric                      | Target             |
| --------------------------- | ------------------ |
| Write throughput            | 100k+/sec per node |
| ID lookup                   | <1 ms              |
| Vector search (1M vectors)  | 1-5 ms             |
| Vector search (10M vectors) | 5-20 ms            |
| Replication latency         | <5 ms LAN          |
| Recovery from crash         | WAL replay only    |
| Memory overhead             | Configurable by M  |

### Recommended implementation stack

* Language: C++20

* Build: CMake

* RPC: Apache Thrift

* Compression: Zstd + LZ4

* Hashing: XXH3 or Murmur3

* SIMD: xsimd or compiler intrinsics

* Metrics: prometheus-cpp (optional)

* Files: mmap + direct I/O option

* Concurrency: folly-style primitives or std::atomic

### Final recommendation

The strongest differentiator is to combine Cassandra’s LSM storage model with segmented HNSW indexes. Instead of treating HNSW as a mutable global graph, treat it as an immutable per-segment graph managed by compaction, analogous to SSTables. This design preserves Cassandra’s exceptional write path, enables tunable ANN accuracy through `ef_search`, supports efficient CPU execution, and scales naturally across virtual-node ownership boundaries. It is also considerably simpler to implement correctly in C++ than a globally mutable distributed graph, while aligning with recent research directions for dynamic vector indexing and long-term graph quality maintenance.

arxiv.org +3
