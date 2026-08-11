# Aster client API (target)

Collection-centric public API for the **server / SaaS** product. The wire
surface that clients implement is [`aster/rpc/aster.thrift`](../aster/rpc/aster.thrift).
This document describes the longer-term collection configuration model
(accuracy, storage tiers, consistency, quotas).

**Status:** design target for the **server / SaaS** wire API. Today you embed
[`aster::Db`](tutorials/database-management.md) (stable in-process contract in
`aster/db/db.h`; see [versioning.md](versioning.md) § Embedded C++ API) or call
the language facades (stubs until M5) — see
[client libraries tutorial](tutorials/client-libraries.md).

## Hierarchy

```
Project
 └── Collection          ← main optimization / tenancy boundary
      ├── documents
      └── vector search
```

A collection owns dimension, metric, index settings, storage mode,
consistency defaults, and resource limits.

## Wire operations (Thrift)

| RPC | Purpose |
| --- | --- |
| `createCollection` / `dropCollection` | Lifecycle |
| `upsert` / `get` / `remove` | Point mutations / reads |
| `search` | Top-k ANN (+ tag AND filter, `efSearch`, consistency) |

Document fields: `id`, float32 vector (LE bytes), optional CBOR metadata,
optional tags, optional client timestamp.

## Collection config (product model)

Logical groups (not all on the wire yet):

| Group | Examples |
| --- | --- |
| Vector | `dimension`, `metric` (L2 / DOT / COSINE), encoding |
| Index | HNSW `m`, `ef_construction`, `ef_search_default`; accuracy presets |
| Storage | backend (local / S3), compression, HOT / WARM / COLD cache mode |
| Consistency | `replication_factor`, write/read `ONE` \| `QUORUM` \| `ALL` |
| Resources | memory budget, max vectors, max QPS, storage quota, isolation |

### Resource limits + isolation (M8-T05)

Configured via Thrift `CollectionConfig.resourceLimits` / `Db::Options`
(`aster/db/resource_limits.h`). Numeric `0` means unlimited. Exceeding a
cap returns `StatusCode::kResourceExhausted` (RPC: `AsterError`).

| Field | Enforced on | Semantics |
| --- | --- | --- |
| `maxVectors` | Upsert (new live id) | Live row count hard cap |
| `memoryBudgetBytes` | Upsert / Delete | Write-path memtable + arena (existing budget) |
| `maxQps` | Search | Rolling 1-second admission window |
| `storageQuotaBytes` | Upsert (new live id) | Estimated vector bytes (`live × dim × 4`) |
| `isolation` | configure (persisted) | `SHARED` (default) or `DEDICATED` scheduling hint |

Safe online: `Db::SetResourceLimits` / reconfigure via collection config.

### Accuracy presets (planned)

| Profile | Intent |
| --- | --- |
| `COST_OPTIMIZED` | Small `M` / `ef` |
| `BALANCED` | Default |
| `HIGH_RECALL` / `MAX_RECALL` | Larger graph / search effort |

### Storage modes

| Mode | Layout |
| --- | --- |
| HOT | RAM + SSD (default local path; no object-store I/O) |
| WARM | SSD + object store — searchable index stays local; segment/index objects are mirrored to S3 |
| COLD | Object store primarily — same mirroring as WARM, plus non-evictable HNSW upper-layer pins; each search clears the S3 block cache (cold-worker model) while pins survive |

Mode switch is safe online (`Db::SetStorageMode` / collection config `storageMode`).
WARM and COLD require a configured object store on the server.

### Mutability

Safe online: `ef_search_default`, cache size, limits, replication factor.  
Requires rebuild: dimension, metric, HNSW `M`, quantization.

## Client facade contract

Every language client:

1. Transport — framed TCP, optional TLS, pooling, retries, seed failover
2. Generated Thrift types
3. Idiomatic facade — `connect` → `collection` → upsert/get/delete/search

Vectors use native types (`list[float]`, `[]float32`, `Float32Array`, …).
Errors carry `AsterError.code`.

Versioning: one `vX.Y.Z` across server and all clients —
[versioning.md](versioning.md).

## Wire compatibility (Thrift IDL freeze, M5-T01)

`aster/rpc/aster.thrift` defines the binary wire protocol. After M5-T01, we
consider the IDL *frozen* under the following rules:

### Change classes

Wire-compatible (non-breaking) changes (clients built against an older
version remain compatible):

1. Add new `optional` fields with new field IDs.
2. Add new enum values.
3. Add new RPC methods (older clients simply never call them).

Wire-breaking changes (require a new product `MAJOR` and client refresh):

1. Change the type of an existing field.
2. Change requiredness/semantics of an existing field (including defaults).
3. Remove an existing field or change its meaning.
4. Reuse an existing field ID for a different meaning.

### Deprecation window

Fields are deprecated in-place (documented in IDL comments) and are kept
until the next `MAJOR` bump. If a field must be retired sooner for
operational reasons, treat it as breaking and bump `MAJOR` immediately.

Field IDs are never reused.

### IDL major version

There is no runtime negotiation in the RPC surface yet. Clients must compile
against a matching IDL MAJOR:

- `aster/rpc/aster.thrift` exports `ASTER_IDL_MAJOR`.
- `ASTER_IDL_MAJOR` must match the product `MAJOR` from the repo root
  `VERSION` (`X` in `vX.Y.Z`).

During rolling upgrades within the same `MAJOR`, older clients remain
compatible with newer servers because unknown/new OPTIONAL fields are
ignored by Thrift decoders.
