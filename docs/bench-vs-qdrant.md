# Aster vs Qdrant (kind + MinIO)

The Qdrant comparison reuses the Aster benchmark corpus and query workload so
that both engines receive the same deterministic, L2-normalized vectors and
queries. Aster runs as multiple shards with MinIO synchronization; Qdrant runs
as one container in the same kind cluster. Both use cosine HNSW with `M=16`,
`efConstruction=200`, and `ef=128`.

## Run

```bash
make bench-vs-qdrant-smoke                    # 2k × 2048, 2 Aster shards
TARGET_VECTORS=40000 make bench-vs-qdrant     # fixed laptop corpus
make bench-vs-qdrant                          # 100M target, auto-scaled
make bench-vs-qdrant-clean
```

The full profile targets 100M × 2048 but automatically reduces the actual
corpus and Aster shard count to fit host and pod memory. Set
`FORCE_NODES=1 COMPARE_NODES=<count>` to bypass the shard cap.

## Methodology

1. Generate a deterministic corpus with seed `42`; Aster and Qdrant use the
   same IDs and vectors.
2. Upsert into Aster by shard and bulk-upload the same vectors to Qdrant.
3. Flush Aster, then query both engines with the same vectors.
4. Report load throughput, query latency, Jaccard@k, Qdrant recall@k relative
   to Aster, top-1 agreement, score MAE, and rank differences.

Results are written to `bench-results/vs-qdrant-<profile>-<stamp>/`.
`report.json` retains the generic `competitor_*` fields; the legacy
`milvus_*` fields remain in Milvus reports for compatibility.

## Requirements

The harness needs Docker, kind, kubectl, Python 3.11–3.13, and Bazel.
Qdrant is deployed from the pinned `qdrant/qdrant:v1.12.6` image. Unlike the
Milvus benchmark, this target does not require Helm.
