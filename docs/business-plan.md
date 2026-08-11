# Aster business plan

Positioning for a low-cost, serverless vector-database SaaS built on the
open-source Aster engine. Companion: [development-plan.md](development-plan.md)
(engineering milestones), [design.md](design.md) (engine architecture).

## Positioning

> Cheapest production-ready vector DB for small teams: simple API, S3
> economics, serverless scaling.

Not another “better HNSW” product. The gap is **cost and ops** for teams that
find Pinecone expensive and Milvus/Qdrant clusters heavy.

| Differentiator | How |
| --- | --- |
| S3-first storage | Immutable segments; workers are disposable |
| Shared multi-tenant compute | Spot/K8s pools, not per-tenant clusters |
| Simple API | Collection CRUD + search (HTTP today; Thrift clients later) |
| Open-source engine | Same kernel embeddable from MCU to server |

## Market

| Option | Strength | Weakness for SMBs |
| --- | --- | --- |
| Pinecone / Weaviate Cloud / Qdrant Cloud | DX, managed | Always-on pricing |
| Milvus / Zilliz | Power, open source | Ops complexity (K8s + deps) |
| pgvector | Cheap if Postgres already | Large vector workloads |
| Hyperscaler vector search | Integrated | Vendor lock-in, pricing |

**Primary customers:** AI startups and SaaS teams adding RAG / semantic search
($50–500/mo budgets). Free tier for developers. Embedded / edge is longer-term.

## Pricing sketch (illustrative)

| Tier | Price | Rough limits |
| --- | --- | --- |
| Free | $0 | ~10k vectors, 100k QPM, 1 collection |
| Developer | ~$19/mo | ~1M vectors, shared compute |
| Startup | ~$99/mo | ~20M vectors, backups |
| Growth | ~$499/mo | ~100M vectors, dedicated cache / higher SLA |
| Enterprise | Custom | VPC, SSO, audit, dedicated |

Unit economics rely on S3 storage (cheap) + shared/spot compute + cold
collections that cost nearly nothing when idle. Target gross margin: high
if multi-tenancy holds.

## Cloud architecture (SaaS)

Do not fight ephemeral compute: **S3 is source of truth; workers are
replaceable**.

```
Clients → API gateway → coordinator
              ↓
     query / write / compact workers (ephemeral)
              ↓
     immutable segments + manifests on S3
```

- **Writes:** WAL → memtable → immutable segment upload → atomic manifest swap
- **Queries:** warm cache of hot segments; two-level ANN (collection routing →
  per-segment HNSW) so cold data stays on S3
- **Multi-tenancy:** shared worker pools with per-tenant quotas (not one pod
  per tenant)
- **Control-plane metadata** (tenants, keys, billing, manifest pointers):
  strongly consistent store (e.g. Postgres/DynamoDB) — not S3

Main risks: S3 latency (mitigate with segment cache), cold starts (warm
pools), noisy neighbors (scheduler budgets). Compete on cost/simplicity at
good-enough recall, not max recall.

## Delivery phases

Aligns with engineering milestones in the development plan.

| Phase | Goal | Engineering |
| --- | --- | --- |
| 0 — Validate | Landing page, waitlist, ~20 customer interviews | Little/no product code |
| 1 — Core engine | Excellent single-node Aster + HTTP collection API | M1–M4 (partial today) |
| 2 — Distributed cloud | S3 backend, workers, tenancy, compaction | M7–M8 |
| 3 — SaaS MVP | Signup, keys, billing, usage, docs; private beta | Control plane on M8 |
| 4 — Production | Backups/DR, enterprise, hybrid search, SLOs | M9+ |

**Status (2026-08):** Phase 1 kernel exists — multi-collection HTTP API,
usage meters, durable LSM + exact search. Still open: segmented HNSW,
compression, Thrift RPC, S3/spot workers, billing.

Do not spend a year on the perfect distributed system before paying users.
Path: ship a sharp single-node product → 20–50 paying users → build S3/spot
architecture around real workloads.

## Strategy

Start as “Firebase/Vercel for AI search” for indie hackers and small AI
SaaS — not enterprise displacement of Pinecone/Milvus.

A small profitable SaaS ($1–5M ARR) is plausible if cost and DX land.
Larger outcomes require becoming the default serverless vector DB for
developers. Distribution and adoption matter more than index novelty.
