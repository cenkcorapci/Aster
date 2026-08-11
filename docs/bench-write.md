# Laptop write microbench (M1-T14)

Measures durable `Db::Upsert` throughput with periodic `Flush()` and
size-tiered compaction enabled — the M1 exit criterion of
**≥100k upserts/sec on a laptop**.

## Harness

```bash
# Primary config (defaults): dim=8, 300k rows, Flush every 8k, wal_sync=never
bazel run -c opt //aster/qa:write_bench

# JSON line (CI / scripts)
bazel run -c opt //aster/qa:write_bench -- --json

# Higher-dimension check
bazel run -c opt //aster/qa:write_bench -- --dim 32 --flush-every 10000 --json
```

Smoke (always in `bazel test //aster/...`):

```bash
bazel test //aster/qa:write_bench_test
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--rows N` | 300000 | Timed upserts |
| `--dim D` | 8 | Vector dimension |
| `--flush-every N` | 8000 | Explicit `Flush()` cadence (0 = size trigger only) |
| `--wal-sync` | `never` | `always` \| `everyms` \| `never` |
| `--warmup N` | 2000 | Untimed upserts before the clock |
| `--json` | off | Single JSON object on stdout |

Compaction: `compaction_tier_threshold=4`, `max_segments_before_compact=8`
(engine defaults). Timed window includes flushes and any auto-compaction they
trigger; a final `Flush()` seals the last memtable.

## Measured (Apple M4)

| Host | Apple M4, macOS 26.5.2 |
| --- | --- |
| Date | 2026-08-11 |
| Build | `bazel run -c opt //aster/qa:write_bench` |

### Primary (meets ≥100k)

```text
--rows 300000 --dim 8 --wal-sync never --flush-every 8000
```

| Run | upserts/sec | segments (end) |
| --- | ---: | ---: |
| 1 | 259480 | 5 |
| 2 | 238803 | 5 |
| 3 | 230937 | 5 |
| 4 | 241235 | 5 |
| 5 | 231036 | 5 |
| **median** | **~238800** | |

**Headline: ~239k upserts/sec** (median of 5), well above the 100k target.

### dim=32 (also ≥100k)

```text
--rows 300000 --dim 32 --wal-sync never --flush-every 10000
```

| Run | upserts/sec |
| --- | ---: |
| 1 | 128374 |
| 2 | 136148 |
| 3 | 138497 |
| **median** | **~136k** |

### Notes

- `wal_sync=never` matches the embedded/throughput profile (OS page cache).
  With `wal_sync=always` (fsync per Upsert) the same machine measured
  ~32k upserts/sec at dim=8 — durability cost, not an engine correctness gap.
- Variance comes mainly from when size-tiered compaction runs inside the
  timed window; medians above still clear 100k with flush+compaction on.
- Kill-9 recovery fuzz (separate target) uses `wal_sync=always` so acked
  writes are fsync-durable across `SIGKILL`.

## Kill -9 fuzz

```bash
# CI default (~15s)
bazel test //aster/qa:kill9_fuzz_test

# 1h soak (M1 exit criterion)
ASTER_FUZZ_SECONDS=3600 bazel test //aster/qa:kill9_fuzz_test --test_timeout=4000
```

Optional: `ASTER_FUZZ_ROUNDS=N` caps crash rounds early.
