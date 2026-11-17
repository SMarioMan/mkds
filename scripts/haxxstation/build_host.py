#!/usr/bin/env python3
"""Stage untouched Station Stage-1 + wrapped Stage-2 assets into `/haxx/` NitroFS and build Host ROM.

Implements Milestone M3 & M6 host-packaging invariants:
  1. Verifies exact SHA-1 of user-provided untrimmed MKDS USA (`AMCE-00`) and Station Vol. 1 USA.
  2. Verifies `mb/ds_demo_client.srl` and `mb/ds_demo_client.sgn` in `--wrapped-station-rom` are
     byte-for-byte identical to the original Station ROM (`audit_wrapped_station_package`).
  3. Relocates all custom added files (`mb_child_*`, `station_*`, and the 30 generated `D` archives)
     under `extract/usa/files/haxx/` (alphabetically after `/data` and `/dwc`), keeping `data/` and
     `dwc/` 100% pure retail so ALL `610` retail MKDS USA file IDs (`0..609`) remain untouched!
  4. Rebuilds `build/usa/mkds_usa.nds` + `build/usa/host_nm.txt` via `ninja rom`, writes `--out`,
     and verifies embedded NitroFS file IDs, `D`-archive redirect targets, Stage-1 byte identity,
     and (when `--assert-matches-provisional` is given) byte-for-byte parity of `ARM9`, `ARM7`,
     `ARM9_OVT`, `FAT` entry count, `FNT`, and `ov000..ov003` against the provisional Host ROM.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_station import audit_wrapped_station_package  # noqa: E402
from nitrofs_util import (  # noqa: E402
    EXPECTED_MKDS_USA_SHA1,
    EXPECTED_STATION_SGN_SHA256,
    EXPECTED_STATION_SRL_SHA256,
    EXPECTED_STATION_USA_SHA1,
    list_nitrofs_files,
    read_nitrofs_file,
    sha1_file,
    sha256_bytes,
    sha256_file,
)

REDIRECTED_D_ARCHIVE_PAIRS = [
    ("data/Course/desert_course.carc", "haxx/Course/desert_courseD.carc"),
    ("data/Course/desert_courseTex.carc", "haxx/Course/desert_courseDTex.carc"),
    ("data/Course/town_course.carc", "haxx/Course/town_courseD.carc"),
    ("data/Course/town_courseTex.carc", "haxx/Course/town_courseDTex.carc"),
    ("data/Course/pinball_course.carc", "haxx/Course/pinball_courseD.carc"),
    ("data/Course/pinball_courseTex.carc", "haxx/Course/pinball_courseDTex.carc"),
    ("data/Course/ridge_course.carc", "haxx/Course/ridge_courseD.carc"),
    ("data/Course/ridge_courseTex.carc", "haxx/Course/ridge_courseDTex.carc"),
    ("data/Course/snow_course.carc", "haxx/Course/snow_courseD.carc"),
    ("data/Course/snow_courseTex.carc", "haxx/Course/snow_courseDTex.carc"),
    ("data/Course/mario_course.carc", "haxx/Course/mario_courseD.carc"),
    ("data/Course/mario_courseTex.carc", "haxx/Course/mario_courseDTex.carc"),
    ("data/Course/airship_course.carc", "haxx/Course/airship_courseD.carc"),
    ("data/Course/airship_courseTex.carc", "haxx/Course/airship_courseDTex.carc"),
    ("data/Course/stadium_course.carc", "haxx/Course/stadium_courseD.carc"),
    ("data/Course/stadium_courseTex.carc", "haxx/Course/stadium_courseDTex.carc"),
    ("data/Course/garden_course.carc", "haxx/Course/garden_courseD.carc"),
    ("data/Course/garden_courseTex.carc", "haxx/Course/garden_courseDTex.carc"),
    ("data/Course/koopa_course.carc", "haxx/Course/koopa_courseD.carc"),
    ("data/Course/koopa_courseTex.carc", "haxx/Course/koopa_courseDTex.carc"),
    ("data/Course/rainbow_course.carc", "haxx/Course/rainbow_courseD.carc"),
    ("data/Course/rainbow_courseTex.carc", "haxx/Course/rainbow_courseDTex.carc"),
    ("data/Course/old_donut_sfc.carc", "haxx/Course/old_donut_sfcD.carc"),
    ("data/Course/old_donut_sfcTex.carc", "haxx/Course/old_donut_sfcDTex.carc"),
    ("data/Course/old_luigi_agb.carc", "haxx/Course/old_luigi_agbD.carc"),
    ("data/Course/old_luigi_agbTex.carc", "haxx/Course/old_luigi_agbDTex.carc"),
    ("data/Course/old_kinoko_gc.carc", "haxx/Course/old_kinoko_gcD.carc"),
    ("data/Course/old_kinoko_gcTex.carc", "haxx/Course/old_kinoko_gcDTex.carc"),
    ("data/Course/old_yoshi_gc.carc", "haxx/Course/old_yoshi_gcD.carc"),
    ("data/Course/old_yoshi_gcTex.carc", "haxx/Course/old_yoshi_gcDTex.carc"),
]


def stage_into_haxx_tree(extract_usa: Path, retail_rel_paths: set[str]) -> list[str]:
    """Move any non-retail files out of `data/` into `haxx/` and update `path_order.txt`."""
    files_root = extract_usa / "files"
    haxx_root = files_root / "haxx"
    haxx_course = haxx_root / "Course"
    haxx_course.mkdir(parents=True, exist_ok=True)

    boot_dir = files_root / "data" / "Boot"
    if boot_dir.is_dir():
        for p in sorted(boot_dir.iterdir()):
            if p.is_file() and f"data/Boot/{p.name}" not in retail_rel_paths:
                shutil.move(str(p), str(haxx_root / p.name))

    course_dir = files_root / "data" / "Course"
    if course_dir.is_dir():
        for p in sorted(course_dir.iterdir()):
            if p.is_file() and f"data/Course/{p.name}" not in retail_rel_paths:
                shutil.move(str(p), str(haxx_course / p.name))

    path_order = extract_usa / "path_order.txt"
    stock_order = extract_usa / "path_order.stock.txt"
    if not stock_order.exists():
        shutil.copyfile(path_order, stock_order)

    base_lines = [ln.strip() for ln in stock_order.read_text().splitlines() if ln.strip()]
    appended: list[str] = []
    for p in sorted(haxx_root.rglob("*")):
        if p.is_file():
            appended.append("/" + p.relative_to(files_root).as_posix())

    path_order.write_text("\n".join(base_lines + appended) + "\n")
    return appended


def extract_exec_and_ovt_summary(rom: bytes) -> dict[str, object]:
    arm9_off, arm9_entry, arm9_ram, arm9_size, arm7_off, arm7_entry, arm7_ram, arm7_size = struct.unpack_from(
        "<8I", rom, 0x20
    )
    fnt_off, fnt_size, fat_off, fat_size, ovt9_off, ovt9_size = struct.unpack_from("<6I", rom, 0x40)
    ovt_bytes = rom[ovt9_off : ovt9_off + ovt9_size]
    ov_hashes = []
    for i in range(4):
        _, _, _, _, _, _, fid, _ = struct.unpack_from("<8I", ovt_bytes, i * 32)
        s_off, e_off = struct.unpack_from("<II", rom[fat_off + fid * 8 : fat_off + (fid + 1) * 8])
        ov_hashes.append(sha256_bytes(rom[s_off:e_off]))
    return {
        "arm9_entry": arm9_entry,
        "arm9_ram": arm9_ram,
        "arm9_size": arm9_size,
        "arm9_sha256": sha256_bytes(rom[arm9_off : arm9_off + arm9_size]),
        "arm7_entry": arm7_entry,
        "arm7_ram": arm7_ram,
        "arm7_size": arm7_size,
        "arm7_sha256": sha256_bytes(rom[arm7_off : arm7_off + arm7_size]),
        "fnt_size": fnt_size,
        "fnt_sha256": sha256_bytes(rom[fnt_off : fnt_off + fnt_size]),
        "fat_entry_count": fat_size // 8,
        "ovt9_size": ovt9_size,
        "ovt9_sha256": sha256_bytes(ovt_bytes),
        "ov0_to_3_sha256": ov_hashes,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mkds-rom", type=Path, required=True, help="Untrimmed retail MKDS USA 32 MiB ROM")
    ap.add_argument("--station-rom", type=Path, required=True, help="Unmodified DS Download Station Vol. 1 USA ROM")
    ap.add_argument("--wrapped-station-rom", type=Path, required=True, help="dspatch-produced Station package")
    ap.add_argument("--out", type=Path, required=True, help="Output path for built integrated MKDS Host ROM")
    ap.add_argument(
        "--assert-matches-provisional",
        type=Path,
        help="Optional provisional Host ROM whose ARM9/ARM7/OVT/overlays/FAT-count must match final output",
    )
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent.parent

    mkds_sha1 = sha1_file(args.mkds_rom)
    if mkds_sha1 != EXPECTED_MKDS_USA_SHA1:
        raise SystemExit(f"ERROR: MKDS USA SHA-1 mismatch: got {mkds_sha1}, expected {EXPECTED_MKDS_USA_SHA1}")

    st_sha1 = sha1_file(args.station_rom)
    if st_sha1 != EXPECTED_STATION_USA_SHA1:
        raise SystemExit(f"ERROR: Station USA SHA-1 mismatch: got {st_sha1}, expected {EXPECTED_STATION_USA_SHA1}")

    wrap_audit = audit_wrapped_station_package(args.station_rom, args.wrapped_station_rom)
    if not wrap_audit["signed_stage1_byte_identical"]:
        raise SystemExit("ERROR: Wrapped Station package failed Stage-1 byte-identity audit!")

    orig_mkds_bytes = args.mkds_rom.read_bytes()
    orig_files = list_nitrofs_files(orig_mkds_bytes)
    station_bytes = args.station_rom.read_bytes()
    wrapped_bytes = args.wrapped_station_rom.read_bytes()

    orig_srl = read_nitrofs_file(station_bytes, "mb/ds_demo_client.srl")
    orig_sgn = read_nitrofs_file(station_bytes, "mb/ds_demo_client.sgn")
    assert sha256_bytes(orig_srl) == EXPECTED_STATION_SRL_SHA256
    assert sha256_bytes(orig_sgn) == EXPECTED_STATION_SGN_SHA256
    assert orig_srl[-0x88:] == orig_sgn[:0x88]

    demomenu_bytes = read_nitrofs_file(wrapped_bytes, "ds_demo/demomenu")
    rom0000d_bytes = read_nitrofs_file(wrapped_bytes, "ds_demo/rom0000d")

    extract_usa = repo_root / "extract" / "usa"
    haxx_dir = extract_usa / "files" / "haxx"
    haxx_dir.mkdir(parents=True, exist_ok=True)

    (haxx_dir / "station_client.srl").write_bytes(orig_srl)
    (haxx_dir / "station_demomenu.bin").write_bytes(demomenu_bytes)
    (haxx_dir / "station_rom0000d.bin").write_bytes(rom0000d_bytes)

    appended_paths = stage_into_haxx_tree(extract_usa, set(orig_files.keys()))

    built_rom = repo_root / "build" / "usa" / "mkds_usa.nds"
    rom_cfg = repo_root / "build" / "usa" / "build" / "rom_config.yaml"
    for stale in (built_rom, rom_cfg):
        if stale.exists():
            stale.unlink()

    subprocess.run(["ninja", "rom"], cwd=repo_root, check=True)
    nm_txt = subprocess.check_output(["arm-none-eabi-nm", "-n", "build/usa/arm9.o"], cwd=repo_root, text=True)
    (repo_root / "build" / "usa" / "host_nm.txt").write_text(nm_txt)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built_rom, args.out)

    out_bytes = args.out.read_bytes()
    out_files = list_nitrofs_files(out_bytes)

    # 1. Verify every original retail MKDS file path & file ID (0..609) is preserved 100%!
    for rel_path, (orig_fid, orig_s, orig_e) in orig_files.items():
        if rel_path not in out_files:
            raise AssertionError(f"Missing original MKDS NitroFS file in Host ROM: {rel_path}")
        new_fid, new_s, new_e = out_files[rel_path]
        if new_fid != orig_fid:
            raise AssertionError(f"Original file ID shifted for {rel_path}: {orig_fid} -> {new_fid}")
        if out_bytes[new_s:new_e] != orig_mkds_bytes[orig_s:orig_e]:
            raise AssertionError(f"Original retail asset bytes altered for {rel_path} (fid={orig_fid})!")

    # 2. Verify all 30 D-archive redirects exist with valid non-zero extents!
    d_redirect_manifest = []
    for src_rel, dst_rel in REDIRECTED_D_ARCHIVE_PAIRS:
        if src_rel not in out_files or dst_rel not in out_files:
            raise AssertionError(f"Missing D-archive redirect path: {src_rel} -> {dst_rel}")
        src_fid, src_s, src_e = out_files[src_rel]
        dst_fid, dst_s, dst_e = out_files[dst_rel]
        assert dst_e > dst_s, f"Empty D-archive {dst_rel}"
        d_redirect_manifest.append(
            {
                "src_path": "/" + src_rel,
                "src_fid": src_fid,
                "src_size": src_e - src_s,
                "dst_path": "/" + dst_rel,
                "dst_fid": dst_fid,
                "dst_size": dst_e - dst_s,
            }
        )

    # 3. Verify embedded Station Stage-1 SRL is byte-for-byte identical to untouched Station ROM!
    emb_srl = read_nitrofs_file(out_bytes, "haxx/station_client.srl")
    emb_menu = read_nitrofs_file(out_bytes, "haxx/station_demomenu.bin")
    emb_rom0 = read_nitrofs_file(out_bytes, "haxx/station_rom0000d.bin")
    assert emb_srl == orig_srl, "Embedded station_client.srl is NOT byte-identical to Station ROM!"
    assert emb_menu == demomenu_bytes, "Embedded station_demomenu.bin mismatch!"
    assert emb_rom0 == rom0000d_bytes, "Embedded station_rom0000d.bin mismatch!"

    final_exec = extract_exec_and_ovt_summary(out_bytes)
    provisional_match = None
    if args.assert_matches_provisional is not None:
        prov_exec = extract_exec_and_ovt_summary(args.assert_matches_provisional.read_bytes())
        if final_exec != prov_exec:
            raise AssertionError(
                f"Final Host executable/OVT/overlay/FAT layout diverged from provisional Host!\n"
                f"Provisional: {json.dumps(prov_exec, indent=2)}\n"
                f"Final:       {json.dumps(final_exec, indent=2)}"
            )
        provisional_match = True

    added_file_ids = {}
    for rel in appended_paths:
        key = rel.lstrip("/")
        if key in out_files:
            fid, s_off, e_off = out_files[key]
            added_file_ids[rel] = {"file_id": fid, "size": e_off - s_off, "sha256": sha256_bytes(out_bytes[s_off:e_off])}

    summary = {
        "host_rom": str(args.out),
        "host_rom_size": len(out_bytes),
        "host_rom_sha256": sha256_file(args.out),
        "game_code": bytes(out_bytes[0x0C:0x10]).decode("ascii"),
        "signed_stage1_srl_fid": out_files["haxx/station_client.srl"][0],
        "signed_stage1_srl_sha256": sha256_bytes(emb_srl),
        "signed_stage1_byte_identical": True,
        "station_demomenu_fid": out_files["haxx/station_demomenu.bin"][0],
        "station_demomenu_size": len(emb_menu),
        "station_demomenu_sha256": sha256_bytes(emb_menu),
        "station_rom0000d_fid": out_files["haxx/station_rom0000d.bin"][0],
        "station_rom0000d_size": len(emb_rom0),
        "station_rom0000d_sha256": sha256_bytes(emb_rom0),
        "original_retail_files_verified": len(orig_files),
        "d_archive_redirects_verified": len(d_redirect_manifest),
        "provisional_layout_match": provisional_match,
        "exec_and_ovt": final_exec,
        "added_file_ids": added_file_ids,
    }
    args.out.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
