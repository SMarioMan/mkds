#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

cmake --build build/dlp_harness
python3 scripts/run_simple32_validation.py "$@"
