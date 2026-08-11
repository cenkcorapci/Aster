# Aster documentation

| Doc | Purpose |
| --- | --- |
| [Tutorials](tutorials/README.md) | Hands-on guides (start here) |
| [Business plan](business-plan.md) | SaaS positioning, pricing, cloud architecture |
| [Development plan](development-plan.md) | Milestones and roadmap |
| [Design](design.md) | Engine architecture (LSM + segmented HNSW + ring) |
| [Code structure](code-structure.md) | Layers, packages, profiles |
| [Indexing](indexing.md) | ANN / HNSW contract (normative for M2+) |
| [HNSW format](hnsw-format.md) | On-disk `.hnsw` graph layout (M2) |
| [SSTable format](sstable-format.md) | On-disk `.ast` segment layout |
| [Client API](client-api.md) | Collection-centric product API (target) |
| [Versioning](versioning.md) | Single-version policy; embedded `aster::Db` contract |
| [Write bench](bench-write.md) | Laptop upserts/sec + kill-9 fuzz (M1-T14) |
| [Latency bench](bench-latency.md) | 1M×384d search p50 < 5ms (M2-T11) |
| [Tasks](tasks.md) / [Start the tasks](start-the-tasks.md) | Parallel work board |

Related:

- [`../tla/README.md`](../tla/README.md) — formal specs
- [`../clients/README.md`](../clients/README.md) — client packages
- Root [`README.md`](../README.md) — quick start
