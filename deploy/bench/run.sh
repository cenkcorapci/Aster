#!/usr/bin/env bash
# Orchestrate a local-kind Aster bench: 50 BusyBox nodes, mixed write/update/search.
#
# Usage:
#   ./deploy/bench/run.sh local
#   ./deploy/bench/run.sh minio
#
# Environment overrides:
#   NODES=50 TARGET_VECTORS=100000000 DIMENSION=16 DURATION=180
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
DIMENSION="${DIMENSION:-16}"
DURATION="${DURATION:-180}"
IMAGE="${IMAGE:-aster:local}"
CLUSTER="${CLUSTER:-aster-bench}"
NS=aster-bench

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
OUT_DIR="${ROOT}/bench-results/${MODE}-${STAMP}"
mkdir -p "$OUT_DIR"
export FORCE_COLOR="${FORCE_COLOR:-1}"

# ---- Scale to this machine -------------------------------------------------
HOST_MEM_BYTES="$(sysctl -n hw.memsize 2>/dev/null || awk '/MemTotal/ {print $2*1024}' /proc/meminfo)"
HOST_CPUS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
HOST_MEM_GB="$(python3 - <<PY
print(round(${HOST_MEM_BYTES}/1024/1024/1024, 1))
PY
)"

# Leave headroom for kind + OS + MinIO. ~35% of RAM for Aster working sets.
USABLE_GB="$(python3 - <<PY
mem=${HOST_MEM_GB}
reserve = 6.0 if "${MODE}" == "minio" else 4.0
print(max(2.0, mem - reserve))
PY
)"

# Per-node vector budget from RAM: bytes ≈ vectors * dim * 4 * 2.5 (index+overhead)
SCALE="$(python3 - <<PY
nodes=int("${NODES}")
target=int("${TARGET_VECTORS}")
dim=int("${DIMENSION}")
usable_gb=float("${USABLE_GB}")
cpus=int("${HOST_CPUS}")

# Cap nodes by CPU (kind pods are chatty) and by RAM (~400Mi working set/pod).
max_nodes_by_cpu = max(4, cpus * 6)
max_nodes_by_ram = max(4, int((usable_gb * 1024) / 400))
nodes = min(nodes, max_nodes_by_cpu, max_nodes_by_ram)

bytes_budget = usable_gb * 0.40 * (1024**3)  # 40% of usable for vectors
per_vec = dim * 4 * 2.5
max_total = int(bytes_budget / per_vec)
actual_total = min(target, max_total)
per_node = max(1000, actual_total // nodes)
actual_total = per_node * nodes
scale_factor = actual_total / float(target)

print(f"{nodes} {per_node} {actual_total} {scale_factor:.6f}")
PY
)"

read -r NODES VECTORS_PER_NODE ACTUAL_TOTAL SCALE_FACTOR <<<"$SCALE"

log "Host: ${HOST_CPUS} CPUs, ${HOST_MEM_GB} GiB RAM"
log "Mode: ${MODE}  nodes=${NODES}  target_vectors=${TARGET_VECTORS}  actual_vectors=${ACTUAL_TOTAL}"
log "Per-node working set: ${VECTORS_PER_NODE} × dim=${DIMENSION}  duration=${DURATION}s"
log "Scale factor vs 100M target: ${SCALE_FACTOR}"
log "Results: ${OUT_DIR}"

cat >"${OUT_DIR}/config.json" <<EOF
{
  "mode": "${MODE}",
  "stamp": "${STAMP}",
  "host_cpus": ${HOST_CPUS},
  "host_mem_gb": ${HOST_MEM_GB},
  "nodes": ${NODES},
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

# ---- Image -----------------------------------------------------------------
log "Building BusyBox Aster image (${IMAGE})"
(cd "$ROOT" && ./scripts/docker-build.sh) | tee "${OUT_DIR}/docker-build.log" | tail -20

# ---- kind cluster ----------------------------------------------------------
if ! kind get clusters 2>/dev/null | grep -qx "$CLUSTER"; then
  log "Creating kind cluster ${CLUSTER}"
  kind create cluster --config "${ROOT}/deploy/bench/kind-config.yaml" --name "$CLUSTER" \
    | tee "${OUT_DIR}/kind-create.log"
else
  log "Reusing kind cluster ${CLUSTER}"
fi
kubectl config use-context "kind-${CLUSTER}" >/dev/null

log "Loading image into kind"
kind load docker-image "$IMAGE" --name "$CLUSTER"

# Best-effort metrics-server for kubectl top (ignore failures).
if ! kubectl -n kube-system get deploy metrics-server >/dev/null 2>&1; then
  log "Installing metrics-server (optional)"
  kubectl apply -f https://github.com/kubernetes-sigs/metrics-server/releases/download/v0.7.2/components.yaml \
    >/dev/null 2>&1 || warn "metrics-server install skipped"
  kubectl -n kube-system patch deploy metrics-server --type=json \
    -p='[{"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"--kubelet-insecure-tls"}]' \
    >/dev/null 2>&1 || true
fi

# ---- Deploy ----------------------------------------------------------------
log "Reset namespace ${NS}"
kubectl delete namespace "$NS" --ignore-not-found --wait=true 2>/dev/null || true
# namespace termination can race; wait
for _ in $(seq 1 60); do
  kubectl get ns "$NS" >/dev/null 2>&1 || break
  sleep 2
done
kubectl apply -f "${ROOT}/deploy/bench/namespace.yaml"

if [[ "$MODE" == "minio" ]]; then
  log "Pulling and loading MinIO images into kind"
  docker pull minio/minio:latest
  docker pull minio/mc:latest
  kind load docker-image minio/minio:latest --name "$CLUSTER"
  kind load docker-image minio/mc:latest --name "$CLUSTER"
  log "Deploying MinIO"
  kubectl apply -f "${ROOT}/deploy/bench/minio.yaml"
  kubectl -n "$NS" rollout status deploy/minio --timeout=180s
  kubectl -n "$NS" wait --for=condition=complete job/minio-init --timeout=180s
  TEMPLATE="${ROOT}/deploy/bench/aster-minio.yaml"
else
  TEMPLATE="${ROOT}/deploy/bench/aster-local.yaml"
fi

RENDERED="${OUT_DIR}/aster-rendered.yaml"
sed -e "s/__REPLICAS__/${NODES}/g" \
    -e "s/__VECTORS__/${VECTORS_PER_NODE}/g" \
    -e "s/__DIMENSION__/${DIMENSION}/g" \
    -e "s/__DURATION__/${DURATION}/g" \
    -e "s|__IMAGE__|${IMAGE}|g" \
    "$TEMPLATE" >"$RENDERED"

log "Deploying ${NODES} Aster bench pods (${MODE} storage)"
kubectl apply -f "$RENDERED"

# ---- Wait for run ----------------------------------------------------------
log "Waiting for pods to start"
kubectl -n "$NS" rollout status statefulset/aster-node --timeout=300s || true

DEADLINE=$((DURATION + 240))
log "Soaking ${DURATION}s (overall wait budget ${DEADLINE}s) — streaming sample logs"
END_AT=$((SECONDS + DEADLINE))
FINISHED=0
while (( SECONDS < END_AT )); do
  RUNNING=$(kubectl -n "$NS" get pods -l app=aster-bench --no-headers 2>/dev/null | grep -c Running || true)
  # Pods stay Running after bench (sleep infinity); detect completion via logs.
  DONE_COUNT=0
  for p in $(kubectl -n "$NS" get pods -l app=aster-bench -o jsonpath='{.items[*].metadata.name}' 2>/dev/null); do
    if kubectl -n "$NS" logs "$p" -c aster 2>/dev/null | grep -q '"phase":"final"'; then
      DONE_COUNT=$((DONE_COUNT + 1))
    fi
  done
  printf '%s[%s]%s running=%s finished=%s / %s\n' \
    "$C_DIM" "$(date +%H:%M:%S)" "$C_RESET" "$RUNNING" "$DONE_COUNT" "$NODES"
  if [[ "$DONE_COUNT" -ge "$NODES" ]]; then
    ok "All ${NODES} nodes emitted final metrics"
    FINISHED=1
    break
  fi
  SAMPLE_POD=$(kubectl -n "$NS" get pods -l app=aster-bench -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || true)
  if [[ -n "$SAMPLE_POD" ]]; then
    kubectl -n "$NS" logs "$SAMPLE_POD" -c aster 2>/dev/null | tail -1 | sed "s/^/${C_MAGENTA}${SAMPLE_POD}${C_RESET} /" || true
  fi
  kubectl -n "$NS" top pods 2>/dev/null | head -6 >>"${OUT_DIR}/kubectl-top.txt" || true
  sleep 15
done
if [[ "$FINISHED" -ne 1 ]]; then
  warn "Timed out waiting for all finals; collecting whatever is available"
fi

# ---- Collect ---------------------------------------------------------------
log "Collecting logs and cluster metrics"
kubectl -n "$NS" get pods -o wide >"${OUT_DIR}/pods.txt" || true
kubectl -n "$NS" describe statefulset aster-node >"${OUT_DIR}/statefulset.txt" || true
kubectl top nodes >"${OUT_DIR}/nodes-top.txt" 2>/dev/null || warn "metrics-server not available for kubectl top nodes"
kubectl -n "$NS" top pods >"${OUT_DIR}/pods-top.txt" 2>/dev/null || true

mkdir -p "${OUT_DIR}/logs"
for p in $(kubectl -n "$NS" get pods -l app=aster-bench -o jsonpath='{.items[*].metadata.name}'); do
  kubectl -n "$NS" logs "$p" -c aster >"${OUT_DIR}/logs/${p}.log" 2>/dev/null || true
  if [[ "$MODE" == "minio" ]]; then
    kubectl -n "$NS" logs "$p" -c minio-sync >"${OUT_DIR}/logs/${p}-minio-sync.log" 2>/dev/null || true
  fi
done

if [[ "$MODE" == "minio" ]]; then
  kubectl -n "$NS" logs job/minio-init >"${OUT_DIR}/minio-init.log" 2>/dev/null || true
  kubectl -n "$NS" get svc minio -o yaml >"${OUT_DIR}/minio-svc.yaml" 2>/dev/null || true
fi

# ---- Aggregate + report ----------------------------------------------------
log "Aggregating metrics and writing report"
FORCE_COLOR=1 python3 "${ROOT}/deploy/bench/report.py" \
  --out-dir "$OUT_DIR" \
  --mode "$MODE" \
  --config "${OUT_DIR}/config.json"

ok "Bench complete"
printf '%s\n' "${OUT_DIR}/REPORT.md"
printf '%s\n' "${OUT_DIR}/summary.json"
