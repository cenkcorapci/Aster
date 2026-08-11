# Aster vs Milvus comparison (100M × 2048, MinIO)

Realistic head-to-head for **speed** and **result agreement** under a shared
workload. Both systems use **MinIO** as object storage on kind. Aster runs
**multi-shard**; Milvus runs **standalone** (rocksmq) so Helm’s default Pulsar
stack does not OOM the laptop.

## What is compared

| Aspect | Aster | Milvus |
| --- | --- | --- |
| Topology | N StatefulSet shards + MinIO sync | Helm standalone + external MinIO |
| Search | Exact (client scatter-gather merge) | ANN (HNSW); Aster exact used as ground truth |
| Metric | Cosine | Cosine (`IP` on L2-normalized vectors) |
| Corpus | Deterministic seed `42`, shared IDs `doc-{i}` | Same |

**Target profile:** `100_000_000` vectors × dim `2048`. Local machines auto-scale
`actual_vectors` to fit RAM; reports always include `target` vs `actual`.

## Make targets

```bash
make bench-vs-milvus              # full compare (auto-scaled shards + corpus)
make bench-vs-milvus-smoke        # tiny smoke (~2k vectors, 2 shards)
make bench-vs-milvus-clean        # delete kind cluster aster-vs-milvus
```

**Note:** `make` exports `NODES=50` for other benches; this suite **ignores**
`NODES` and uses `COMPARE_NODES` (default 4 full / 2 smoke) with a hard kind
cap of 4. Dedicated cluster: `aster-vs-milvus`.

```bash
# Cap is automatic on laptops (≤4 shards). To force more:
make bench-vs-milvus FORCE_NODES=1 COMPARE_NODES=8

make bench-vs-milvus COMPARE_NODES=4 QUERIES=20 TARGET_VECTORS=100000000
```

## Methodology (accuracy)

1. Load the same vectors into Aster shards (`id % N → shard`) and one Milvus collection.
2. Run the same query vectors.
3. Aster: query every shard, merge global top-k (exact).
4. Milvus: native search (HNSW).
5. Treat Aster hits as ground truth; compute Milvus recall@k, Jaccard@k,
   rank disagreement, and score MAE on overlapping IDs.

## Outputs

`bench-results/vs-milvus-<stamp>/`

- `report.json` — full metrics
- `report.txt` — human CLI summary
- `aster/`, `milvus/` — raw load/search timings
- `scale.json` — host RAM scaling decision

## Dependencies

`docker`, `kind`, `kubectl`, `python3`, `bazel` (for Aster image).

`helm` is required for the Milvus cluster chart. If it is missing, `run.sh`
installs it via Homebrew (`brew install helm`) or the official get-helm-3
script.
