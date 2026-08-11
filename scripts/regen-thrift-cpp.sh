#!/usr/bin/env bash
# Regenerate checked-in C++ Thrift stubs from aster/rpc/aster.thrift.
#
# Requires the Apache Thrift compiler on PATH (tested with 0.24.x).
# Full hermetic codegen (compiler + runtime as Bazel tools) is deferred;
# stubs are checked in so `bazel build //aster/rpc/...` stays hermetic.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDL="${ROOT}/aster/rpc/aster.thrift"
OUT="${ROOT}/aster/rpc/gen-cpp"

if ! command -v thrift >/dev/null 2>&1; then
  echo "error: thrift compiler not found on PATH" >&2
  echo "  install e.g. 'brew install thrift' or see https://thrift.apache.org/" >&2
  exit 1
fi

VERSION="$(thrift -version 2>&1 || true)"
echo "Using ${VERSION}"
echo "IDL: ${IDL}"
echo "OUT: ${OUT}"

mkdir -p "${OUT}"
# -out writes files directly into OUT (no gen-cpp/ nesting).
# no_skeleton: skip empty *_server.skeleton.cpp.
thrift --gen cpp:no_skeleton -out "${OUT}" "${IDL}"

echo "Generated:"
ls -1 "${OUT}"
