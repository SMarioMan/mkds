#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 scripts/stage_stock_cloneboot.py

if [[ -f scripts/build_simple32_d_archives.py ]] && [[ "${SKIP_D_ARCHIVES:-0}" != "1" ]]; then
    python3 scripts/build_simple32_d_archives.py
    python3 scripts/stage_stock_cloneboot.py
fi

rm -f build/usa/mkds_usa.nds build/usa/build/rom_config.yaml
ninja rom
arm-none-eabi-nm -n build/usa/arm9.o > build/usa/host_nm.txt
echo "[container_build_rom] Built build/usa/mkds_usa.nds ($(stat -c%s build/usa/mkds_usa.nds) bytes) and build/usa/host_nm.txt"
