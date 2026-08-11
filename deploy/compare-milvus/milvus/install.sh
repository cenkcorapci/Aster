#!/usr/bin/env bash
# Install / upgrade Milvus standalone (Helm) against shared MinIO.
# Always uninstall first so leftover pulsar/pulsarv3 from older values is gone.
set -euo pipefail

NS="${NS:-compare-milvus}"
RELEASE="${RELEASE:-milvus}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VALUES="${ROOT}/milvus/values.yaml"
if [[ "${PROFILE:-full}" == "smoke" ]]; then
  VALUES="${ROOT}/milvus/values-smoke.yaml"
fi

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }

ensure_helm() {
  if command -v helm >/dev/null 2>&1; then
    return 0
  fi
  echo "==> helm not found — installing"
  if command -v brew >/dev/null 2>&1; then
    brew install helm
  else
    curl -fsSL https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-3 | bash
  fi
  need helm
}

ensure_helm
need kubectl

helm repo add milvus https://zilliztech.github.io/milvus-helm/ >/dev/null 2>&1 || true
helm repo update milvus >/dev/null

# Ensure MinIO bucket exists before Milvus starts writing.
kubectl -n "$NS" wait --for=condition=complete job/minio-init --timeout=180s || true

echo "==> purging previous Milvus release (drops leftover Pulsar stacks)"
helm uninstall "$RELEASE" -n "$NS" --wait --timeout 5m 2>/dev/null || true
# Belt-and-suspenders: chart upgrades can leave orphaned Pulsar STS around.
kubectl -n "$NS" delete sts,deploy,svc,job,pvc,cm,secret \
  -l "app.kubernetes.io/instance=${RELEASE}" --ignore-not-found --wait=false 2>/dev/null || true
kubectl -n "$NS" delete sts,deploy,svc,job,pvc \
  -l 'app in (pulsar,pulsarv3)' --ignore-not-found --wait=false 2>/dev/null || true
# Give the API a moment after mass deletes on a small kind cluster.
sleep 3

echo "==> helm upgrade --install ${RELEASE} (standalone + external MinIO) values=$(basename "$VALUES")"
helm upgrade --install "$RELEASE" milvus/milvus \
  --namespace "$NS" \
  --create-namespace \
  -f "$VALUES" \
  --set cluster.enabled=false \
  --set standalone.messageQueue=rocksmq \
  --set pulsar.enabled=false \
  --set pulsarv3.enabled=false \
  --set kafka.enabled=false \
  --set woodpecker.enabled=false \
  --set minio.enabled=false \
  --set externalS3.enabled=true \
  --set externalS3.host=aster-minio \
  --set externalS3.port=9000 \
  --set externalS3.accessKey=asterbench \
  --set externalS3.secretKey=asterbench-secret \
  --set externalS3.bucketName=milvus-bucket \
  --set externalS3.rootPath=files \
  --set externalS3.useSSL=false \
  --set externalS3.cloudProvider=minio \
  --set etcd.replicaCount=1 \
  --set etcd.persistence.enabled=false \
  --timeout 15m \
  --wait=false

echo "==> waiting for milvus standalone"
for i in $(seq 1 90); do
  ready="$(kubectl -n "$NS" get pods -l app.kubernetes.io/instance="${RELEASE}" \
    --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l | tr -d ' ')"
  echo "    running pods: ${ready} (attempt ${i})"
  if kubectl -n "$NS" wait --for=condition=Ready pod \
      -l "app.kubernetes.io/instance=${RELEASE},component=standalone" \
      --timeout=15s 2>/dev/null; then
    echo "OK milvus standalone ready"
    exit 0
  fi
  if kubectl -n "$NS" wait --for=condition=Ready pod \
      -l "app.kubernetes.io/instance=${RELEASE},component=proxy" \
      --timeout=5s 2>/dev/null; then
    echo "OK milvus proxy ready"
    exit 0
  fi
  if kubectl -n "$NS" get pods -o json 2>/dev/null | python3 -c '
import json,sys
d=json.load(sys.stdin)
for p in d.get("items",[]):
  if p.get("status",{}).get("phase")!="Running":
    continue
  conds=p.get("status",{}).get("conditions") or []
  if not any(c.get("type")=="Ready" and c.get("status")=="True" for c in conds):
    continue
  for c in p.get("spec",{}).get("containers") or []:
    for port in c.get("ports") or []:
      if port.get("containerPort")==19530:
        sys.exit(0)
sys.exit(1)
' 2>/dev/null; then
    echo "OK milvus serving on 19530"
    exit 0
  fi
  sleep 10
done

echo "WARN: milvus not Ready within timeout; dumping pods" >&2
kubectl -n "$NS" get pods -o wide >&2 || true
kubectl -n "$NS" logs -l component=standalone --tail=40 >&2 || true
exit 1
