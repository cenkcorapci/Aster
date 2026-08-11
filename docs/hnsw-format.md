# Aster HNSW graph binary format

**Status:** implemented for M2-T01 (insert/search land in M2-T02/T03)  
**Audience:** `aster/index/hnsw_graph.*`

Normative on-disk layout for an Aster **immutable per-segment HNSW graph**
(`.hnsw`). Topology only: vectors remain in the sibling SSTable (see
[`sstable-format.md`](sstable-format.md)). Lifecycle and search semantics
live in [`indexing.md`](indexing.md).

---

## 1. Goals and non-goals

**Goals**

- One immutable file per READY segment graph (`index/seg_<id>.hnsw`).
- Byte-identical layout on every host: **little-endian throughout**.
- Detect torn / corrupt files via magic, version, length, and CRC-32.
- Round-trip equality with the in-memory `HnswGraph` structure.

**Non-goals (this revision)**

- Embedding vector payloads (those stay in `.ast`).
- Delta / varint adjacency compression (planned once graphs are large).
- Online mutation of an existing `.hnsw` (graphs are rebuilt or merged).

---

## 2. File identity

| Item | Value |
| --- | --- |
| Extension | `.hnsw` |
| Typical path | `<table>/index/seg_<id:06llu>.hnsw` |
| Header magic (`u32`) | `0x41535448` ASCII `"ASTH"` |
| Footer magic (`u32`) | `0x41535447` ASCII `"ASTG"` |
| Format version (`u16`) | `1` |
| Endianness | Little-endian for every multi-byte integer |
| CRC | CRC-32, IEEE 802.3 / ISO 3309 polynomial (`0xEDB88320` reflected), same algorithm as `aster::Crc32` in `aster/storage/wal.cc` |

Writers MUST reject (or never produce) unsupported magic/version.
Readers MUST treat unknown version as corrupt/unreadable.

---

## 3. Top-level layout

```text
+-------------------------------------------------------------+
| 0x0000  File header (fixed 64 bytes)                        |
+-------------------------------------------------------------+
|         Body: node table + per-layer adjacency              |
+-------------------------------------------------------------+
|         Footer (16 bytes: body CRC + trailer)               |
+-------------------------------------------------------------+
```

`file_size == 64 + payload_bytes + 16`. Readers MUST enforce this equality
(`payload_bytes` is stored in the header).

---

## 4. File header (64 bytes)

All fields little-endian.

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `u32` | `magic` | `0x41535448` (`ASTH`) |
| 4 | `u16` | `format_version` | `1` |
| 6 | `u16` | `header_bytes` | `64` |
| 8 | `u32` | `flags` | MUST be `0` in v1 |
| 12 | `u32` | `m` | max degree on layers ≥ 1 (`HnswParams::m`) |
| 16 | `u32` | `m0` | max degree on layer 0; MUST equal `2·m` |
| 20 | `u32` | `ef_construction` | build beam (recorded for ops/debug) |
| 24 | `u32` | `ef_search_default` | default query beam |
| 28 | `u32` | `max_layers` | hierarchy cap from `HnswParams` |
| 32 | `u32` | `entry_point` | node index; `0xFFFFFFFF` iff `node_count == 0` |
| 36 | `u32` | `node_count` | dense node ids `0 .. node_count-1` |
| 40 | `u16` | `max_level` | highest occupied layer (0-based); `0` if empty |
| 42 | `u16` | `reserved0` | MUST be `0` |
| 44 | `u64` | `segment_id` | optional association with `.ast`; `0` allowed |
| 52 | `u32` | `payload_bytes` | byte length of the body |
| 56 | `u32` | `reserved1` | MUST be `0` |
| 60 | `u32` | `header_crc` | CRC-32 of bytes `[0, 60)` |

Invariants:

- `m ≥ 1`, `max_layers ∈ [1, 255]`, `max_level < max_layers`.
- If `node_count == 0`: `entry_point == 0xFFFFFFFF` and `max_level == 0`.
- If `node_count > 0`: `entry_point < node_count` and
  `node_level[entry_point] == max_level`.

---

## 5. Body

### 5.1 Node table

For each node `i` in `0 .. node_count-1` (8 bytes each):

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `u32` | `row_ordinal` | live-row slot in the sibling SSTable vector/ID index |
| 4 | `u8` | `level` | max layer for this node (inclusive); `≤ max_level` |
| 5 | `u8[3]` | `pad` | MUST be zero |

### 5.2 Adjacency

For `layer` from `0` to `max_level` inclusive, for each node with
`level ≥ layer`, emit:

| Type | Name | Notes |
| --- | --- | --- |
| `u16` | `degree` | `≤ m0` when `layer == 0`, else `≤ m` |
| `u32[degree]` | `neighbors` | node indices; no self-loops; each neighbor MUST also have `level ≥ layer` |

Nodes absent from a layer contribute nothing (no placeholder empty list).

---

## 6. Footer (16 bytes)

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `u32` | `body_crc` | CRC-32 of the body (`payload_bytes` bytes) |
| 4 | `u32` | `footer_magic` | `0x41535447` (`ASTG`) |
| 8 | `u16` | `format_version` | MUST match header (`1`) |
| 10 | `u16` | `reserved` | MUST be `0` |
| 12 | `u32` | `pad` | MUST be `0` (keeps footer at 16 bytes) |

---

## 7. Crash safety

Publication follows the same temp + `fsync` + rename pattern as SSTables
([`sstable-format.md`](sstable-format.md) §10). The manifest MUST NOT point
at a partially written `.hnsw` (see indexing lifecycle
`PENDING → BUILDING → READY`).

---

## 8. Versioning

- Bump `format_version` on any incompatible change to header, body layout,
  CRC scope, or degree rules.
- Additive reserved fields stay zero until a version bump claims them.

---

## 9. Summary

```text
 0                64                         64+payload        end
 |---- header ----|-------- body ------------|---- footer ----|
 | ASTH ver=1 ... | nodes + adjacency        | CRC ASTG ver=1 |
```
