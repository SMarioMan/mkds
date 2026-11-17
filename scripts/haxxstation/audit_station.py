#!/usr/bin/env python3
"""Wrap an unsigned DS guest with pinned `dspatch` and audit Stage-1 & Stage-2 invariants.

Verifies:
  1. DS Download Station Vol. 1 USA SHA-1 == f18b55f3e1259c03e10d0ecb549693b42905ceb5.
  2. Runs `dspatch -s <station> -i <input_nds> -o <out_nds>` (or audits an already generated `--out-rom`).
  3. Asserts `mb/ds_demo_client.srl` (306,824 B) and `mb/ds_demo_client.sgn` (155 B) in the output
     Station package match the original untouched Station ROM byte-for-byte (and that `.srl` ends
     with `.sgn[:0x88]`).
  4. Audits `ds_demo/demomenu` and `ds_demo/rom0000d` (exploit overwrite address `0x02332C40`,
     ARM9/ARM7 unwrapped load ranges, header CRC16, and zero collision with Station `0x02320000..0x0235E700`).
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
from nitrofs_util import (  # noqa: E402
    EXPECTED_STATION_SGN_SHA256,
    EXPECTED_STATION_SRL_SHA256,
    EXPECTED_STATION_USA_SHA1,
    crc16_modbus,
    list_nitrofs_files,
    read_nitrofs_file,
    sha1_file,
    sha256_bytes,
    sha256_file,
)


def audit_wrapped_station_package(station_rom_path: Path, wrapped_rom_path: Path) -> dict:
    st_sha1 = sha1_file(station_rom_path)
    if st_sha1 != EXPECTED_STATION_USA_SHA1:
        raise ValueError(
            f"Station ROM SHA-1 mismatch: got {st_sha1}, expected {EXPECTED_STATION_USA_SHA1}"
        )

    st_bytes = station_rom_path.read_bytes()
    wr_bytes = wrapped_rom_path.read_bytes()

    orig_srl = read_nitrofs_file(st_bytes, "mb/ds_demo_client.srl")
    orig_sgn = read_nitrofs_file(st_bytes, "mb/ds_demo_client.sgn")
    wrap_srl = read_nitrofs_file(wr_bytes, "mb/ds_demo_client.srl")
    wrap_sgn = read_nitrofs_file(wr_bytes, "mb/ds_demo_client.sgn")

    if orig_srl != wrap_srl:
        raise AssertionError("CRITICAL: mb/ds_demo_client.srl in wrapped output differs from untouched Station ROM!")
    if orig_sgn != wrap_sgn:
        raise AssertionError("CRITICAL: mb/ds_demo_client.sgn in wrapped output differs from untouched Station ROM!")
    if sha256_bytes(orig_srl) != EXPECTED_STATION_SRL_SHA256:
        raise AssertionError(f"Unexpected ds_demo_client.srl SHA-256: {sha256_bytes(orig_srl)}")
    if sha256_bytes(orig_sgn) != EXPECTED_STATION_SGN_SHA256:
        raise AssertionError(f"Unexpected ds_demo_client.sgn SHA-256: {sha256_bytes(orig_sgn)}")
    if orig_srl[-0x88:] != orig_sgn[:0x88]:
        raise AssertionError("Station SRL trailing 0x88 signature trailer does not match ds_demo_client.sgn[:0x88]!")

    demomenu = read_nitrofs_file(wr_bytes, "ds_demo/demomenu")
    rom0000d = read_nitrofs_file(wr_bytes, "ds_demo/rom0000d")

    (
        wrap_arm9_off,
        wrap_arm9_entry,
        wrap_arm9_ram,
        wrap_arm9_size,
        wrap_arm7_off,
        wrap_arm7_entry,
        wrap_arm7_ram,
        wrap_arm7_size,
    ) = struct.unpack_from("<8I", rom0000d, 0x20)
    hdr_crc_stored = struct.unpack_from("<H", rom0000d, 0x15E)[0]
    hdr_crc_calc = crc16_modbus(rom0000d[:0x15E])
    if hdr_crc_stored != hdr_crc_calc:
        raise AssertionError(
            f"rom0000d header CRC16 mismatch: stored={hdr_crc_stored:#06x} calc={hdr_crc_calc:#06x}"
        )

    if (wrap_arm9_off, wrap_arm9_ram, wrap_arm9_size) != (0x180, 0x02332C40, 0x100):
        raise AssertionError(
            f"Unexpected HaxxStation exploit header fields: off={wrap_arm9_off:#x} "
            f"ram={wrap_arm9_ram:#x} size={wrap_arm9_size:#x}"
        )

    real_arm7_off, real_arm9_off, real_arm9_ram, real_arm9_size = struct.unpack_from(
        "<4I", rom0000d, 0x180 + 0x3C
    )
    if real_arm7_off != wrap_arm7_off:
        raise AssertionError(
            f"Exploit literal arm7_off ({real_arm7_off:#x}) != header arm7_off ({wrap_arm7_off:#x})"
        )
    if real_arm9_ram + real_arm9_size > 0x02320000:
        raise AssertionError(
            f"Real ARM9 load range [{real_arm9_ram:#010x}..{real_arm9_ram + real_arm9_size:#010x}) "
            f"collides with running Station client at 0x02320000!"
        )

    wr_files = list_nitrofs_files(wr_bytes)
    return {
        "station_rom_sha1": st_sha1,
        "wrapped_rom": str(wrapped_rom_path),
        "wrapped_rom_size": len(wr_bytes),
        "wrapped_rom_sha256": sha256_file(wrapped_rom_path),
        "signed_stage1_srl_size": len(wrap_srl),
        "signed_stage1_srl_sha256": sha256_bytes(wrap_srl),
        "signed_stage1_sgn_size": len(wrap_sgn),
        "signed_stage1_sgn_sha256": sha256_bytes(wrap_sgn),
        "signed_stage1_byte_identical": True,
        "demomenu_size": len(demomenu),
        "demomenu_sha256": sha256_bytes(demomenu),
        "rom0000d_size": len(rom0000d),
        "rom0000d_sha256": sha256_bytes(rom0000d),
        "rom0000d_header_crc16": f"{hdr_crc_stored:#06x}",
        "exploit_arm9_header": {
            "offset": f"{wrap_arm9_off:#x}",
            "entry": f"{wrap_arm9_entry:#010x}",
            "load_addr": f"{wrap_arm9_ram:#010x}",
            "size": f"{wrap_arm9_size:#x}",
        },
        "unwrapped_guest_ranges": {
            "arm9_file_offset": f"{real_arm9_off:#x}",
            "arm9_entry": f"{wrap_arm9_entry:#010x}",
            "arm9_ram": f"{real_arm9_ram:#010x}",
            "arm9_size": real_arm9_size,
            "arm7_file_offset": f"{wrap_arm7_off:#x}",
            "arm7_entry": f"{wrap_arm7_entry:#010x}",
            "arm7_ram": f"{wrap_arm7_ram:#010x}",
            "arm7_size": wrap_arm7_size,
            "arm7_reloc_shim_used": wrap_arm7_ram == 0x02380000 and rom0000d[wrap_arm7_off : wrap_arm7_off + 4] == b"\x2c\x00\x9f\xe5",
        },
        "nitrofs_file_count": len(wr_files),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--station-rom", type=Path, required=True)
    ap.add_argument("--input-rom", type=Path, help="Unsigned guest .nds to wrap with dspatch")
    ap.add_argument("--out-rom", type=Path, required=True, help="Output wrapped Station .nds")
    ap.add_argument("--extract-dir", type=Path, help="Optional dir to write untouched srl, sgn, demomenu, rom0000d")
    ap.add_argument("--report-out", type=Path, help="Optional JSON audit output path")
    args = ap.parse_args()

    st_sha1 = sha1_file(args.station_rom)
    if st_sha1 != EXPECTED_STATION_USA_SHA1:
        print(
            f"ERROR: Station ROM SHA-1 mismatch: got {st_sha1}, expected {EXPECTED_STATION_USA_SHA1}",
            file=sys.stderr,
        )
        return 1

    if args.input_rom is not None:
        dspatch_bin = shutil.which("dspatch") or "/opt/mkds-tools/bin/dspatch"
        args.out_rom.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [dspatch_bin, "-s", str(args.station_rom), "-i", str(args.input_rom), "-o", str(args.out_rom)],
            check=True,
        )

    report = audit_wrapped_station_package(args.station_rom, args.out_rom)

    if args.extract_dir is not None:
        args.extract_dir.mkdir(parents=True, exist_ok=True)
        wr_bytes = args.out_rom.read_bytes()
        for rel, out_name in (
            ("mb/ds_demo_client.srl", "ds_demo_client.srl"),
            ("mb/ds_demo_client.sgn", "ds_demo_client.sgn"),
            ("ds_demo/demomenu", "demomenu"),
            ("ds_demo/rom0000d", "rom0000d"),
        ):
            (args.extract_dir / out_name).write_bytes(read_nitrofs_file(wr_bytes, rel))

    if args.report_out is not None:
        args.report_out.parent.mkdir(parents=True, exist_ok=True)
        args.report_out.write_text(json.dumps(report, indent=2) + "\n")

    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
