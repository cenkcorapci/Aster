# Compact vector encoding

How Aster packs sealed vectors for **RAM and disk**. Normative wire values live
in [`sstable-format.md`](sstable-format.md) (§5 `vector_encoding`, §7.4).

## Purpose

At **200 000 × 512** float32 vectors the payload alone is **≈ 409.6 MiB**. The
HNSW graph is a small fraction of that (`indexing.md` §10.2). Encoding shrinks
on-disk size; **mmap cold residency** keeps sealed payloads out of anonymous
RSS so corpus size does not force heap size.

## Encodings

| `VectorEncoding` | Header byte | Bytes / vector (`D` dims) | 200k×512 payload |
| --- | ---: | --- | ---: |
| `kFloat32` | 0 | `4D` | 409.6 MiB |
| `kFloat16` | 1 | `2D` | 204.8 MiB |
| `kInt8Sq` | 2 | `D + 4` | ≈ 103.2 MiB |
| `kBinaryRabitq` | 3 | `⌈D/8⌉ + 4` | ≈ 13.6 MiB |

### float16

IEEE-754 binary16, little-endian. Near-lossless for typical embedding ranges.

### int8_sq (recommended default for the 200k×512 target)

Per-vector asymmetric scalar quantization:

```text
scale = max |x_i| / 127
code_i = round(clamp(x_i / scale, -127, 127))
```

No training set. L2 / Dot score packed codes directly; Cosine still decodes to
a scratch buffer for norms. Expect ~4× compression with high recall; raise
`ef_search` / over-fetch if needed.

### binary_rabitq

RaBitQ-inspired **1 bit/dim** (Gao & Long, SIGMOD 2024) without vendoring Faiss:

1. Apply a fixed seeded ±1 diagonal (cheap JL-style preconditioner).
2. Store sign bits + an `f32` scale so reconstructed energy matches `‖x‖₂`.
3. Dot scores packed bits directly; other metrics decode to ±scale.

Best extreme compression; recall is lower — prefer for cold tiers or when disk
footprint is the hard constraint. Multi-bit RaBitQ / PQ are reserved for future
header values.

## API

```cpp
aster::Db::Options options;
options.dimension = 512;
options.data_dir = "/var/lib/aster/compact";
options.vector_encoding = aster::VectorEncoding::kInt8Sq;
// Default StoragePolicy::vectors = Residency::kCold (mmap, no heap copy).
// options.storage_policy.vectors = aster::Residency::kPinned;  // force heap

auto opened = aster::Db::Open(options);
if (!opened.ok()) { /* handle opened.status() */ }
std::unique_ptr<aster::Db> db = std::move(opened.value());

aster::Row row;
row.id = "doc-1";
row.vector.resize(512);
row.timestamp = 1;
db->Upsert(std::move(row));
db->Flush();  // packs + remaps the sealed segment; WAL/memtable stay float32
```

- **Memtable / WAL** stay float32 (write path unchanged).
- **Flush / compact** write the SSTable vector block in the chosen encoding,
  then reopen via mmap when `data_dir` is set.
- **Sealed segments** mmap uncompressed vector blocks by default
  (`StoragePolicy::vectors = kCold`). `CompactVectorStore` may be file-backed
  (`HeapBytesUsed() == 0`); `Segment::VectorBytesUsed()` is the logical
  payload size, `VectorHeapBytesUsed()` is anonymous RAM.
- `Get` materializes one vector; exact / HNSW search scores packed codes
  without keeping a float32 copy of the corpus.
- HNSW build expands via `MaterializedRows()` once, persists `.hnsw`, then
  READY search reuses the sealed packed/mmap store.

### Residency (`StoragePolicy`)

| `Residency` | Sealed vectors |
| --- | --- |
| `kCold` (default) | mmap, demand-paged (`MADV_RANDOM`) |
| `kCached` | mmap + prefault of the vector extent |
| `kPinned` | heap copy of the packed payload |

Encoding and residency are orthogonal: e.g. `int8_sq + cold` or
`float16 + pinned`. Report **anonymous heap** and **file-backed** sizes
separately; mmap RSS is reclaimable page cache, not a hard working-set claim.

## Status

| Approach | Verdict in Aster |
| --- | --- |
| float16 / int8 SQ | Shipped |
| RaBitQ 1-bit | Shipped as `binary_rabitq` (practical subset) |
| mmap sealed vectors | Shipped — default cold residency |
| Product quantization | Deferred — codebook train + LUT distance |

## Tests

- `//aster/index:vector_encoding_test` — sizes for 200k×512, round-trips, RMSE
- `//aster/storage:storage_test` — SSTable encodings, mmap cold/pinned, packed search
- `//aster/integration:integration_test` — durable int8 + binary disk size
- `//aster/bench:ram_efficiency_bench` — anonymous heap vs logical payload after reopen
