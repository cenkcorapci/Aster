#!/bin/sh
# BusyBox ash entrypoint: prepare durable volume, drop root, exec Aster.
#
# ASTER_MODE=demo   → /usr/local/bin/aster (default)
# ASTER_MODE=bench  → /usr/local/bin/aster-bench with env-derived flags
set -eu

ASTER_BIN="${ASTER_BIN:-/usr/local/bin/aster}"
ASTER_BENCH_BIN="${ASTER_BENCH_BIN:-/usr/local/bin/aster-bench}"
ASTER_DATA_DIR="${ASTER_DATA_DIR:-/data}"
ASTER_MODE="${ASTER_MODE:-demo}"
SU_EXEC="${SU_EXEC:-/usr/local/sbin/su-exec}"

# Refuse path escape when running as root (bench / docker safety).
case "$ASTER_DATA_DIR" in
  /data|/data/*) ;;
  *)
    if [ "$(id -u)" = "0" ]; then
      echo "error: ASTER_DATA_DIR must be /data or under it (got: $ASTER_DATA_DIR)" >&2
      exit 2
    fi
    ;;
esac

export ASTER_DATA_DIR
mkdir -p "$ASTER_DATA_DIR" /results

if [ "$#" -eq 0 ]; then
  if [ "$ASTER_MODE" = "bench" ]; then
    set -- \
      --data-dir "$ASTER_DATA_DIR" \
      --node-id "${ASTER_NODE_ID:-0}" \
      --id-base "${ASTER_ID_BASE:-0}" \
      --vectors "${ASTER_VECTORS:-100000}" \
      --dimension "${ASTER_DIMENSION:-16}" \
      --duration "${ASTER_DURATION:-120}" \
      --report-every "${ASTER_REPORT_EVERY:-5}" \
      --top-k "${ASTER_TOP_K:-10}" \
      --flush-every "${ASTER_FLUSH_EVERY:-500}"
    ASTER_BIN="$ASTER_BENCH_BIN"
  else
    set -- --data-dir "$ASTER_DATA_DIR"
  fi
fi

if [ "$(id -u)" = "0" ] && grep -q '^aster:' /etc/passwd 2>/dev/null; then
  chown -R aster:aster "$ASTER_DATA_DIR" /results || true
  if [ -x "$SU_EXEC" ]; then
    exec "$SU_EXEC" aster:aster "$ASTER_BIN" "$@"
  fi
  echo "error: su-exec missing; refusing to run as root" >&2
  exit 1
fi

exec "$ASTER_BIN" "$@"
