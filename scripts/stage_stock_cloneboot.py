#!/usr/bin/env python3
"""Stage untouched retail USA MKDS Cloneboot child image and ARM9 overlays into NitroFS.

Reads user-supplied gitignored `extract/baserom_mkds_usa.nds` (asserting exact retail USA
SHA-1 691e00d9a5dd80b04f80cc7559503e8b06848785) and writes gitignored files under
`extract/usa/files/data/Boot/`:
  - mb_child_usa.bin  (0x1100c0 = 1,114,304 bytes: stock header w/ mUnk_60 |= 0x00406000,
                       stock 0x88-byte RSA-1024 trailer from 0x1860B9E, stock 0xe7534-byte
                       Cloneboot ARM9 image, and stock 0x289a4-byte ARM7 image)
  - mb_child_ovt.bin  (128 bytes: stock ARM9 overlay table)
  - mb_child_ov0.bin .. mb_child_ov3.bin (stock compressed ARM9 overlays 0..3)

Also updates `extract/usa/path_order.txt` idempotently from `extract/usa/path_order.stock.txt`
so all original retail NitroFS file IDs remain unchanged.
"""

from __future__ import annotations

import hashlib
import shutil
import struct
import sys
from pathlib import Path

EXPECTED_USA_SHA1 = "691e00d9a5dd80b04f80cc7559503e8b06848785"
STOCK_D_ARCHIVES = {
    "/data/Course/beach_courseD.carc",
    "/data/Course/beach_courseDTex.carc",
    "/data/Course/cross_courseD.carc",
    "/data/Course/cross_courseDTex.carc",
    "/data/Course/mansion_courseD.carc",
    "/data/Course/mansion_courseDTex.carc",
    "/data/Course/old_baby_gcD.carc",
    "/data/Course/old_baby_gcDTex.carc",
    "/data/Course/old_koopa_agbD.carc",
    "/data/Course/old_koopa_agbDTex.carc",
    "/data/Course/old_luigi_gcD.carc",
    "/data/Course/old_luigi_gcDTex.carc",
    "/data/Course/old_mario_sfcD.carc",
    "/data/Course/old_mario_sfcDTex.carc",
    "/data/Course/old_momo_64D.carc",
    "/data/Course/old_momo_64DTex.carc",
}


def update_path_order(extract_usa: Path) -> list[str]:
    path_order = extract_usa / "path_order.txt"
    stock_order = extract_usa / "path_order.stock.txt"
    if not stock_order.exists():
        shutil.copyfile(path_order, stock_order)

    base_lines = [ln.strip() for ln in stock_order.read_text().splitlines() if ln.strip()]
    known = set(base_lines) | STOCK_D_ARCHIVES

    files_root = extract_usa / "files"
    appended: list[str] = []

    boot_dir = files_root / "data" / "Boot"
    if boot_dir.is_dir():
        for p in sorted(boot_dir.iterdir()):
            if p.is_file() and p.name.startswith("mb_child_"):
                rel = "/" + p.relative_to(files_root).as_posix()
                if rel not in known:
                    appended.append(rel)
                    known.add(rel)

    course_dir = files_root / "data" / "Course"
    if course_dir.is_dir():
        for p in sorted(course_dir.iterdir()):
            if p.is_file():
                rel = "/" + p.relative_to(files_root).as_posix()
                if rel not in known:
                    appended.append(rel)
                    known.add(rel)

    path_order.write_text("\n".join(base_lines + appended) + "\n")
    return appended


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    baserom_path = repo_root / "extract" / "baserom_mkds_usa.nds"
    extract_usa = repo_root / "extract" / "usa"

    if not baserom_path.is_file():
        print(f"ERROR: Missing {baserom_path}", file=sys.stderr)
        return 1

    rom = baserom_path.read_bytes()
    rom_sha1 = hashlib.sha1(rom).hexdigest()
    if rom_sha1 != EXPECTED_USA_SHA1:
        print(
            f"ERROR: baserom SHA-1 mismatch: got {rom_sha1}, expected {EXPECTED_USA_SHA1}",
            file=sys.stderr,
        )
        return 1

    arm9_off, arm9_entry, arm9_ram, arm9_size, arm7_off, arm7_entry, arm7_ram, arm7_size = (
        struct.unpack("<8I", rom[0x20:0x40])
    )
    fat_off, fat_size, ovt9_off, ovt9_size = struct.unpack("<4I", rom[0x48:0x58])
    rsa_off = struct.unpack("<I", rom[0x80:0x84])[0]
    if rsa_off == 0:
        rsa_off = 0x01000000

    assert arm9_off == 0x4000 and arm9_ram == 0x02000000 and arm9_size == 0xE7534
    assert arm7_off == 0x141600 and arm7_ram == 0x02380000 and arm7_size == 0x289A4
    assert ovt9_size == 0x80

    # 1. Exact 0x160-byte DS header prepared by func_02008570 (mUnk_60 |= 0x00406000)
    hdr = bytearray(rom[0x000:0x160])
    unk60 = struct.unpack("<I", hdr[0x60:0x64])[0] | 0x00406000
    hdr[0x60:0x64] = struct.pack("<I", unk60)

    # 2. Exact 0x88-byte Nintendo RSA-1024 Wireless Multiboot signature trailer
    rsa_sig = rom[rsa_off : rsa_off + 0x88]

    # 3. Exact 0xe7534-byte Cloneboot ARM9 payload:
    #    - [0x0000..0x0800): zeroed on disk; copied at runtime from live Host RAM 0x02000000..0x02000800
    #      so DS BIOS Key1 decrypted Secure Area ("encryObj") matches hardware & emulator identically.
    #    - [0x0800..0x1000): untouched retail 0x4800..0x5000 with AutoloadCallback (0x0a58) = 0xe12fff1e (bx lr).
    #    - [0x1000..0x3000): zeroed (matching CopyOrClearRange(v14, 0x4000, 0x8000, 1) outside data_02161788).
    #    - [0x3000..0x4000): untouched retail 0x7000..0x8000.
    #    - [0x4000..0xe7534): untouched retail 0x8000..0xeb534.
    arm9_cb = bytearray(arm9_size)
    arm9_cb[0x0800:0x1000] = rom[0x4800:0x5000]
    arm9_cb[0x0A58:0x0A5C] = struct.pack("<I", 0xE12FFF1E)
    arm9_cb[0x3000:0x4000] = rom[0x7000:0x8000]
    arm9_cb[0x4000:arm9_size] = rom[0x8000 : 0x4000 + arm9_size]

    # 4. Exact 0x289a4-byte Cloneboot ARM7 payload
    arm7_cb = rom[arm7_off : arm7_off + arm7_size]

    boot_dir = extract_usa / "files" / "data" / "Boot"
    boot_dir.mkdir(parents=True, exist_ok=True)

    mb_blob = bytes(hdr) + rsa_sig + bytes(arm9_cb) + arm7_cb
    assert len(mb_blob) == 0x1100C0
    (boot_dir / "mb_child_usa.bin").write_bytes(mb_blob)

    # 5. Stock ARM9 overlay table + compressed overlay binaries (ov0..ov3)
    ovt_bytes = rom[ovt9_off : ovt9_off + ovt9_size]
    (boot_dir / "mb_child_ovt.bin").write_bytes(ovt_bytes)
    for i in range(4):
        ent = ovt_bytes[i * 32 : (i + 1) * 32]
        ov_id, _, _, _, _, _, file_id, _ = struct.unpack("<8I", ent)
        start, end = struct.unpack("<II", rom[fat_off + file_id * 8 : fat_off + file_id * 8 + 8])
        (boot_dir / f"mb_child_ov{ov_id}.bin").write_bytes(rom[start:end])

    appended = update_path_order(extract_usa)
    print(
        f"Staged mb_child_usa.bin ({len(mb_blob)} B, sha256={hashlib.sha256(mb_blob).hexdigest()}), "
        f"mb_child_ovt.bin (128 B), and mb_child_ov0..3.bin; path_order appended {len(appended)} entries."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
