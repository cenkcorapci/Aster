#!/usr/bin/env bash
# Run the multi-tenant Catalog bench (local process, durable data dir).
#
# Usage:
#   ./deploy/bench-multitenant/run.sh smoke|default|large
#
# Env: TENANTS CONCURRENT QUERIES TOP_K OUT_ROOT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE="${1:-default}"
case "$PROFILE" in
  smoke|default|large) ;;
  *)
    echo "usage: $0 {smoke|default|large}" >&2
    exit 2
    ;;
esac

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need bazel
need python3

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${OUT_ROOT:-${ROOT}/bench-results}/multitenant-${PROFILE}-${STAMP}"
mkdir -p "$OUT"
DATA="${OUT}/data"
mkdir -p "$DATA"

EXTRA=()
if [[ -n "${TENANTS:-}" ]]; then EXTRA+=(--tenants "$TENANTS"); fi
if [[ -n "${CONCURRENT:-}" ]]; then EXTRA+=(--concurrent "$CONCURRENT"); fi
if [[ -n "${QUERIES:-}" ]]; then EXTRA+=(--queries "$QUERIES"); fi
if [[ -n "${TOP_K:-}" ]]; then EXTRA+=(--top-k "$TOP_K"); fi

echo "==> multi-tenant bench profile=${PROFILE}"
echo "    results: ${OUT}"

(
  cd "$ROOT"
  bazel run //aster/bench:multi-tenant-bench -- \
    --data-dir "$DATA" \
    --profile "$PROFILE" \
    --out-json "${OUT}/report.json" \
    "${EXTRA[@]}"
) | tee "${OUT}/driver.log"

# Compact CLI table from JSON if present
if [[ -f "${OUT}/report.json" ]]; then
  python3 - <<PY
import json, pathlib
p = pathlib.Path("${OUT}/report.json")
r = json.loads(p.read_text())
lines = [
  f"profile={r.get('profile')} tenants={r.get('tenants')} "
  f"collections={r.get('total_collections')} rows={r.get('total_rows')} "
  f"target_recall={r.get('target_recall_rate'):.3f} wall_ms={r.get('wall_ms'):.1f} "
  f"rss_kb={r.get('rss_kb')}",
  f"{'tenant':10s} {'dim':>6} {'rows':>8} {'vps':>10} {'p50':>10} {'p95':>10} ok",
]
for t in r.get("tenant_results", []):
  for ix in t.get("indexes", []):
    lines.append(
      f"{t['tenant']:10s} {ix['dimension']:6d} {ix['rows']:8d} "
      f"{ix['upsert_vps']:10.1f} {ix['search_p50_ms']:10.3f} "
      f"{ix['search_p95_ms']:10.3f} {'yes' if ix['ok'] else 'NO'}"
    )
text = "\\n".join(lines) + "\\n"
pathlib.Path("${OUT}/report.txt").write_text(text)
print(text)
PY
fi

echo "OK wrote ${OUT}/report.json"
