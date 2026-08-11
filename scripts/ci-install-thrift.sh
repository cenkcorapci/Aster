#!/usr/bin/env bash
# Install Apache Thrift *compiler* 0.24.x for CI / local codegen.
# Matches the @apache_thrift runtime version in MODULE.bazel.
#
# Usage: ./scripts/ci-install-thrift.sh
# Idempotent: no-ops when `thrift -version` already reports 0.24.
set -euo pipefail

THRIFT_VERSION="${THRIFT_VERSION:-0.24.0}"
PREFIX="${THRIFT_PREFIX:-/usr/local}"

if command -v thrift >/dev/null 2>&1; then
  ver="$(thrift -version 2>&1 || true)"
  if echo "${ver}" | grep -q "${THRIFT_VERSION}"; then
    echo "Using existing ${ver}"
    exit 0
  fi
  echo "Found ${ver}; building ${THRIFT_VERSION} into ${PREFIX} for codegen consistency."
fi

if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
  echo "Installing thrift via Homebrew..."
  brew list thrift >/dev/null 2>&1 || brew install thrift
  thrift -version
  exit 0
fi

echo "Building Apache Thrift compiler ${THRIFT_VERSION} from source..."
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  build-essential cmake bison flex pkg-config

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT
curl -fsSL \
  "https://github.com/apache/thrift/archive/refs/tags/v${THRIFT_VERSION}.tar.gz" \
  -o "${WORKDIR}/thrift.tgz"
tar -xzf "${WORKDIR}/thrift.tgz" -C "${WORKDIR}"

cmake -S "${WORKDIR}/thrift-${THRIFT_VERSION}" -B "${WORKDIR}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DBUILD_COMPILER=ON \
  -DBUILD_LIBRARIES=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TUTORIALS=OFF \
  -DWITH_JAVA=OFF \
  -DWITH_PYTHON=OFF \
  -DWITH_HASKELL=OFF \
  -DWITH_NODEJS=OFF \
  -DWITH_GO=OFF \
  -DWITH_RUST=OFF \
  -DWITH_ZLIB=OFF \
  -DWITH_LIBEVENT=OFF \
  -DWITH_OPENSSL=OFF \
  -DWITH_QT5=OFF

cmake --build "${WORKDIR}/build" -j"$(nproc)" --target thrift-compiler
sudo cmake --install "${WORKDIR}/build" --component Unspecified 2>/dev/null || \
  sudo cmake --install "${WORKDIR}/build"

# Some CMake layouts install as thrift-compiler; ensure `thrift` is on PATH.
if ! command -v thrift >/dev/null 2>&1; then
  if [[ -x "${PREFIX}/bin/thrift" ]]; then
    export PATH="${PREFIX}/bin:${PATH}"
  elif [[ -x "${WORKDIR}/build/compiler/cpp/thrift" ]]; then
    sudo cp "${WORKDIR}/build/compiler/cpp/thrift" "${PREFIX}/bin/thrift"
  elif [[ -x "${WORKDIR}/build/bin/thrift" ]]; then
    sudo cp "${WORKDIR}/build/bin/thrift" "${PREFIX}/bin/thrift"
  else
    # Fall back to thrift-compiler binary name.
    bin="$(find "${WORKDIR}/build" -type f -name 'thrift*' -perm -111 | head -1)"
    if [[ -z "${bin}" ]]; then
      echo "error: thrift compiler binary not found after build" >&2
      exit 1
    fi
    sudo cp "${bin}" "${PREFIX}/bin/thrift"
  fi
fi

thrift -version
