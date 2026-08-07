#!/usr/bin/env bash
# Orchestrate a local-kind Aster bench: 50 BusyBox nodes, mixed write/update/search.
#
# Usage:
#   ./deploy/bench/run.sh local
#   ./deploy/bench/run.sh minio
#
# Environment overrides:
#   NODES=50 TARGET_VECTORS=100000000 DURATION=180
#   DIMENSIONS="256 2048 4096"   # default matrix
#   DIMENSION=512                # single-dim override
#   IMAGE=aster:local CLUSTER=aster-bench
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:-}"
if [[ "$MODE" != "local" && "$MODE" != "minio" ]]; then
  echo "usage: $0 {local|minio}" >&2
  exit 2
fi

NODES="${NODES:-50}"
TARGET_VECTORS="${TARGET_VECTORS:-100000000}"
DURATION="${DURATION:-180}"
IMAGE="${IMAGE:-aster:local}"
CLUSTER="${CLUSTER:-aster-bench}"
NS=aster-bench

# Dimension matrix: DIMENSION= wins; else DIMENSIONS; else 256 2048 4096.
if [[ -n "${DIMENSION:-}" ]]; then
  DIM_LIST=("$DIMENSION")
else
  # shellcheck disable=SC2206
  DIM_LIST=(${DIMENSIONS:-256 2048 4096})
fi

# Colors
if [[ -t 1 ]] || [[ "${FORCE_COLOR:-}" == "1" ]]; then
  C_RESET=$'\033[0m'
  C_BOLD=$'\033[1m'
  C_DIM=$'\033[2m'
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_BLUE=$'\033[34m'
  C_MAGENTA=$'\033[35m'
  C_CYAN=$'\033[36m'
else
  C_RESET= C_BOLD= C_DIM= C_RED= C_GREEN= C_YELLOW= C_BLUE= C_MAGENTA= C_CYAN=
fi

log()  { printf '%s%s%s %s\n' "$C_CYAN" "==>" "$C_RESET" "$*"; }
ok()   { printf '%s%s%s %s\n' "$C_GREEN" "OK " "$C_RESET" "$*"; }
warn() { printf '%s%s%s %s\n' "$C_YELLOW" "!! " "$C_RESET" "$*"; }
err()  { printf '%s%s%s %s\n' "$C_RED" "ERR" "$C_RESET" "$*" >&2; }

need() {
  command -v "$1" >/dev/null 2>&1 || { err "missing dependency: $1"; exit 1; }
}

need docker
need kubectl
need kind
need bazel
need python3

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
BASE_OUT="${ROOT}/bench-results/${MODE}-${STAMP}"
mkdir -p "$BASE_OUT"
export FORCE_COLOR="${FORCE_COLOR:-1}"

HOST_MEM_BYTES="$(sysctl -n hw.memsize 2>/dev/null || awk '/MemTotal/ {print $2*1024}' /proc/meminfo)"
HOST_CPUS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
HOST_MEM_GB="$(python3 - <<PY
print(round(${HOST_MEM_BYTES}/1024/1024/1024, 1))
PY
)"
USABLE_GB="$(python3 - <<PY
mem=${HOST_MEM_GB}
reserve = 6.0 if "${MODE}" == "minio" else 4.0
print(max(2.0, mem - reserve))
PY
)"

log "Host: ${HOST_CPUS} CPUs, ${HOST_MEM_GB} GiB RAM"
log "Mode: ${MODE}  dimension matrix: ${DIM_LIST[*]}  duration=${DURATION}s"
log "Results root: ${BASE_OUT}"

# ---- Image + kind (once) ---------------------------------------------------
log "Building BusyBox Aster image (${IMAGE})"
(cd "$ROOT" && ./scripts/docker-build.sh) | tee "${BASE_OUT}/docker-build.log" | tail -20

if ! kind get clusters 2>/dev/null | grep -qx "$CLUSTER"; then
  log "Creating kind cluster ${CLUSTER}"
  kind create cluster --config "${ROOT}/deploy/bench/kind-config.yaml" --name "$CLUSTER" \
    | tee "${BASE_OUT}/kind-create.log"
else
  log "Reusing kind cluster ${CLUSTER}"
fi
kubectl config use-context "kind-${CLUSTER}" >/dev/null

log "Loading image into kind"
kind load docker-image "$IMAGE" --name "$CLUSTER"

if ! kubectl -n kube-system get deploy metrics-server >/dev/null 2>&1; then
  log "Installing metrics-server (optional)"
  kubectl apply -f https://github.com/kubernetes-sigs/metrics-server/releases/download/v0.7.2/components.yaml \
    >/dev/null 2>&1 || warn "metrics-server install skipped"
  kubectl -n kube-system patch deploy metrics-server --type=json \
    -p='[{"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"--kubelet-insecure-tls"}]' \
    >/dev/null 2>&1 || true
fi

if [[ "$MODE" == "minio" ]]; then
  log "Pulling and loading MinIO images into kind"
  docker pull minio/minio:latest
  docker pull minio/mc:latest
  kind load docker-image minio/minio:latest --name "$CLUSTER"
  kind load docker-image minio/mc:latest --name "$CLUSTER"
fi

REPORTS=()
for DIMENSION in "${DIM_LIST[@]}"; do
  OUT_DIR="${BASE_OUT}/dim-${DIMENSION}"
  mkdir -p "$OUT_DIR"
  log "======== dimension=${DIMENSION} ========"

  SCALE="$(python3 - <<PY
nodes=int("${NODES}")
target=int("${TARGET_VECTORS}")
dim=int("${DIMENSION}")
usable_gb=float("${USABLE_GB}")
cpus=int("${HOST_CPUS}")
max_nodes_by_cpu = max(4, cpus * 6)
max_nodes_by_ram = max(4, int((usable_gb * 1024) / 400))
nodes = min(nodes, max_nodes_by_cpu, max_nodes_by_ram)
bytes_budget = usable_gb * 0.40 * (1024**3)
per_vec = dim * 4 * 2.5
max_total = int(bytes_budget / per_vec)
actual_total = min(target, max_total)
per_node = max(1000, actual_total // nodes)
actual_total = per_node * nodes
scale_factor = actual_total / float(target)
print(f"{nodes} {per_node} {actual_total} {scale_factor:.6f}")
PY
)"
  read -r RUN_NODES VECTORS_PER_NODE ACTUAL_TOTAL SCALE_FACTOR <<<"$SCALE"

  log "nodes=${RUN_NODES}  vectors/node=${VECTORS_PER_NODE}  total=${ACTUAL_TOTAL}  scale=${SCALE_FACTOR}"

  cat >"${OUT_DIR}/config.json" <<EOF
{
  "mode": "${MODE}",
  "stamp": "${STAMP}",
  "host_cpus": ${HOST_CPUS},
  "host_mem_gb": ${HOST_MEM_GB},
  "nodes": ${RUN_NODES},
  "target_vectors": ${TARGET_VECTORS},
  "actual_vectors": ${ACTUAL_TOTAL},
  "vectors_per_node": ${VECTORS_PER_NODE},
  "dimension": ${DIMENSION},
  "duration_sec": ${DURATION},
  "scale_factor": ${SCALE_FACTOR},
  "image": "${IMAGE}",
  "cluster": "${CLUSTER}"
}
EOF

  log "Reset namespace ${NS}"
  kubectl delete namespace "$NS" --ignore-not-found --wait=true 2>/dev/null || true
  for _ in $(seq 1 60); do
    kubectl get ns "$NS" >/dev/null 2>&1 || break
    sleep 2
  done
  kubectl apply -f "${ROOT}/deploy/bench/namespace.yaml"

  if [[ "$MODE" == "minio" ]]; then
    log "Deploying MinIO"
    kubectl apply -f "${ROOT}/deploy/bench/minio.yaml"
    kubectl -n "$NS" rollout status deploy/minio --timeout=180s
    kubectl -n "$NS" wait --for=condition=complete job/minio-init --timeout=180s
    TEMPLATE="${ROOT}/deploy/bench/aster-minio.yaml"
  else
    TEMPLATE="${ROOT}/deploy/bench/aster-local.yaml"
  fi

  RENDERED="${OUT_DIR}/aster-rendered.yaml"
  sed -e "s/__REPLICAS__/${RUN_NODES}/g" \
      -e "s/__VECTORS__/${VECTORS_PER_NODE}/g" \
      -e "s/__DIMENSION__/${DIMENSION}/g" \
      -e "s/__DURATION__/${DURATION}/g" \
      -e "s|__IMAGE__|${IMAGE}|g" \
      "$TEMPLATE" >"$RENDERED"

  log "Deploying ${RUN_NODES} Aster bench pods (dim=${DIMENSION})"
  kubectl apply -f "$RENDERED"
  kubectl -n "$NS" rollout status statefulset/aster-node --timeout=300s || true

  DEADLINE=$((DURATION + 240))
  log "Soaking ${DURATION}s (budget ${DEADLINE}s)"
  END_AT=$((SECONDS + DEADLINE))
  FINISHED=0
  while (( SECONDS < END_AT )); do
    RUNNING=$(kubectl -n "$NS" get pods -l app=aster-bench --no-headers 2>/dev/null | grep -c Running || true)
    DONE_COUNT=0
    for p in $(kubectl -n "$NS" get pods -l app=aster-bench -o jsonpath='{.items[*].metadata.name}' 2>/dev/null); do
      if kubectl -n "$NS" logs "$p" -c aster 2>/dev/null | grep -q '"phase":"final"'; then
        DONE_COUNT=$((DONE_COUNT + 1))
      fi
    done
    printf '%s[%s]%s dim=%s running=%s finished=%s / %s\n' \
      "$C_DIM" "$(date +%H:%M:%S)" "$C_RESET" "$DIMENSION" "$RUNNING" "$DONE_COUNT" "$RUN_NODES"
    if [[ "$DONE_COUNT" -ge "$RUN_NODES" ]]; then
      ok "dim=${DIMENSION}: all nodes finished"
      FINISHED=1
      break
    fi
    sleep 15
  done
  if [[ "$FINISHED" -ne 1 ]]; then
    warn "dim=${DIMENSION}: timed out; collecting partial results"
  fi

  kubectl -n "$NS" get pods -o wide >"${OUT_DIR}/pods.txt" || true
  mkdir -p "${OUT_DIR}/logs"
  for p in $(kubectl -n "$NS" get pods -l app=aster-bench -o jsonpath='{.items[*].metadata.name}'); do
    kubectl -n "$NS" logs "$p" -c aster >"${OUT_DIR}/logs/${p}.log" 2>/dev/null || true
  done

  FORCE_COLOR=1 python3 "${ROOT}/deploy/bench/report.py" \
    --out-dir "$OUT_DIR" \
    --mode "$MODE" \
    --config "${OUT_DIR}/config.json"
  REPORTS+=("${OUT_DIR}/REPORT.md")
done

ok "Bench matrix complete (${#DIM_LIST[@]} dimensions)"
printf '%s\n' "${REPORTS[@]}"
