# Aster SSTable binary format (RFC)

**Status:** implemented for M1 (compression / HNSW side-car still open)  
**Audience:** SSTable writer/reader in `aster/storage/sstable.*`

This document is the normative on-disk layout for an Aster **immutable
segment file** (`.ast`). It matches the logical segment contents in
[`design.md`](design.md) / [`code-structure.md`](code-structure.md), the
tombstone and LWW rules in [`indexing.md`](indexing.md), and the in-memory
`Segment` / `Row` model in `aster/storage` and `aster/core/types.h`.

HNSW graphs are **not** stored in this file. They live beside the segment as
derived data (`index/seg_NNNNNN.hnsw` per `code-structure.md`) and may be
rebuilt from the vector block at any time.

---

## 1. Goals and non-goals

**Goals**

- Single immutable file per flushed or compacted segment.
- Point lookup by ID without a full scan (bloom → sparse index → ID index).
- Independent per-block compression.
- Crash-safe publication via temp write + rename + manifest swap (see §10).
- Byte-identical layout on every host: **little-endian throughout**.

**Non-goals (this revision)**

- Embedding the HNSW adjacency structure.
- Streaming / appendable mutation of an existing `.ast`.
- Big-endian hosts writing native order (hosts must encode LE explicitly).

---

## 2. File identity

| Item | Value |
| --- | --- |
| Extension | `.ast` |
| Typical path | `<table>/segments/seg_<id:06llu>.ast` |
| Magic (`u32`) | `0x41535453` ASCII `"ASTS"` (distinct from WAL `"ASTR"` = `0x41535452`) |
| Format version (`u16`) | `1` |
| Endianness | Little-endian for every multi-byte integer and IEEE-754 float |
| CRC | CRC-32, IEEE 802.3 / ISO 3309 polynomial (`0xEDB88320` reflected), same algorithm as `aster::Crc32` in `aster/storage/wal.cc` |

Writers MUST reject (or never produce) a file whose magic or version is
unsupported. Readers MUST treat unknown version as corrupt/unreadable.

---

## 3. Top-level layout

Blocks appear in this **fixed order**. No reordering is allowed in v1.
A block may be empty (`compressed_len == 0` and `uncompressed_len == 0`);
the **tree block** is optional and is absent when both lengths are zero.

```text
+-------------------------------------------------------------+
| 0x0000  File header (fixed 128 bytes) + block directory      |
+-------------------------------------------------------------+
|         Bloom filter block                                  |
+-------------------------------------------------------------+
|         Sparse index block                                  |
+-------------------------------------------------------------+
|         ID index block                                      |
+-------------------------------------------------------------+
|         Vector block                                        |
+-------------------------------------------------------------+
|         Metadata block (CBOR payloads)                      |
+-------------------------------------------------------------+
|         Tag bitmap block                                    |
+-------------------------------------------------------------+
|         Tree block (optional; may be empty)                 |
+-------------------------------------------------------------+
|         Footer (per-block CRCs + header CRC + trailer)      |
+-------------------------------------------------------------+
```

ASCII / hex sketch of the first bytes of a minimal empty-ish header:

```text
offset   00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
000000   53 54 53 41 01 00 80 00 00 00 00 00 .... ....  ; "ASTS", ver=1, hdr=128
         ^ magic LE: 53 54 53 41 = ASTS
               ^ u16 version = 0x0001
                     ^ u16 header_bytes = 0x0080
```

(On a little-endian dump, magic bytes read `53 54 53 41` = `"STSA"` in
ASCII left-to-right; the logical four-character tag is still **ASTS**.)

---

## 4. Alignment and sizing rules

1. The **file header** is exactly **128 bytes**, then immediately followed by
   the **block directory** (7 × 16 = 112 bytes). Total prelude =
   `128 + 112 = 240` bytes. Call this `prelude_size`.
2. Block *payload* regions start at the offsets recorded in the directory.
   Each block start offset MUST be a multiple of **8**.
3. The **vector block**’s *uncompressed* float payload (after any block
   framing described in §7.4) MUST begin at a file offset that is a multiple
   of **32**, so SIMD loads can assume 32-byte alignment when the block is
   memory-mapped uncompressed (`compression == None`). When the vector
   block is compressed, the writer still pads the *uncompressed* image so
   that after decompression the float array is 32-byte aligned relative to
   the start of the uncompressed block image (see §7.4).
4. Writers pad with `0x00` between the end of one block and the next start
   offset. Padding bytes are **not** covered by that block’s CRC (CRC covers
   only `[offset, offset + on_disk_len)`).
5. `row_count` includes tombstones. `live_row_count` counts rows with the
   tombstone flag clear. Always `live_row_count ≤ row_count`.
6. Rows in the ID index are strictly sorted by `id` ascending
   (`std::string` / byte-wise lexicographic order, same as
   `Segment::Get`). Duplicate IDs MUST NOT appear inside one SSTable
   (flush/compaction produce at most one version per id).

---

## 5. File header (128 bytes)

All fields little-endian.

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `u32` | `magic` | `0x41535453` |
| 4 | `u16` | `format_version` | `1` |
| 6 | `u16` | `header_bytes` | `128` (size of this header only; directory follows) |
| 8 | `u32` | `flags` | bitfield, see below |
| 12 | `u32` | `dimension` | vector length; `0` if `live_row_count == 0` |
| 16 | `u8` | `metric` | `0=L2`, `1=Dot`, `2=Cosine` (`aster::Metric`) |
| 17 | `u8` | `vector_encoding` | `0=float32` (only value in v1) |
| 18 | `u16` | `reserved0` | MUST be `0` |
| 20 | `u64` | `segment_id` | matches `Segment::id()` / filename id |
| 28 | `u64` | `row_count` | ID-index cardinality (incl. tombstones) |
| 36 | `u64` | `live_row_count` | non-tombstone rows |
| 44 | `u64` | `min_timestamp` | min `Row.timestamp` among all rows; `0` if empty |
| 52 | `u64` | `max_timestamp` | max `Row.timestamp`; `0` if empty |
| 60 | `u32` | `sparse_stride` | sparse index stores every *N*th row; default `16`; MUST be ≥ 1 |
| 64 | `u32` | `bloom_num_bits` | bloom filter bit length; `0` iff bloom block empty |
| 68 | `u32` | `bloom_num_hashes` | *k* hash functions; `0` iff bloom empty |
| 72 | `u64` | `bloom_hash_seed` | seed passed to `Hash64` for bloom probes |
| 80 | `u32` | `created_unix_s` | wall-clock hint for ops; not used for LWW |
| 84 | `u32` | `reserved1` | MUST be `0` |
| 88 | `u64` | `reserved2` | MUST be `0` |
| 96 | `u64` | `reserved3` | MUST be `0` |
| 104 | `u64` | `reserved4` | MUST be `0` |
| 112 | `u64` | `reserved5` | MUST be `0` |
| 120 | `u64` | `reserved6` | MUST be `0` |

### 5.1 `flags` bitfield

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `HAS_TREE` | Tree block present (`on_disk_len > 0`) |
| 1 | `HAS_TAGS` | Tag bitmap block present (`on_disk_len > 0`) |
| 2–31 | reserved | MUST be `0` in v1 |

Compression is **not** a file-level flag; it is per-block in the directory.

---

## 6. Block directory (112 bytes)

Immediately after the 128-byte header, seven descriptors in **block order**:

| Index | Block |
| --- | --- |
| 0 | Bloom filter |
| 1 | Sparse index |
| 2 | ID index |
| 3 | Vector |
| 4 | Metadata |
| 5 | Tag bitmap |
| 6 | Tree |

Each descriptor is **16 bytes**:

| Offset | Type | Name |
| --- | --- | --- |
| 0 | `u64` | `offset` — absolute file offset of the first on-disk byte of this block |
| 8 | `u32` | `on_disk_len` — byte length of on-disk contents (`0` = empty) |
| 12 | `u8` | `compression` — `0=None`, `1=LZ4`, `2=ZSTD` |
| 13 | `u8` | `reserved` — MUST be `0` |
| 14 | `u16` | `pad` — MUST be `0` |

**Uncompressed length:** every non-empty block’s uncompressed image begins
with a `u32` length prefix (see §7). Directory entries do not duplicate it.
When `compression == None`, `on_disk_len` MUST equal the uncompressed image
size (prefix + payload). When `compression != None`, `on_disk_len` is the
size of the compressor output of that entire uncompressed image.

**Empty block:** `on_disk_len == 0`, `compression == 0`, `pad == 0`.
`offset` MUST be 8-byte aligned and ≥ `prelude_size` (240); it MAY equal the
next block’s start. Store `block_crc[i] = 0` (see §9).

**Invariants (non-empty):** `offset ≥ 240`, `offset % 8 == 0`, and
`offset + on_disk_len ≤ footer_offset`. The first non-empty block SHOULD
start at offset `240` (already 8-aligned).

---

## 7. Block payloads

Unless noted, “uncompressed image” means the bytes after decompression
(or the on-disk bytes when `compression == None`). Every non-empty
uncompressed image begins with:

```text
u32 uncompressed_payload_len;  // bytes following this field
u8  payload[uncompressed_payload_len];
```

So `uncompressed_image_size = 4 + uncompressed_payload_len`. When
`compression == None`, `on_disk_len` MUST equal `uncompressed_image_size`.
When compressed, `on_disk_len` is the size of the compressor output of the
entire uncompressed image (including the `u32` length prefix).

### 7.1 Bloom filter block

Filters **all** row IDs in this segment (including tombstoned IDs), so a
negative bloom result skips the segment for point lookups even when the
latest version elsewhere is a delete.

Uncompressed payload:

```text
u8 bitset[bloom_num_bits / 8];   // bloom_num_bits MUST be divisible by 8
```

Probe for id `id`:

```text
h = Hash64(id, bloom_hash_seed)
for i in 0 .. bloom_num_hashes-1:
  bit = (h + i * 0x9E3779B97F4A7C15) % bloom_num_bits
  test bitset[bit / 8] & (1 << (bit % 8))
```

(`Hash64` = `aster::Hash64`.) If `bloom_num_bits == 0`, the block MUST be
empty and lookups MUST fall through to the sparse/ID index (no filtering).

Recommended defaults for M1: `bloom_num_bits = max(64, next_pow2(row_count * 10))`,
`bloom_num_hashes = 4`, `bloom_hash_seed = 0`.

### 7.2 Sparse index block

Accelerates binary search over the ID index. One entry per
`floor(row_count / sparse_stride)` boundaries, plus an entry for row `0`
if `row_count > 0`.

Uncompressed payload: a sequence of records, sorted by `row_ordinal`:

```text
repeated:
  u32 row_ordinal;          // 0 .. row_count-1; first is 0
  u32 id_index_offset;      // byte offset into ID index *payload* (after its u32 len)
  u16 id_len;
  u8  id_bytes[id_len];     // full id of rows_[row_ordinal]
```

`sparse_stride` from the header is the intended spacing; the writer MUST
emit an entry for ordinal `0, stride, 2*stride, …` and MUST emit a final
entry for `row_count - 1` when `row_count > 0` if it is not already
included. Readers binary-search these entries by `id_bytes` to bound the
ID-index scan range.

### 7.3 ID index block

Authoritative ordered map from id → row fields and pointers into other
blocks. Uncompressed payload is `row_count` concatenated records:

```text
repeated row_count times, sorted by id_bytes ascending:
  u16 id_len;
  u8  id_bytes[id_len];
  u8  row_flags;            // bit0 = TOMBSTONE; bits1–7 MUST be 0
  u8  reserved;             // MUST be 0
  u64 timestamp;            // Row.timestamp (LWW)
  u64 version;              // Row.version (LWW tie-break)
  u32 vector_slot;          // index into vector block row array; or UINT32_MAX if tombstone
  u32 metadata_offset;      // offset into metadata *payload* (after metadata u32 len)
  u32 metadata_len;         // 0 allowed (empty metadata)
  u32 tag_row_ordinal;      // same as this row’s ordinal in 0..row_count-1 (redundancy for readers)
```

**Tombstones**

- `row_flags & 0x01 != 0`.
- `vector_slot` MUST be `0xFFFFFFFF`.
- `metadata_len` SHOULD be `0` (writers MAY store empty CBOR); readers ignore
  metadata/tags for visibility when reconciling a winning tombstone.
- Tombstones **are** present in the bloom filter and ID index so deletes
  survive flush and participate in LWW across segments (`indexing.md` §5).
- Compaction may drop tombstones only under the full-overlap rule
  (`NoResurrection`); that policy is outside this file format.

**Live rows**

- `vector_slot` is in `0 .. live_row_count-1`, dense, assigned in ID order
  among live rows (first live row → slot 0, etc.).

### 7.4 Vector block

Stores **only live** vectors, slot-major, float32, little-endian.

Uncompressed image layout:

```text
u32 uncompressed_payload_len;   // = 4 + pad_to_align + 4*live_row_count*dimension
u32 pad_to_align;               // 0 .. 31
u8  align_pad[pad_to_align];    // zeros
float32 floats[live_row_count * dimension];  // slot s at index s * dimension
```

**Alignment:** let `base` be the file offset (or buffer offset) of the
`u32 uncompressed_payload_len` field. Choose the smallest
`pad_to_align ∈ [0, 31]` such that
`(base + 8 + pad_to_align) % 32 == 0`. Then `&floats[0]` is 32-byte aligned.
When `compression == None`, writers MUST also choose directory `offset` so
`offset % 32 == 0` (so mmap’d floats stay aligned in the file).

If `live_row_count == 0`, the vector block MUST be empty (`on_disk_len = 0`).
### 7.5 Metadata block (CBOR)

Opaque per-row blobs. `Row.metadata` remains an opaque byte string at the
storage layer; **CBOR** is the recommended logical encoding
(`aster/core/cbor.h`: JSON fixture ↔ CBOR helpers). Writers MAY store any
bytes; readers that expect structured metadata SHOULD decode CBOR.

Uncompressed payload:

```text
u8 blob_region[];   // concatenation of per-row metadata byte strings
```

Row `i`’s bytes are `blob_region[metadata_offset .. metadata_offset + metadata_len)`.
Offsets are relative to the start of `blob_region` (the metadata payload
after the block’s outer `u32` length). Tombstones typically use
`metadata_len = 0`. Blocks may set `compression` to LZ4/ZSTD for the whole
image; there is no per-row compression flag in v1.

Whole-block compression of already-CBOR data is independent of a future
optional “CBOR then Zstd per value” table option; v1 writers compress at
block granularity only.

### 7.6 Tag bitmap block

Maps tag string → set of row ordinals (including ordinals of tombstoned
rows that still carry tags if present; typically tombstones have empty
tag sets).

Uncompressed payload:

```text
u32 tag_count;
repeated tag_count times:
  u16 tag_len;
  u8  tag_bytes[tag_len];     // UTF-8 tag string
  u32 roaring_len;
  u8  roaring_blob[roaring_len];  // portable Roaring bitmap bytes
```

Bitmap membership is by **row ordinal** (position in the ID index), not by
`vector_slot`. Empty tag set for the segment ⇒ empty block
(`HAS_TAGS` clear).

**Roaring bytes:** portable (little-endian) Roaring bitmap serialization.
Full encode/decode lands with M2-T07. **For M1-T02:** if every row has an
empty tag set, write an empty tag block and clear `HAS_TAGS`. If any row
has tags, either (a) emit correct portable roaring payloads and set
`HAS_TAGS`, or (b) leave the block empty / clear `HAS_TAGS` and document
in the PR that filtered disk search is deferred — unit tests without tags
remain valid.

### 7.7 Tree block (optional)

Hierarchical `attrs` / tree column from `design.md`. Exact node encoding
is not specified in v1.

**M1-T02 MUST** write an empty tree block (`on_disk_len = 0`,
`HAS_TREE = 0`).
---

## 8. Footer

The footer begins at `footer_offset =` maximum over blocks of
`align8(offset + on_disk_len)` (after the last block’s payload and any
padding). It is fixed-size:

```text
u32 block_crc[7];     // CRC of on-disk bytes of blocks 0..6 (see §9)
u32 header_crc;       // CRC of bytes [0, prelude_size) = header + directory
u32 footer_magic;     // 0x41535446 ("ASTF")
u32 format_version;   // echo: must equal header format_version (1)
```

Total footer size: `7×4 + 4 + 4 + 4 = 40` bytes.

Readers SHOULD verify `footer_magic` and `format_version` before trusting
CRCs. Locating the footer: `file_size - 40` MUST equal `footer_offset`
computed from the directory (no trailing junk in v1).

---

## 9. CRC covering rules (normative)

Algorithm: identical to `aster::Crc32` (init `0xFFFFFFFF`, poly reflected
`0xEDB88320`, final XOR `0xFFFFFFFF`).

| CRC field | Covers exactly | Empty case |
| --- | --- | --- |
| `block_crc[i]` | File bytes `[desc[i].offset, desc[i].offset + desc[i].on_disk_len)` | If `on_disk_len == 0`, store `0` (do not hash padding) |
| `header_crc` | File bytes `[0, prelude_size)` i.e. 240-byte header+directory | never empty |

**Not covered by any CRC:** inter-block padding, and the footer itself
(the footer is authenticated by matching `header_crc` / `block_crc` after
recompute; a torn footer fails magic/length checks).

**Compression interaction:** CRCs are over **on-disk** (post-compression)
bytes, never over the decompressed image. That way readers can reject
corrupt compressed frames before allocating decompress buffers.

**WAL relationship:** WAL records use the same CRC function over their
payload only. SSTable does not share framing with WAL; only the CRC
routine is shared.

---

## 10. Manifest interaction and atomic swap points

Publication sequence (per vnode / table), matching
`indexing.md` §4.3 / §6.2 and `code-structure.md` S3/POSIX layout:

1. Flush/compaction builds the in-memory `Segment` (sorted rows, LWW-closed).
2. Writer creates `seg_<id>.ast.tmp` (or a unique temp name in the same
   directory), writes header → blocks → footer, `fsync`s the file.
3. Atomic rename: `seg_<id>.ast.tmp` → `seg_<id>.ast`.
4. Optionally write `seg_<id>.hnsw` the same way (temp + rename) when the
   graph reaches `READY`; the SSTable is valid and searchable via exact
   scan **before** any HNSW file exists.
5. Manifest update (M1-T05): write a new manifest generation listing the
   new segment id/path and dropping compacted inputs, then temp + rename
   the manifest object/file.

**Atomic swap points that matter for crash recovery**

| Crash moment | Durable outcome |
| --- | --- |
| During `.ast.tmp` write | Temp ignored; old manifest unchanged |
| After `.ast` rename, before manifest rename | Orphan `.ast` allowed; must not be read until referenced; GC later |
| After manifest rename | New generation visible; readers refcount old generation until release |

Readers MUST only open SSTables named by the **current** manifest
generation. The SSTable file itself has no internal “ready” flag beyond
CRC validity; visibility is entirely a manifest concern.

---

## 11. Writer checklist (M1-T02)

Enough to implement without other docs:

1. Sort/dedupe rows by id (one `Row` per id); keep tombstones.
2. Assign `vector_slot` for live rows in id order; build float matrix.
3. Fill header fields; `format_version = 1`; `magic = ASTS`.
4. Build bloom over all ids; sparse entries every `sparse_stride`; ID index
   records; metadata blob region; tag bitmaps (or empty); empty tree.
5. Choose per-block `compression` (`None` default for Tiny profile).
6. Layout: write 240-byte prelude with directory offsets patched after
   measuring each block; 8-align (32-align vectors when uncompressed);
   write blocks; compute CRCs; write 40-byte footer.
7. `fsync` + rename into place; leave manifest to M1-T05.

Round-trip test target: write N rows → file exists → independent reader
checks magic/version, recomputes CRCs, binary-searches an id, and matches
in-memory `Segment::Get` for fixture data (reader lands in M1-T03; writer
tests may parse with a test-only decoder).

---

## 12. Versioning

- Bump `format_version` on any incompatible change to header, directory,
  block order, or CRC scope.
- Additive reserved fields stay zero until a version bump claims them.
- Parallel `.hnsw` format is versioned separately (M2).

---

## 13. Summary diagram (byte flow)

```text
 0                         128                    240
 |------- header ---------|-- block directory ---|
 | magic ASTS ver=1 ...   | B0..B6 descriptors   |
                           |
                           v
              [pad][bloom][pad][sparse][pad][id index][pad]
              [vector float32s][pad][metadata][pad][tags][pad][tree?]
                           |
                           v
              footer: crc0..crc6 | header_crc | ASTF | ver
```
