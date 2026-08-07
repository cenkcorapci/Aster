# Aster task board

Atomic, claimable sub-tasks for the roadmap in
[`development-plan.md`](development-plan.md). Agents (human or AI) pick work
from here using the protocol in [`start-the-tasks.md`](start-the-tasks.md).

Hands-on docs: [tutorials](tutorials/README.md). Progress snapshot lives in
the development plan.

## How to read a task

| Field | Meaning |
| --- | --- |
| **ID** | Stable handle (`M1-T03`). Never reuse. |
| **Status** | `open` · `claimed` · `in_progress` · `blocked` · `done` · `cancelled` |
| **Lane** | Parallelism bucket — agents in different lanes rarely collide on files |
| **Depends** | Task IDs that must be `done` before this one can start |
| **Blocks** | Milestone exit criteria or later tasks waiting on this |
| **Touch** | Expected paths (claim ownership while working) |
| **Done when** | Acceptance criteria (testable) |

Status updates happen in this file (see start-the-tasks.md § Claiming).

### Lanes

| Lane | Owns | Safe to parallelize with |
| --- | --- | --- |
| `storage` | `aster/storage/**`, WAL/SSTable/manifest | `index`, `dist`, `rpc`, `clients-*` |
| `index` | `aster/index/**`, HNSW/SIMD/recall | `storage` (after shared interfaces), `dist`, `rpc` |
| `db` | `aster/db/**`, flush/compaction orchestration | wait for storage+index interfaces |
| `platform` | `aster/platform/**`, profiles, allocators | most lanes |
| `rpc` | `aster/rpc/**`, Thrift server, collections API | `clients-*`, `obs` |
| `obs` | `aster/metrics/**`, Grafana, Docker | `rpc`, `clients-*` |
| `clients-cpp` … `clients-js` | matching `clients/<lang>/**` | each other + `rpc` (after IDL freeze) |
| `release` | `MODULE.bazel`, CI, packaging | late; serialize with publishers |
| `dist` | `aster/distributed/**`, gossip/repair | after M2; careful with `db` |
| `cloud` | S3 backend, tenancy, SaaS config | after `platform` S3 skeleton |
| `spec` | `tla/**`, `docs/indexing.md` | always; code that changes semantics waits on spec |
| `qa` | fuzz, soak, Jepsen, recall CI | after the feature under test |

---

## M0 — Foundation

| ID | Status | Lane | Title |
| --- | --- | --- | --- |
| M0-T01 | done | — | Bazel workspace, core types, Status/Result |
| M0-T02 | done | — | Exact index + distance kernels |
| M0-T03 | done | — | WAL, memtable, segment, LWW compaction |
| M0-T04 | done | — | Consistent-hash ring |
| M0-T05 | done | — | Db facade + CLI demo |
| M0-T06 | done | — | Thrift IDL + seven client API stubs |
| M0-T07 | done | — | TLA+ AsterLsmIndex + AsterReplication (TLC green) |
| M0-T08 | done | — | docs: development-plan, indexing reference |

---

## M1 — Durable single-node engine

**Exit:** kill -9 fuzz recovers with zero acked-write loss; ≥100k upserts/sec with flush+compaction on a laptop.

### Storage format & recovery

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M1-T01 | done | storage | — | Spec SSTable binary layout (header, blocks, footer CRC) as a short RFC in `docs/` | `docs/sstable-format.md` (new) | Format doc reviewed; block order + CRC rules unambiguous |
| M1-T02 | done | storage | M1-T01 | Implement SSTable writer (encode one segment to disk) | `aster/storage/sstable*` | Round-trip unit test: write N rows → file exists → byte layout matches RFC |
| M1-T03 | done | storage | M1-T02 | Implement SSTable reader (mmap or pread) with ID binary search | `aster/storage/sstable*` | `Get(id)` from disk matches in-memory segment for fixture data |
| M1-T04 | done | storage | M1-T02 | Bloom filter + sparse index in SSTable | `aster/storage/`, `aster/index/bloom*` | Negative lookups skip disk I/O in test; false-positive rate documented |
| M1-T05 | done | storage | M1-T03 | Segment manifest with atomic swap (temp + rename) | `aster/storage/manifest*` | Crash between write and rename leaves previous generation intact |
| M1-T06 | done | storage | M1-T05, M0-T03 | Crash recovery: load manifest + replay WAL into memtable | `aster/db/`, `aster/storage/` | Property test: random write/flush/kill sequence → acked state restored (`WalTruncationSafe`) |
| M1-T07 | done | storage | M0-T03 | WAL group-commit (`EVERY_MS`) + truncate after successful flush | `aster/storage/wal*` | Group-commit latency measured; truncate leaves only post-flush records |
| M1-T08 | open | storage | M1-T02 | Binary RowId (16-byte UUID) with string conversion helpers | `aster/core/types*`, callers | Wire + disk use binary; API still accepts string; migration note in RFC |

### Background engine

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M1-T09 | open | db | M1-T05 | Background flush thread (size/time triggers) | `aster/db/` | Memtable flushes without explicit `Flush()` under write load |
| M1-T10 | open | db | M1-T03, M1-T09 | Size-tiered compaction scheduler | `aster/db/`, `aster/storage/` | Tier threshold triggers merge; segment count bounded under write soak |
| M1-T11 | open | db | M1-T10 | Tombstone GC only on full-overlap compaction | `aster/storage/segment*`, tests | Unit + TLA mapping: partial compact keeps tombstones; full purges (`NoResurrection`) |
| M1-T12 | open | storage | M1-T02 | CBOR metadata encode/decode | `aster/storage/` or `aster/core/` | Fixture JSON ↔ CBOR bytes round-trip |
| M1-T13 | open | storage | M1-T02 | LZ4/Zstd block compression behind feature flag | `aster/storage/`, `MODULE.bazel` | Compressed SSTable smaller than raw; uncompressed mode still default for Tiny |
| M1-T14 | open | qa | M1-T06, M1-T09, M1-T10 | kill -9 fuzz harness + laptop write benchmark | `aster/tests/` or `aster/qa/` | Fuzz green for 1h; ≥100k upserts/sec documented |

---

## M2 — Segmented HNSW

**Exit:** 1M×384d search p50 < 5 ms; recall@10 ≥ 0.95 @ ef=128 on CI datasets.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M2-T01 | open | index | M1-T03 | HNSW graph data structures + on-disk graph file format | `aster/index/hnsw*`, `docs/` | Graph serialize/load round-trip; format note linked from indexing.md |
| M2-T02 | open | index | M2-T01 | HNSW insert/build (`M`, `ef_construction`, `max_layers`) | `aster/index/hnsw*` | Build on fixture; neighbors within degree bounds; heuristic selection tested |
| M2-T03 | open | index | M2-T02 | HNSW search with per-query `ef_search` | `aster/index/hnsw*` | Recall vs exact index on small fixture ≥ 0.9 @ high ef |
| M2-T04 | open | db | M2-T03, M1-T09 | Wire PENDING→BUILDING→READY build state machine | `aster/db/`, `aster/storage/` | Segment searchable via exact until READY; then graph; matches TLA SegState |
| M2-T05 | open | index | M2-T02 | Compaction graph merge: rebuild-from-rows | `aster/index/`, `aster/storage/` | Compacted segment has one READY graph over live rows |
| M2-T06 | open | index | M2-T05 | Compaction graph merge: insert-into-largest (optional opt) | `aster/index/hnsw*` | Benchmark shows win on skewed merges; staleness debt forces rebuild |
| M2-T07 | open | index | M1-T03 | Tag roaring bitmaps per segment + post-filter over-fetch | `aster/index/tags*`, `aster/db/` | Filtered search matches exact filter semantics; adaptive fetch_k |
| M2-T08 | open | index | M0-T02 | SIMD distance: AVX2 + runtime dispatch | `aster/index/distance*` | Correct vs scalar; measurable speedup on supported CPU |
| M2-T09 | open | index | M2-T08 | SIMD distance: AVX-512 + ARM NEON | `aster/index/distance*` | CI builds for amd64+arm64; dispatch selects best |
| M2-T10 | open | qa | M2-T03, M2-T04 | Recall CI gate (SIFT1M/GloVe subset, recall@10 ≥ 0.95 @ ef=128) | `.github/` or CI + `aster/qa/` | Nightly job; PR gate on regression |
| M2-T11 | open | qa | M2-T04, M2-T08 | Latency bench: 1M×384d search p50 < 5 ms | `aster/qa/` | Numbers checked into docs or CI artifact |

---

## M3 — Embedded & platform profiles

**Exit:** edge profile on Raspberry Pi-class target, <128 MB RSS, 1M vectors on SSD.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M3-T01 | open | db | M1-T06 | Stabilize embedded `aster::Db` public API + versioning | `aster/db/db.h`, docs | Header is the contract; no breaking changes without note |
| M3-T02 | open | release | M3-T01 | Amalgamated / installable embedded library target | `aster/`, BUILD files | Downstream can depend on one `cc_library` or release tarball |
| M3-T03 | done | platform | M0 | PosixStorage backend (files + mmap) | `aster/platform/posix*` | Implements `StorageBackend`; Db can persist via it |
| M3-T04 | open | platform | M3-T03 | S3 storage backend skeleton (Put/Get/List/Remove) | `aster/platform/s3*` | Integration test against LocalStack or mock; not production-complete |
| M3-T05 | done | platform | — | Compile-time profiles: Tiny / Edge / Server | `aster/`, `.bazelrc` | Feature flags compile; Tiny excludes HNSW |
| M3-T05a | done | platform | M3-T05 | Arduino / bare-metal `//aster/embedded` + BusyBox musl image | `aster/embedded/`, `deploy/docker/`, `scripts/` | `libembedded.a` builds; `aster:local` image runs demo |
| M3-T06 | open | platform | M2-T09, M3-T05 | ARM (NEON) CI build for Edge profile | CI | Green arm64 job; local: `--config=raspberry_pi` / `build-matrix.sh --full` |
| M3-T07 | open | platform | M1-T09 | Arena / slab allocators on write path + memory budget | `aster/core/memory*`, `aster/db/` | Budget exceeded → clear Status; no unbounded growth in soak |
| M3-T08 | open | qa | M3-T05, M3-T03, M2-T04 | Pi/edge validation runbook + results | `docs/` | Documented RSS <128 MB with 1M vectors |

---

## M4 — Server & observability

**Exit:** 24h soak under mixed load; ASan/TSan clean; Docker image <15 MB.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M4-T01 | open | rpc | M3-T01, M0-T06 | Thrift codegen in Bazel for C++ server stubs | `aster/rpc/`, `MODULE.bazel` | Generated types build; service interface compiles |
| M4-T02 | open | rpc | M4-T01, M2-T04 | Framed-TCP Thrift server implementing Aster service | `aster/rpc/`, `aster/cli/` | Integration: upsert/get/search over localhost |
| M4-T03 | open | rpc | M4-T02 | Optional TLS transport | `aster/rpc/` | TLS accept + insecure still works |
| M4-T04 | open | rpc | M4-T02 | Collection create/drop/configure API | `aster/db/`, `aster/rpc/` | Matches `client-api.md` lifecycle states |
| M4-T05 | open | rpc | M4-T02 | TOML config loader for server | `aster/cli/`, config schema | Documented knobs load; bad config → clear error |
| M4-T06 | done | obs | M0 | Prometheus `/metrics` endpoint (real counters/histograms) | `aster/metrics/` | scrapeable; key latency metrics present |
| M4-T07 | open | obs | M4-T06 | Grafana dashboard JSON shipped in repo | `deploy/` or `docs/` | Import works against local Prometheus |
| M4-T08 | done | release | M4-T02 | Static Docker image (<15 MB) | `deploy/docker/`, `scripts/docker-build.sh` | BusyBox musl demo image builds (~3.6 MB); RPC serve still needs M4-T02 |


| M4-T09 | open | qa | M4-T02, M4-T06 | 24h soak + ASan/TSan CI jobs | CI, `aster/qa/` | Soak report; sanitizer jobs green |

---

## M5 — Client libraries (highly parallel)

**Exit:** language-agnostic conformance suite green for all seven clients.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M5-T01 | open | rpc | M4-T01 | Freeze IDL + document wire compatibility rules | `aster/rpc/aster.thrift`, `clients/README.md` | Version field / compat policy written |
| M5-T02 | open | release | M5-T01 | Thrift codegen Bazel rules for all client languages | `clients/*/BUILD.bazel`, `MODULE.bazel` | Each language generates stubs in CI |
| M5-T03 | open | qa | M5-T01, M4-T02 | Conformance YAML corpus + server test fixture | `clients/conformance/` | Corpus runs against C++ reference client |
| M5-T04 | open | clients-cpp | M5-T02, M4-T02 | C++ client transport (pool, retry, failover) | `clients/cpp/` | Conformance green |
| M5-T05 | open | clients-python | M5-T02, M4-T02 | Python client transport | `clients/python/` | Conformance green |
| M5-T06 | open | clients-go | M5-T02, M4-T02 | Go client transport | `clients/go/` | Conformance green |
| M5-T07 | open | clients-rust | M5-T02, M4-T02 | Rust client transport (async) | `clients/rust/` | Conformance green |
| M5-T08 | open | clients-java | M5-T02, M4-T02 | Java client transport | `clients/java/` | Conformance green |
| M5-T09 | open | clients-scala | M5-T02, M4-T02 | Scala client transport | `clients/scala/` | Conformance green |
| M5-T10 | open | clients-js | M5-T02, M4-T02 | JavaScript/TypeScript client transport | `clients/javascript/` | Conformance green |
| M5-T11 | open | qa | M5-T04…T10 | Aggregate conformance CI matrix | CI | One dashboard; all seven required |

---

## M6 — Release engineering

**Exit:** dry-run publish of `v0.1.0-rc` to test registries.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M6-T01 | open | release | M5-T02 | Add language rulesets to MODULE.bazel; replace filegroups | `MODULE.bazel`, `clients/**` | `bazel build //clients/...` produces real packages |
| M6-T02 | open | release | M6-T01 | PyPI wheel publish job (test index) | CI | Dry-run upload succeeds |
| M6-T03 | open | release | M6-T01 | crates.io publish job (dry-run) | CI | Dry-run succeeds |
| M6-T04 | open | release | M6-T01 | Maven Central (Java + Scala) dry-run | CI | Dry-run succeeds |
| M6-T05 | open | release | M6-T01 | npm publish dry-run | CI | Dry-run succeeds |
| M6-T06 | open | release | M6-T01 | Go module tag + Docker Hub + GitHub release assets | CI | Tag pipeline documented and rehearsed |
| M6-T07 | done | release | — | Single-version policy + CHANGELOG automation | docs, scripts | `vX.Y.Z` bumps all packages together |
| M6-T08 | open | release | M6-T02…T07 | Rehearse `v0.1.0-rc` end-to-end | CI | Checklist signed off |

---

## M7 — Distribution

**Exit:** 5-node cluster survives kill/partition/rejoin; quorum RYW under fault injection; anomalies reproduced in TLA+ first.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M7-T00 | open | spec | M2-T04 | Extend/confirm TLA+ before coding new protocol bits | `tla/` | Spec still TLC-green; new actions documented if needed |
| M7-T01 | open | dist | M7-T00, M0-T04 | Gossip membership + phi-accrual failure detector | `aster/distributed/gossip*` | Nodes join/leave; dead marked within detector window |
| M7-T02 | open | dist | M7-T01 | Publish vnode ownership via gossip | `aster/distributed/` | Ring view converges; RingCoverage tests |
| M7-T03 | open | dist | M7-T02, M4-T02 | Coordinator write path (ONE/QUORUM/ALL) | `aster/distributed/`, `aster/db/` | Matches AsterReplication Write semantics |
| M7-T04 | open | dist | M7-T03 | Coordinator read + read repair | `aster/distributed/` | Quorum read returns LWW merge; repair updates lagging replica |
| M7-T05 | open | dist | M7-T03 | Hinted handoff | `aster/distributed/` | Down replica receives hints on recovery |
| M7-T06 | open | dist | M7-T03, M2-T03 | Distributed search scatter-gather + MergeTopK | `aster/distributed/`, `aster/query/` | Multi-node search ≡ single-node on same data |
| M7-T07 | open | dist | M7-T06 | Replica-aware recall retry | `aster/distributed/` | Low-hit queries retry other replica / higher ef |
| M7-T08 | open | dist | M7-T04 | Anti-entropy repair (Merkle over segment row sets) | `aster/distributed/repair*` | Divergent replicas converge without full resync |
| M7-T09 | open | qa | M7-T03…T08 | Jepsen-style fault injection suite | `aster/qa/jepsen*` or similar | Kill/partition/rejoin scenarios automated |
| M7-T10 | open | spec | M7-T09 | Anomaly → TLA+ loop (reproduce or refute before fix) | `tla/`, docs | Process documented; at least one dry-run |

---

## M8 — Cloud & SaaS

**Exit:** cold-start node from S3 only; cost model benchmarked.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M8-T01 | open | cloud | M3-T04, M1-T05 | S3 backend complete (multipart, range GET, block cache) | `aster/platform/s3*` | Spot-kill recovery from S3 alone |
| M8-T02 | open | cloud | M8-T01, M2-T01 | Pin HNSW upper layers locally for S3 segments | `aster/index/`, `aster/platform/` | Cold search latency within documented bound |
| M8-T03 | open | cloud | M4-T04 | HOT/WARM/COLD storage modes | `aster/db/`, config | Mode switch behaves per client-api.md |
| M8-T04 | open | cloud | M2-T02 | Accuracy profiles (COST_OPTIMIZED … MAX_RECALL) | `aster/db/`, `aster/index/` | Presets map to HNSW params; documented |
| M8-T05 | open | cloud | M4-T04 | Per-collection resource limits + isolation levels | `aster/db/`, `aster/rpc/` | Limits enforced; Status on exceed |
| M8-T06 | open | cloud | M4-T04 | Multi-tenancy: projects, API keys, quotas | `aster/rpc/`, auth | Key auth required; quota exceeded rejected |
| M8-T07 | open | qa | M8-T01…T06 | Cold-start + cost model benchmark | `docs/`, `aster/qa/` | Numbers published |

---

## M9 — 1.0 hardening

**Exit:** published perf targets met; security review done; docs site live.

| ID | Status | Lane | Depends | Title | Touch | Done when |
| --- | --- | --- | --- | --- | --- | --- |
| M9-T01 | open | qa | M7, M2 | Verify & publish design.md performance targets | `docs/`, benches | Targets met or gaps filed |
| M9-T02 | open | qa | M4-T03, M8-T06 | Security review (TLS, authn/z) | docs + fixes | Written review + remediations |
| M9-T03 | open | qa | M1, M4 | Fuzz WAL / SSTable / RPC decoders | `aster/qa/fuzz*` | Continuous fuzz jobs; no crashers open |
| M9-T04 | open | release | M6 | Docs site + operations guide | `docs/` or site | Deployed; ops runbooks linked |
| M9-T05 | open | release | M6, M7 | Upgrade/downgrade story | docs | Version skew matrix tested |
| M9-T06 | open | release | M9-T01…T05 | 1.0 release checklist | — | Tag `v1.0.0` |

---

## Parallelism snapshot (what can start now)

Right after M0, these are **simultaneously open** with low collision risk:

1. **M1-T01** (storage RFC) — unlocks the whole storage lane  
2. **M2-T01** can *design* graph format in docs while M1-T01 proceeds (implementation waits on M1-T03)  
3. **M3-T05** (profile flags) — independent  
4. **M3-T07** arena design (implementation after M1 flush lands)  
5. **spec** polish / more TLC bounds anytime  
6. **M4-T06** metrics enrichment (partial) without server  

Do **not** start M5 client transports until M5-T01 (IDL freeze) and M4-T02 (server).  
Do **not** start M7 until M2 exit criteria hold (plan gate).

See [`start-the-tasks.md`](start-the-tasks.md) for how agents claim and coordinate.
