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
| Resources | memory budget, max QPS, storage quota, isolation |

### Accuracy presets (planned)

| Profile | Intent |
| --- | --- |
| `COST_OPTIMIZED` | Small `M` / `ef` |
| `BALANCED` | Default |
| `HIGH_RECALL` / `MAX_RECALL` | Larger graph / search effort |

### Storage modes (planned)

| Mode | Layout |
| --- | --- |
| HOT | RAM + SSD (+ object store) |
| WARM | SSD + object store |
| COLD | Object store primarily |

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
