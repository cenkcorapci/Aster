#!/bin/sh
# BusyBox ash entrypoint: prepare durable volume, drop root, exec Aster.
set -eu

ASTER_BIN="${ASTER_BIN:-/usr/local/bin/aster}"
ASTER_DATA_DIR="${ASTER_DATA_DIR:-/data}"
SU_EXEC="${SU_EXEC:-/usr/local/sbin/su-exec}"

export ASTER_DATA_DIR
mkdir -p "$ASTER_DATA_DIR"

if [ "$#" -eq 0 ]; then
  set -- --data-dir "$ASTER_DATA_DIR"
fi

# Drop to the image user after fixing volume ownership. Prefer su-exec because
# BusyBox `su` swallows dashed argv (e.g. --help, --data-dir).
if [ "$(id -u)" = "0" ] && grep -q '^aster:' /etc/passwd 2>/dev/null; then
  chown -R aster:aster "$ASTER_DATA_DIR" || true
  if [ -x "$SU_EXEC" ]; then
    exec "$SU_EXEC" aster:aster "$ASTER_BIN" "$@"
  fi
fi

exec "$ASTER_BIN" "$@"
