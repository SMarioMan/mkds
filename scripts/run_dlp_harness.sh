#!/usr/bin/env bash
set -euo pipefail

cmake -B build/dlp_harness -S scripts/dlp_harness -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C build/dlp_harness

exec ./build/dlp_harness/dlp_harness "$@"
