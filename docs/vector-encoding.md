# Compact vector encoding

How Aster packs sealed vectors for **RAM and disk**. Normative wire values live
in [`sstable-format.md`](sstable-format.md) (§5 `vector_encoding`, §7.4). Design
notes: [`superpowers/specs/2026-10-20-compact-vector-encoding-design.md`](superpowers/specs/2026-10-20-compact-vector-encoding-design.md).

## Why

At **200 000 × 512** float32 vectors the payload alone is **≈ 409.6 MiB**. The
HNSW graph is a small fraction of that (`indexing.md` §10.2). Encoding is the
main lever for Tiny/Edge hosts and dense collections.

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

No training set. Distance uses decode-to-float then the existing kernels.
Expect ~4× compression with high recall; raise `ef_search` / over-fetch if needed.

### binary_rabitq

RaBitQ-inspired **1 bit/dim** (Gao & Long, SIGMOD 2024) without vendoring Faiss:

1. Apply a fixed seeded ±1 diagonal (cheap JL-style preconditioner).
2. Store sign bits + an `f32` scale so reconstructed energy matches `‖x‖₂`.
3. Decode to ±scale for scoring.

Best extreme compression; recall is lower — prefer for cold tiers or when RAM
is the hard constraint. Multi-bit RaBitQ / PQ are reserved for future header
values.

## API

```cpp
Db::Options opt;
opt.dimension = 512;
opt.vector_encoding = VectorEncoding::kInt8Sq;  // pack on flush + reopen
```

- **Memtable / WAL** stay float32 (write path unchanged).
- **Flush / compact** write the SSTable vector block in the chosen encoding.
- **Sealed segments** keep a `CompactVectorStore` in RAM (`Segment::VectorBytesUsed()`);
  `Get` materializes one vector; exact search decodes to a scratch buffer.
- HNSW build expands via `MaterializedRows()` then discards the temporary floats.

## SOTA context (choices)

| Approach | Verdict in Aster |
| --- | --- |
| float16 / int8 SQ | Shipped — matches prior `indexing.md` §10.1 plan |
| RaBitQ 1-bit | Shipped as `binary_rabitq` (practical subset) |
| Product quantization | Deferred — codebook train + LUT distance |
| Full Faiss RaBitQ-Library | Deferred — no new deps; theory cited in the design spec |

## Tests

- `//aster/index:vector_encoding_test` — sizes for 200k×512, round-trips, RMSE
- `//aster/storage:storage_test` — SSTable encodings + packed Segment search
- `//aster/integration:integration_test` — durable int8 + binary disk size
