#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "=== [1/3] Building dlp_harness against pinned melonDS core ==="
rm -rf build/dlp_harness
cmake -S scripts/dlp_harness -B build/dlp_harness -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/dlp_harness

echo "=== [2/3] Building & running C logic unit tests ==="
mkdir -p build/tests
gcc -O2 -Wall -Wextra -Werror tests/test_simple32_logic.c -o build/tests/test_simple32_logic
./build/tests/test_simple32_logic

echo "=== [3/3] Staging stock Cloneboot, building 15 D-archives, and linking MKDS Host ROM ==="
./scripts/container_build_rom.sh
