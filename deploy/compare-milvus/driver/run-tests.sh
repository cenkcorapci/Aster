#!/usr/bin/env bash
# Offline test suite for the comparison driver. Run after installing requirements.txt.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

for test_file in \
  test_metrics.py \
  test_workload.py \
  test_aster_client.py \
  test_qdrant_backend.py \
  test_report.py \
  test_compare.py; do
  python3 "$test_file"
done
