#!/usr/bin/env bash
# Aster vs Milvus distributed MinIO comparison (target 100M × 2048).
#
# Usage:
#   ./deploy/compare-milvus/run.sh
#   ./deploy/compare-milvus/run.sh smoke
#
# Env overrides:
#   COMPARE_NODES=4 TARGET_VECTORS=100000000 DIMENSION=2048 TOP_K=10 QUERIES=50
#   IMAGE=aster:local CLUSTER=aster-vs-milvus EF=128 BATCH_SIZE=32
#   FORCE_NODES=1   # do not auto-cap shard count to host RAM
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="${ROOT}/deploy/compare-milvus"
PROFILE="${1:-full}"

# Dedicated cluster — never inherit Makefile CLUSTER=aster-bench / NODES=50.
CLUSTER="${COMPARE_CLUSTER:-aster-vs-milvus}"
IMAGE="${IMAGE:-aster:local}"
NS=compare-milvus
DIMENSION="${DIMENSION:-2048}"
TOP_K="${TOP_K:-10}"
QUERIES="${QUERIES:-50}"
EF="${EF:-128}"
BATCH_SIZE="${BATCH_SIZE:-32}"
SEED="${SEED:-42}"
TARGET_VECTORS="${TARGET_VECTORS:-100000000}"
# Ignore Makefile NODES=50; only COMPARE_NODES (or smoke default) applies.
unset NODES || true
if [[ "$PROFILE" == "smoke" ]]; then
  REQUESTED_NODES="${COMPARE_NODES:-${NODES_SMOKE:-2}}"
  TARGET_VECTORS=2000
  QUERIES="${QUERIES_SMOKE:-10}"
  BATCH_SIZE=16
else
  REQUESTED_NODES="${COMPARE_NODES:-4}"
fi

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

need() {
  command -v "$1" >/dev/null 2>&1 || { err "missing dependency: $1"; exit 1; }
}

ensure_helm() {
  if command -v helm >/dev/null 2>&1; then
    return 0
  fi
  warn "helm not found — installing"
  if command -v brew >/dev/null 2>&1; then
    brew install helm
  else
    curl -fsSL https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-3 | bash
  fi
  if ! command -v helm >/dev/null 2>&1; then
    err "helm install failed; install manually: brew install helm"
    exit 1
  fi
  ok "helm $(helm version --short 2>/dev/null || true)"
}

dump_aster_debug() {
  warn "Aster StatefulSet not Ready — collecting diagnostics"
  kubectl -n "$NS" get sts,pods,svc -o wide | tee "${OUT}/aster-debug.txt" || true
  kubectl -n "$NS" describe sts aster-node | tee -a "${OUT}/aster-debug.txt" || true
  for p in $(kubectl -n "$NS" get pods -l app=aster-compare -o name 2>/dev/null | head -5); do
    echo "==== $p ====" | tee -a "${OUT}/aster-debug.txt"
    kubectl -n "$NS" describe "$p" | tee -a "${OUT}/aster-debug.txt" || true
  done
}

need docker
need kubectl
need kind
ensure_helm
need python3
need bazel

# Serialize vs-milvus runs (smoke + full fighting for one kind cluster).
# mkdir lock — portable on macOS (no flock(1)).
LOCK_DIR="${ROOT}/bench-results/.vs-milvus.lock.d"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  err "another bench-vs-milvus / smoke run holds ${LOCK_DIR}"
  err "remove it only if no compare-milvus process is running"
  exit 1
fi
release_lock() { rmdir "$LOCK_DIR" 2>/dev/null || true; }
trap release_lock EXIT

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${ROOT}/bench-results/vs-milvus-${PROFILE}-${STAMP}"
mkdir -p "$OUT"
export FORCE_COLOR="${FORCE_COLOR:-1}"

# ---- Host scale (100M×2048 ≈ 820 GiB raw; auto-shrink vectors + shards) ----
HOST_MEM_BYTES="$(sysctl -n hw.memsize 2>/dev/null || awk '/MemTotal/ {print $2*1024}' /proc/meminfo)"
HOST_CPUS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
# Use ASTER_SHARDS (not NODES) so Makefile export cannot bleed back in.
read -r HOST_MEM_GB USABLE_GB ACTUAL_VECTORS SCALED MEMORY_LIMIT ASTER_SHARDS NODES_CAPPED <<EOF
$(python3 - <<PY
mem_bytes = ${HOST_MEM_BYTES}
mem_gb = mem_bytes / (1024**3)
# Reserve OS + kind + standalone Milvus + etcd + MinIO (+ polytrade if present)
reserve = 16.0
usable = max(2.0, mem_gb - reserve)
dim = ${DIMENSION}
requested = int(${REQUESTED_NODES})
target = ${TARGET_VECTORS}
force = "${FORCE_NODES:-}" in ("1", "true", "TRUE", "yes")

# ~180Mi request footprint per Aster pod (aster+mc)
per_pod_mi = 180.0
aster_budget_mi = usable * 0.35 * 1024
max_nodes = max(2, int(aster_budget_mi / per_pod_mi))
# Kind laptop soft cap — full used to hit 12 and OOM next to Milvus.
if not force:
    max_nodes = min(max_nodes, 4 if "${PROFILE}" == "full" else 2)
nodes = requested if force else min(requested, max_nodes)
nodes_capped = 0 if nodes == requested else 1

bytes_per = dim * 4 * 3.5
budget = usable * 0.30 * (1024**3)
max_by_ram = int(budget / bytes_per)
per_shard_budget = (usable * 0.30 / max(nodes, 1)) * (1024**3)
max_by_shard = int(per_shard_budget / (dim * 4 * 2.5)) * nodes
cap = max(100, min(max_by_ram, max_by_shard, target))
if target <= 10000:
    cap = target
scaled = 1 if cap < target else 0
pod_mb = max(256, min(1024, int((usable * 0.30 / max(nodes, 1)) * 1024)))
mem_limit = f"{pod_mb}Mi"
print(f"{round(mem_gb,1)} {round(usable,1)} {cap} {scaled} {mem_limit} {nodes} {nodes_capped}")
PY
)
EOF

log "Host: ${HOST_CPUS} CPUs, ${HOST_MEM_GB} GiB RAM (usable≈${USABLE_GB} GiB)"
log "Profile=${PROFILE}  target=${TARGET_VECTORS}  actual=${ACTUAL_VECTORS}  dim=${DIMENSION}"
log "Shards: requested=${REQUESTED_NODES}  using=${ASTER_SHARDS}  cluster=${CLUSTER}"
if [[ "$NODES_CAPPED" == "1" ]]; then
  warn "Capped shards ${REQUESTED_NODES}→${ASTER_SHARDS} for host RAM (Milvus+Aster). FORCE_NODES=1 to override."
fi
if [[ "$SCALED" == "1" ]]; then
  warn "Scaled corpus to fit RAM (raw 100M×2048 needs ~820GiB floats alone)"
fi
log "Results: ${OUT}"

python3 - <<PY
import json
path = "${OUT}/scale-plan.json"
data = {
  "profile": "${PROFILE}",
  "target_vectors": ${TARGET_VECTORS},
  "actual_vectors": ${ACTUAL_VECTORS},
  "dimension": ${DIMENSION},
  "requested_shards": ${REQUESTED_NODES},
  "aster_shards": ${ASTER_SHARDS},
  "shards_capped": ${NODES_CAPPED} == 1,
  "scaled": ${SCALED} == 1,
  "host_ram_gb": ${HOST_MEM_GB},
  "usable_gb": ${USABLE_GB},
  "pod_memory_limit": "${MEMORY_LIMIT}",
  "cluster": "${CLUSTER}",
  "top_k": ${TOP_K},
  "queries": ${QUERIES},
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
print("wrote", path)
PY

# ---- Image + kind ----------------------------------------------------------
log "Building BusyBox Aster image (${IMAGE})"
(cd "$ROOT" && ./scripts/docker-build.sh) | tee "${OUT}/docker-build.log" | tail -20

if ! kind get clusters 2>/dev/null | grep -qx "$CLUSTER"; then
  log "Creating kind cluster ${CLUSTER}"
  kind create cluster --config "${HERE}/kind-config.yaml" --name "$CLUSTER" \
    | tee "${OUT}/kind-create.log"
else
  log "Reusing kind cluster ${CLUSTER}"
fi
kubectl config use-context "kind-${CLUSTER}" >/dev/null
kind load docker-image "$IMAGE" --name "$CLUSTER"

# Preload MinIO client into kind (50× ImagePullBackOff otherwise / disk thrash).
MC_IMG=minio/mc:RELEASE.2024-11-17T19-35-25Z
MINIO_IMG=minio/minio:RELEASE.2024-12-18T13-15-44Z
for img in "$MC_IMG" "$MINIO_IMG"; do
  if ! docker image inspect "$img" >/dev/null 2>&1; then
    log "Pulling ${img}"
    docker pull "$img"
  fi
  kind load docker-image "$img" --name "$CLUSTER" >/dev/null
done

# ---- Namespace + MinIO + Aster ---------------------------------------------
log "Applying namespace + MinIO"
kubectl apply -f "${HERE}/namespace.yaml"
# Drop legacy Service named "minio" if present (injects MINIO_PORT into Milvus).
kubectl -n "$NS" delete svc minio --ignore-not-found 2>/dev/null || true
kubectl -n "$NS" delete deploy minio --ignore-not-found 2>/dev/null || true
kubectl -n "$NS" delete job minio-init --ignore-not-found 2>/dev/null || true
kubectl apply -f "${HERE}/minio.yaml"
kubectl -n "$NS" rollout status deploy/aster-minio --timeout=300s
kubectl -n "$NS" wait --for=condition=complete job/minio-init --timeout=300s || true

log "Deploying Aster ${ASTER_SHARDS} shards (serve + MinIO sync)"
kubectl -n "$NS" delete statefulset aster-node --ignore-not-found --wait=true || true
kubectl -n "$NS" delete pods -l app=aster-compare --ignore-not-found --wait=false || true
sed -e "s|__REPLICAS__|${ASTER_SHARDS}|g" \
    -e "s|__IMAGE__|${IMAGE}|g" \
    -e "s|__MEMORY_LIMIT__|${MEMORY_LIMIT}|g" \
    "${HERE}/aster/statefulset.yaml.tmpl" > "${OUT}/aster-statefulset.yaml"
kubectl apply -f "${OUT}/aster-statefulset.yaml"
kubectl -n "$NS" scale statefulset/aster-node --replicas="${ASTER_SHARDS}"

if ! kubectl -n "$NS" rollout status statefulset/aster-node --timeout=480s; then
  dump_aster_debug
  err "Aster StatefulSet failed to become Ready (see ${OUT}/aster-debug.txt)"
  exit 1
fi
ok "Aster ${ASTER_SHARDS}/${ASTER_SHARDS} shards Ready"

# ---- Milvus cluster --------------------------------------------------------
log "Installing Milvus standalone (Helm) with external MinIO"
chmod +x "${HERE}/milvus/install.sh"
NS="$NS" PROFILE="$PROFILE" "${HERE}/milvus/install.sh" | tee "${OUT}/milvus-install.log"
for i in $(seq 1 60); do
  if kubectl -n "$NS" get svc milvus >/dev/null 2>&1 || \
     kubectl -n "$NS" get svc milvus-milvus >/dev/null 2>&1; then
    break
  fi
  sleep 5
done

MILVUS_SVC="$(kubectl -n "$NS" get svc -o name 2>/dev/null | grep -E 'svc/milvus$|svc/milvus-milvus$' | head -1 | sed 's|service/||' || true)"
if [[ -z "${MILVUS_SVC}" ]]; then
  MILVUS_SVC="$(kubectl -n "$NS" get svc -o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}' \
    | grep -i milvus | grep -v etcd | grep -v minio | grep -v pulsar | grep -v bookie \
    | grep -v zookeeper | grep -v proxy | head -1 || true)"
fi
if [[ -z "${MILVUS_SVC}" ]]; then
  MILVUS_SVC="$(kubectl -n "$NS" get svc -o json | python3 -c '
import json,sys
d=json.load(sys.stdin)
for it in d["items"]:
  ports=it.get("spec",{}).get("ports") or []
  if any(p.get("port")==19530 or p.get("targetPort")==19530 for p in ports):
    print(it["metadata"]["name"]); break
' 2>/dev/null || true)"
fi
if [[ -z "${MILVUS_SVC}" ]]; then
  err "could not find Milvus proxy Service"
  kubectl -n "$NS" get svc,pods | tee "${OUT}/milvus-debug.txt"
  exit 1
fi
ok "Milvus service: ${MILVUS_SVC}"

# ---- Port-forwards ---------------------------------------------------------
PF_DIR="${OUT}/port-forward"
mkdir -p "$PF_DIR"
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  release_lock
}
trap cleanup EXIT

ASTER_URLS=()
for i in $(seq 0 $((ASTER_SHARDS - 1))); do
  local_port=$((18080 + i))
  kubectl -n "$NS" port-forward "pod/aster-node-${i}" "${local_port}:8080" \
    >"${PF_DIR}/aster-${i}.log" 2>&1 &
  PIDS+=($!)
  ASTER_URLS+=("http://127.0.0.1:${local_port}")
done
MILVUS_LOCAL=19530
kubectl -n "$NS" port-forward "svc/${MILVUS_SVC}" "${MILVUS_LOCAL}:19530" \
  >"${PF_DIR}/milvus.log" 2>&1 &
PIDS+=($!)

log "Waiting for port-forwards"
sleep 5
for url in "${ASTER_URLS[@]}"; do
  for _ in $(seq 1 60); do
    if curl -sf "${url}/health" >/dev/null; then break; fi
    sleep 1
  done
done

# ---- Python venv + driver --------------------------------------------------
log "Preparing Python driver venv"
# Prefer 3.11–3.13: pymilvus/grpcio lack wheels on 3.14 and fail to compile.
PY=""
for cand in python3.12 python3.11 python3.13 python3; do
  if command -v "$cand" >/dev/null 2>&1; then
    ver="$("$cand" -c 'import sys; print(f"{sys.version_info[0]}.{sys.version_info[1]}")')"
    major="${ver%%.*}"; minor="${ver#*.}"
    if [[ "$major" -eq 3 && "$minor" -ge 11 && "$minor" -le 13 ]]; then
      PY="$cand"
      break
    fi
  fi
done
if [[ -z "$PY" ]]; then
  err "need Python 3.11–3.13 for pymilvus (found $(python3 --version 2>&1))"
  exit 1
fi
log "Using ${PY} ($("$PY" --version 2>&1))"
VENV="${OUT}/venv"
"$PY" -m venv "$VENV"
# shellcheck disable=SC1091
source "${VENV}/bin/activate"
pip install -q -U pip
pip install -q -r "${HERE}/driver/requirements.txt"

ASTER_URLS_CSV="$(IFS=,; echo "${ASTER_URLS[*]}")"
SCALED_FLAG=()
if [[ "$SCALED" == "1" ]]; then SCALED_FLAG=(--scaled); fi

log "Running comparison driver"
(
  cd "${HERE}/driver"
  python compare.py \
    --out-dir "$OUT" \
    --aster-urls "$ASTER_URLS_CSV" \
    --milvus-uri "http://127.0.0.1:${MILVUS_LOCAL}" \
    --vectors "$ACTUAL_VECTORS" \
    --dimension "$DIMENSION" \
    --batch-size "$BATCH_SIZE" \
    --queries "$QUERIES" \
    --top-k "$TOP_K" \
    --seed "$SEED" \
    --ef "$EF" \
    --target-vectors "$TARGET_VECTORS" \
    --host-ram-gb "$HOST_MEM_GB" \
    --usable-gb "$USABLE_GB" \
    "${SCALED_FLAG[@]}"
) | tee "${OUT}/driver.log"

ok "Done. See ${OUT}/report.txt and ${OUT}/report.json"
deactivate || true
