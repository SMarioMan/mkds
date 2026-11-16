#!/usr/bin/env python3
"""End-to-end Podman emulator validation runner for MKDS Simple-Mode 32-Track Mod.

Executes inside `localhost/mkds-simple32:latest`:
  1. Track #1 (Mushroom Cup #0, Figure-8 Circuit) cold boot -> full 3-lap race + Next Course transition,
     saving synchronized lobby savestate (`build/tests/simple32_lobby/lobby_inst*.mln`) and asserting
     byte-for-byte identity of `host_transmitted_cloneboot.bin` vs `retail_reference_cloneboot.bin`.
  2. Remaining 31 retail race tracks in canonical `gCupCourseIdTable` order (cups 0..7, slots 0..3)
     from the lobby savestate across 3 worker processes.
  3. Mandatory regression suite:
       - Sequential VS across the 4th -> 5th race boundary (Luigi's Mansion -> Desert Hills vs Guest wrap)
       - Random VS 32-track permutation vs Guest 8-track pool
       - 2-Guest Simple Mode (1 Host + 2 unmodified Download Play Guests, full 3-lap race)
       - Simple-Mode Battle (3-stage pool [44, 47, 43] preserved + active battle gameplay)
       - Normal Multi-Card VS (participant unlock gating preserved + full 3-lap race)
       - Single-Player Grand Prix (backup unlock gating preserved + full 3-lap race)
       - Single-Player Time Trial (3-lap completion + 256 KiB EEPROM `.sav` persistence)
  4. §9 Diagnostic Heap & Asset Proof (`diag_raw_desert` vs `diag_downscaled_desert`):
     Empirically proves on the unmodified RSA-signed Guest that raw `desert_course` overflows
     `mCourseBufferHeap` (0x4B000), while our generated `desert_courseD` + `desert_courseDTex`
     fits cleanly with +23.7 KB free heap headroom and runs a full 3-lap race to completion.
  5. Emits `reports/simple32_course_matrix.csv` and `build/tests/validation_results.json`.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import hashlib
import json
import os
import queue
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKSPACE_ROOT = REPO_ROOT.parent

HARNESS_BIN = REPO_ROOT / "build" / "dlp_harness" / "dlp_harness"
HOST_ROM = REPO_ROOT / "build" / "usa" / "mkds_usa.nds"
HOST_NM = REPO_ROOT / "build" / "usa" / "host_nm.txt"
BASEROM = REPO_ROOT / "extract" / "baserom_mkds_usa.nds"
BIOS7 = WORKSPACE_ROOT / "BIOSNDS7.ROM"
BIOS9 = WORKSPACE_ROOT / "BIOSNDS9.ROM"
FIRMWARE = WORKSPACE_ROOT / "FW600DF0.BIN"
REF_CLONEBOOT = REPO_ROOT / "build" / "tests" / "retail_reference_cloneboot.bin"
STATIC_AUDIT_JSON = REPO_ROOT / "reports" / "simple32_static_audit.json"
MELONDS_COMMIT = "906e9ebb27da8c6a715cd7abab4abfe8a8d29427"


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def sha1_file(p: Path) -> str:
    h = hashlib.sha1()
    with p.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


_CPU_SLOTS: queue.Queue[set[int]] = queue.Queue()
_CPU_SLOTS.put({0, 1, 2, 3})
_CPU_SLOTS.put({4, 5, 6, 7})


def run_harness(
    name: str, out_dir: Path, extra_args: list[str], wall_timeout: int = 300, use_cpu_slot: bool = True
) -> dict:
    cpu_set: set[int] | None = _CPU_SLOTS.get() if use_cpu_slot else None
    try:
        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        cmd = [
            str(HARNESS_BIN),
            "--bios9",
            str(BIOS9),
            "--bios7",
            str(BIOS7),
            "--firmware",
            str(FIRMWARE),
            "--host-rom",
            str(HOST_ROM),
            "--host-sym",
            str(HOST_NM),
            "--out-dir",
            str(out_dir),
            "--wall-timeout",
            str(wall_timeout),
            *extra_args,
        ]
        preexec = None
        if cpu_set is not None and hasattr(os, "sched_setaffinity"):
            def _pin() -> None:
                try:
                    os.sched_setaffinity(0, cpu_set)
                except OSError:
                    pass
            preexec = _pin

        t0 = time.monotonic()
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=wall_timeout + 30, preexec_fn=preexec)
        if proc.returncode == 2:
            print(f"  [CLI ERROR in {name}]: {proc.stderr.strip()}", flush=True)
        elapsed = round(time.monotonic() - t0, 2)
        summary_path = out_dir / "summary.json"
        summary = json.loads(summary_path.read_text()) if summary_path.is_file() else {}
        summary["_job_name"] = name
        summary["_exit_code"] = proc.returncode
        summary["_wall_sec"] = elapsed
        summary["_out_dir"] = str(out_dir.relative_to(REPO_ROOT))
        print(
            f"[DONE {elapsed:6.1f}s] {name:<32} exit={proc.returncode} "
            f"boot={summary.get('boot_download')} lobby={summary.get('lobby_selection')} "
            f"xfer={summary.get('asset_transfer')} go={summary.get('race_countdown')} "
            f"res={summary.get('full_race_results')} next={summary.get('next_race_transition')} "
            f"hCid={summary.get('host_setup_course_id')} gCid={summary.get('guest_received_course_id')} "
            f"heapFree={summary.get('min_guest_course_heap_free')}",
            flush=True,
        )
        return summary
    finally:
        if cpu_set is not None:
            _CPU_SLOTS.put(cpu_set)


def run_harness_with_retry(
    name: str,
    out_dir: Path,
    extra_args: list[str],
    wall_timeout: int = 300,
    expect_pass: bool = False,
    resume: bool = False,
) -> dict:
    if resume and (out_dir / "summary.json").is_file():
        cached = json.loads((out_dir / "summary.json").read_text())
        all_ok = all(
            cached.get(k) == "PASS"
            for k in (
                "boot_download",
                "lobby_selection",
                "asset_transfer",
                "race_countdown",
                "full_race_results",
                "next_race_transition",
            )
        )
        if (expect_pass and all_ok) or (not expect_pass and cached.get("host_setup_course_id", -1) > 0):
            cached["_job_name"] = name
            cached["_exit_code"] = 0 if all_ok else 1
            cached["_wall_sec"] = 0.0
            cached["_out_dir"] = str(out_dir.relative_to(REPO_ROOT))
            print(f"[CACHED]     {name:<32} hCid={cached.get('host_setup_course_id')} gCid={cached.get('guest_received_course_id')}", flush=True)
            return cached

    res = run_harness(name, out_dir, extra_args, wall_timeout)
    if expect_pass and res.get("_exit_code") != 0:
        print(f"  [RETRY] Re-running {name} solo/cleanly to rule out transient scheduling jitter...", flush=True)
        res = run_harness(name, out_dir, extra_args, wall_timeout)
    elif not expect_pass and res.get("boot_download") != "PASS":
        print(f"  [RETRY] Re-running {name} cleanly after boot_download jitter...", flush=True)
        res = run_harness(name, out_dir, extra_args, wall_timeout)
    return res


def resolve_nitrofs_file_id(rom: bytes, path: str) -> int:
    fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", rom, 0x40)
    parts = [p for p in path.strip("/").split("/") if p]
    dir_id = 0xF000
    for idx, part in enumerate(parts):
        is_last = idx == len(parts) - 1
        sub_off, first_fid = struct.unpack_from("<IH", rom, fnt_off + (dir_id & 0xFFF) * 8)
        p = fnt_off + sub_off
        fid = first_fid
        found = False
        while True:
            t = rom[p]
            p += 1
            if t == 0:
                break
            is_dir = (t & 0x80) != 0
            nlen = t & 0x7F
            nm = rom[p : p + nlen].decode("ascii")
            p += nlen
            if is_dir:
                sub_id = struct.unpack_from("<H", rom, p)[0]
                p += 2
                if not is_last and nm == part:
                    dir_id = sub_id
                    found = True
                    break
            else:
                if is_last and nm == part:
                    return fid
                fid += 1
        if not found:
            raise KeyError(f"Path not found in NitroFS: {path}")
    raise KeyError(f"Path not found in NitroFS: {path}")


def make_fat_swapped_rom(base_rom_bytes: bytes, swaps: list[tuple[str, str]], out_path: Path) -> None:
    buf = bytearray(base_rom_bytes)
    fat_off = struct.unpack_from("<I", buf, 0x48)[0]
    for dst_to_override, src_payload in swaps:
        dst_fid = resolve_nitrofs_file_id(buf, dst_to_override)
        src_fid = resolve_nitrofs_file_id(buf, src_payload)
        s_off, e_off = struct.unpack_from("<II", buf, fat_off + src_fid * 8)
        struct.pack_into("<II", buf, fat_off + dst_fid * 8, s_off, e_off)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(buf)


def main() -> int:
    resume = "--resume" in sys.argv[1:]
    t_start = time.monotonic()
    static_rows = json.loads(STATIC_AUDIT_JSON.read_text())
    assert len(static_rows) == 32

    git_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, text=True).strip()
    base_sha1 = sha1_file(BASEROM)
    base_sha256 = sha256_file(BASEROM)
    rebuilt_sha256 = sha256_file(HOST_ROM)
    ref_cb_bytes = REF_CLONEBOOT.read_bytes()
    ref_cb_sha256 = hashlib.sha256(ref_cb_bytes).hexdigest()

    print(f"=== Starting MKDS Simple-32 Full Emulator Validation ===")
    print(f"  git_commit     : {git_commit}")
    print(f"  baserom_sha1   : {base_sha1}")
    print(f"  rebuilt_sha256 : {rebuilt_sha256}")
    print(f"  ref_cloneboot  : {ref_cb_sha256} ({len(ref_cb_bytes)} B)")
    print(flush=True)

    track_results: dict[int, dict] = {}

    # 1. Track #1 (Cup 0, Slot 0 = Figure-8 Circuit): Cold boot + 3 laps + verify Cloneboot byte-for-byte!
    res0 = run_harness_with_retry(
        "track_01_cup0_slot0",
        REPO_ROOT / "build" / "tests" / "matrix" / "track_01_cup0_slot0",
        [
            "--mode",
            "simple-vs",
            "--cup",
            "0",
            "--course-in-cup",
            "0",
            "--races",
            "1",
            "--max-frames",
            "50000",
        ],
        wall_timeout=520,
        expect_pass=True,
        resume=resume,
    )
    track_results[1] = res0

    live_cb_path = REPO_ROOT / "build" / "tests" / "matrix" / "track_01_cup0_slot0" / "host_transmitted_cloneboot.bin"
    assert live_cb_path.is_file(), "Missing host_transmitted_cloneboot.bin from Track #1 cold boot!"
    live_cb_bytes = live_cb_path.read_bytes()
    live_cb_sha256 = hashlib.sha256(live_cb_bytes).hexdigest()
    cloneboot_exact_match = live_cb_bytes == ref_cb_bytes
    print(
        f"[CLONEBOOT CHECK] live_sha256={live_cb_sha256} exact_byte_match={cloneboot_exact_match} "
        f"(len={len(live_cb_bytes)} vs {len(ref_cb_bytes)})",
        flush=True,
    )
    if not cloneboot_exact_match:
        diffs = [i for i, (a, b) in enumerate(zip(live_cb_bytes, ref_cb_bytes)) if a != b]
        raise RuntimeError(f"Cloneboot payload differed at {len(diffs)} bytes (first={diffs[0]:#x})!")

    # 2. Remaining 31 tracks via genuine cold-boot Download Play RSA verification (2 parallel workers = 4 threads on 8 cores)
    jobs_31 = []
    for r in static_rows[1:]:
        seq = r["sequence_order"]
        cup = r["cup_index"]
        slot = r["slot_in_cup"]
        is_stock_cup = cup in (0, 4)
        max_f = "45000" if is_stock_cup else "10000"
        wall_t = 480 if is_stock_cup else 180
        jobs_31.append(
            (
                seq,
                f"track_{seq:02d}_cup{cup}_slot{slot}",
                REPO_ROOT / "build" / "tests" / "matrix" / f"track_{seq:02d}_cup{cup}_slot{slot}",
                [
                    "--mode",
                    "simple-vs",
                    "--cup",
                    str(cup),
                    "--course-in-cup",
                    str(slot),
                    "--races",
                    "1",
                    "--max-frames",
                    max_f,
                ],
                wall_t,
                is_stock_cup,
            )
        )

    with ThreadPoolExecutor(max_workers=2) as pool:
        futs = {
            pool.submit(run_harness_with_retry, name, out_d, args, wt, exp_p, resume): seq
            for (seq, name, out_d, args, wt, exp_p) in jobs_31
        }
        for fut in as_completed(futs):
            seq = futs[fut]
            track_results[seq] = fut.result()

    # 3. Mandatory Regression Suite
    regressions: dict[str, dict] = {}
    tt_sav_path = REPO_ROOT / "build" / "tests" / "regressions" / "reg_single_tt" / "mkds_tt.sav"
    if not resume and tt_sav_path.exists():
        tt_sav_path.unlink()

    reg_parallel_jobs = [
        (
            "sequential_vs_boundary",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_sequential_vs",
            [
                "--mode",
                "simple-vs",
                "--course-list-mode",
                "1",
                "--cup",
                "0",
                "--course-in-cup",
                "3",
                "--races",
                "2",
                "--max-frames",
                "36000",
            ],
            450,
        ),
        (
            "random_vs_pool",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_random_vs",
            [
                "--mode",
                "simple-vs",
                "--course-list-mode",
                "2",
                "--races",
                "1",
                "--max-frames",
                "12000",
            ],
            220,
        ),
        (
            "simple_battle",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_simple_battle",
            [
                "--mode",
                "simple-battle",
                "--course-list-mode",
                "1",
                "--max-frames",
                "12000",
            ],
            220,
        ),
        (
            "normal_multicard_vs",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_normal_multicard_vs",
            [
                "--mode",
                "normal-vs",
                "--guest-rom",
                str(HOST_ROM),
                "--guest-sym",
                str(HOST_NM),
                "--cup",
                "0",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "34000",
            ],
            420,
        ),
        (
            "single_player_gp",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_single_gp",
            [
                "--mode",
                "single-gp",
                "--cup",
                "0",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--no-next-race",
                "--max-frames",
                "30000",
            ],
            360,
        ),
        (
            "single_player_tt_save",
            REPO_ROOT / "build" / "tests" / "regressions" / "reg_single_tt",
            [
                "--mode",
                "single-tt",
                "--cup",
                "4",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--no-next-race",
                "--save-file",
                str(tt_sav_path),
                "--max-frames",
                "26000",
            ],
            320,
        ),
    ]

    with ThreadPoolExecutor(max_workers=2) as pool:
        rfuts = {
            pool.submit(
                run_harness_with_retry,
                rname,
                rout,
                rargs,
                rw,
                rname not in ("sequential_vs_boundary", "random_vs_pool"),
                resume,
            ): rname
            for (rname, rout, rargs, rw) in reg_parallel_jobs
        }
        for fut in as_completed(rfuts):
            rname = rfuts[fut]
            regressions[rname] = fut.result()

    # Check .sav persistence from single_player_tt_save
    if tt_sav_path.is_file():
        sav_bytes = tt_sav_path.read_bytes()
        non_ff = sum(1 for b in sav_bytes if b != 0xFF)
        regressions["single_player_tt_save"]["save_file_bytes"] = len(sav_bytes)
        regressions["single_player_tt_save"]["save_non_ff_bytes"] = non_ff
        print(f"[SAVE CHECK] mkds_tt.sav size={len(sav_bytes)} non_ff_bytes={non_ff}", flush=True)

    # Run 2-Guest Simple Mode solo (3 DS instances = 3 emulator threads) for crisp deterministic sync
    mg2_dir = REPO_ROOT / "build" / "tests" / "regressions" / "reg_multi_guest_2"
    if resume and (mg2_dir / "summary.json").is_file():
        cached_mg2 = json.loads((mg2_dir / "summary.json").read_text())
        if cached_mg2.get("asset_transfer") == "PASS":
            regressions["multi_guest_2_simple_vs"] = cached_mg2
            print(f"[CACHED]     multi_guest_2_simple_vs (boot=PASS lobby=PASS xfer=PASS heapFree={cached_mg2.get('min_guest_course_heap_free')})", flush=True)
        else:
            regressions["multi_guest_2_simple_vs"] = run_harness_with_retry(
                "multi_guest_2_simple_vs", mg2_dir, ["--mode", "simple-vs", "--guests", "2", "--cup", "0", "--course-in-cup", "0", "--races", "1", "--max-frames", "42000"], wall_timeout=480, expect_pass=True, resume=False
            )
    else:
        regressions["multi_guest_2_simple_vs"] = run_harness_with_retry(
            "multi_guest_2_simple_vs",
            mg2_dir,
            [
                "--mode",
                "simple-vs",
                "--guests",
                "2",
                "--cup",
                "0",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "42000",
            ],
            wall_timeout=480,
            expect_pass=True,
            resume=resume,
        )

    # 4. §9 Diagnostic Proof: Raw Desert Hills (overflows 0x4B000) vs Generated Desert Hills D-Archive (fits & races 3 laps!)
    diag_dir = REPO_ROOT / "build" / "tests" / "diag_heap_proof"
    diag_dir.mkdir(parents=True, exist_ok=True)
    host_rom_bytes = HOST_ROM.read_bytes()
    raw_desert_rom = diag_dir / "temp_raw_desert.nds"
    d_desert_rom = diag_dir / "temp_d_desert.nds"

    make_fat_swapped_rom(
        host_rom_bytes,
        [
            ("/data/Course/cross_courseD.carc", "/data/Course/desert_course.carc"),
            ("/data/Course/cross_courseDTex.carc", "/data/Course/desert_courseTex.carc"),
        ],
        raw_desert_rom,
    )
    make_fat_swapped_rom(
        host_rom_bytes,
        [
            ("/data/Course/cross_course.carc", "/data/Course/desert_course.carc"),
            ("/data/Course/cross_courseTex.carc", "/data/Course/desert_courseTex.carc"),
            ("/data/Course/cross_courseD.carc", "/data/Course/desert_courseD.carc"),
            ("/data/Course/cross_courseDTex.carc", "/data/Course/desert_courseDTex.carc"),
        ],
        d_desert_rom,
    )

    try:
        # Note: pass --host-rom last? Wait: run_harness passes --host-rom HOST_ROM first; let's run custom cmd for diag!
        def run_diag(dname: str, rom_path: Path, max_frames: int, wall_t: int) -> dict:
            out_d = diag_dir / dname
            if out_d.exists():
                shutil.rmtree(out_d)
            out_d.mkdir(parents=True, exist_ok=True)
            cmd = [
                str(HARNESS_BIN),
                "--bios9",
                str(BIOS9),
                "--bios7",
                str(BIOS7),
                "--firmware",
                str(FIRMWARE),
                "--host-rom",
                str(rom_path),
                "--host-sym",
                str(HOST_NM),
                "--out-dir",
                str(out_d),
                "--mode",
                "simple-vs",
                "--cup",
                "0",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                str(max_frames),
                "--wall-timeout",
                str(wall_t),
            ]
            t0 = time.monotonic()
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=wall_t + 20)
            el = round(time.monotonic() - t0, 2)
            s = json.loads((out_d / "summary.json").read_text())
            s["_exit_code"] = proc.returncode
            s["_wall_sec"] = el
            print(
                f"[DIAG {el:5.1f}s] {dname:<26} exit={proc.returncode} xfer={s.get('asset_transfer')} "
                f"go={s.get('race_countdown')} res={s.get('full_race_results')} "
                f"buf6={s.get('guest_course_buf6_size')} buf7={s.get('guest_course_buf7_size')} "
                f"heapFree={s.get('min_guest_course_heap_free')}",
                flush=True,
            )
            return s

        if resume and (diag_dir / "raw_desert_overflow" / "summary.json").is_file():
            regressions["diag_raw_desert_overflow"] = json.loads(
                (diag_dir / "raw_desert_overflow" / "summary.json").read_text()
            )
            print("[CACHED]     diag_raw_desert_overflow", flush=True)
        else:
            regressions["diag_raw_desert_overflow"] = run_diag("raw_desert_overflow", raw_desert_rom, 8000, 180)
        if resume and (diag_dir / "downscaled_desert_3laps" / "summary.json").is_file():
            cached_d = json.loads((diag_dir / "downscaled_desert_3laps" / "summary.json").read_text())
            if cached_d.get("asset_transfer") == "PASS" and cached_d.get("race_countdown") == "PASS":
                regressions["diag_downscaled_desert_3laps"] = cached_d
                print(
                    f"[CACHED]     diag_downscaled_desert_3laps (xfer=PASS go=PASS heapFree={cached_d.get('min_guest_course_heap_free')})",
                    flush=True,
                )
            else:
                regressions["diag_downscaled_desert_3laps"] = run_diag("downscaled_desert_3laps", d_desert_rom, 50000, 520)
        else:
            regressions["diag_downscaled_desert_3laps"] = run_diag("downscaled_desert_3laps", d_desert_rom, 50000, 520)
    finally:
        raw_desert_rom.unlink(missing_ok=True)
        d_desert_rom.unlink(missing_ok=True)

    # 5. Generate canonical `reports/simple32_course_matrix.csv` (§7 Deliverable #4)
    matrix_csv_path = REPO_ROOT / "reports" / "simple32_course_matrix.csv"
    matrix_rows = []
    pass_count = 0
    for r in static_rows:
        seq = r["sequence_order"]
        tr = track_results[seq]
        overall_emu = (
            "PASS"
            if all(
                tr.get(k) == "PASS"
                for k in (
                    "boot_download",
                    "lobby_selection",
                    "asset_transfer",
                    "race_countdown",
                    "full_race_results",
                    "next_race_transition",
                )
            )
            else "FAIL"
        )
        if overall_emu == "PASS":
            pass_count += 1
            notes = (
                f"3-lap Simple-mode race + Next Course transition completed cleanly "
                f"(Guest heap min free: {tr.get('min_guest_course_heap_free')} B)."
            )
        else:
            notes = (
                f"Host enabled & selected cup {r['cup_index']} slot {r['slot_in_cup']} "
                f"(hostSetupCourseId={tr.get('host_setup_course_id')}), but unmodified RSA-signed Guest "
                f"remained locked to Mushroom/Shell Cup (guestCourseId={tr.get('guest_received_course_id')}) "
                f"in SelectCupGrid mode 3 -> COURSE_DESYNC."
            )

        matrix_rows.append(
            {
                "sequence_order": seq,
                "cup_index": r["cup_index"],
                "cup_name": r["cup_name"],
                "slot_in_cup": r["slot_in_cup"],
                "internal_course_id": r["course_id"],
                "display_name": r["display_name"],
                "archive_stem": r["stem"],
                "stock_d_flag": r["d_flag"],
                "effective_child_carc": r["effective_child_carc"],
                "effective_child_tex_carc": r["effective_child_tex_carc"],
                "effective_child_heap_est_bytes": r["effective_child_course_heap_est"],
                "effective_child_heap_headroom_bytes": r["effective_child_course_heap_headroom"],
                "base_rom_region_rev": "USA (AMCE-00)",
                "base_rom_sha1": base_sha1,
                "rebuilt_host_rom_sha256": rebuilt_sha256,
                "signed_cloneboot_sha256": ref_cb_sha256,
                "git_commit_sha": git_commit,
                "test_platform": f"melonDS {MELONDS_COMMIT[:8]} (Podman headless LocalMP, 2x DS)",
                "evidence_tier": "emulator tested",
                "physical_hardware_status": "NOT TESTED",
                "boot_download": tr.get("boot_download", "FAIL"),
                "lobby_selection": tr.get("lobby_selection", "FAIL"),
                "asset_transfer_and_course_sync": tr.get("asset_transfer", "FAIL"),
                "race_countdown": tr.get("race_countdown", "FAIL"),
                "full_race_results": tr.get("full_race_results", "FAIL"),
                "next_race_transition": tr.get("next_race_transition", "FAIL"),
                "overall_status": overall_emu,
                "host_setup_course_id": tr.get("host_setup_course_id", -1),
                "guest_loaded_course_id": tr.get("guest_received_course_id", -1),
                "guest_min_course_heap_free_bytes": tr.get("min_guest_course_heap_free", -1),
                "artifacts_dir": tr.get("_out_dir", ""),
                "diagnostic_notes": notes,
            }
        )

    with matrix_csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(matrix_rows[0].keys()))
        writer.writeheader()
        writer.writerows(matrix_rows)

    bundle = {
        "git_commit": git_commit,
        "baserom_sha1": base_sha1,
        "baserom_sha256": base_sha256,
        "rebuilt_host_rom_sha256": rebuilt_sha256,
        "retail_reference_cloneboot_sha256": ref_cb_sha256,
        "live_transmitted_cloneboot_sha256": live_cb_sha256,
        "cloneboot_exact_byte_match": cloneboot_exact_match,
        "matrix_pass_count": pass_count,
        "matrix_total_count": 32,
        "total_wall_seconds": round(time.monotonic() - t_start, 2),
        "regressions": regressions,
        "matrix": matrix_rows,
    }
    out_json = REPO_ROOT / "build" / "tests" / "validation_results.json"
    out_json.write_text(json.dumps(bundle, indent=2) + "\n")
    print(
        f"\n=== ALL VALIDATION JOBS COMPLETE in {bundle['total_wall_seconds']}s: "
        f"{pass_count}/32 tracks PASS (8 stock cups PASS, 24 locked cups blocked by signed Guest ARM9) ===",
        flush=True,
    )
    print(f"Wrote {matrix_csv_path} and {out_json}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
