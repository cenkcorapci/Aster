# Tutorial: HTTP API (single-node SaaS kernel)

Firebase-style JSON API over multi-collection durable storage. This is the
Phase 1 surface from [business-plan.md](../business-plan.md): local server
today, control plane / S3 later.

Search is **exact** until HNSW (M2). Auth is an optional shared API key.

## Start the server

```bash
bazel run //aster/cli:aster -- serve --data-dir /tmp/aster-data --port 8080
# optional: --api-key secret   or ASTER_API_KEY=secret
```

## Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/health` | Liveness (no auth) |
| `GET` | `/metrics` | Prometheus text |
| `GET` | `/v1/usage` | Upserts / searches / vector estimate |
| `GET` | `/v1/collections` | List collections |
| `PUT` | `/v1/collections/{name}` | Create (`dimension`, `metric`) |
| `GET` | `/v1/collections/{name}` | Describe |
| `DELETE` | `/v1/collections/{name}` | Drop (catalog entry; data dir kept) |
| `PUT` | `/v1/collections/{name}/docs/{id}` | Upsert document |
| `GET` | `/v1/collections/{name}/docs/{id}` | Get document |
| `DELETE` | `/v1/collections/{name}/docs/{id}` | Tombstone delete |
| `POST` | `/v1/collections/{name}/search` | Top-k search |
| `POST` | `/v1/collections/{name}/flush` | Flush memtable |
| `POST` | `/v1/collections/{name}/compact` | Full compact |

Header when `--api-key` is set: `X-Api-Key: secret` or `Authorization: Bearer secret`.

Default bind is `127.0.0.1` (not public). Request bodies are capped (16 MiB);
dimension ≤ 8192; `top_k` ≤ 1000. Document ids cannot contain `/` or `..`.

## Example

```bash
BASE=http://127.0.0.1:8080
KEY=()  # or (-H "X-Api-Key: secret")

curl -s "${KEY[@]}" -X PUT "$BASE/v1/collections/products" \
  -H 'Content-Type: application/json' \
  -d '{"dimension":3,"metric":"cosine"}'

curl -s "${KEY[@]}" -X PUT "$BASE/v1/collections/products/docs/sku-1" \
  -H 'Content-Type: application/json' \
  -d '{"vector":[0.1,0.2,0.3],"tags":["sale"],"timestamp":1}'

curl -s "${KEY[@]}" -X POST "$BASE/v1/collections/products/search" \
  -H 'Content-Type: application/json' \
  -d '{"vector":[0.1,0.2,0.3],"top_k":5,"tags":["sale"]}'

curl -s "${KEY[@]}" "$BASE/v1/usage"
```

## On-disk layout

```
data_dir/
  CATALOG                 # name, dimension, metric
  products/
    MANIFEST
    WAL
    seg_000001.ast
  …
```

## What’s next (business plan)

| Phase | Still open |
| --- | --- |
| 1b | Segmented HNSW, SSTable compression |
| 2 | S3 backend, spot workers |
| 3 | Signup, billing, per-tenant keys, dashboard |

Related: [database management](database-management.md) (embed `Db`),
[client libraries](client-libraries.md) (Thrift facades → M5).
