#!/usr/bin/env bash
# Cross-compile a fully-static musl Aster binary and pack it into BusyBox.
#
# Usage:
#   ./scripts/docker-build.sh              # host arch
#   ./scripts/docker-build.sh amd64        # linux/amd64
#   ./scripts/docker-build.sh arm64        # linux/arm64
#   IMAGE=aster:dev ./scripts/docker-build.sh
#
# Requires: bazel, docker.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

IMAGE="${IMAGE:-aster:local}"
BUSYBOX_IMAGE="${BUSYBOX_IMAGE:-busybox:1.37.0-musl}"
ARCH_ARG="${1:-}"

host_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo amd64 ;;
    aarch64|arm64) echo arm64 ;;
    *) echo "unsupported host arch: $(uname -m)" >&2; exit 1 ;;
  esac
}

ARCH="${ARCH_ARG:-$(host_arch)}"
case "$ARCH" in
  amd64|x86_64)
    ARCH=amd64
    BAZEL_CONFIG=linux_musl_amd64
    PLATFORM=linux/amd64
    ;;
  arm64|aarch64)
    ARCH=arm64
    BAZEL_CONFIG=linux_musl_arm64
    PLATFORM=linux/arm64
    ;;
  *)
    echo "usage: $0 [amd64|arm64]" >&2
    exit 2
    ;;
esac

STAGING="$ROOT/deploy/docker/.build"
mkdir -p "$STAGING"
trap 'rm -rf "$STAGING"' EXIT

echo "==> bazel build --config=${BAZEL_CONFIG} //aster/cli:aster //aster/bench:aster-bench //deploy/docker:su-exec"
bazel build --config="${BAZEL_CONFIG}" //aster/cli:aster //aster/bench:aster-bench //deploy/docker:su-exec

resolve_bin() {
  local target="$1"
  local fallback="$2"
  local path
  path="$(bazel cquery --config="${BAZEL_CONFIG}" "${target}" --output=files 2>/dev/null | head -1 || true)"
  if [[ -n "$path" && -f "$path" ]]; then
    printf '%s\n' "$path"
    return 0
  fi
  if [[ -f "$fallback" ]]; then
    printf '%s\n' "$fallback"
    return 0
  fi
  echo "error: built binary not found for ${target}" >&2
  exit 1
}

ASTER_BIN="$(resolve_bin //aster/cli:aster bazel-bin/aster/cli/aster)"
BENCH_BIN="$(resolve_bin //aster/bench:aster-bench bazel-bin/aster/bench/aster-bench)"
SUEXEC_BIN="$(resolve_bin //deploy/docker:su-exec bazel-bin/deploy/docker/su-exec)"

cp -f "$ASTER_BIN" "$STAGING/aster"
cp -f "$BENCH_BIN" "$STAGING/aster-bench"
cp -f "$SUEXEC_BIN" "$STAGING/su-exec"
cp -f "$ROOT/deploy/docker/entrypoint.sh" "$STAGING/entrypoint.sh"
chmod 755 "$STAGING/aster" "$STAGING/aster-bench" "$STAGING/su-exec" "$STAGING/entrypoint.sh"

echo "==> static link check"
if command -v file >/dev/null; then
  file "$STAGING/aster" "$STAGING/aster-bench" "$STAGING/su-exec" || true
fi
if command -v otool >/dev/null 2>&1 && [[ "$(uname -s)" == Darwin ]]; then
  :
elif command -v readelf >/dev/null 2>&1; then
  if readelf -d "$STAGING/aster" 2>/dev/null | grep -q NEEDED; then
    echo "error: aster has shared library NEEDED entries; expected fully static" >&2
    exit 1
  fi
  echo "    aster: no DT_NEEDED (fully static)"
fi

SIZE_BYTES="$(wc -c < "$STAGING/aster" | tr -d ' ')"
SIZE_MB="$(awk "BEGIN { printf \"%.2f\", ${SIZE_BYTES}/1024/1024 }")"
echo "==> aster binary size: ${SIZE_MB} MiB (${SIZE_BYTES} bytes)"

echo "==> docker build (${PLATFORM}) -> ${IMAGE}"
docker build \
  --platform="${PLATFORM}" \
  --build-arg "BUSYBOX_IMAGE=${BUSYBOX_IMAGE}" \
  -t "${IMAGE}" \
  -f "$ROOT/deploy/docker/Dockerfile" \
  "$STAGING"

echo "==> image: ${IMAGE}"
docker image inspect "${IMAGE}" --format 'Size: {{.Size}} bytes' || true
echo "Run:  docker run --rm -v aster-data:/data ${IMAGE}"
echo "Help: docker run --rm ${IMAGE} --help"
