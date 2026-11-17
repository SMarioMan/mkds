#!/usr/bin/env python3
"""Deterministic staged build pipeline (M6) for MKDS Simple-Mode 32-Track HaxxStation Host ROM.

CLI contract:
  python3 scripts/haxxstation/build_all.py \
    --mkds-rom "$MKDS_ROM" \
    --station-rom "$STATION_ROM" \
    --region usa \
    --guests 1 \
    --out build/haxxstation/mkds-simple32-haxxstation-host.nds

Stages:
  1. Strict SHA-1 validation of untrimmed MKDS USA (`691e00d9...`) & Station Vol. 1 USA (`f18b55f3...`);
     stage stock Cloneboot fallback files + generate 15 reduced `D` archives (`30` `.carc` files).
  2. Build standalone `marker-probe.nds` and wrap `station-probe-host.nds` via pinned `dspatch`.
  3. Build `provisional-host.nds` with fixed final FAT count/layout and HaxxStation host C module.
  4. Derive executable-only `mkds-simple32-guest.nds` from `provisional-host.nds` + `host_nm.txt`,
     verify decoded ARM9 disassembly (`GetSelectCupGridCupEnabled` & `BuildCourseList`), update
     OVT/overlay HMAC-SHA1 digests, and wrap `station-mkds-guest-host.nds` via `dspatch`.
  5. Embed final `demomenu` + `rom0000d` + untouched `station_client.srl` into final single-cartridge
     MKDS Host ROM (`--out`), asserting 100% `ARM9`/`ARM7`/`ARM9_OVT`/`ov0..3`/`FAT` parity against
     `provisional-host.nds` and byte-for-byte preservation of Nintendo's signed Stage-1 SRL.
  6. Emit source-safe `reports/haxxstation/build_manifest.json` and `reports/haxxstation/memory_map.md`.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_station import audit_wrapped_station_package  # noqa: E402
from nitrofs_util import (  # noqa: E402
    EXPECTED_MKDS_USA_SHA1,
    EXPECTED_STATION_USA_SHA1,
    sha1_file,
    sha256_file,
)


def run_step(cmd: list[str], cwd: Path) -> None:
    print(f"[build_all] + {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mkds-rom", type=Path, required=True, help="Untrimmed 32 MiB retail MKDS USA ROM")
    ap.add_argument("--station-rom", type=Path, required=True, help="Unmodified DS Download Station Vol. 1 USA ROM")
    ap.add_argument("--region", choices=["usa"], default="usa", help="Target region (only usa / AMCE-00 supported)")
    ap.add_argument(
        "--guests",
        type=int,
        default=1,
        help="Target guest party count metadata (1..7; Host ROM dynamically supports 1..7 guests at runtime)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("build/haxxstation/mkds-simple32-haxxstation-host.nds"),
        help="Output path for final single-cartridge MKDS HaxxStation Host ROM",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=Path("reports/haxxstation/build_manifest.json"),
    )
    ap.add_argument("--skip-d-archives-if-present", action="store_true", default=True)
    args = ap.parse_args()

    if not (1 <= args.guests <= 7):
        raise SystemExit(f"ERROR: --guests must be in 1..7, got {args.guests}")

    repo_root = Path(__file__).resolve().parent.parent.parent
    mkds_rom = args.mkds_rom.resolve()
    station_rom = args.station_rom.resolve()
    out_rom = (repo_root / args.out) if not args.out.is_absolute() else args.out
    manifest_out = (repo_root / args.manifest_out) if not args.manifest_out.is_absolute() else args.manifest_out

    # 1. Strict input SHA-1 verification
    mkds_sha1 = sha1_file(mkds_rom)
    if mkds_sha1 != EXPECTED_MKDS_USA_SHA1:
        raise SystemExit(f"ERROR: MKDS USA SHA-1 mismatch: got {mkds_sha1}, expected {EXPECTED_MKDS_USA_SHA1}")
    station_sha1 = sha1_file(station_rom)
    if station_sha1 != EXPECTED_STATION_USA_SHA1:
        raise SystemExit(f"ERROR: Station USA SHA-1 mismatch: got {station_sha1}, expected {EXPECTED_STATION_USA_SHA1}")

    baserom_dest = repo_root / "extract" / "baserom_mkds_usa.nds"
    if not baserom_dest.is_file() or sha1_file(baserom_dest) != EXPECTED_MKDS_USA_SHA1:
        baserom_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(mkds_rom, baserom_dest)

    arm7_bios_dest = repo_root / "arm7_bios.bin"
    if not arm7_bios_dest.is_file():
        for cand in (mkds_rom.parent / "BIOSNDS7.ROM", Path("/work/BIOSNDS7.ROM")):
            if cand.is_file():
                shutil.copyfile(cand, arm7_bios_dest)
                break

    work_dir = repo_root / "build" / "haxxstation"
    work_dir.mkdir(parents=True, exist_ok=True)

    # Ensure base ROM extraction is up-to-date BEFORE staging D-archives & Station assets into extract/usa/
    run_step(["ninja", "extract/usa/config.yaml"], repo_root)

    run_step([sys.executable, "scripts/stage_stock_cloneboot.py"], repo_root)

    yoshi_d_data = repo_root / "extract" / "usa" / "files" / "data" / "Course" / "old_yoshi_gcDTex.carc"
    yoshi_d_haxx = repo_root / "extract" / "usa" / "files" / "haxx" / "Course" / "old_yoshi_gcDTex.carc"
    if not (args.skip_d_archives_if_present and (yoshi_d_data.is_file() or yoshi_d_haxx.is_file())):
        run_step([sys.executable, "scripts/build_simple32_d_archives.py"], repo_root)
        run_step([sys.executable, "scripts/stage_stock_cloneboot.py"], repo_root)

    # 2. Build standalone marker probe + dspatch Station probe host (M1 artifact & provisional seed)
    probe_nds = work_dir / "marker-probe.nds"
    probe_wrapped = work_dir / "station-probe-host.nds"
    dspatch_bin = shutil.which("dspatch") or "/opt/mkds-tools/bin/dspatch"
    run_step(
        [sys.executable, "scripts/haxxstation/build_probe.py", "--station-rom", str(station_rom), "--out", str(probe_nds)],
        repo_root,
    )
    run_step([dspatch_bin, "-s", str(station_rom), "-i", str(probe_nds), "-o", str(probe_wrapped)], repo_root)
    probe_audit = audit_wrapped_station_package(station_rom, probe_wrapped)

    # 3. Build provisional embedded MKDS Host ROM with fixed final FAT position & entry count
    provisional_host = work_dir / "provisional-host.nds"
    run_step(
        [
            sys.executable,
            "scripts/haxxstation/build_host.py",
            "--mkds-rom",
            str(mkds_rom),
            "--station-rom",
            str(station_rom),
            "--wrapped-station-rom",
            str(probe_wrapped),
            "--out",
            str(provisional_host),
        ],
        repo_root,
    )

    # 4. Derive modified executable-only MKDS Stage-2 guest & wrap via dspatch
    guest_nds = work_dir / "mkds-simple32-guest.nds"
    guest_wrapped = work_dir / "station-mkds-guest-host.nds"
    mem_map_md = repo_root / "reports" / "haxxstation" / "memory_map.md"
    run_step(
        [
            sys.executable,
            "scripts/haxxstation/build_mkds_client.py",
            "--host-rom",
            str(provisional_host),
            "--host-sym",
            str(repo_root / "build" / "usa" / "host_nm.txt"),
            "--mkds-rom",
            str(mkds_rom),
            "--station-rom",
            str(station_rom),
            "--out",
            str(guest_nds),
            "--wrapped-out",
            str(guest_wrapped),
            "--memory-map-out",
            str(mem_map_md),
        ],
        repo_root,
    )

    # 5. Build FINAL single-cartridge MKDS HaxxStation Host ROM & assert provisional parity
    run_step(
        [
            sys.executable,
            "scripts/haxxstation/build_host.py",
            "--mkds-rom",
            str(mkds_rom),
            "--station-rom",
            str(station_rom),
            "--wrapped-station-rom",
            str(guest_wrapped),
            "--assert-matches-provisional",
            str(provisional_host),
            "--out",
            str(out_rom),
        ],
        repo_root,
    )

    git_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo_root, text=True).strip()
    guest_meta = json.loads(guest_nds.with_suffix(".json").read_text())
    host_meta = json.loads(out_rom.with_suffix(".json").read_text())

    manifest = {
        "schema_version": 1,
        "region": args.region,
        "game_code": "AMCE-00",
        "git_base_head": git_head,
        "configured_guests_arg": args.guests,
        "dynamic_runtime_guest_support": "1..7",
        "bootstrap_mode_default": "haxxstation",
        "pinned_tools": {
            "dspatch_commit": "32856c4b68c8e45d90fe135d5555a4596fbd2955",
            "melonds_commit": "906e9ebb",
            "container_image": "localhost/mkds-simple32:latest",
        },
        "verified_inputs": {
            "mkds_usa_untrimmed_sha1": mkds_sha1,
            "download_station_vol1_usa_sha1": station_sha1,
        },
        "m1_probe_audit": probe_audit,
        "m2_guest_manifest": guest_meta,
        "m6_final_host_manifest": host_meta,
    }
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"[build_all] SUCCESS -> {out_rom} (sha256={sha256_file(out_rom)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
