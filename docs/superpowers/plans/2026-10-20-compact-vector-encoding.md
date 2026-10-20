# Compact Vector Encoding Implementation Plan

> **For agentic workers:** Execute task-by-task. Spec: `docs/superpowers/specs/2026-10-20-compact-vector-encoding-design.md`.

**Goal:** Pack 200k×512 vectors for minimal RAM + disk via float16 / int8 SQ / binary RaBitQ-style encodings.

**Architecture:** New `aster/index/vector_encoding` codec + `CompactVectorStore`; SSTable header byte + vector-block layouts; `Db::Options::vector_encoding` packs on flush and keeps segments packed in memory.

**Tech Stack:** C++20, Bazel, GoogleTest (existing Aster stack).

**Spec:** `docs/superpowers/specs/2026-10-20-compact-vector-encoding-design.md`

## Global Constraints

- No new third-party deps (no Faiss).
- Little-endian packed layouts; unknown encoding → soft fail on read.
- Memtable / WAL remain float32.
- Tiny/Arduino: scalar-only codec sources linkable if pulled into embedded later.

---

### Task 1: Codec + CompactVectorStore (TDD)

**Files:** `aster/index/vector_encoding.h`, `vector_encoding.cc`, `vector_encoding_test.cc`, `BUILD.bazel`

- [ ] Failing tests for size formulas, round-trips, MSE bounds
- [ ] Implement encodings + `CompactVectorStore`
- [ ] `bazel test //aster/index:vector_encoding_test`

### Task 2: SSTable wire format

**Files:** `sstable.h/cc`, `storage_test.cc`, `docs/sstable-format.md`

- [ ] `SstableWriteOptions::vector_encoding`
- [ ] Write/read packed payloads; reject unknown encoding
- [ ] `bazel test //aster/storage:storage_test`

### Task 3: Segment + Db packing

**Files:** `segment.h/cc`, `db.h/cc`, `exact_index` path, integration test

- [ ] Segment optional `CompactVectorStore`; Get/Search decode
- [ ] Flush/Open honor `Options::vector_encoding`
- [ ] Integration: durable round-trip + on-disk size vs f32

### Task 4: Docs + commit

- [ ] `docs/vector-encoding.md`, indexing/design/CHANGELOG/README index
- [ ] Commit + push
