#!/usr/bin/env sh
# Build and install Aster from a source snapshot.
set -eu

REPOSITORY="${ASTER_REPOSITORY:-cenkcorapci/Aster}"
REF="${ASTER_REF:-main}"
PREFIX="${PREFIX:-$HOME/.local}"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/aster-install.XXXXXX")"

cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

need() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'Missing required command: %s\n' "$1" >&2
    exit 1
  }
}

need curl
need tar
need c++

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) bazelisk_asset="bazelisk-darwin-arm64" ;;
  Darwin-x86_64) bazelisk_asset="bazelisk-darwin-amd64" ;;
  Linux-x86_64) bazelisk_asset="bazelisk-linux-amd64" ;;
  Linux-aarch64|Linux-arm64) bazelisk_asset="bazelisk-linux-arm64" ;;
  *)
    printf 'Unsupported platform: %s-%s\n' "$(uname -s)" "$(uname -m)" >&2
    exit 1
    ;;
esac

if command -v bazel >/dev/null 2>&1; then
  BAZEL="$(command -v bazel)"
else
  BAZEL="$WORK_DIR/bazelisk"
  curl -fsSL \
    "https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/${bazelisk_asset}" \
    -o "$BAZEL"
  chmod +x "$BAZEL"
fi

curl -fsSL \
  "https://github.com/${REPOSITORY}/archive/refs/heads/${REF}.tar.gz" \
  -o "$WORK_DIR/aster.tar.gz"
tar -xzf "$WORK_DIR/aster.tar.gz" -C "$WORK_DIR"
SOURCE_DIR="$(find "$WORK_DIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)"

(
  cd "$SOURCE_DIR"
  "$BAZEL" build -c opt //aster/cli:aster
)

mkdir -p "$PREFIX/bin"
install -m 755 "$SOURCE_DIR/bazel-bin/aster/cli/aster" "$PREFIX/bin/aster"
printf 'Installed Aster to %s/bin/aster\n' "$PREFIX"
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) printf 'Add %s/bin to PATH to run `aster` directly.\n' "$PREFIX" ;;
esac
