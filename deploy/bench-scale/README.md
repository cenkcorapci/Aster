# Elastic scale bench (15 ↔ 50 nodes)

Continuous **sawtooth** membership changes between `--min-nodes` and
`--max-nodes` (default **15..50**) with **RF=2** replication.

## Guarantees under test

| Property | How |
| --- | --- |
| No data loss | Every acked key must remain `Get`-able on ≥1 live replica after every scale event |
| Accuracy unchanged | Scatter-gather top-k must match offline exact ground truth after every event |
| Safe scale-down | `PlanRemoveNode` refuses RF=2 remove when survivors do not cover; drain+migrate first |
| Durable backends | `local` (POSIX dirs) and `minio` (object-store stand-in mirror) |

Engine pieces: `aster/distributed/rebalance.*`, `POST /v1/admin/drain` for pod preStop.

## Make targets

```bash
make bench-scale-smoke          # 3↔8 quick
make bench-scale                # 15↔50 local + minio backends
make bench-scale-test           # rebalance unit tests
```

## Manual

```bash
bazel run //aster/bench:scale-bench -- --profile full --backend local \
  --data-dir /tmp/aster-scale-local --out-json /tmp/local.json

bazel run //aster/bench:scale-bench -- --profile full --backend minio \
  --data-dir /tmp/aster-scale-node --object-dir /tmp/aster-scale-objects \
  --out-json /tmp/minio.json
```
