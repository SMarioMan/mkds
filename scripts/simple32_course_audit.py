#!/usr/bin/env python3
"""Reproducible course & archive resource audit for all 32 MKDS retail race tracks.

Reads gCupCourseIdTable, data_02154228 (course stem & Download Play 'D' flag table),
SelectCupGrid/SelectCupCourse UI tables, and the extracted NitroFS files under
extract/<ver>/files/data/Course/ to audit:
  1. Cup/course index -> internal CourseId -> stem -> host & child archive paths.
  2. Existence of requested .carc and Tex.carc files on the retail ROM (including D-variants).
  3. Compressed & LZ77-decompressed sizes against stock Download Play child limits:
     - Child mCompressedBuffer: 0x4b000 (307,200 bytes)
     - Child mCourseBufferHeap: 0x4b000 (307,200 bytes, holds uncompressed .carc + compressed Tex.carc)
     - Pending file queue entries: 12 slots (RequestRaceArchives uses 6)
"""
import argparse
import csv
import json
import struct
import sys
from pathlib import Path

CUP_NAMES = [
    "Mushroom Cup",
    "Flower Cup",
    "Star Cup",
    "Special Cup",
    "Shell Cup",
    "Banana Cup",
    "Leaf Cup",
    "Lightning Cup",
]

DISPLAY_NAMES_USA = {
    (0, 0): "Figure-8 Circuit",
    (0, 1): "Yoshi Falls",
    (0, 2): "Cheep Cheep Beach",
    (0, 3): "Luigi's Mansion",
    (1, 0): "Desert Hills",
    (1, 1): "Delfino Square",
    (1, 2): "Waluigi Pinball",
    (1, 3): "Shroom Ridge",
    (2, 0): "DK Pass",
    (2, 1): "Tick-Tock Clock",
    (2, 2): "Mario Circuit",
    (2, 3): "Airship Fortress",
    (3, 0): "Wario Stadium",
    (3, 1): "Peach Gardens",
    (3, 2): "Bowser Castle",
    (3, 3): "Rainbow Road",
    (4, 0): "SNES Mario Circuit 1",
    (4, 1): "N64 Moo Moo Farm",
    (4, 2): "GBA Peach Circuit",
    (4, 3): "GCN Luigi Circuit",
    (5, 0): "SNES Donut Plains 1",
    (5, 1): "N64 Frappe Snowland",
    (5, 2): "GBA Bowser Castle 2",
    (5, 3): "GCN Baby Park",
    (6, 0): "SNES Koopa Beach 2",
    (6, 1): "N64 Choco Mountain",
    (6, 2): "GBA Luigi Circuit",
    (6, 3): "GCN Mushroom Bridge",
    (7, 0): "SNES Choco Island 2",
    (7, 1): "N64 Banshee Boardwalk",
    (7, 2): "GBA Sky Garden",
    (7, 3): "GCN Yoshi Circuit",
}

CHILD_COMPRESSED_BUF_LIMIT = 0x4B000  # InitBlockEntries(heap, 1) in code_0204f240.c
CHILD_COURSE_HEAP_LIMIT = 0x4B000     # secondFrontHeap in func_02039e08 (code_02039e08.c)
ARM9_BASE = 0x02000000


def load_symbols(sym_path: Path) -> dict[str, int]:
    syms: dict[str, int] = {}
    for line in sym_path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2].startswith("addr:"):
            syms[parts[0]] = int(parts[2].split(":", 1)[1], 16)
    return syms


def align_up(val: int, align: int = 32) -> int:
    return (val + align - 1) & ~(align - 1)


def read_u32(blob: bytes, addr: int) -> int:
    return struct.unpack_from("<I", blob, addr - ARM9_BASE)[0]


def read_cstr(blob: bytes, addr: int) -> str:
    off = addr - ARM9_BASE
    end = blob.index(b"\x00", off)
    return blob[off:end].decode("ascii")


def inspect_carc(path: Path) -> tuple[bool, int, int]:
    if not path.is_file():
        return False, 0, 0
    raw = path.read_bytes()
    comp_size = len(raw)
    hdr = struct.unpack_from("<I", raw, 0)[0]
    if (hdr & 0xFF) != 0x10:
        raise ValueError(f"{path}: expected LZ77 (0x10) header byte, got {hdr & 0xff:#x}")
    uncomp_size = hdr >> 8
    return True, comp_size, uncomp_size


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--version", default="usa", choices=["usa", "eur"])
    ap.add_argument("--csv-out", type=Path)
    ap.add_argument("--json-out", type=Path)
    args = ap.parse_args()

    root: Path = args.repo_root
    ver: str = args.version
    arm9_path = root / "extract" / ver / "arm9" / "arm9.bin"
    files_dir = root / "extract" / ver / "files"
    sym_path = root / "config" / ver / "arm9" / "symbols.txt"

    if not arm9_path.is_file():
        sys.exit(f"Missing {arm9_path}; extract ROM first with configure.py")

    arm9 = arm9_path.read_bytes()
    syms = load_symbols(sym_path)

    cup_table_addr = syms["gCupCourseIdTable"]
    course_row_addr = syms["data_02154228"]
    simple_cups_addr = syms["data_021540c0"]
    full_cups_addr = syms["data_02154108"]
    grid_course_ids_addr = syms["gSelectCupGridCourseIds"]
    grid_to_cup_addr = syms["data_0215c85c"]
    cup_to_course_ui_addr = syms["data_0215cadc"]
    course_ui_ids_addr = syms["data_0215cbec"]

    simple_cups = [read_u32(arm9, simple_cups_addr + 4 * i) for i in range(2)]
    full_cups = [read_u32(arm9, full_cups_addr + 4 * i) for i in range(8)]
    grid_to_cup = [read_u32(arm9, grid_to_cup_addr + 4 * i) for i in range(8)]
    cup_to_ui = [read_u32(arm9, cup_to_course_ui_addr + 4 * i) for i in range(8)]

    print(f"=== MKDS Course & Download Play Archive Audit ({ver.upper()}) ===")
    print(f"Stock Simple 2-cup order (data_021540c0): {simple_cups}")
    print(f"Full VS 8-cup order      (data_02154108): {full_cups}")
    print(f"SelectCupGrid->CupIndex  (data_0215c85c): {grid_to_cup}")
    print(f"CupIndex->SelectCupCourse(data_0215cadc): {cup_to_ui}")
    print()

    rows = []
    seen_ids = set()
    any_missing = False
    any_over_limit = False

    hdr_fmt = (
        f"{'#':>2} {'Cup':<13} {'S':<1} {'ID':>2} {'Display Name':<22} "
        f"{'Stem':<18} {'D':<1} {'Child .carc':<24} {'Comp':>7} {'Uncomp':>7} "
        f"{'TexComp':>7} {'TexUnc':>7} {'FrmUsed':>7} {'Headroom':>8} {'Status'}"
    )
    print(hdr_fmt)
    print("-" * len(hdr_fmt))

    for seq_idx, cup_idx in enumerate(full_cups):
        for slot in range(4):
            course_id = read_u32(arm9, cup_table_addr + (cup_idx * 4 + slot) * 4)
            # Cross-check UI tables
            grid_idx = grid_to_cup.index(cup_idx)
            ui_grid_cid = read_u32(arm9, grid_course_ids_addr + (grid_idx * 4 + slot) * 4)
            ui_course_cid = read_u32(arm9, course_ui_ids_addr + (cup_to_ui[cup_idx] * 4 + slot) * 4)
            assert course_id == ui_grid_cid == ui_course_cid, (
                f"Mismatch at cup {cup_idx} slot {slot}: "
                f"gCupCourseIdTable={course_id}, grid={ui_grid_cid}, courseUI={ui_course_cid}"
            )
            assert course_id not in seen_ids, f"Duplicate course ID {course_id}"
            seen_ids.add(course_id)

            stem_ptr = read_u32(arm9, course_row_addr + course_id * 8)
            d_flag = read_u32(arm9, course_row_addr + course_id * 8 + 4)
            stem = read_cstr(arm9, stem_ptr)

            host_carc_rel = f"data/Course/{stem}.carc"
            host_tex_rel = f"data/Course/{stem}Tex.carc"
            d_suffix = "D" if d_flag == 1 else ""
            child_carc_rel = f"data/Course/{stem}{d_suffix}.carc"
            child_tex_rel = f"data/Course/{stem}{d_suffix}Tex.carc"

            # Check if a generated or retail D file exists on disk even when d_flag == 0
            alt_d_carc_rel = f"data/Course/{stem}D.carc"
            alt_d_tex_rel = f"data/Course/{stem}DTex.carc"
            alt_d_carc_exists = (files_dir / alt_d_carc_rel).is_file()
            alt_d_tex_exists = (files_dir / alt_d_tex_rel).is_file()

            h_ok, h_comp, h_uncomp = inspect_carc(files_dir / host_carc_rel)
            ht_ok, ht_comp, ht_uncomp = inspect_carc(files_dir / host_tex_rel)
            c_ok, c_comp, c_uncomp = inspect_carc(files_dir / child_carc_rel)
            ct_ok, ct_comp, ct_uncomp = inspect_carc(files_dir / child_tex_rel)

            # FrmHeap overhead: 0x10-byte NNSiFndFrmHeapHeader at creation + per-alloc alignment to 0x20
            frm_used = 0x20 + align_up(c_uncomp, 0x20) + align_up(ct_comp, 0x20)
            headroom = CHILD_COURSE_HEAP_LIMIT - frm_used
            comp_ok = c_comp <= CHILD_COMPRESSED_BUF_LIMIT
            heap_ok = headroom >= 0
            files_ok = h_ok and ht_ok and c_ok and ct_ok

            # Effective wireless-FAT-redirected child archives (when CreateRomArchiveBlock redirects over-budget stems)
            if not heap_ok and alt_d_carc_exists and alt_d_tex_exists:
                eff_carc_rel = alt_d_carc_rel
                eff_tex_rel = alt_d_tex_rel
                _, eff_comp, eff_uncomp = inspect_carc(files_dir / eff_carc_rel)
                _, eff_tcomp, eff_tuncomp = inspect_carc(files_dir / eff_tex_rel)
            else:
                eff_carc_rel = child_carc_rel
                eff_tex_rel = child_tex_rel
                eff_comp, eff_uncomp = c_comp, c_uncomp
                eff_tcomp, eff_tuncomp = ct_comp, ct_uncomp
            eff_frm_used = 0x20 + align_up(eff_uncomp, 0x20) + align_up(eff_tcomp, 0x20)
            eff_headroom = CHILD_COURSE_HEAP_LIMIT - eff_frm_used
            eff_ok = (eff_comp <= CHILD_COMPRESSED_BUF_LIMIT) and (eff_headroom >= 0)

            status_parts = []
            if not files_ok:
                status_parts.append("MISSING_FILE")
                any_missing = True
            if not comp_ok:
                status_parts.append("OVER_COMP_BUF")
                any_over_limit = True
            if not heap_ok:
                if eff_ok:
                    status_parts.append(f"STOCK_OVER({-headroom}B)->D_REDIRECT_OK(+{eff_headroom}B)")
                else:
                    status_parts.append(f"OVER_COURSE_HEAP({-headroom}B)")
                    any_over_limit = True
            status = "OK" if not status_parts else ",".join(status_parts)

            disp_name = DISPLAY_NAMES_USA[(cup_idx, slot)]
            row = {
                "sequence_order": seq_idx * 4 + slot + 1,
                "cup_index": cup_idx,
                "cup_name": CUP_NAMES[cup_idx],
                "slot_in_cup": slot,
                "course_id": course_id,
                "display_name": disp_name,
                "stem": stem,
                "d_flag": d_flag,
                "d_files_present_on_rom": alt_d_carc_exists and alt_d_tex_exists,
                "stock_simple_enabled": cup_idx in simple_cups,
                "host_carc": "/" + host_carc_rel,
                "host_tex_carc": "/" + host_tex_rel,
                "child_carc": "/" + child_carc_rel,
                "child_tex_carc": "/" + child_tex_rel,
                "effective_child_carc": "/" + eff_carc_rel,
                "effective_child_tex_carc": "/" + eff_tex_rel,
                "host_carc_comp": h_comp,
                "host_carc_uncomp": h_uncomp,
                "host_tex_comp": ht_comp,
                "host_tex_uncomp": ht_uncomp,
                "child_carc_comp": c_comp,
                "child_carc_uncomp": c_uncomp,
                "child_tex_comp": ct_comp,
                "child_tex_uncomp": ct_uncomp,
                "effective_child_carc_comp": eff_comp,
                "effective_child_carc_uncomp": eff_uncomp,
                "effective_child_tex_comp": eff_tcomp,
                "effective_child_tex_uncomp": eff_tuncomp,
                "child_course_heap_est": frm_used,
                "effective_child_course_heap_est": eff_frm_used,
                "child_course_heap_limit": CHILD_COURSE_HEAP_LIMIT,
                "child_course_heap_headroom": headroom,
                "effective_child_course_heap_headroom": eff_headroom,
                "child_comp_buf_limit": CHILD_COMPRESSED_BUF_LIMIT,
                "static_audit_status": status,
            }
            rows.append(row)
            print(
                f"{row['sequence_order']:>2} {CUP_NAMES[cup_idx]:<13} {slot:<1} {course_id:>2} "
                f"{disp_name:<22} {stem:<18} {d_flag:<1} "
                f"{Path(child_carc_rel).name:<24} {c_comp:>7} {c_uncomp:>7} "
                f"{ct_comp:>7} {ct_uncomp:>7} {frm_used:>7} {headroom:>+8} {status}"
            )

    print()
    print(f"Unique retail course IDs verified across 8 cups x 4 slots: {len(seen_ids)} / 32")
    print(f"Missing files: {any_missing}; Static heap/buffer overflows: {any_over_limit}")

    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        with args.csv_out.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"Wrote CSV audit to {args.csv_out}")

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(rows, indent=2) + "\n")
        print(f"Wrote JSON audit to {args.json_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
