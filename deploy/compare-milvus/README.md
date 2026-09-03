# Aster vs Milvus comparison (100M × 2048, MinIO)

Realistic head-to-head for **speed** and **result agreement** under a shared
workload. Both systems use **MinIO** as object storage on kind. Aster runs
**multi-shard**; Milvus runs **standalone** (rocksmq) so Helm’s default Pulsar
stack does not OOM the laptop.

Measured laptop numbers: [docs/bench-vs-milvus.md](../../docs/bench-vs-milvus.md).

## What is compared

| Aspect | Aster | Milvus |
| --- | --- | --- |
| Topology | N StatefulSet shards + MinIO sync | Helm standalone + external MinIO |
| Search | Per-segment HNSW, client scatter-gather | HNSW ANN (one collection) |
| Metric | Cosine | Cosine (`IP` on L2-normalized vectors) |
| Corpus | Deterministic seed `42`, shared IDs `doc-{i}` | Same |

**Target profile:** `100_000_000` vectors × dim `2048`. Local machines auto-scale
`actual_vectors` (and Aster pod memory) to fit RAM; reports always include
`target` vs `actual`.

## Make targets

```bash
make bench-vs-milvus-smoke                    # ~2k vectors, 2 shards
TARGET_VECTORS=40000 make bench-vs-milvus     # laptop full (documented)
make bench-vs-milvus                          # 100M target, auto-scaled
make bench-vs-milvus-clean                    # delete kind cluster aster-vs-milvus
```

**Note:** `make` exports `NODES=50` for other benches; this suite **ignores**
`NODES` and uses `COMPARE_NODES` (default 4 full / 2 smoke) with a hard kind
cap of 4. Dedicated cluster: `aster-vs-milvus`.

```bash
# Cap is automatic on laptops (≤4 shards). To force more:
make bench-vs-milvus FORCE_NODES=1 COMPARE_NODES=8

make bench-vs-milvus COMPARE_NODES=4 QUERIES=20 TARGET_VECTORS=100000000
```

## Measured (Apple M4, 32 GiB, 2026-09-03)

| Profile | Vectors | Shards | Aster p50 | Milvus p50 | Jaccard@10 | Recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Smoke | 2 000 | 2 | 17.8 ms | 4.5 ms | 0.91 | 0.95 |
| Laptop full | 40 000 | 4 | 41.5 ms | 4.6 ms | 0.20 | 0.32 |

Milvus wins query latency on this topology (single process vs HTTP
scatter-gather). Agreement is high at 2k and lower at 40k — both sides are
ANN; treat recall as comparative, not a hard gate. Details and load timings:
[docs/bench-vs-milvus.md](../../docs/bench-vs-milvus.md).

## Methodology (accuracy)

1. Load the same vectors into Aster shards (`id % N → shard`) and one Milvus collection.
2. Run the same query vectors.
3. Aster: query every shard, merge global top-k (HNSW per shard).
4. Milvus: native search (HNSW).
5. Compare hit sets: Milvus recall@k vs Aster, Jaccard@k, rank disagreement,
   and score MAE on overlapping IDs.

## Outputs

`bench-results/vs-milvus-<stamp>/`

- `report.json` — full metrics
- `report.txt` — human CLI summary
- `aster/`, `milvus/` — raw load/search timings
- `scale.json` — host RAM scaling decision

## Dependencies

`docker`, `kind`, `kubectl`, `python3` (3.11–3.13), `bazel` (for Aster image).

`helm` is required for the Milvus cluster chart. If it is missing, `run.sh`
installs it via Homebrew (`brew install helm`) or the official get-helm-3
script.

Driver requirements pin `setuptools<82` because pymilvus 2.4 still imports
`pkg_resources`.
