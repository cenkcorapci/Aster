# Aster development plan

Roadmap for Aster: peer-to-peer, low-resource vector database in C++20
(Bazel), shipped as server, embedded library, and seven clients from one
monorepo.

Companion docs: [design](design.md), [code structure](code-structure.md),
[client API](client-api.md), [indexing](indexing.md), [tutorials](tutorials/README.md),
[tasks](tasks.md), [TLA+](../tla/README.md).

## Guiding rules

1. **Specify before building the hard parts.** Index lifecycle and
   replication are model-checked in TLA+ first
   (`tla/AsterLsmIndex.tla`, `tla/AsterReplication.tla`).
2. **Correctness baseline first.** Every ANN path is validated against the
   exact index; recall gates land with HNSW (M2).
3. **Single-node quality before distribution.** Excellent embedded/local
   engine before gossip and replication.
4. **The monorepo is the product.** One version tag for server + clients.

## Milestone status and gates

This is the canonical status and gate record. Task-level status and completed
task history live in [tasks.md](tasks.md). A milestone is complete only when
its gate is satisfied; completing its listed implementation tasks alone does
not establish that the gate was met.

| Milestone | Status | Gate |
| --- | --- | --- |
| M0 — Foundation | Complete | Foundation delivered |
| M1 — Durable engine | In progress | Kill -9 recovery and sustained-write gate |
| M2 — Segmented HNSW | Implementation tasks complete; gate pending verification | Recall and latency gate |
| M3 — Embedded & profiles | Implementation tasks complete; gate pending verification | Edge-profile resource gate |
| M4 — Server & observability | In progress | Soak, sanitizer, and image gate |
| M5 — Client libraries | In progress | Seven-client conformance gate |
| M6 — Release engineering | In progress | Release dry-run gate |
| M7 — Distribution | In progress | Five-node fault-tolerance gate |
| M8 — Cloud & SaaS | In progress | S3 cold-start and cost-model gate |
| M9 — 1.0 hardening | Not started | Published targets, security review, and operations-doc gate |

The board shows M1-T08, M4-T09, client transports, release work, most
distribution work, tenancy, and hardening as open. M2 and M3 are therefore
not unstarted: their recorded implementation tasks are complete, while their
milestone gates still need evidence.

## Milestones

### M0 — Foundation (done)

Bazel 9 (bzlmod), C++20 tree, tests + CI.

Delivered: `Status`/`Result`, LWW types, hash, scalar distances, exact
index, WAL + CRC, memtable, segments, compaction rules, consistent-hash
ring, top-k merge, `Db` facade, demo CLI, Thrift IDL, seven client stubs,
TLA+ specs.

### M1 — Durable single-node engine (~6 weeks)

Goal: crash-safe disk engine matching `tla/AsterLsmIndex.tla`.

Dependencies: M0. The remaining board task is M1-T08; the milestone gate is
the kill -9 recovery and sustained-write evidence recorded on the board.

### M2 — Segmented HNSW (~8 weeks)

Goal: real ANN per [indexing.md](indexing.md).

Dependencies: M1 durable storage interfaces. All listed M2 implementation
tasks are recorded complete; milestone completion waits for the recall and
latency gate evidence.

### M3 — Embedded & platform profiles (~4 weeks)

Goal: “SQLite for vectors” as a first-class product.

Dependencies vary by profile: platform targets can proceed independently,
while persistence and ANN integrations use the M1 and M2 interfaces recorded
by their tasks. All listed M3 tasks are recorded complete; milestone
completion waits for the edge-profile resource gate.

### M4 — Server & observability (~4 weeks)

Thrift RPC server, collection management, TOML config, Prometheus,
production Docker image. Depends on M2 and M3 interfaces. Exit: 24h soak,
ASan/TSan clean.

### M5 — Client libraries: transport + protocol (~6 weeks)

Bazel Thrift codegen for all seven languages; pooling/retries/failover;
conformance suite. Depends on the M4 server gate and the frozen IDL.

### M6 — Release engineering (~3 weeks)

Language rulesets in `MODULE.bazel`; publish PyPI / crates.io / Maven /
npm / Go tags / Docker Hub from `vX.Y.Z`. Depends on M5 package outputs. See
[versioning.md](versioning.md).

### M7 — Distribution (~10 weeks)

Gossip, replication (ONE/QUORUM/ALL), read repair, hinted handoff,
scatter-gather search, anti-entropy — match `tla/AsterReplication.tla`.
Depends on the M2 gate and the M4 server interface.

### M8 — Cloud & SaaS (~8 weeks)

S3 backend, HOT/WARM/COLD, accuracy profiles, multi-tenancy / quotas.
Depends on M7 distribution and M4 collection management.

### M9 — 1.0 hardening (~6 weeks)

Published perf targets, security review, fuzzing, docs site / ops guide.
Depends on the preceding delivery milestones and their gates.

## Dependency roadmap

The critical delivery path is M1 → M2 → M7 → M8 → M9. M3 and M4 can proceed
once their stated interfaces are ready; M5 follows the M4 server and frozen
IDL, and M6 follows the package outputs from M5. Do not advance to a
dependent milestone merely because implementation tasks are complete: satisfy
the predecessor's gate first.

## Risk register

| Risk | Impact | Mitigation |
| --- | --- | --- |
| HNSW recall across many small segments | Search quality | Auto-compact bounds segments; recall CI; exact fallback for small segments |
| Tombstone / LWW resurrection | Semantics bugs | TLA+ `NoResurrection`; property tests |
| Seven-language Bazel churn | Releases | Pin rulesets; conformance suite |
| S3 latency on cold search | SaaS | Block cache + pinned upper layers; WARM default |
| Scope before single-node quality | Everything | Milestone gates; M7 waits on M2 exit criteria |
