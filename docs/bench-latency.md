# Latency bench (M2-T11): 1M×384d search p50 < 5ms

This bench measures **end-to-end** ANN search latency using the regular
`Db::Search()` path (fetch top candidates from the segment HNSW graph,
merge, reconcile, and exact rerank).

## Targets

- Dataset/workload: **1,000,000 base vectors × 384 dimensions**
- Queries: 2,000 queries (200 warmup, 1,800 measured)
- Workload knob: `ef_search=128` (collection HNSW query beam)
- Gate: **p50_ms < 5.0**

## Harness

### Smoke (always safe)

```bash
bazel test //aster/qa:latency_bench_smoke_test
```

### Run the real gate workload (heavy; recommended on performance hardware)

```bash
bazel run -c opt //aster/qa:latency_bench -- \
  --scale=ci \
  --out-json /tmp/aster-latency-ci.json
```

Exit codes:

- `0`: p50_ms is below threshold
- `2`: gate failure (p50_ms >= threshold)

You can override the threshold:

```bash
ASTER_LATENCY_P50_THRESHOLD_MS=6.0 bazel test //aster/qa:latency_bench_gate_test
```

## Output (JSON schema)

The bench writes a single JSON object to `--out-json`:

- `p50_ms`: median search latency across measured queries
- `p95_ms`: 95th percentile latency (useful for tail regressions)
- `avg_ms`: mean latency
- `pass`: boolean for the configured `p50_ms < threshold` gate

## How to interpret p50

`p50_ms` is the median of per-query timings captured by the bench’s timed
loop. If `p50_ms` is close to the 5ms threshold, small CPU frequency / load
changes can flip the pass/fail result; for that reason, CI publishes the
full JSON artifact and PRs are expected to regress only when both median and
tail move.

