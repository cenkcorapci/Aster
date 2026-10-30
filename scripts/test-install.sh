#!/usr/bin/env bash
# Offline integration test for scripts/install.sh, including Bazelisk fallback.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/aster-install-test.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

FAKE_BIN="${WORK_DIR}/bin"
PREFIX="${WORK_DIR}/prefix"
mkdir -p "$FAKE_BIN"

cat >"${FAKE_BIN}/uname" <<'EOF'
#!/usr/bin/env sh
case "$1" in
  -s) echo Linux ;;
  -m) echo x86_64 ;;
esac
EOF

cat >"${FAKE_BIN}/curl" <<'EOF'
#!/usr/bin/env sh
set -eu
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-o" ]; then
    output="$2"
    shift 2
  else
    shift
  fi
done
case "$output" in
  *bazelisk)
    cat >"$output" <<'BAZEL'
#!/usr/bin/env sh
exit 0
BAZEL
    chmod +x "$output"
    ;;
  *) : >"$output" ;;
esac
EOF

cat >"${FAKE_BIN}/tar" <<'EOF'
#!/usr/bin/env sh
set -eu
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-C" ]; then
    destination="$2"
    shift 2
  else
    shift
  fi
done
mkdir -p "$destination/Aster-main/bazel-bin/aster/cli"
cat >"$destination/Aster-main/bazel-bin/aster/cli/aster" <<'ASTER'
#!/usr/bin/env sh
echo aster
ASTER
chmod +x "$destination/Aster-main/bazel-bin/aster/cli/aster"
EOF

cat >"${FAKE_BIN}/c++" <<'EOF'
#!/usr/bin/env sh
exit 0
EOF
chmod +x "${FAKE_BIN}/"*

# Restrict PATH so a host Bazel cannot bypass the Bazelisk fallback under test.
PATH="${FAKE_BIN}:/usr/bin:/bin" PREFIX="$PREFIX" "$ROOT/scripts/install.sh"
test -x "$PREFIX/bin/aster"
test "$("$PREFIX/bin/aster")" = "aster"
echo "ok"
