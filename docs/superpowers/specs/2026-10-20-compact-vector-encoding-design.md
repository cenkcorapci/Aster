# Compact vector encoding for Aster

**Status:** approved for implementation (user waived iterative design gates)  
**Date:** 2026-10-20  
**Goal:** Store **200 000 × 512-d** vectors using as little **RAM and disk** as possible while remaining compatible with Aster’s LSM + segmented HNSW model.

## 1. Problem

Baseline float32 storage:

| | Bytes |
| --- | ---: |
| Per vector | `512 × 4 = 2048` |
| 200k vectors | **≈ 409.6 MiB** |

At this scale the **vector block dominates** both RSS and `.ast` size (graph adjacency is ~128 B/vector at `M=16`). Encoding is the highest-leverage lever (see `docs/indexing.md` §10.1–10.2).

## 2. SOTA survey (2024–2025)

| Technique | Compression | Recall / notes | Fit for Aster |
| --- | --- | --- | --- |
| **IEEE float16** | 2× | Near-lossless for typical embedding ranges | Best “safe” drop-in |
| **Scalar int8 (SQ)** | 4× | ~0.99 recall with over-fetch + optional f32 rerank (Weaviate/Qdrant) | Production default for memory |
| **Product quantization (PQ)** | up to ~24–64× | Lower recall; LUT distance; train codebook | Deferred (heavy train + distance path) |
| **Binary / BBQ** | 32× | Strong on normalized embeddings | Good extreme mode |
| **RaBitQ (SIGMOD 2024) + multi-bit (2025)** | 32× (1 bit/dim) with **error bounds**; asymptotically optimal space–error | Best theory + SIMD IP estimates | Adopt a **practical 1-bit RaBitQ-style** codec without vendoring Faiss |

**Recommendation:** ship a ladder — `float32 → float16 → int8_sq → binary_rabitq` — selectable per collection. Default for the 200k×512 target: **int8_sq** (~102 MiB). Extreme: **binary_rabitq** (~13–15 MiB).

## 3. Design

### 3.1 `VectorEncoding` (header byte already reserved)

SSTable header offset 17 (`docs/sstable-format.md`):

| Value | Name | Bytes / vector (dim=`D`) |
| ---: | --- | --- |
| 0 | `float32` | `4D` |
| 1 | `float16` | `2D` |
| 2 | `int8_sq` | `D + 4` (int8 codes + f32 scale) |
| 3 | `binary_rabitq` | `⌈D/8⌉ + 4` (bit codes + f32 reconstruct scale) |

Unknown values → reader returns corruption/unsupported (forward-compatible).

### 3.2 Codec semantics

**float16** — round-trip via IEEE-754 binary16; little-endian on disk.

**int8_sq** — per-vector asymmetric scalar quantization:

```text
scale = max_i |x_i| / 127   (or 0 if all-zero)
code_i = round(clamp(x_i / scale, -127, 127))
reconstruct_i = code_i * scale
```

No training set; flush-time only. Optional later: segment-wide percentiles (Elastic OSQ).

**binary_rabitq** — RaBitQ-inspired 1-bit codes without an external dependency:

1. Apply a **fixed seeded random ±1 diagonal** (cheap JL-style preconditioner; same seed for encode/decode).
2. Pack `sign(Rx)_i` into `⌈D/8⌉` bytes.
3. Store `scale = ‖x‖₂ / √D` so reconstructed `±scale` vectors match original energy.
4. Distance path: decode to a scratch float buffer, then reuse existing `Score()`.

Multi-bit RaBitQ / PQ remain future encodings (new header values).

### 3.3 Memory path — `CompactVectorStore`

Contiguous packed codes owned by a segment:

- `Encode(encoding, dim, float* n×dim)` → store
- `Decode(i, float* out)` / `DecodeAll`
- `BytesUsed()` for budgets / tests
- Segment may hold `shared_ptr<const CompactVectorStore>` and leave `Row::vector` empty for live rows; `Get` materializes one vector; exact/HNSW distance uses decode-to-scratch or a decoded float cache only when building HNSW (build can expand once, then drop).

**Memtable** stays float32 (write amplification is tiny vs sealed segments). Packing happens on **flush / compact / open**.

### 3.4 Disk path — SSTable vector block

Header `vector_encoding` selects layout of the existing vector block (§7.4). Framing (`u32 pad_to_align` + pad) unchanged; payload after pad is encoding-specific packed bytes (still 32-byte aligned start for SIMD-friendly codecs).

`SstableWriteOptions::vector_encoding` + `Db::Options::vector_encoding` control writers. Readers honor the header byte.

### 3.5 Size budget (200k × 512)

| Encoding | Payload (approx) |
| --- | ---: |
| float32 | 409.6 MiB |
| float16 | 204.8 MiB |
| int8_sq | 102.4 MiB + 0.76 MiB scales ≈ **103.2 MiB** |
| binary_rabitq | 12.8 MiB + 0.76 MiB scales ≈ **13.6 MiB** |

Tests assert `BytesUsed()` formulas and round-trip error bounds; an integration test flushes a durable DB with int8/binary and checks on-disk vector-block size vs float32.

## 4. Non-goals (this change)

- Vendoring Faiss / RaBitQ-Library
- PQ codebook training
- Changing WAL row encoding (still float32)
- Guaranteeing identical HNSW recall without increasing `ef_search` / over-fetch

## 5. Testing

- Unit: encode/decode round-trips; size formulas for `N=200000, D=512`; bit-exact float16 edges; int8/binary MSE bounds
- Storage: SSTable write/read all encodings; reject unknown encoding
- Integration: durable flush + reopen with packed encoding; search still finds near neighbor on a small synthetic set
- Docs: `sstable-format.md`, `indexing.md` §10.1, `docs/vector-encoding.md`, CHANGELOG

## 6. Risks

| Risk | Mitigation |
| --- | --- |
| HNSW built on quantized vectors lowers recall | Document; allow float16 as safe default; binary requires higher `ef` |
| Old Aster binaries misread new encodings | Readers fail closed on unknown `vector_encoding` |
| Arduino / Tiny size | Encoding sources are scalar C++20; no new deps |
