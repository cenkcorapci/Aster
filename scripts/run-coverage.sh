#!/usr/bin/env bash
# Run instrumented tests and print line coverage for //aster (excl. cli).
#
# Usage:
#   ./scripts/run-coverage.sh
#
# Target: >= 90% line coverage on aster library code.
#
# On macOS, Bazel C++ coverage needs Apple llvm-profdata / llvm-cov wired via
# --test_env / --repo_env (see bazelbuild/bazel#14970).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

find_tool() {
  local name="$1"
  if command -v xcrun >/dev/null 2>&1; then
    xcrun --find "$name" 2>/dev/null && return 0
  fi
  command -v "$name"
}

LLVM_COV="$(find_tool llvm-cov)"
LLVM_PROFDATA="$(find_tool llvm-profdata)"

if [[ -z "${LLVM_COV}" || -z "${LLVM_PROFDATA}" ]]; then
  echo "error: llvm-cov and llvm-profdata are required for coverage" >&2
  exit 1
fi

echo "==> llvm-cov:      ${LLVM_COV}"
echo "==> llvm-profdata: ${LLVM_PROFDATA}"
echo "==> bazel coverage //aster/..."

bazel coverage //aster/... \
  --combined_report=lcov \
  --instrumentation_filter='^//aster' \
  --instrument_test_targets=false \
  --experimental_generate_llvm_lcov \
  --copt=-ffile-compilation-dir=. \
  --repo_env="GCOV=${LLVM_PROFDATA}" \
  --repo_env="BAZEL_LLVM_COV=${LLVM_COV}" \
  --repo_env="BAZEL_LLVM_PROFDATA=${LLVM_PROFDATA}" \
  --test_env="GENERATE_LLVM_LCOV=1" \
  --test_env="BAZEL_USE_LLVM_NATIVE_COVERAGE=1" \
  --test_env="COVERAGE_GCOV_PATH=${LLVM_PROFDATA}" \
  --test_env="LLVM_COV=${LLVM_COV}" \
  --test_env="LLVM_PROFDATA=${LLVM_PROFDATA}" \
  --test_output=errors

REPORT="$(bazel info output_path)/_coverage/_coverage_report.dat"
if [[ ! -f "$REPORT" ]]; then
  echo "Coverage report not found at $REPORT" >&2
  exit 1
fi

echo "==> Coverage report: $REPORT"
python3 - <<'PY' "$REPORT"
import sys
from collections import defaultdict

path = sys.argv[1]
# SF:<file>  DA:<line>,<hits>  end_of_record
lines_hit = defaultdict(lambda: [0, 0])  # file -> [hit, total]

with open(path) as f:
    cur = None
    for line in f:
        line = line.strip()
        if line.startswith("SF:"):
            cur = line[3:]
            # normalize to repo-relative aster/...
            if "/aster/" in cur:
                cur = "aster/" + cur.split("/aster/", 1)[1]
            elif cur.startswith("aster/"):
                pass
            else:
                cur = None
        elif cur and line.startswith("DA:"):
            _, rest = line.split(":", 1)
            _ln, hits = rest.split(",")
            lines_hit[cur][1] += 1
            if int(hits) > 0:
                lines_hit[cur][0] += 1
        elif line == "end_of_record":
            cur = None

# Exclude CLI binary and test sources from the product-coverage gate.
skip_prefixes = ("aster/cli/",)
items = []
hit = total = 0
for sf, (h, t) in sorted(lines_hit.items()):
    if any(sf.startswith(p) for p in skip_prefixes):
        continue
    if "/_test" in sf or sf.endswith("_test.cc"):
        continue
    if t == 0:
        continue
    items.append((sf, h, t, (100.0 * h / t) if t else 100.0))
    hit += h
    total += t

print()
print(f"{'File':<42} {'Hit':>6} {'Tot':>6} {'Cov%':>7}")
print("-" * 65)
for sf, h, t, pct in items:
    print(f"{sf:<42} {h:6d} {t:6d} {pct:6.1f}%")
print("-" * 65)
pct = (100.0 * hit / total) if total else 0.0
print(f"{'TOTAL (aster libs, excl. cli)':<42} {hit:6d} {total:6d} {pct:6.1f}%")
print()
if total == 0:
    print("FAIL: no line coverage data collected (macOS llvm env misconfigured?)", file=sys.stderr)
    sys.exit(2)
if pct < 90.0:
    print(f"FAIL: coverage {pct:.1f}% < 90% target", file=sys.stderr)
    sys.exit(2)
print(f"OK: coverage {pct:.1f}% meets >= 90% target")
PY
