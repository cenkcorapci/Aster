#!/usr/bin/env bash
# bump-version.sh — sync Aster's single version across known package files.
#
# Usage:
#   ./scripts/bump-version.sh [--dry-run] <X.Y.Z>
#   ./scripts/bump-version.sh [--dry-run]          # use VERSION file as target
#
# Updates (in order):
#   VERSION
#   MODULE.bazel                 module(version = "…")
#   clients/python/pyproject.toml
#   clients/rust/Cargo.toml      [package] version only
#   clients/javascript/package.json
#
# Idempotent: re-running with the same version leaves content unchanged
# (modulo whitespace already matching). Exits 0 when already in sync.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DRY_RUN=0
VERSION_ARG=""

usage() {
  cat <<'EOF'
Usage: bump-version.sh [--dry-run] [<X.Y.Z>]

  Bumps VERSION and known client/Bazel version fields to the given semver.
  If no version is passed, reads the current VERSION file (useful to re-sync).

  --dry-run   Print actions without writing files.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      if [[ -n "$VERSION_ARG" ]]; then
        echo "error: unexpected argument: $1" >&2
        usage >&2
        exit 2
      fi
      VERSION_ARG="$1"
      shift
      ;;
  esac
done

# Semver core + optional pre-release / build metadata (no leading v).
semver_re='^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$'

if [[ -n "$VERSION_ARG" ]]; then
  NEW_VERSION="${VERSION_ARG#v}"
else
  if [[ ! -f VERSION ]]; then
    echo "error: VERSION file missing; pass an explicit version" >&2
    exit 1
  fi
  NEW_VERSION="$(tr -d '[:space:]' < VERSION)"
fi

if [[ ! "$NEW_VERSION" =~ $semver_re ]]; then
  echo "error: invalid version '$NEW_VERSION' (expected X.Y.Z or pre-release)" >&2
  exit 1
fi

write_file() {
  local path="$1"
  local content="$2"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "dry-run: would write $path"
    return 0
  fi
  # Atomic-ish write for CI friendliness.
  local tmp
  tmp="$(mktemp "${path}.XXXXXX")"
  printf '%s' "$content" >"$tmp"
  mv "$tmp" "$path"
}

replace_in_file() {
  local path="$1"
  local perl_expr="$2"
  if [[ ! -f "$path" ]]; then
    echo "error: missing file: $path" >&2
    exit 1
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "dry-run: would update $path -> $NEW_VERSION"
    return 0
  fi
  perl -i -0pe "$perl_expr" "$path"
}

echo "Bumping Aster version -> $NEW_VERSION"

# 1. VERSION (single line, trailing newline)
write_file VERSION "${NEW_VERSION}"$'\n'
echo "  VERSION"

# 2. MODULE.bazel — only the aster module() version=, not bazel_dep versions.
replace_in_file MODULE.bazel \
  's/(module\(\s*\n\s*name\s*=\s*"aster",\s*\n\s*version\s*=\s*")[^"]*(")/${1}'"$NEW_VERSION"'${2}/'
echo "  MODULE.bazel"

# 3. Python pyproject.toml — [project] version =
replace_in_file clients/python/pyproject.toml \
  's/(?m)^version\s*=\s*"[^"]*"/version = "'"$NEW_VERSION"'"/'
echo "  clients/python/pyproject.toml"

# 4. Rust Cargo.toml — first version = under [package] (file is small / single package)
replace_in_file clients/rust/Cargo.toml \
  's/(?m)^version\s*=\s*"[^"]*"/version = "'"$NEW_VERSION"'"/'
echo "  clients/rust/Cargo.toml"

# 5. JavaScript package.json — top-level "version"
replace_in_file clients/javascript/package.json \
  's/("version"\s*:\s*")[^"]*(")/${1}'"$NEW_VERSION"'${2}/'
echo "  clients/javascript/package.json"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "dry-run complete (no files written)"
else
  echo "done: all known packages at $NEW_VERSION"
fi
