#!/usr/bin/env bash
# Build Aster for the supported CPU / OS / profile matrix.
#
# Usage:
#   ./scripts/build-matrix.sh           # host + arduino + musl (this arch)
#   ./scripts/build-matrix.sh --full    # also zig cross + Edge arm64 (Pi)
#   ./scripts/build-matrix.sh --quick   # host default + arduino only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="${1:---default}"

host_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo amd64 ;;
    aarch64|arm64) echo arm64 ;;
    *) echo "unknown" ;;
  esac
}

host_os() {
  case "$(uname -s)" in
    Darwin) echo macos ;;
    Linux) echo linux ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo windows ;;
    *) echo "unknown" ;;
  esac
}

ARCH="$(host_arch)"
OS="$(host_os)"
FAILED=0

run() {
  local label="$1"
  shift
  echo
  echo "==> ${label}"
  echo "    $*"
  if "$@"; then
    echo "    OK"
  else
    echo "    FAIL" >&2
    FAILED=$((FAILED + 1))
  fi
}

echo "Host: ${OS}/${ARCH}"
echo "Mode: ${MODE}"

# Always: default host toolchain (Apple Silicon / Intel Mac / Linux native).
run "host default //aster/..." bazel build //aster/...
run "host tests" bazel test //aster/...

# Profile smoke builds on the host.
run "profile tiny" bazel build --config=tiny //aster/embedded //aster/core //aster/index
run "profile edge" bazel build --config=edge //aster/db //aster/cli:aster
run "profile server" bazel build --config=server //aster/db //aster/cli:aster

# Arduino / MCU Tiny library (no POSIX).
run "arduino embedded" bazel build --config=arduino //aster/embedded
run "arduino embedded tests" bazel test --config=arduino //aster/embedded:embedded_test

if [[ "$MODE" == "--quick" ]]; then
  echo
  if [[ "$FAILED" -ne 0 ]]; then
    echo "FAILED: ${FAILED} step(s)" >&2
    exit 1
  fi
  echo "OK: quick matrix passed"
  exit 0
fi

# Explicit host platform label (documents Apple Silicon vs Intel).
case "${OS}/${ARCH}" in
  macos/arm64)
    run "macos_arm64 (Apple Silicon)" bazel build --config=macos_arm64 //aster/cli:aster
    ;;
  macos/amd64)
    run "macos_amd64 (Intel Mac)" bazel build --config=macos_amd64 //aster/cli:aster
    ;;
  linux/amd64)
    run "linux_amd64" bazel build --config=linux_amd64 //aster/cli:aster
    ;;
  linux/arm64)
    run "linux_arm64" bazel build --config=linux_arm64 //aster/cli:aster
    ;;
esac

# BusyBox static musl for this arch.
case "$ARCH" in
  amd64)
    run "linux_musl_amd64" bazel build --config=linux_musl_amd64 //aster/cli:aster
    ;;
  arm64)
    run "linux_musl_arm64" bazel build --config=linux_musl_arm64 //aster/cli:aster
    ;;
esac

if [[ "$MODE" == "--full" ]]; then
  run "zig linux amd64" bazel build --config=zig_linux_amd64 //aster/cli:aster
  run "zig linux arm64" bazel build --config=zig_linux_arm64 //aster/cli:aster
  # M3-T06: Edge + linux/arm64 (NEON). Native Pi / CI: raspberry_pi; else zig cross.
  if [[ "$OS" == "linux" && "$ARCH" == "arm64" ]]; then
    run "raspberry_pi (Edge + linux_arm64)" \
      bazel build --config=raspberry_pi //aster/...
    run "raspberry_pi tests" \
      bazel test --config=raspberry_pi //aster/...
  else
    run "raspberry_pi_cross (Edge + zig linux arm64)" \
      bazel build --config=raspberry_pi_cross //aster/cli:aster
  fi
  # Optional: cross Darwin / Windows (may need extra SDKs).
  if [[ "$OS" == "macos" && "$ARCH" == "arm64" ]]; then
    run "zig macos amd64 (Intel from Apple Silicon)" \
      bazel build --config=zig_macos_amd64 //aster/core //aster/embedded
  fi
fi

echo
if [[ "$FAILED" -ne 0 ]]; then
  echo "FAILED: ${FAILED} step(s)" >&2
  exit 1
fi
echo "OK: build matrix passed"
