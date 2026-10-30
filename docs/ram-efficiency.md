# RAM-efficiency design choices

Why Aster optimises sealed-segment memory the way it does. Normative packing
and wire formats: [vector-encoding.md](vector-encoding.md). Engine overview:
[design.md](design.md). Indexing / HNSW memory layout: [indexing.md](indexing.md)
§10.

## Problem

Compression alone does not make a vector database low-RAM. If sealed
`CompactVectorStore` payloads (or a second float32 copy for HNSW) stay
heap-resident after flush/reopen, **corpus size still tracks anonymous RSS**.
Aster already had the right primitives for a different model: LSM + immutable
SSTables, compact encodings, per-segment indexes, POSIX/S3 backends, and a
hot/cold distinction for HNSW upper-layer pins.

The goal is: **a sealed collection must not require RAM proportional to the
full vector payload.** Latency, recall, and page-cache budget then become
trade-offs — not a hard “fits in DRAM” gate.

## Guiding principles

1. **Mutability boundary.** WAL and memtable stay float32 and heap-backed.
   Aggressive residency and packing apply when a segment is sealed or
   compacted — never by mutating mapped pages in place.
2. **Encoding ≠ residency.** `VectorEncoding` chooses on-disk / logical bytes
   per vector. `StoragePolicy::Residency` chooses whether those bytes are
   pinned, prefaulted/cached, or cold-mapped. Users can pick
   `float16 + cold`, `int8_sq + cached`, etc. independently.
3. **File-backed before algorithm churn.** mmap (or equivalent range reads)
   for immutable vector blocks is higher leverage than inventing a new ANN
   first. Aster already ships int8 (~4×) and 1-bit (~32× asymptotic) encodings;
   mmap removes the requirement that even those bytes stay resident.
4. **Measure anonymous vs file-backed separately.** mmap virtual size ≠ RAM.
   Report `VectorHeapBytesUsed()` / anonymous RSS apart from resident
   file-backed pages and logical `VectorBytesUsed()`.
5. **Pin only navigation state.** Upper HNSW layers / entry points stay small
   and local; bulk layer-0 adjacency and full vectors are the reclaimable
   plane.

## Chosen architecture (three planes)

```text
Query
  → Resident navigation   (HNSW upper layers / centroids — tens of MB)
  → Traversal plane       (mmap/SSD: layer-0 graph + SQ/RaBitQ/PQ codes)
  → Full-fidelity vectors (mmap/SSD: float16/float32 or compact SSTable block)
       ↑ only for Get / optional exact rerank of a small candidate set
```

This matches the shape used by DiskANN (graph + compact codes vs full
vectors on SSD), Faiss mmap IVF lists, Annoy’s immutable mmap indexes, and
Qdrant’s cold/cached/pinned placement — adapted to Aster’s per-segment LSM
lifecycle (new file → fsync → atomic manifest publish; old mappings live until
readers drop their segment ref).

## Decision log

| Choice | Decision | Why |
| --- | --- | --- |
| First milestone | mmap sealed vector blocks | Dominant RAM was packed payload + reopen `DecodeAll`; immutable segments make mmap natural |
| Default residency | `Residency::kCold` | Lowest steady-state anonymous heap; OS may reclaim untouched pages |
| `kCached` / `kPinned` | Explicit opt-in | Prefault or heap-copy when latency matters more than cold RSS |
| Reopen path | No `DecodeAll` on open | Bit-compatible search via packed/`Get`; avoid float32 spike |
| Durable flush | Write SSTable then remap | Sealed in-memory segment matches disk residency immediately |
| HNSW READY search | Score from packed/mmap store | Avoid a second full float32 corpus beside the segment |
| Distance hot path | Direct int8 / binary kernels (L2/Dot) | mmap fixes residency; decode-to-float would burn CPU instead |
| Cosine on packed | Decode to scratch when needed | Norm fidelity; keep kernels simple |
| PQ / OPQ | Deferred | Encoding already compresses; PQ helps when the *traversal* working set itself must be tiny — after mmap |
| Whole-segment compression | Not for vector/graph pages | Random ANN access; Zstd/LZ4 belong on metadata / cold blobs |
| Graph layer-0 mmap | Planned after locality relayout | Cold random HNSW without page locality is a latency trap |
| Clustered disk index (SPANN/HFresh-style) | Later optional index type | Predictable I/O at huge scale; not required to break RAM∝N |
| `O_DIRECT` / io_uring | Later | Correct mmap + measurable page cache first |
| Public API | Additive `storage_policy` | Existing `vector_encoding` unchanged |

## What shipped vs deferred

**Shipped**

- `MappedFile` for sealed `.ast` reads
- `CompactVectorStore` file-backed views (`HeapBytesUsed() == 0` when mapped)
- `StoragePolicy` / `Residency` (`cold` / `cached` / `pinned`)
- Bounded `PageCache` helper for hot-page promotion
- Flush/Open handoff without heap-proportional vector payload
- Packed HNSW scoring + direct int8/binary L2/Dot
- `//aster/bench:ram_efficiency_bench` (asserts anonymous vector heap = 0 under cold)

**Deferred (ordered)**

1. mmap / cold layer-0 graph with pinned upper layers (needs locality-aware
   compaction for acceptable latency)
2. Approximate traversal + exact batch rerank over-fetch
3. PQ64/PQ96 (then OPQ if recall warrants)
4. Async NVMe / record cache when page faults dominate
5. Optional clustered disk index

## Operational knobs

```cpp
options.vector_encoding = aster::VectorEncoding::kInt8Sq;   // size on disk
options.storage_policy.vectors = aster::Residency::kCold;   // default
options.storage_policy.cache_budget_bytes = 256ull << 20;   // hot-page budget
```

| Goal | Prefer |
| --- | --- |
| Minimum anonymous RSS | `kCold` + `int8_sq` or `binary_rabitq` |
| Lower cold-query latency | `kCached` (prefault) or `kPinned` |
| Highest fidelity | `float16`/`float32` still with `kCold` if RAM-bound |

Object-store HOT/WARM/COLD (`StorageMode`) is orthogonal: it controls
mirroring and S3 block-cache behaviour, not SSTable mmap residency.

## Benchmark expectations

After warm-up with a corpus larger than the RAM budget, treat these as
**engineering targets** (not claimed current results):

| Scale (approx.) | Anonymous working-set intent |
| --- | --- |
| 100k × 768-d | tens–low hundreds of MiB, not full float32 payload |
| 1M × 768-d | hundreds of MiB with cold mmap + compact encoding |
| 10M × 1536-d | ~1–1.5 GiB class budgets only if vectors/graph base stay file-backed |

Always record: recall@k, latency percentiles, **anonymous RSS**, heap vector
bytes, resident file pages if available, page faults / bytes read per query,
and cold vs warm first-query latency.

Reproduce the anonymous-heap gate locally:

```bash
bazel run //aster/bench:ram_efficiency_bench -- --n 15000 --dim 768 --encoding int8_sq
```

## References (external ideas, not dependencies)

- Annoy — immutable mmap index, optional prefault
- Faiss — `OnDiskInvertedLists` / mmap IVF
- Qdrant — cold/cached/pinned for vectors and HNSW links
- DiskANN / SPANN / HFresh — disk traversal + compact resident codes / centroids

Aster deliberately vendors none of these; the decisions above are the
adaptation to an LSM sealed-segment engine.
