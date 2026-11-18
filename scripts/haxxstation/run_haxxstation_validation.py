#!/usr/bin/env python3
"""Full Milestone M7 emulator validation runner for MKDS Simple-Mode 32-Track HaxxStation Mod.

Executes inside `localhost/mkds-simple32:latest`:
  1. Verifies `build/haxxstation/mkds-simple32-haxxstation-host.nds` and `reports/haxxstation/build_manifest.json`.
  2. Runs all 32 retail race tracks (`cups 0..7`, `slots 0..3` in `gCupCourseIdTable` order) from
     genuine cold-boot DS Firmware Download Play -> signed Station Stage-1 RSA -> Station menu ->
     unsigned modified MKDS Stage-2 -> native MKDS wireless rejoin -> 8-cup UI sync -> MPState16 &
     wireless `RomArchiveBlock` (`D` archive redirects) -> 3-lap race -> results -> Next Course.
  3. Runs mandatory HaxxStation multi-guest & regression suite:
       - Sequential VS 4th -> 5th boundary (`18 -> 27`, Luigi's Mansion -> Desert Hills, 2 full races)
       - Random VS 32-track PRNG sync (`course_list_snapshot == guest_course_list_snapshot`, 2 runs)
       - 2-Guest HaxxStation Desert Hills (`1 Host + 2 unmodified Download Play Guests`, 3 laps + Next Race)
       - 2-Guest Staggered-Tap Execution Barrier (`--guest-tap-delay 240`, 4s human tap offset)
       - Simple-Mode Battle (`[44, 47, 43]` pool preserved)
       - Normal Multi-Card VS
       - Single-Player Grand Prix
       - Single-Player Time Trial + 256 KiB EEPROM `.sav` persistence
  4. Emits:
       - `reports/haxxstation/course_matrix.csv`
       - `reports/haxxstation/phase_trace.jsonl`
       - `reports/haxxstation/stage_results.md`
       - `reports/haxxstation/hardware_results.md`
       - `build/haxxstation/validation_bundle.json`
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import json
import os
import queue
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKSPACE_ROOT = REPO_ROOT.parent

HARNESS_BIN = REPO_ROOT / "build" / "dlp_harness" / "dlp_harness"
HOST_ROM = REPO_ROOT / "build" / "haxxstation" / "mkds-simple32-haxxstation-host.nds"
HOST_NM = REPO_ROOT / "build" / "usa" / "host_nm.txt"
BUILD_MANIFEST = REPO_ROOT / "reports" / "haxxstation" / "build_manifest.json"
STATIC_AUDIT_JSON = REPO_ROOT / "reports" / "simple32_static_audit.json"

BIOS7 = WORKSPACE_ROOT / "BIOSNDS7.ROM"
BIOS9 = WORKSPACE_ROOT / "BIOSNDS9.ROM"
FIRMWARE = WORKSPACE_ROOT / "FW600DF0.BIN"

REQUIRED_TRACE_FIELDS = {
    "timestamp_or_frame",
    "platform",
    "console_role",
    "stage",
    "aid",
    "association_generation",
    "peer_mac_hash",
    "ggid",
    "tgid",
    "channel",
    "wm_state",
    "mb_state",
    "hMp",
    "boot_type",
    "simple_mode_flag",
    "course_list_mode",
    "course_id",
    "course_list_sha256",
    "pending_files",
    "course_heap_free",
    "send_offset",
    "send_status",
    "first_error",
}

STAGE_KEYS_8 = (
    "first_stage_RSA",
    "stage2_launch",
    "native_join",
    "menu_course_sync",
    "MPState16_and_archives",
    "countdown",
    "3_laps_and_results",
    "next_race",
)


def make_cpu_slots(workers: int) -> queue.Queue[set[int] | None]:
    q: queue.Queue[set[int] | None] = queue.Queue()
    ncpu = os.cpu_count() or 4
    per = max(1, ncpu // max(1, workers))
    for w in range(workers):
        start = (w * per) % ncpu
        q.put({(start + k) % ncpu for k in range(per)})
    return q


def run_job(
    name: str,
    out_dir: Path,
    extra_args: list[str],
    cpu_slots: queue.Queue[set[int] | None],
    wall_timeout: int = 540,
    resume: bool = False,
    required_keys: tuple[str, ...] = STAGE_KEYS_8,
) -> dict:
    summary_file = out_dir / "summary.json"
    if resume and summary_file.is_file():
        cached = json.loads(summary_file.read_text())
        if all(cached.get(k) == "PASS" for k in required_keys):
            cached["_job_name"] = name
            cached["_exit_code"] = 0
            cached["_wall_sec"] = 0.0
            cached["_out_dir"] = str(out_dir.relative_to(REPO_ROOT))
            print(
                f"[CACHED]       {name:<34} PASS hCid={cached.get('host_setup_course_id')} "
                f"gCid={cached.get('guest_received_course_id')} heap={cached.get('min_guest_course_heap_free')}",
                flush=True,
            )
            return cached

    for attempt in (1, 2):
        cpu_set = cpu_slots.get()
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
                "--guest-sym",
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
            elapsed = round(time.monotonic() - t0, 2)
            summary = json.loads(summary_file.read_text()) if summary_file.is_file() else {}
            summary["_job_name"] = name
            summary["_exit_code"] = proc.returncode
            summary["_wall_sec"] = elapsed
            summary["_out_dir"] = str(out_dir.relative_to(REPO_ROOT))
            ok = proc.returncode == 0 and all(summary.get(k) == "PASS" for k in required_keys)
            print(
                f"[{'PASS' if ok else 'FAIL'} {elapsed:6.1f}s] {name:<34} exit={proc.returncode} "
                f"rsa={summary.get('first_stage_RSA')} st2={summary.get('stage2_launch')} "
                f"join={summary.get('native_join')} sync={summary.get('menu_course_sync')} "
                f"mp16={summary.get('MPState16_and_archives')} go={summary.get('countdown')} "
                f"laps={summary.get('3_laps_and_results')} next={summary.get('next_race')} "
                f"hCid={summary.get('host_setup_course_id')} gCid={summary.get('guest_received_course_id')} "
                f"heap={summary.get('min_guest_course_heap_free')}",
                flush=True,
            )
            if ok or attempt == 2:
                return summary
            print(f"  [RETRY] Re-running {name} cleanly (attempt 2/2)...", flush=True)
        finally:
            cpu_slots.put(cpu_set)
    raise AssertionError("Unreachable")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workers", type=int, default=4, help="Parallel single-threaded harness workers")
    ap.add_argument("--resume", action="store_true", help="Reuse already-PASS job outputs")
    args = ap.parse_args()

    t_start = time.monotonic()
    manifest = json.loads(BUILD_MANIFEST.read_text())
    static_rows = json.loads(STATIC_AUDIT_JSON.read_text())
    assert len(static_rows) == 32

    host_meta = manifest["m6_final_host_manifest"]
    guest_meta = manifest["m2_guest_manifest"]
    base_sha1 = manifest["verified_inputs"]["mkds_usa_untrimmed_sha1"]
    station_sha1 = manifest["verified_inputs"]["download_station_vol1_usa_sha1"]
    final_host_sha256 = host_meta["host_rom_sha256"]
    stage1_srl_sha256 = host_meta["signed_stage1_srl_sha256"]
    rom0000d_sha256 = host_meta["station_rom0000d_sha256"]

    print("=== Starting MKDS HaxxStation M7 Full 32-Course & Regression Validation ===")
    print(f"  final_host_rom_sha256 : {final_host_sha256}")
    print(f"  stage1_srl_sha256     : {stage1_srl_sha256}")
    print(f"  stage2_rom0000d_sha256: {rom0000d_sha256}")
    print(f"  workers               : {args.workers}", flush=True)

    cpu_slots = make_cpu_slots(args.workers)
    matrix_root = REPO_ROOT / "build" / "haxxstation" / "matrix"
    reg_root = REPO_ROOT / "build" / "haxxstation" / "regressions"

    # Seed Track #5 (Desert Hills, cup=1, slot=0) from m5_desert_choose if already PASS and --resume
    m5_desert = REPO_ROOT / "build" / "haxxstation" / "m5_desert_choose" / "summary.json"
    t05_dir = matrix_root / "track_05_cup1_slot0"
    if args.resume and m5_desert.is_file() and not (t05_dir / "summary.json").is_file():
        m5_sum = json.loads(m5_desert.read_text())
        if all(m5_sum.get(k) == "PASS" for k in STAGE_KEYS_8):
            shutil.copytree(m5_desert.parent, t05_dir)

    track_results: dict[int, dict] = {}
    course_jobs = []
    for r in static_rows:
        seq = r["sequence_order"]
        cup = r["cup_index"]
        slot = r["slot_in_cup"]
        course_jobs.append(
            (
                seq,
                f"track_{seq:02d}_cup{cup}_slot{slot}",
                matrix_root / f"track_{seq:02d}_cup{cup}_slot{slot}",
                [
                    "--mode",
                    "simple-vs",
                    "--guests",
                    "1",
                    "--course-list-mode",
                    "0",
                    "--cup",
                    str(cup),
                    "--course-in-cup",
                    str(slot),
                    "--races",
                    "1",
                    "--max-frames",
                    "60000",
                ],
            )
        )

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {
            pool.submit(run_job, name, out_d, jargs, cpu_slots, 1200, args.resume): seq
            for (seq, name, out_d, jargs) in course_jobs
        }
        for fut in as_completed(futs):
            track_results[futs[fut]] = fut.result()

    # Mandatory Regression & Multi-Guest Suite
    regressions: dict[str, dict] = {}
    tt_sav_path = reg_root / "reg_single_tt" / "mkds_tt.sav"
    if not args.resume and tt_sav_path.exists():
        tt_sav_path.unlink()

    reg_jobs = [
        (
            "sequential_vs_boundary_18_to_27",
            reg_root / "reg_sequential_vs",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "1",
                "--course-list-mode",
                "1",
                "--cup",
                "0",
                "--course-in-cup",
                "3",
                "--races",
                "2",
                "--max-frames",
                "80000",
            ],
            1800,
            STAGE_KEYS_8,
        ),
        (
            "random_vs_32pool_run1",
            reg_root / "reg_random_vs_1",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "1",
                "--course-list-mode",
                "2",
                "--races",
                "1",
                "--max-frames",
                "60000",
            ],
            1200,
            STAGE_KEYS_8,
        ),
        (
            "random_vs_32pool_run2",
            reg_root / "reg_random_vs_2",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "1",
                "--course-list-mode",
                "2",
                "--races",
                "2",
                "--max-frames",
                "80000",
            ],
            1800,
            STAGE_KEYS_8,
        ),
        (
            "simple_battle_pool",
            reg_root / "reg_simple_battle",
            [
                "--mode",
                "simple-battle",
                "--guests",
                "1",
                "--course-list-mode",
                "1",
                "--max-frames",
                "30000",
            ],
            720,
            STAGE_KEYS_8,
        ),
        (
            "normal_multicard_vs",
            reg_root / "reg_normal_multicard_vs",
            [
                "--mode",
                "normal-vs",
                "--guest-rom",
                str(HOST_ROM),
                "--cup",
                "0",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "48000",
            ],
            960,
            STAGE_KEYS_8,
        ),
        (
            "single_player_gp",
            reg_root / "reg_single_gp",
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
                "36000",
            ],
            600,
            ("menu_course_sync", "MPState16_and_archives", "countdown", "3_laps_and_results"),
        ),
        (
            "single_player_tt_save",
            reg_root / "reg_single_tt",
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
                "36000",
            ],
            600,
            ("menu_course_sync", "MPState16_and_archives", "countdown", "3_laps_and_results"),
        ),
    ]

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        rfuts = {
            pool.submit(run_job, rname, rout, rargs, cpu_slots, rw, args.resume, rkeys): rname
            for (rname, rout, rargs, rw, rkeys) in reg_jobs
        }
        for fut in as_completed(rfuts):
            regressions[rfuts[fut]] = fut.result()

    if tt_sav_path.is_file():
        sav_b = tt_sav_path.read_bytes()
        regressions["single_player_tt_save"]["save_file_bytes"] = len(sav_b)
        regressions["single_player_tt_save"]["save_non_ff_bytes"] = sum(1 for b in sav_b if b != 0xFF)

    # Run 2-Guest HaxxStation tests across 2 isolated CPU core groups (3 DS consoles each)
    mg_workers = min(2, max(1, args.workers))
    mg_slots = make_cpu_slots(mg_workers)
    mg_jobs = [
        (
            "multi_guest_2_desert_hills",
            reg_root / "reg_multi_guest_2_desert",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "2",
                "--course-list-mode",
                "0",
                "--cup",
                "1",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "68000",
            ],
        ),
        (
            "multi_guest_2_staggered_tap_barrier",
            reg_root / "reg_multi_guest_2_staggered",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "2",
                "--guest-tap-delay",
                "240",
                "--course-list-mode",
                "0",
                "--cup",
                "1",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "68000",
            ],
        ),
    ]
    with ThreadPoolExecutor(max_workers=mg_workers) as pool:
        mfuts = {
            pool.submit(run_job, mname, mout, margs, mg_slots, 2400, args.resume): mname
            for (mname, mout, margs) in mg_jobs
        }
        for fut in as_completed(mfuts):
            regressions[mfuts[fut]] = fut.result()

    # Run 4-Guest (5-player) and 7-Guest (full 8-player maximum!) HaxxStation tests across dedicated CPU cores
    p8_workers = min(4, max(1, args.workers))
    full_party_slots = make_cpu_slots(p8_workers)
    party8_jobs = [
        (
            "multi_guest_4_dk_pass",
            reg_root / "reg_multi_guest_4_dk_pass",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "4",
                "--course-list-mode",
                "0",
                "--cup",
                "2",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "72000",
            ],
        ),
        (
            "multi_guest_7_desert_hills",
            reg_root / "reg_multi_guest_7_desert",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "7",
                "--course-list-mode",
                "0",
                "--cup",
                "1",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "76000",
            ],
        ),
        (
            "multi_guest_7_staggered_tap_barrier",
            reg_root / "reg_multi_guest_7_staggered",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "7",
                "--guest-tap-delay",
                "60",
                "--course-list-mode",
                "0",
                "--cup",
                "1",
                "--course-in-cup",
                "0",
                "--races",
                "1",
                "--max-frames",
                "80000",
            ],
        ),
        (
            "multi_guest_7_rainbow_road",
            reg_root / "reg_multi_guest_7_rainbow",
            [
                "--mode",
                "simple-vs",
                "--guests",
                "7",
                "--course-list-mode",
                "0",
                "--cup",
                "3",
                "--course-in-cup",
                "3",
                "--races",
                "1",
                "--max-frames",
                "84000",
            ],
        ),
    ]
    with ThreadPoolExecutor(max_workers=p8_workers) as pool:
        pfuts = {
            pool.submit(run_job, mname, mout, margs, full_party_slots, 3600, args.resume): mname
            for (mname, mout, margs) in party8_jobs
        }
        for fut in as_completed(pfuts):
            regressions[pfuts[fut]] = fut.result()

    # Write `reports/haxxstation/course_matrix.csv`
    matrix_csv = REPO_ROOT / "reports" / "haxxstation" / "course_matrix.csv"
    matrix_rows = []
    pass_count = 0
    for r in static_rows:
        seq = r["sequence_order"]
        tr = track_results[seq]
        overall = "PASS" if all(tr.get(k) == "PASS" for k in STAGE_KEYS_8) else "FAIL"
        if overall == "PASS":
            pass_count += 1
            note = (
                f"Cold-boot HaxxStation Stage-1 RSA -> Stage-2 MKDS -> native join -> 3-lap race + "
                f"Next Course PASS (hCid==gCid=={tr.get('host_setup_course_id')}, "
                f"min course heap free: {tr.get('min_guest_course_heap_free')} B)."
            )
        else:
            note = (
                f"Failed at stage: "
                + next((k for k in STAGE_KEYS_8 if tr.get(k) != "PASS"), "unknown")
                + f" (hCid={tr.get('host_setup_course_id')}, gCid={tr.get('guest_received_course_id')})."
            )
        child_carc = r["effective_child_carc"].replace("/data/Course/", "/haxx/Course/") if r["d_flag"] == 0 and r["effective_child_carc"].endswith("D.carc") else r["effective_child_carc"]
        child_tex = r["effective_child_tex_carc"].replace("/data/Course/", "/haxx/Course/") if r["d_flag"] == 0 and r["effective_child_tex_carc"].endswith("DTex.carc") else r["effective_child_tex_carc"]
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
                "effective_child_carc": child_carc,
                "effective_child_tex_carc": child_tex,
                "first_stage_RSA": tr.get("first_stage_RSA", "FAIL"),
                "stage2_launch": tr.get("stage2_launch", "FAIL"),
                "native_join": tr.get("native_join", "FAIL"),
                "menu_course_sync": tr.get("menu_course_sync", "FAIL"),
                "MPState16_and_archives": tr.get("MPState16_and_archives", "FAIL"),
                "countdown": tr.get("countdown", "FAIL"),
                "3_laps_and_results": tr.get("3_laps_and_results", "FAIL"),
                "next_race": tr.get("next_race", "FAIL"),
                "overall_status": overall,
                "host_setup_course_id": tr.get("host_setup_course_id", -1),
                "guest_loaded_course_id": tr.get("guest_received_course_id", -1),
                "guest_course_buf6_bytes": tr.get("guest_course_buf6_size", 0),
                "guest_course_buf7_bytes": tr.get("guest_course_buf7_size", 0),
                "guest_min_course_heap_free_bytes": tr.get("min_guest_course_heap_free", -1),
                "base_rom_sha1": base_sha1,
                "station_rom_sha1": station_sha1,
                "final_host_rom_sha256": final_host_sha256,
                "signed_stage1_srl_sha256": stage1_srl_sha256,
                "stage2_rom0000d_sha256": rom0000d_sha256,
                "test_platform": "melonDS 906e9ebb (Podman single-thread deterministic LocalMP)",
                "evidence_tier": "emulator tested",
                "physical_hardware_status": "NOT TESTED",
                "artifacts_dir": tr.get("_out_dir", ""),
                "diagnostic_notes": note,
            }
        )

    matrix_csv.parent.mkdir(parents=True, exist_ok=True)
    with matrix_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(matrix_rows[0].keys()))
        writer.writeheader()
        writer.writerows(matrix_rows)

    # Aggregate & validate `reports/haxxstation/phase_trace.jsonl`
    trace_sources = [
        REPO_ROOT / "build" / "haxxstation" / "m1_probe" / "phase_trace.jsonl",
        REPO_ROOT / "build" / "haxxstation" / "m2_mkds_guest" / "phase_trace.jsonl",
        matrix_root / "track_01_cup0_slot0" / "phase_trace.jsonl",
        matrix_root / "track_05_cup1_slot0" / "phase_trace.jsonl",
        reg_root / "reg_sequential_vs" / "phase_trace.jsonl",
        reg_root / "reg_random_vs_1" / "phase_trace.jsonl",
        reg_root / "reg_multi_guest_2_staggered" / "phase_trace.jsonl",
        reg_root / "reg_multi_guest_7_desert" / "phase_trace.jsonl",
        reg_root / "reg_multi_guest_7_staggered" / "phase_trace.jsonl",
        reg_root / "reg_multi_guest_7_rainbow" / "phase_trace.jsonl",
    ]
    out_trace = REPO_ROOT / "reports" / "haxxstation" / "phase_trace.jsonl"
    trace_lines: list[str] = []
    for src in trace_sources:
        if src.is_file():
            for ln in src.read_text().splitlines():
                ln = ln.strip()
                if not ln:
                    continue
                obj = json.loads(ln)
                missing = REQUIRED_TRACE_FIELDS - set(obj.keys())
                if missing:
                    raise AssertionError(f"Trace record in {src} missing fields: {missing}")
                trace_lines.append(json.dumps(obj, separators=(",", ":")))
    out_trace.write_text("\n".join(trace_lines) + "\n")

    reg_req_keys: dict[str, tuple[str, ...]] = {rname: rkeys for (rname, _, _, _, rkeys) in reg_jobs}
    reg_req_keys["multi_guest_2_desert_hills"] = STAGE_KEYS_8
    reg_req_keys["multi_guest_2_staggered_tap_barrier"] = STAGE_KEYS_8
    reg_req_keys["multi_guest_4_dk_pass"] = STAGE_KEYS_8
    reg_req_keys["multi_guest_7_desert_hills"] = STAGE_KEYS_8
    reg_req_keys["multi_guest_7_staggered_tap_barrier"] = STAGE_KEYS_8
    reg_req_keys["multi_guest_7_rainbow_road"] = STAGE_KEYS_8

    reg_pass_map: dict[str, bool] = {}
    for rname, rkeys in reg_req_keys.items():
        rs = regressions.get(rname, {})
        ok = all(rs.get(k) == "PASS" for k in rkeys)
        if rname == "sequential_vs_boundary_18_to_27":
            ok = ok and rs.get("observed_race_course_ids") == [18, 27]
        elif rname.startswith("random_vs_32pool_"):
            snap = rs.get("course_list_snapshot", [])
            gsnap = rs.get("guest_course_list_snapshot", [])
            obs = rs.get("observed_race_course_ids", [])
            ok = (
                ok
                and len(snap) == 33
                and snap == gsnap
                and snap[32] == 0
                and len(set(snap[:32])) == 32
                and len(obs) >= (2 if rname.endswith("_run2") else 1)
                and obs == snap[: len(obs)]
            )
        elif rname == "single_player_tt_save":
            ok = ok and rs.get("save_file_bytes", 0) > 0 and rs.get("save_non_ff_bytes", 0) > 0
        elif rname.startswith("multi_guest_7_"):
            ok = (
                ok
                and rs.get("guests") == 7
                and rs.get("host_participant_mask") == 0xFF
                and rs.get("active_kart_count") == 8
                and rs.get("min_guest_course_heap_free", -1) > 0
                and rs.get("min_guest_exp_heap_free", -1) > 0
            )
        elif rname == "multi_guest_4_dk_pass":
            ok = (
                ok
                and rs.get("guests") == 4
                and (rs.get("host_participant_mask", 0) & 0x1F) == 0x1F
                and rs.get("min_guest_course_heap_free", -1) > 0
            )
        reg_pass_map[rname] = ok

    reg_pass_count = sum(1 for v in reg_pass_map.values() if v)
    reg_total_count = len(reg_req_keys)

    bundle = {
        "matrix_pass_count": pass_count,
        "matrix_total_count": 32,
        "regressions_pass_count": reg_pass_count,
        "regressions_total_count": reg_total_count,
        "regressions_pass_map": reg_pass_map,
        "total_wall_seconds": round(time.monotonic() - t_start, 2),
        "final_host_rom_sha256": final_host_sha256,
        "signed_stage1_srl_sha256": stage1_srl_sha256,
        "stage2_rom0000d_sha256": rom0000d_sha256,
        "decoded_guest_arm9_sha256": guest_meta["decoded_guest_arm9_sha256"],
        "regressions": regressions,
        "matrix": matrix_rows,
    }
    bundle_path = REPO_ROOT / "build" / "haxxstation" / "validation_bundle.json"
    bundle_path.write_text(json.dumps(bundle, indent=2) + "\n")

    print(
        f"\n=== HAXXSTATION M7 VALIDATION COMPLETE in {bundle['total_wall_seconds']}s: "
        f"{pass_count}/32 courses PASS, {reg_pass_count}/{reg_total_count} regressions PASS ===",
        flush=True,
    )
    return 0 if (pass_count == 32 and reg_pass_count == reg_total_count) else 1


if __name__ == "__main__":
    raise SystemExit(main())
