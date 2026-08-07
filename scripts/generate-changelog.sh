#!/usr/bin/env bash
# generate-changelog.sh — append or refresh a CHANGELOG.md section from git.
#
# Usage:
#   ./scripts/generate-changelog.sh [--dry-run] [<X.Y.Z>]
#
# Without a version argument, writes/refreshes ## [Unreleased] using commits
# since the latest v* tag (or all commits if none).
#
# With a version, writes/refreshes ## [X.Y.Z] - <date> using commits since the
# previous v* tag (or all commits if this is the first release section).
#
# Idempotent: an existing section with the same heading is replaced in place
# rather than duplicated.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DRY_RUN=0
VERSION_ARG=""

usage() {
  cat <<'EOF'
Usage: generate-changelog.sh [--dry-run] [<X.Y.Z>]

  Appends or refreshes a CHANGELOG.md section from git log since the previous
  v* tag. Creates CHANGELOG.md with a Keep-a-Changelog-style header if missing.

  --dry-run   Print the section that would be written; do not modify files.
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

semver_re='^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$'

if [[ -n "$VERSION_ARG" ]]; then
  NEW_VERSION="${VERSION_ARG#v}"
  if [[ ! "$NEW_VERSION" =~ $semver_re ]]; then
    echo "error: invalid version '$NEW_VERSION'" >&2
    exit 1
  fi
  SECTION_TITLE="[$NEW_VERSION]"
  DATE_STAMP="$(date -u +%Y-%m-%d)"
  HEADING="## ${SECTION_TITLE} - ${DATE_STAMP}"
else
  NEW_VERSION=""
  SECTION_TITLE="[Unreleased]"
  HEADING="## ${SECTION_TITLE}"
fi

# Latest annotated/lightweight tag matching v*
latest_tag="$(git tag -l 'v*' --sort=-v:refname | head -n1 || true)"

if [[ -n "$NEW_VERSION" && -n "$latest_tag" ]]; then
  # If regenerating the same version as the latest tag, walk from the tag before it.
  if [[ "$latest_tag" == "v${NEW_VERSION}" ]]; then
    range_start="$(git tag -l 'v*' --sort=-v:refname | sed -n '2p' || true)"
  else
    range_start="$latest_tag"
  fi
elif [[ -z "$NEW_VERSION" && -n "$latest_tag" ]]; then
  range_start="$latest_tag"
else
  range_start=""
fi

if [[ -n "$range_start" ]]; then
  log_range="${range_start}..HEAD"
  echo "Collecting commits: $log_range"
else
  log_range="HEAD"
  echo "Collecting commits: full history (no prior v* tag)"
fi

# One bullet per subject (bash 3.2-compatible; no mapfile).
bullets=()
while IFS= read -r subject || [[ -n "$subject" ]]; do
  [[ -z "$subject" ]] && continue
  case "$subject" in
    "chore: bump version"*|"chore(release):"*|"Release v"*|"Bump version"*)
      continue
      ;;
  esac
  bullets+=("- ${subject}")
done < <(git log "$log_range" --pretty=format:'%s' --no-merges 2>/dev/null || true)

if [[ ${#bullets[@]} -eq 0 ]]; then
  bullets+=("- No user-facing changes recorded since ${range_start:-repository start}.")
fi

section="${HEADING}"$'\n'
for b in "${bullets[@]}"; do
  section+="${b}"$'\n'
done

header="$(cat <<'EOF'
# Changelog

All notable changes to Aster (server and clients) are documented in this file.
The project follows a single-version policy; see [docs/versioning.md](docs/versioning.md).
EOF
)"
# Ensure a blank line between intro and first section.
header+=$'\n'

CHANGELOG="$ROOT/CHANGELOG.md"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "----- section -----"
  printf '%s' "$section"
  echo "----- end -----"
  exit 0
fi

if [[ ! -f "$CHANGELOG" ]]; then
  printf '%s\n%s\n' "$header" "$section" >"$CHANGELOG"
  echo "created CHANGELOG.md with ${SECTION_TITLE}"
  exit 0
fi

# Replace existing section with the same title, or insert after header.
# Match ## [Unreleased] or ## [X.Y.Z] - date through the next ## or EOF.
python3 - "$CHANGELOG" "$SECTION_TITLE" "$section" "$header" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
title = sys.argv[2]  # e.g. [Unreleased] or [0.1.0]
new_section = sys.argv[3]
default_header = sys.argv[4]

text = path.read_text()
# Heading line: ## [title] optional " - date"
heading_re = re.compile(
    rf"^## {re.escape(title)}(?:\s+-\s+\d{{4}}-\d{{2}}-\d{{2}})?\s*$",
    re.MULTILINE,
)
# Section body until next ## heading or EOF
section_re = re.compile(
    rf"^## {re.escape(title)}(?:\s+-\s+\d{{4}}-\d{{2}}-\d{{2}})?\s*\n"
    rf"(?:.*?)(?=^## |\Z)",
    re.MULTILINE | re.DOTALL,
)

new_section = new_section if new_section.endswith("\n") else new_section + "\n"

if section_re.search(text):
    text = section_re.sub(lambda _: new_section, text, count=1)
else:
    # Insert after the first heading block (# Changelog ...)
    if not text.lstrip().startswith("#"):
        text = default_header + "\n" + text
    # Prefer Unreleased first; version sections after Unreleased or after intro.
    if title == "[Unreleased]":
        m = re.search(r"^# .*\n+(?:.*?\n)*?(?=^## |\Z)", text, re.MULTILINE)
        if m:
            insert_at = m.end()
            text = text[:insert_at] + new_section + ("" if text[insert_at:].startswith("\n") else "\n") + text[insert_at:]
        else:
            text = text.rstrip() + "\n\n" + new_section
    else:
        # After Unreleased section if present, else after intro header.
        unr = re.search(
            r"^## \[Unreleased\]\s*\n(?:.*?)(?=^## |\Z)",
            text,
            re.MULTILINE | re.DOTALL,
        )
        if unr:
            insert_at = unr.end()
            text = text[:insert_at] + "\n" + new_section + text[insert_at:]
        else:
            m = re.search(r"^# .*\n+(?:.*?\n)*?(?=^## |\Z)", text, re.MULTILINE)
            if m:
                insert_at = m.end()
                text = text[:insert_at] + new_section + text[insert_at:]
            else:
                text = text.rstrip() + "\n\n" + new_section

path.write_text(text if text.endswith("\n") else text + "\n")
print(f"updated CHANGELOG.md section {title}")
PY
