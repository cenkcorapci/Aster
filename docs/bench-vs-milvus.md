# Aster vs Milvus (kind + MinIO)

Head-to-head on a laptop kind cluster: Aster multi-shard `serve` + MinIO sync
vs Milvus standalone (rocksmq) on the same external MinIO. Shared deterministic
corpus (seed `42`), cosine / IP on L2-normalized vectors, HNSW `M=16`,
`efConstruction=200`, `ef=128`.

Harness: [`deploy/compare-milvus/`](../deploy/compare-milvus/README.md).

## Targets

```bash
make bench-vs-milvus-smoke             # 2k × 2048, 2 Aster shards
make bench-vs-milvus                   # 100M×2048 target (auto-scaled to RAM)
TARGET_VECTORS=40000 make bench-vs-milvus   # fixed laptop corpus (documented below)
make bench-vs-milvus-clean
```

Default full profile still *targets* 100M×2048; on a 32 GiB host the scaler
shrinks `actual_vectors` and caps Aster pod memory so shards do not OOM next
to Milvus + etcd + MinIO.

## Measured (Apple M4, 32 GiB, 2026-09-03)

Kind cluster `aster-vs-milvus`, image `aster:local`, Milvus `v2.4.15`.

### Smoke — 2 000 × 2048, 2 shards, 10 queries

| Metric | Aster | Milvus |
| --- | ---: | ---: |
| Load throughput | 358 vps | 7 018 vps |
| Search p50 | 17.8 ms | 4.5 ms |
| Search avg | 22.4 ms | 5.3 ms |
| Mean Jaccard@10 | — | 0.91 |
| Mean Milvus recall@10 vs Aster | — | 0.95 |
| Top-1 agreement | — | 0.90 |

Artifacts: `bench-results/vs-milvus-smoke-20260903T095207Z/`.

### Laptop full — 40 000 × 2048, 4 shards, 50 queries

| Metric | Aster | Milvus |
| --- | ---: | ---: |
| Load | 142 s (281 vps) | insert 5.4 s (7 372 vps) |
| Flush + index | 372 s | index 44 s + load 1.5 s |
| Search p50 | 41.5 ms | 4.6 ms |
| Search p95 | 54.8 ms | 5.1 ms |
| Search avg | 43.8 ms | 4.7 ms |
| Mean Jaccard@10 | — | 0.20 |
| Mean Milvus recall@10 vs Aster | — | 0.32 |
| Top-1 agreement | — | 0.34 |

Artifacts: `bench-results/vs-milvus-full-20260903T095601Z/`.

### Results

- **Latency:** Aster search is client-side scatter-gather across shards over
  HTTP; Milvus answers from one standalone process. Expect Milvus to win raw
  query latency on this topology.
- **Agreement:** both sides use HNSW ANN (not Aster exact). At 2k the top-10
  sets mostly overlap; at 40k they diverge more (different graphs / sharding).
  Raise `ef` / align segment layout before treating recall as a regression gate.
- **Load:** Aster upserts one HTTP doc at a time into memtables then builds
  per-segment HNSW on flush; Milvus batch-inserts then builds one collection
  index — insert vps is not an apples-to-apples engine microbench.

## Re-run notes

- Needs Docker, kind, kubectl, helm, Python 3.11–3.13, Bazel.
- Driver pins `setuptools<82` (pymilvus 2.4 still imports `pkg_resources`).
- Corpus size is limited by **pod memory**, not only host RAM — see
  `scale-plan.json` in each result dir.
