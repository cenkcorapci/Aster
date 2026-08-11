# M3-T08 — Pi/edge RSS validation runbook (<128 MB)

## Goal
Verify the **Edge / Raspberry Pi-class** build profile keeps resident memory bounded while handling a **1M-vector working set**.

Acceptance gate:
- `rss_mb < 128.00` (measured by the benchmark via `getrusage(RUSAGE_SELF).ru_maxrss`)
- `vectors_per_node == 1,000,000`
- `dimension == 16`

## Why this harness
This repo already ships a bench runner that measures RSS and emits JSON:
- Bazel target: `//aster/bench:aster-bench`
- It prints JSON lines for `phase=start|progress|final`, including:
  - `rss_mb`
  - `vectors_per_node`
  - `dimension`
  - `approx_rows` (useful sanity check: “how many rows were actually created/visible at the end”)

## Prerequisites
- Bazel (for `bazel run`)
- `python3` (for parsing the JSON output log)
- Disk space for a durable `--data-dir` directory (SSD preferred on the real Pi)

## One-command run (Edge profile on a Linux host)

1. Run and capture logs:
```bash
WORK=/tmp/aster-m3-t08-rss
rm -rf "$WORK"

bazel run -c opt --config=edge //aster/bench:aster-bench -- \
  --data-dir "$WORK" \
  --vectors 1000000 \
  --dimension 16 \
  --duration 90 \
  --report-every 15 \
  --top-k 10 \
  --flush-every 500 \
  --accuracy-probes 64 \
  2>&1 | tee "$WORK/driver.log"
```

2. Extract and enforce pass/fail:
```bash
python3 - <<'PY'
import json, pathlib, re
log = pathlib.Path("/tmp/aster-m3-t08-rss/driver.log").read_text(errors="ignore").splitlines()
final = None
for line in log:
    if '"phase":"final"' in line:
        # Bench prints a single JSON object per line; grab the first '{...}'.
        m = re.search(r'(\{.*\})', line)
        final = json.loads(m.group(1) if m else line)
        break
if not final:
    raise SystemExit("No phase=final JSON line found")

rss_mb = float(final["rss_mb"])
vecs = int(final["vectors_per_node"])
dim = int(final["dimension"])

print(f"rss_mb={rss_mb:.2f}MB vectors_per_node={vecs} dimension={dim}")

fail = rss_mb >= 128.00 or vecs != 1_000_000 or dim != 16
raise SystemExit(1 if fail else 0)
PY
```

### Notes on `approx_rows`
The benchmark’s `--vectors` is the configured working-set size (`vectors_per_node`), but the actual number of rows created during a fixed `--duration` is reported as `approx_rows` in the final JSON.

For the Pi/edge runbook below, treat `approx_rows` as a **sanity check** that you exercised “enough of the 1M set” for a meaningful RSS measurement.

## Pi / edge execution guidance (manual)

On the real target (or Edge CI), use the same command and configuration, but extend runtime until `approx_rows` is high enough.

1. Run the same benchmark command (adjust only `--duration`):
- start with `--duration 180`
- if the final JSON shows `approx_rows` far below `1_000_000`, increase `--duration` and re-run

2. Use the same parser / pass-fail snippet.

### Recommended stop condition
Aim for:
- `rss_mb < 128.00`
- `approx_rows >= 900000` (stronger confidence you approached the 1M working set on this hardware)

If RSS passes but `approx_rows` is low, the RSS bound may still be real, but the run is weaker as “1M-vector validation.”

## Concrete result (documented)
Edge-profile run on this host (Mac ARM, `--config=edge`):
- vectors_per_node: `1,000,000`
- dimension: `16`
- duration: `90s`
- final `rss_mb`: `107.22 MB`
- final `approx_rows`: `54,065`

This is the measurement used to mark `M3-T08` as documented; on Pi you should re-run and ensure `approx_rows` approaches the 1M set while keeping `rss_mb` under 128 MB.

