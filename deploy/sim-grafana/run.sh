#!/usr/bin/env bash
# Boot Aster live simulation: 50-node MinIO cluster, continuous scale,
# 50 indexes (target up to 1B rows, auto-scaled to host), Prometheus + Grafana.
#
#   ./deploy/sim-grafana/run.sh
#   ./deploy/sim-grafana/run.sh stop
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="${ROOT}/deploy/sim-grafana"
ACTION="${1:-start}"
# Never inherit Makefile CLUSTER=aster-bench — this suite owns aster-sim.
CLUSTER="${SIM_CLUSTER:-aster-sim}"
NS=aster-sim
IMAGE="${IMAGE:-aster:local}"

MAX_NODES="${MAX_NODES:-50}"
MIN_NODES="${MIN_NODES:-15}"
START_NODES="${START_NODES:-25}"
INDEXES="${INDEXES:-50}"
TARGET_TOTAL_ROWS="${TARGET_TOTAL_ROWS:-1000000000}"
SCALE_EVERY_SEC="${SCALE_EVERY_SEC:-45}"
WORK_RPS="${WORK_RPS:-20}"
GRAFANA_HOST_PORT="${GRAFANA_HOST_PORT:-3000}"
PROM_HOST_PORT="${PROM_HOST_PORT:-9090}"

if [[ -t 1 ]] || [[ "${FORCE_COLOR:-}" == "1" ]]; then
  C_RESET=$'\033[0m'; C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'
else
  C_RESET= C_CYAN= C_GREEN= C_YELLOW= C_RED=
fi
log()  { printf '%s%s%s %s\n' "$C_CYAN" "==>" "$C_RESET" "$*"; }
ok()   { printf '%s%s%s %s\n' "$C_GREEN" "OK " "$C_RESET" "$*"; }
warn() { printf '%s%s%s %s\n' "$C_YELLOW" "!! " "$C_RESET" "$*"; }
err()  { printf '%s%s%s %s\n' "$C_RED" "ERR" "$C_RESET" "$*" >&2; }

need() { command -v "$1" >/dev/null 2>&1 || { err "missing: $1"; exit 1; }; }

port_in_use() {
  local port="$1"
  if command -v lsof >/dev/null 2>&1; then
    lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1
  else
    docker ps --format '{{.Ports}}' | grep -q ":${port}->"
  fi
}

print_urls() {
  cat <<EOF

${C_GREEN}Aster simulation is running${C_RESET}

  Grafana:    http://127.0.0.1:${GRAFANA_HOST_PORT}   (admin / aster, anonymous viewer OK)
  Prometheus: http://127.0.0.1:${PROM_HOST_PORT}
  Dashboard:  Aster → "Aster live simulation"

  Cluster:    kind/${CLUSTER}  namespace/${NS}
  Scale:      ${MIN_NODES} ↔ ${MAX_NODES} every ${SCALE_EVERY_SEC}s
  Indexes:    ${INDEXES} (dims mix, target rows ${TARGET_TOTAL_ROWS}, actual ${ACTUAL_TOTAL_ROWS:-?})
  Backend:    MinIO (aster-shards)

  Stop:       make sim-grafana-stop

EOF
}

if [[ "$ACTION" == "stop" ]]; then
  # Always target aster-sim (not Makefile CLUSTER=aster-bench).
  kind delete cluster --name "${SIM_CLUSTER:-aster-sim}" || true
  ok "cluster ${SIM_CLUSTER:-aster-sim} deleted"
  exit 0
fi

need docker
need kind
need kubectl
need bazel
need python3

# Cap actual rows to host RAM (1B is the aspirational target).
HOST_MEM_BYTES="$(sysctl -n hw.memsize 2>/dev/null || awk '/MemTotal/ {print $2*1024}' /proc/meminfo)"
read -r HOST_MEM_GB ACTUAL_TOTAL_ROWS MEMORY_LIMIT <<EOF
$(python3 - <<PY
mem_gb = ${HOST_MEM_BYTES} / (1024**3)
usable = max(2.0, mem_gb - 8.0)
# Budget ~0.25 of usable across 50 nodes for vectors (float32) + LSM overhead.
budget = usable * 0.25 * (1024**3)
# Assume avg dim ~512 for mixed indexes → 2KB/vector raw * 3 overhead
bytes_per = 512 * 4 * 3
actual = max(5000, min(int(${TARGET_TOTAL_ROWS}), int(budget / bytes_per)))
pod_mb = max(256, min(768, int((usable * 0.35 / max(${MAX_NODES},1)) * 1024)))
print(f"{round(mem_gb,1)} {actual} {pod_mb}Mi")
PY
)
EOF

if [[ "$START_NODES" -lt "$MIN_NODES" ]]; then START_NODES=$MIN_NODES; fi
if [[ "$START_NODES" -gt "$MAX_NODES" ]]; then START_NODES=$MAX_NODES; fi

log "Host RAM ≈ ${HOST_MEM_GB} GiB"
log "Nodes ${MIN_NODES}↔${MAX_NODES} (start ${START_NODES}), indexes=${INDEXES}"
log "Row budget: target=${TARGET_TOTAL_ROWS} actual=${ACTUAL_TOTAL_ROWS} (auto-scaled)"
warn "A true 1B-vector corpus needs multi-TB; actual budget fits this machine"

log "Building Aster image"
(cd "$ROOT" && ./scripts/docker-build.sh) | tail -15

if kind get clusters 2>/dev/null | grep -qx "$CLUSTER"; then
  log "Reusing existing kind cluster ${CLUSTER}"
  kubectl config use-context "kind-${CLUSTER}" >/dev/null
  if kubectl -n "$NS" get deploy/grafana >/dev/null 2>&1; then
    ok "Simulation stack already present — refreshing workloads"
  fi
else
  if port_in_use "$GRAFANA_HOST_PORT" || port_in_use "$PROM_HOST_PORT"; then
    err "Host ports ${GRAFANA_HOST_PORT}/${PROM_HOST_PORT} are already in use,"
    err "but kind cluster '${CLUSTER}' does not exist."
    err "Free them (make sim-grafana-stop / stop other stacks) or set:"
    err "  GRAFANA_HOST_PORT=3001 PROM_HOST_PORT=9091 make sim-grafana"
    err "Current listeners:"
    docker ps --format 'table {{.Names}}\t{{.Ports}}' | grep -E "3000|9090|NAMES" || true
    exit 1
  fi
  log "Creating kind cluster ${CLUSTER}"
  # Render kind config with chosen host ports.
  KIND_CFG="${HERE}/.kind-config.generated.yaml"
  sed -e "s/hostPort: 3000/hostPort: ${GRAFANA_HOST_PORT}/" \
      -e "s/hostPort: 9090/hostPort: ${PROM_HOST_PORT}/" \
      "${HERE}/kind-config.yaml" >"$KIND_CFG"
  kind create cluster --config "$KIND_CFG" --name "$CLUSTER"
fi
kubectl config use-context "kind-${CLUSTER}" >/dev/null
kind load docker-image "$IMAGE" --name "$CLUSTER"

# Preload heavy images into kind (avoids in-cluster pull / disk thrash).
for img in \
  minio/minio:RELEASE.2024-12-18T13-15-44Z \
  minio/mc:RELEASE.2024-11-17T19-35-25Z \
  prom/prometheus:v2.54.1 \
  grafana/grafana:11.2.0 \
  python:3.12-slim
do
  if ! docker image inspect "$img" >/dev/null 2>&1; then
    log "Pulling ${img}"
    docker pull "$img"
  fi
  log "Loading ${img} into kind"
  kind load docker-image "$img" --name "$CLUSTER" >/dev/null
done

log "Namespace + MinIO"
kubectl apply -f "${HERE}/namespace.yaml"
kubectl apply -f "${HERE}/minio.yaml"
kubectl -n "$NS" rollout status deploy/minio --timeout=300s
kubectl -n "$NS" wait --for=condition=complete job/minio-init --timeout=300s || true

log "Prometheus + Grafana"
kubectl apply -f "${HERE}/monitoring/prometheus.yaml"
# Dashboard ConfigMap from JSON file
kubectl -n "$NS" create configmap grafana-dashboards \
  --from-file=aster-sim.json="${HERE}/grafana/dashboards/aster-sim.json" \
  --dry-run=client -o yaml | kubectl apply -f -
kubectl apply -f "${HERE}/monitoring/grafana.yaml"
kubectl -n "$NS" rollout status deploy/prometheus --timeout=300s
kubectl -n "$NS" rollout status deploy/grafana --timeout=300s

log "Aster StatefulSet (${START_NODES} pods, MinIO sync)"
sed -e "s|__REPLICAS__|${START_NODES}|g" \
    -e "s|__IMAGE__|${IMAGE}|g" \
    -e "s|__MEMORY_LIMIT__|${MEMORY_LIMIT}|g" \
    "${HERE}/aster/statefulset.yaml.tmpl" | kubectl apply -f -
kubectl -n "$NS" rollout status statefulset/aster-node --timeout=600s || \
  warn "StatefulSet not fully Ready yet (continuing)"

log "Simulation controller (CRUD + scale)"
kubectl -n "$NS" create configmap sim-controller-code \
  --from-file=controller.py="${HERE}/controller/controller.py" \
  --from-file=requirements.txt="${HERE}/controller/requirements.txt" \
  --dry-run=client -o yaml | kubectl apply -f -
sed -e "s|__MIN_NODES__|${MIN_NODES}|g" \
    -e "s|__MAX_NODES__|${MAX_NODES}|g" \
    -e "s|__START_NODES__|${START_NODES}|g" \
    -e "s|__INDEXES__|${INDEXES}|g" \
    -e "s|__TARGET_TOTAL_ROWS__|${TARGET_TOTAL_ROWS}|g" \
    -e "s|__ACTUAL_TOTAL_ROWS__|${ACTUAL_TOTAL_ROWS}|g" \
    -e "s|__SCALE_EVERY_SEC__|${SCALE_EVERY_SEC}|g" \
    -e "s|__WORK_RPS__|${WORK_RPS}|g" \
    "${HERE}/controller/deployment.yaml.tmpl" | kubectl apply -f -
kubectl -n "$NS" rollout status deploy/sim-controller --timeout=300s || true

print_urls

mkdir -p "${ROOT}/bench-results"
STATUS="${ROOT}/bench-results/sim-grafana-status.txt"
{
  echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "grafana=http://127.0.0.1:${GRAFANA_HOST_PORT}"
  echo "prometheus=http://127.0.0.1:${PROM_HOST_PORT}"
  echo "cluster=${CLUSTER}"
  echo "min_nodes=${MIN_NODES} max_nodes=${MAX_NODES} start=${START_NODES}"
  echo "indexes=${INDEXES} target_rows=${TARGET_TOTAL_ROWS} actual_rows=${ACTUAL_TOTAL_ROWS}"
} >"$STATUS"
ok "status written to ${STATUS}"
