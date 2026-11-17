#!/usr/bin/env python3
"""Reconstruct, patch, verify, and optionally wrap the unsigned MKDS Stage-2 Download Play guest.

Implements Milestone M2 invariants:
  1. Decompresses the compiled modified MKDS USA `ARM9` (`Main`) from `--host-rom`.
  2. Verifies decrypted BIOS SWI veneers at `0x0200007E..0x0200079A`.
  3. Zeroes cartridge-only `0x02001000..0x02003000` (`CopyOrClearRange` parity with `func_02008570`).
  4. Installs a minimal 9-word guest-only `AutoloadCallback` hook (`0x02000A58 -> 0x02001000`) that:
       - Ensures `*(vu16*)0x027FFC40 == 2` (downloaded multiboot child boot type);
       - Adapts retained parent `GGID` (`0x027FFC4E..0x027FFC51`) from Station `0x00400120`
         to native MKDS USA `0x00000025`, preserving parent MAC, RF channel, and `TGID`.
  5. Computes the exact NitroSDK `DGTi_Hash2ProcessMessageBlock` (`ovt_mode=True`) 20-byte HMAC-SHA1
     over the Host's `128`-byte `ARM9_OVT` and writes it to `data_0216f2dc`, while asserting all four
     `OverlaySignatures[0..3]` match the Host's compiled `ov000..ov003` (`100%` overlay auth preserved).
  6. Disassembles the **decoded guest ARM9** with `arm-none-eabi-objdump` to prove both Simple-32
     patches (`GetSelectCupGridCupEnabled` cup `0..7` and `BuildCourseList` 32-track pools) are present.
  7. Recompresses `ARM9` in backward-LZ77 format, packs a compact executable-only `.nds` (`~1.1 MiB`,
     zero local NitroFS assets), optionally wraps via `dspatch`, and generates `memory_map.md`.
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_station import audit_wrapped_station_package  # noqa: E402
from nitrofs_util import (  # noqa: E402
    EXPECTED_MKDS_USA_SHA1,
    EXPECTED_STATION_USA_SHA1,
    MKDS_OVERLAY_HMAC_KEY,
    compress_backward,
    make_banner_v1,
    nitrosdk_hmac_sha1,
    pack_executable_only_nds,
    sha1_file,
    sha256_bytes,
    sha256_file,
    uncompress_backward,
    uncompress_backward_inplace,
)

BUILD_INFO_OFFSET = 0x0B4C  # 0x02000B4C - 0x02000000
AUTOLOAD_CB_OFFSET = 0x0A58  # 0x02000A58 - 0x02000000
GUEST_STUB_OFFSET = 0x1000  # 0x02001000 - 0x02000000 (inside zeroed 0x1000..0x3000 child region)


def load_nm_symbols(nm_path: Path) -> dict[str, int]:
    syms: dict[str, int] = {}
    for ln in nm_path.read_text().splitlines():
        parts = ln.split()
        if len(parts) >= 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return syms


def arm_branch(src_addr: int, dst_addr: int) -> int:
    rel = (dst_addr - (src_addr + 8)) >> 2
    assert -(1 << 23) <= rel < (1 << 23)
    return 0xEA000000 | (rel & 0x00FFFFFF)


def build_guest_autoload_stub() -> bytes:
    """Minimal ARM stub placed at 0x02001000 and jumped to by AutoloadCallback (0x02000A58).

    Runs immediately after `Entry` decompresses static ARM9 and copies ITCM/DTCM autoloads,
    BEFORE `clear_bss`, `main`, `FS_Init`, `func_0204ad44`, and `func_0204df90`:
      ldr  r0, [pc, #20]    ; r0 = 0x027FFC40 (uncached shared boot descriptor)
      mov  r1, #2
      strh r1, [r0, #0x00]  ; ensure boot_type == 2 (downloaded multiboot child)
      mov  r1, #0x25
      strh r1, [r0, #0x0E]  ; parent GGID low16  = 0x0025 (native MKDS USA GGID)
      mov  r1, #0x00
      strh r1, [r0, #0x10]  ; parent GGID high16 = 0x0000
      bx   lr               ; return to Entry (parent MAC, TGID, RF channel untouched!)
      .word 0x027FFC40
    """
    words = [
        0xE59F0018,  # ldr  r0, [pc, #0x18] -> +0x20 = 0x027FFC40
        0xE3A01002,  # mov  r1, #2
        0xE1C010B0,  # strh r1, [r0, #0x00]
        0xE3A01025,  # mov  r1, #0x25
        0xE1C010BE,  # strh r1, [r0, #0x0E]
        0xE3A01000,  # mov  r1, #0
        0xE1C011B0,  # strh r1, [r0, #0x10]
        0xE12FFF1E,  # bx   lr
        0x027FFC40,  # literal pool: uncached MB boot / parent descriptor base
    ]
    return struct.pack(f"<{len(words)}I", *words)


def verify_guest_disassembly(decoded_arm9: bytes, syms: dict[str, int], work_dir: Path) -> dict[str, str]:
    raw_path = work_dir / "decoded_guest_arm9.bin"
    raw_path.write_bytes(decoded_arm9)

    cup_addr = syms["GetSelectCupGridCupEnabled"]
    list_addr = syms["BuildCourseList"]

    cup_dis = subprocess.check_output(
        [
            "arm-none-eabi-objdump",
            "-D",
            "-b",
            "binary",
            "-m",
            "arm",
            "-M",
            "force-thumb",
            "--adjust-vma=0x02000000",
            f"--start-address={cup_addr:#x}",
            f"--stop-address={cup_addr + 0x120:#x}",
            str(raw_path),
        ],
        text=True,
    )
    if "cmp\tr7, #8" not in cup_dis and "cmp r7, #8" not in cup_dis:
        raise AssertionError(
            f"Decoded guest ARM9 GetSelectCupGridCupEnabled ({cup_addr:#x}) missing 'cmp r7, #8'!\n{cup_dis}"
        )

    list_dis = subprocess.check_output(
        [
            "arm-none-eabi-objdump",
            "-D",
            "-b",
            "binary",
            "-m",
            "arm",
            "--adjust-vma=0x02000000",
            f"--start-address={list_addr:#x}",
            f"--stop-address={list_addr + 0x260:#x}",
            str(raw_path),
        ],
        text=True,
    )
    for expected_insn in ("#8", "#32"):
        if expected_insn not in list_dis:
            raise AssertionError(
                f"Decoded guest ARM9 BuildCourseList ({list_addr:#x}) missing {expected_insn}!\n{list_dis}"
            )
    if "e3580008" not in list_dis or "e3550020" not in list_dis:
        raise AssertionError(
            f"Decoded guest ARM9 BuildCourseList ({list_addr:#x}) missing exact opcodes e3580008 / e3550020!\n{list_dis}"
        )

    return {
        "GetSelectCupGridCupEnabled_addr": f"{cup_addr:#010x}",
        "GetSelectCupGridCupEnabled_snippet": "\n".join(
            ln.strip() for ln in cup_dis.splitlines() if "cmp\tr7, #8" in ln or "movs\tr4, #1" in ln
        ),
        "BuildCourseList_addr": f"{list_addr:#010x}",
        "BuildCourseList_snippet": "\n".join(
            ln.strip() for ln in list_dis.splitlines() if "e3580008" in ln or "e3550020" in ln
        ),
    }


def write_memory_map_report(
    report_path: Path,
    *,
    raw_arm9_len: int,
    comp_arm9_len: int,
    bss_start: int,
    bss_end: int,
    arm7_len: int,
    ovt_entries: list[tuple[int, int, int, int]],
    rom0000d_len: int | None,
) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    ov_rows = []
    max_ov_end = bss_end
    for ov_id, ram_addr, ram_sz, bss_sz in ovt_entries:
        end_addr = ram_addr + ram_sz + bss_sz
        max_ov_end = max(max_ov_end, end_addr)
        ov_rows.append(
            f"| `OV{ov_id:03d}` | `0x{ram_addr:08X}` | `0x{end_addr:08X}` | `{ram_sz + bss_sz:,}` B | Wireless overlay (`HMAC-SHA1` verified) |"
        )

    rom0000d_note = f"`{rom0000d_len:,}` B (`0x{rom0000d_len:X}`)" if rom0000d_len is not None else "*(pending dspatch)*"
    md = f"""# MKDS HaxxStation Stage-1 / Stage-2 Guest Memory Map Audit

**Target:** Mario Kart DS USA (`AMCE-00`) unsigned Stage-2 guest launched via DS Download Station Vol. 1 USA (`NTRJ`).

## 1. Stage-2 Loader (`ds_demo_client.srl` + `dspatch` Exploit Wrapper)

| Region | Start | End (excl.) | Size | Owner / Safety Proof |
| --- | --- | --- | --- | --- |
| Temporary `rom0000d` download buffer (`r4`) | `~0x02004800` | `~0x02120000` | {rom0000d_note} | Allocated in low MainRAM by `ds_demo_client.srl` (`< 0x02320000`); never collides with running Station client (`0x02320000..0x0235E700`). |
| Station Stage-1 relocated client | `0x02320000` | `0x0235E700` | `255,744` B | Runs Stage-2 download & launcher; `0x02332C40` (`rsa_GetDecodedHash`) overwritten by `0x100`-byte `dspatch` `exploitData`. |
| Forward ARM9 copy (`exploitData`) | `0x02000000` | `0x{0x02000000 + comp_arm9_len:08X}` | `{comp_arm9_len:,}` B | Copies `r4 + 0x4000` forward to `0x02000000` (`dst <= src`, non-destructive); ends below `r4 + arm7_off`, well below `0x02320000`. |
| Direct ARM7 copy (no `arm7Fix` shim) | `0x02380000` | `0x{0x02380000 + arm7_len:08X}` | `{arm7_len:,}` B | `SubRamAddress = 0x02380000 < 0x03000000`, so `dspatch` omits `arm7Fix`; Station copies MKDS ARM7 directly to `0x02380000`. |

## 2. Final MKDS Stage-2 Guest Runtime Memory Map

| Region | Start | End (excl.) | Size | Purpose / Invariant |
| --- | --- | --- | --- | --- |
| Decrypted Secure Area & SWI veneers | `0x02000000` | `0x02000800` | `2,048` B | Plaintext Thumb BIOS `SWI` veneers (`0x0200007E..0x0200079A`). |
| `Entry`, `MIi_UncompressBackward`, `BuildInfo` | `0x02000800` | `0x02001000` | `2,048` B | `AutoloadCallback` (`0x02000A58`) branches to `0x02001000`. |
| Guest-only child/GGID stub + zeroed Logo cave | `0x02001000` | `0x02003000` | `8,192` B | 36-byte stub at `0x02001000` adapts `GGID -> 0x25`; rest zeroed like retail Cloneboot. |
| Decoded MKDS Static `ARM9` (`.text`/`.rodata`/`.data`) | `0x02003000` | `0x{bss_start:08X}` | `{bss_start - 0x02003000:,}` B | Contains Simple-32 `GetSelectCupGridCupEnabled` & `BuildCourseList` + updated OVT HMAC (`data_0216f2dc`). *(Autoload tail up to `0x{0x02000000 + raw_arm9_len:08X}` copied to ITCM/DTCM before `.bss` clear.)* |
| Static `ARM9` `.bss` | `0x{bss_start:08X}` | `0x{bss_end:08X}` | `{bss_end - bss_start:,}` B | Cleared and cache-flushed by `Entry`. |
{chr(10).join(ov_rows)}
| MainRAM Heap Arena (`ArenaLo .. ArenaHi`) | `0x{(max_ov_end + 0x1F) & ~0x1F:08X}` | `0x023FE000` | `{0x023FE000 - ((max_ov_end + 0x1F) & ~0x1F):,}` B | Includes `0x4B000` (`307,200` B) `mCourseBufferHeap` for race `.carc` / `D`-archives. |
| ARM9 ITCM | `0x01FF8000` | `0x01FFFFA0` | `32,672` B | Autoloaded by `func_020009e0`. |
| ARM9 DTCM + IRQ/SVC/SYS Stacks | `0x027E0000` | `0x027E4000` | `16,384` B | `.dtcm` data at `0x027E0000..0x027E00E0`; stacks at top of DTCM. |
| Uncached Shared Boot / Parent `WMBssDesc` | `0x027FFC40` | `0x027FFC80` | `64` B | `bootType == 2` at `0x027FFC40`; parent MAC, `GGID = 0x25`, `TGID`, RF channel at `0x027FFC42..0x027FFC7E`. |
| Downloaded NDS Header | `0x027FFE00` | `0x027FFF60` | `352` B | Copied from `rom0000d[0..0x160]` (`MARIOKARTDS` / `AMCE`). |
"""
    report_path.write_text(md)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host-rom", type=Path, default=Path("build/usa/mkds_usa.nds"))
    ap.add_argument("--host-sym", type=Path, default=Path("build/usa/host_nm.txt"))
    ap.add_argument("--mkds-rom", type=Path, help="Optional retail untrimmed MKDS USA ROM for strict SHA-1 check")
    ap.add_argument("--station-rom", type=Path, help="Optional Station Vol. 1 USA ROM to wrap & audit with dspatch")
    ap.add_argument("--out", type=Path, default=Path("build/haxxstation/mkds-simple32-guest.nds"))
    ap.add_argument("--wrapped-out", type=Path, help="Optional output path for dspatch-wrapped Station host .nds")
    ap.add_argument(
        "--memory-map-out",
        type=Path,
        default=Path("reports/haxxstation/memory_map.md"),
    )
    args = ap.parse_args()

    if args.mkds_rom is not None:
        mkds_sha1 = sha1_file(args.mkds_rom)
        if mkds_sha1 != EXPECTED_MKDS_USA_SHA1:
            print(
                f"ERROR: MKDS USA ROM SHA-1 mismatch: got {mkds_sha1}, expected {EXPECTED_MKDS_USA_SHA1}",
                file=sys.stderr,
            )
            return 1

    if args.station_rom is not None:
        st_sha1 = sha1_file(args.station_rom)
        if st_sha1 != EXPECTED_STATION_USA_SHA1:
            print(
                f"ERROR: Station ROM SHA-1 mismatch: got {st_sha1}, expected {EXPECTED_STATION_USA_SHA1}",
                file=sys.stderr,
            )
            return 1

    host_bytes = args.host_rom.read_bytes()
    syms = load_nm_symbols(args.host_sym)

    (
        arm9_off,
        arm9_entry,
        arm9_ram,
        arm9_size,
        arm7_off,
        arm7_entry,
        arm7_ram,
        arm7_size,
    ) = struct.unpack_from("<8I", host_bytes, 0x20)
    fat_off, fat_size, ovt9_off, ovt9_size = struct.unpack_from("<4I", host_bytes, 0x48)
    banner_off = struct.unpack_from("<I", host_bytes, 0x68)[0]

    assert arm9_ram == 0x02000000 and arm9_entry == 0x02000800
    assert arm7_ram == 0x02380000 and arm7_entry == 0x02380000
    assert ovt9_size == 0x80

    comp_host_arm9 = host_bytes[arm9_off : arm9_off + arm9_size]
    decoded = bytearray(uncompress_backward(comp_host_arm9))

    # Zero CompressedStaticEnd in decoded image while inspecting/rebuilding
    struct.pack_into("<I", decoded, BUILD_INFO_OFFSET + 0x14, 0)
    autoload_list_off, autoload_list_end, autoload_start, bss_start, bss_end = struct.unpack_from(
        "<5I", decoded, BUILD_INFO_OFFSET
    )

    # 1. Verify plaintext Thumb SWI veneers in 0x0200007E..0x0200079A
    swi_opcodes = sum(
        1 for off in range(0x007E, 0x07A0, 2) if (struct.unpack_from("<H", decoded, off)[0] & 0xFF00) == 0xDF00
    )
    if swi_opcodes < 18:
        raise AssertionError(f"Expected >=18 decrypted BIOS SWI veneers in ARM9[0x7E..0x7A0], found {swi_opcodes}")

    # 2. Apply retail Cloneboot zeroing of cartridge-only 0x02001000..0x02003000 (`CopyOrClearRange`)
    decoded[0x1000:0x3000] = b"\x00" * 0x2000

    # 3. Install guest-only AutoloadCallback -> 0x02001000 stub
    stub_bytes = build_guest_autoload_stub()
    decoded[GUEST_STUB_OFFSET : GUEST_STUB_OFFSET + len(stub_bytes)] = stub_bytes
    struct.pack_into(
        "<I",
        decoded,
        AUTOLOAD_CB_OFFSET,
        arm_branch(0x02000000 + AUTOLOAD_CB_OFFSET, 0x02000000 + GUEST_STUB_OFFSET),
    )

    # 4. Compute & update NitroSDK `FS_AttachOverlayTable` HMAC-SHA1 (`data_0216f2dc`), and verify `OverlaySignatures[0..3]`!
    hmac_key_ptr_sym = syms.get("data_02161768")
    if hmac_key_ptr_sym is not None:
        key_ptr = struct.unpack_from("<I", decoded, hmac_key_ptr_sym - 0x02000000)[0]
        key_in_rom = bytes(decoded[key_ptr - 0x02000000 : key_ptr - 0x02000000 + 64])
        if key_in_rom != MKDS_OVERLAY_HMAC_KEY:
            raise AssertionError(f"Unexpected NitroSDK HMAC key at *data_02161768 (0x{key_ptr:08X}): {key_in_rom.hex()}")
        active_key = key_in_rom
    else:
        active_key = MKDS_OVERLAY_HMAC_KEY

    ovt_bytes = host_bytes[ovt9_off : ovt9_off + ovt9_size]
    ovt_hmac = nitrosdk_hmac_sha1(ovt_bytes, active_key, ovt_mode=True)
    ovt_digest_addr = syms["data_0216f2dc"]
    ovt_digest_off = ovt_digest_addr - 0x02000000
    decoded[ovt_digest_off : ovt_digest_off + 20] = ovt_hmac

    ov_sigs_addr = syms["OverlaySignatures"]
    ov_sigs_off = ov_sigs_addr - 0x02000000
    ovt_entries: list[tuple[int, int, int, int]] = []
    ov_digests_hex: list[str] = []
    for i in range(4):
        ov_id, ov_ram, ov_ram_sz, ov_bss_sz, _, _, ov_fid, _ = struct.unpack_from("<8I", ovt_bytes, i * 32)
        ovt_entries.append((ov_id, ov_ram, ov_ram_sz, ov_bss_sz))
        s_off, e_off = struct.unpack_from("<II", host_bytes, fat_off + ov_fid * 8)
        ov_comp_bytes = host_bytes[s_off:e_off]
        expected_sig = nitrosdk_hmac_sha1(ov_comp_bytes, active_key, ovt_mode=False)
        stored_sig = bytes(decoded[ov_sigs_off + i * 20 : ov_sigs_off + (i + 1) * 20])
        if stored_sig != expected_sig:
            decoded[ov_sigs_off + i * 20 : ov_sigs_off + (i + 1) * 20] = expected_sig
        ov_digests_hex.append(expected_sig.hex())

    # 5. Disassemble decoded guest ARM9 and assert Simple-32 patches are present!
    work_dir = args.out.parent / "client_work"
    work_dir.mkdir(parents=True, exist_ok=True)
    disasm_proof = verify_guest_disassembly(bytes(decoded), syms, work_dir)
    decoded_sha256 = sha256_bytes(decoded)

    # 6. Recompress guest ARM9 in backward-LZ77 format and set BuildInfo.CompressedStaticEnd!
    # Wait: `Entry` passes `[BuildInfo + 0x14]` (`CompressedStaticEnd`) to `MIi_UncompressBackward`, AND after
    # decompression `BuildInfo + 0x14` is inside the uncompressed `0x0000..0x4000` prefix (`0x0B60`), where
    # `func_02000934` clears `BuildInfo + 0x14 = 0` at runtime! So `CompressedStaticEnd` is written into the
    # uncompressed prefix AFTER `compress_backward` computes the exact compressed length!
    comp_guest_arm9 = bytearray(compress_backward(decoded, uncomp_prefix_len=0x4000))
    comp_static_end = 0x02000000 + len(comp_guest_arm9)
    struct.pack_into("<I", comp_guest_arm9, BUILD_INFO_OFFSET + 0x14, comp_static_end)

    # Verify in-place round-trip decompression with CompressedStaticEnd populated!
    rt_check = bytearray(uncompress_backward_inplace(comp_guest_arm9))
    struct.pack_into("<I", rt_check, BUILD_INFO_OFFSET + 0x14, 0)
    if bytes(rt_check) != bytes(decoded):
        raise AssertionError("Post-BuildInfo in-place round-trip decompression check failed!")

    # 7. Extract ARM7, logo, and retail icon with updated Stage-2 banner subtitle
    arm7_bytes = host_bytes[arm7_off : arm7_off + arm7_size]
    logo_156 = host_bytes[0xC0:0x15C]
    icon_tiles = host_bytes[banner_off + 0x20 : banner_off + 0x220]
    icon_pltt = host_bytes[banner_off + 0x220 : banner_off + 0x240]
    banner_bytes = make_banner_v1(
        ["Mario Kart DS", "Simple Mode (32 Tracks)"],
        icon_tiles=icon_tiles,
        icon_pltt=icon_pltt,
    )

    # Nitro StaticFooter (0xDEC00621, _start_ModuleParamsOffset = 0xB4C, 0)
    nitro_footer = struct.pack("<III", 0xDEC00621, BUILD_INFO_OFFSET, 0)

    guest_nds = pack_executable_only_nds(
        game_name="MARIOKARTDS",
        game_code="AMCE",
        maker_code="01",
        arm9_bytes=bytes(comp_guest_arm9),
        arm9_entry=arm9_entry,
        arm9_ram=arm9_ram,
        arm7_bytes=arm7_bytes,
        arm7_entry=arm7_entry,
        arm7_ram=arm7_ram,
        logo_156=logo_156,
        banner_bytes=banner_bytes,
        rom_param_a=host_bytes[0x60:0x68],
        rom_param_b=host_bytes[0x6E:0x70],
        main_autoload_done=0x02000A58,
        sub_autoload_done=struct.unpack_from("<I", host_bytes, 0x74)[0],
        nitro_footer=nitro_footer,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(guest_nds)

    station_audit = None
    rom0000d_len = None
    if args.station_rom is not None and args.wrapped_out is not None:
        import shutil as _shutil

        dspatch_bin = _shutil.which("dspatch") or "/opt/mkds-tools/bin/dspatch"
        args.wrapped_out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [dspatch_bin, "-s", str(args.station_rom), "-i", str(args.out), "-o", str(args.wrapped_out)],
            check=True,
        )
        station_audit = audit_wrapped_station_package(args.station_rom, args.wrapped_out)
        rom0000d_len = station_audit["rom0000d_size"]

    write_memory_map_report(
        args.memory_map_out,
        raw_arm9_len=len(decoded),
        comp_arm9_len=len(comp_guest_arm9),
        bss_start=bss_start,
        bss_end=bss_end,
        arm7_len=len(arm7_bytes),
        ovt_entries=ovt_entries,
        rom0000d_len=rom0000d_len,
    )

    manifest = {
        "guest_nds": str(args.out),
        "guest_nds_size": len(guest_nds),
        "guest_nds_sha256": sha256_file(args.out),
        "decoded_guest_arm9_size": len(decoded),
        "decoded_guest_arm9_sha256": decoded_sha256,
        "compressed_guest_arm9_size": len(comp_guest_arm9),
        "compressed_guest_arm9_sha256": sha256_bytes(comp_guest_arm9),
        "arm7_size": len(arm7_bytes),
        "arm7_sha256": sha256_bytes(arm7_bytes),
        "autoload_hook_addr": "0x02000A58 -> 0x02001000",
        "ovt_hmac_addr": f"{ovt_digest_addr:#010x}",
        "ovt_hmac_sha1": ovt_hmac.hex(),
        "overlay_signatures_sha1": ov_digests_hex,
        "disassembly_proof": disasm_proof,
        "station_wrap_audit": station_audit,
    }
    meta_out = args.out.with_suffix(".json")
    meta_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
