# Multi-tenant Catalog bench

Exercises **many tenants × many indexes** with mixed **vector dimensions** and
**row counts** on the multi-collection Catalog (SaaS kernel). Separate from
`deploy/bench` (single-collection soak) and `deploy/compare-milvus`.

## Make targets

```bash
make bench-multitenant-test    # bazel gtest smoke
make bench-multitenant-smoke   # 3 tenants × 4 index sizes
make bench-multitenant         # 8 tenants × 9 dim/row combos (default)
make bench-multitenant-large   # 12 tenants × larger corpora
```

## Profiles

| Profile | Tenants | Indexes / tenant (dim × rows) |
| --- | --- | --- |
| `smoke` | 3 | 64×64, 256×128, 512×48, 2048×32 |
| `default` | 8 | 64×5k … 4096×128 (9 sizes) |
| `large` | 12 | 64×50k … 4096×500 |

Each index is a Catalog collection named `t{N}_d{dim}_n{rows}`. Workers run
tenants concurrently. Metrics: upsert vps, flush/compact ms, search p50/p95/p99,
planted-target recall, RSS, usage counters.

## Outputs

`bench-results/multitenant-<profile>-<stamp>/report.json` plus CLI table.

## Manual

```bash
bazel run //aster/bench:multi-tenant-bench -- \
  --data-dir /tmp/aster-mt --profile smoke --out-json /tmp/mt.json
```
