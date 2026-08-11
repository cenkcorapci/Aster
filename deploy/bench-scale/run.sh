#!/usr/bin/env bash
# Run elastic scale bench for local and/or minio backends.
# Usage: ./deploy/bench-scale/run.sh [smoke|full] [local|minio|both]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE="${1:-full}"
BACKENDS="${2:-both}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${ROOT}/bench-results/scale-${PROFILE}-${STAMP}"
mkdir -p "$OUT"

run_one() {
  local be="$1"
  local dir="${OUT}/${be}"
  mkdir -p "$dir"
  echo "==> scale-bench profile=${PROFILE} backend=${be}"
  local extra=()
  if [[ "$be" == "minio" ]]; then
    extra+=(--object-dir "${dir}/objects")
  fi
  (
    cd "$ROOT"
    # bash 3.2 + set -u: empty arrays are "unbound" — expand safely.
    bazel run //aster/bench:scale-bench -- \
      --profile "$PROFILE" \
      --backend "$be" \
      --data-dir "${dir}/data" \
      --out-json "${dir}/report.json" \
      ${extra[@]+"${extra[@]}"}
  ) | tee "${dir}/driver.log"
}

case "$BACKENDS" in
  local) run_one local ;;
  minio) run_one minio ;;
  both)
    run_one local
    run_one minio
    ;;
  *)
    echo "usage: $0 {smoke|full} {local|minio|both}" >&2
    exit 2
    ;;
esac

echo "OK results under ${OUT}"
