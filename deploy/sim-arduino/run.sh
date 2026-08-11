#!/usr/bin/env bash
# Build Aster Tiny ESP32 firmware and run it under Espressif QEMU.
#
#   ./deploy/sim-arduino/run.sh           # build + QEMU expect ASTER_OK
#   ./deploy/sim-arduino/run.sh build     # firmware only
#   ./deploy/sim-arduino/run.sh native    # host smoke (no MCU)
#   ./deploy/sim-arduino/run.sh qemu      # QEMU only (expects prior build)
#
# Requires: Python 3, PlatformIO (`pio`), curl, tar.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="${ROOT}/deploy/sim-arduino"
CACHE="${ROOT}/.cache/sim-arduino"
BUILD_DIR="${HERE}/.pio/build/esp32dev"
QEMU_TAG="${ASTER_QEMU_TAG:-esp-develop-9.2.2-20260417}"
QEMU_VER="${ASTER_QEMU_VER:-esp_develop_9.2.2_20260417}"
EXPECT_TEXT="${ASTER_EXPECT_TEXT:-ASTER_OK}"
TIMEOUT_SEC="${ASTER_QEMU_TIMEOUT_SEC:-90}"
ENV_NAME="${ASTER_PIO_ENV:-esp32dev}"

C_GREEN=$'\033[32m'
C_RED=$'\033[31m'
C_RESET=$'\033[0m'

log() { printf '%s\n' "$*" >&2; }
ok() { printf '%s%s%s\n' "$C_GREEN" "$*" "$C_RESET" >&2; }
err() { printf '%s%s%s\n' "$C_RED" "$*" "$C_RESET" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    err "missing required command: $1"
    exit 1
  fi
}

ensure_pio() {
  local venv_pio="${HERE}/.venv/bin/pio"
  if [[ -x "$venv_pio" ]]; then
    # Prefer project venv (Homebrew Python is PEP 668–blocked).
    # shellcheck disable=SC1091
    source "${HERE}/.venv/bin/activate"
    return
  fi
  if command -v pio >/dev/null 2>&1; then
    return
  fi
  need_cmd python3
  log "Creating ${HERE}/.venv and installing PlatformIO + esptool"
  python3 -m venv "${HERE}/.venv"
  # shellcheck disable=SC1091
  source "${HERE}/.venv/bin/activate"
  python3 -m pip install -U pip platformio esptool
  need_cmd pio
}

host_triple() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "${os}/${arch}" in
    Darwin/arm64) echo "aarch64-apple-darwin" ;;
    Darwin/x86_64) echo "x86_64-apple-darwin" ;;
    Linux/aarch64|Linux/arm64) echo "aarch64-linux-gnu" ;;
    Linux/x86_64) echo "x86_64-linux-gnu" ;;
    *)
      err "unsupported host for Espressif QEMU: ${os}/${arch}"
      exit 1
      ;;
  esac
}

ensure_qemu() {
  local triple archive url dest bin
  triple="$(host_triple)"
  archive="qemu-xtensa-softmmu-${QEMU_VER}-${triple}.tar.xz"
  url="https://github.com/espressif/qemu/releases/download/${QEMU_TAG}/${archive}"
  dest="${CACHE}/qemu-${QEMU_VER}-${triple}"
  bin="${dest}/bin/qemu-system-xtensa"
  if [[ -x "$bin" ]]; then
    echo "$bin"
    return
  fi
  need_cmd curl
  need_cmd tar
  mkdir -p "$CACHE"
  log "Downloading Espressif QEMU (${triple})…"
  curl -fL --retry 3 -o "${CACHE}/${archive}" "$url"
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -xJf "${CACHE}/${archive}" -C "$dest" --strip-components=1
  if [[ ! -x "$bin" ]]; then
    # Some archives nest bin/ at top level already.
    bin="$(find "$dest" -type f -name qemu-system-xtensa | head -n1)"
  fi
  if [[ -z "${bin}" || ! -x "$bin" ]]; then
    err "qemu-system-xtensa not found after extract in ${dest}"
    exit 1
  fi
  echo "$bin"
}

pio_build() {
  local env="$1"
  ensure_pio
  log "Building PlatformIO env=${env}"
  (cd "$HERE" && pio run -e "$env")
}

merge_flash() {
  need_cmd python3
  local out="$1"
  local boot="${BUILD_DIR}/bootloader.bin"
  local parts="${BUILD_DIR}/partitions.bin"
  local app="${BUILD_DIR}/firmware.bin"
  for f in "$boot" "$parts" "$app"; do
    if [[ ! -f "$f" ]]; then
      err "missing build artifact: $f (run build first)"
      exit 1
    fi
  done
  mkdir -p "$(dirname "$out")"
  local merged="${out}.partial"
  # Prefer project venv esptool, then PlatformIO package, then module.
  if python3 -c 'import esptool' 2>/dev/null; then
    python3 -m esptool --chip esp32 merge-bin -o "$merged" \
      --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x1000 "$boot" \
      0x8000 "$parts" \
      0x10000 "$app"
  else
    local esptool_py
    esptool_py="$(find "${HOME}/.platformio/packages" -type f -name esptool.py 2>/dev/null | head -n1 || true)"
    if [[ -z "$esptool_py" ]]; then
      err "esptool not found; re-run ensure_pio / install esptool in .venv"
      exit 1
    fi
    python3 "$esptool_py" --chip esp32 merge_bin -o "$merged" \
      --flash_mode dio --flash_freq 40m --flash_size 4MB \
      0x1000 "$boot" \
      0x8000 "$parts" \
      0x10000 "$app"
  fi
  # Espressif QEMU requires an exact 2/4/8/16 MiB SPI flash image.
  local size=$((4 * 1024 * 1024))
  dd if=/dev/zero of="$out" bs=1 count=0 seek="$size" status=none
  dd if="$merged" of="$out" conv=notrunc status=none
  rm -f "$merged"
  ok "flash image → $out ($(wc -c <"$out") bytes)"
}

stop_qemu() {
  local pid="$1"
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return
  fi
  # SIGTERM first; wedged guests (post Guru Meditation) often ignore it.
  kill "$pid" 2>/dev/null || true
  local i
  for i in 1 2 3 4 5; do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return
    fi
    sleep 0.2
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

run_qemu_once() {
  local qemu_bin="$1"
  local flash="$2"
  local logf="$3"
  local pid elapsed

  rm -f "$logf"
  # -nic none: openeth ISR + flash ops race in QEMU (cache-disabled panic).
  # wdt_disable: Espressif docs recommend this for non-interactive boots.
  "$qemu_bin" \
    -machine esp32 \
    -nographic \
    -nic none \
    -global driver=timer.esp32.timg,property=wdt_disable,value=true \
    -drive file="${flash}",if=mtd,format=raw \
    >"$logf" 2>&1 &
  pid=$!

  elapsed=0
  while (( elapsed < TIMEOUT_SEC )); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      break
    fi
    if grep -q "${EXPECT_TEXT}" "$logf" 2>/dev/null; then
      stop_qemu "$pid"
      return 0
    fi
    if grep -q 'ASTER_FAIL' "$logf" 2>/dev/null; then
      stop_qemu "$pid"
      return 2
    fi
    # Guest already panicked — retry instead of burning the full timeout.
    if grep -Eqi 'Guru Meditation|panic.ed|Cache disabled but cached' "$logf" 2>/dev/null; then
      stop_qemu "$pid"
      return 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  stop_qemu "$pid"
  return 1
}

run_qemu() {
  local qemu_bin flash logf attempt max_attempts rc
  # Activate project venv so merge_flash can `python3 -m esptool`.
  ensure_pio
  qemu_bin="$(ensure_qemu)"
  if ! "$qemu_bin" --version >/dev/null 2>&1; then
    err "Espressif QEMU failed to start. On macOS install deps:"
    err "  brew install libgcrypt sdl2 glib gettext pixman jpeg libpng snappy libslirp vde libssh"
    "$qemu_bin" --version
    exit 1
  fi
  flash="${CACHE}/esp32-flash.bin"
  logf="${CACHE}/qemu-serial.log"
  merge_flash "$flash"

  max_attempts="${ASTER_QEMU_RETRIES:-3}"
  for attempt in $(seq 1 "$max_attempts"); do
    log "Starting QEMU attempt ${attempt}/${max_attempts} (timeout ${TIMEOUT_SEC}s, expect '${EXPECT_TEXT}')…"
    set +e
    run_qemu_once "$qemu_bin" "$flash" "$logf"
    rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
      ok "QEMU serial matched ${EXPECT_TEXT}"
      grep -E 'ASTER_|aster-sim' "$logf" || true
      return 0
    fi
    if [[ "$rc" -eq 2 ]]; then
      err "firmware reported ASTER_FAIL"
      grep 'ASTER_FAIL' "$logf" >&2 || true
      exit 1
    fi
    err "attempt ${attempt} did not observe '${EXPECT_TEXT}'"
    tail -n 40 "$logf" >&2 || true
  done

  err "gave up after ${max_attempts} QEMU attempts"
  exit 1
}

cmd="${1:-all}"
case "$cmd" in
  build)
    pio_build "$ENV_NAME"
    ;;
  native)
    pio_build native
    # PlatformIO native produces a runnable binary under .pio/build/native/
    bin="$(find "${HERE}/.pio/build/native" -maxdepth 1 -type f -perm -111 ! -name '*.*' | head -n1 || true)"
    if [[ -z "$bin" ]]; then
      bin="$(find "${HERE}/.pio/build/native" -type f -name 'program' | head -n1 || true)"
    fi
    if [[ -z "$bin" || ! -x "$bin" ]]; then
      err "native binary not found under ${HERE}/.pio/build/native"
      exit 1
    fi
    log "Running native smoke: $bin"
    "$bin"
    ok "native smoke passed"
    ;;
  qemu)
    run_qemu
    ;;
  all|start)
    pio_build "$ENV_NAME"
    run_qemu
    ;;
  stop|clean)
    rm -rf "${HERE}/.pio" "${CACHE}/esp32-flash.bin" "${CACHE}/qemu-serial.log"
    ok "cleaned PlatformIO build + flash/log cache (QEMU toolchain kept in ${CACHE})"
    ;;
  *)
    err "usage: $0 [all|build|native|qemu|clean]"
    exit 2
    ;;
esac
