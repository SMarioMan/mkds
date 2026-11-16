# Mario Kart DS — Simple-Mode 32-Track Host Patch (`feature/simple-mode-32-tracks`)

Reproducible **host-ROM-only** implementation, Download Play asset pipeline, C unit test suite, and multi-instance `melonDS` wireless validation harness investigating and implementing 32-track Simple-mode (*DS Download Play*) support on *Mario Kart DS* (USA revision `AMCE-00`).

> **Important Status Summary (§9 Escalation):**  
> All host-side requirements—8-cup / 32-track UI selection (`SelectCupGrid` mode `3`), 32-track Sequential & Random VS course lists (`BuildCourseList`), linker-shift & `OS_GetInitArenaLo` safety, byte-for-byte preservation of the retail RSA-1024 signed Download Play guest payload (`SHA-256: f044f74ec045b96ab8982f9b2a7765fb18b462633627364c55b3c8c41c6b3008`), and automatic `D`-archive generation + wireless `RomArchiveBlock` FAT redirection bringing all 32 tracks inside the stock guest's `0x4B000` (`307,200` B) `mCourseBufferHeap`—are **implemented, built in Podman, and verified**.  
> However, live multi-instance Download Play testing across all 32 tracks and decompiled source verification prove that **MKDS Local Wireless Multiplayer never transmits `courseId` or `mCourseList` packets from host to guest**. Instead, the unmodified signed guest `ARM9` executable independently steps `SelectCupGrid` (`mMode == 3`, locked to cups `{0, 4}`) and `BuildCourseList` (`m = 2`, locked to `data_021540c0 = {0, 4}`) in guest RAM. Consequently, **8/32 tracks** (Mushroom + Shell Cups) pass end-to-end 3-lap races with unmodified guests, while the **24 tracks in Flower, Star, Special, Banana, Leaf, and Lightning Cups** experience deterministic host/guest `COURSE_DESYNC` unless the guest executable is also patched (requiring an alternate delivery path such as HaxxStation under a separate revised plan; see `reports/simple32_findings.md`).

---

## 1. Prerequisites & Copyright Hygiene

No Nintendo ROMs, BIOS dumps, firmware images, extracted assets, or signed binaries are tracked in Git. Supply your own legally dumped files locally before building:

| Input File | Path (relative to workspace root `..`) | Expected Size & Hash |
|---|---|---|
| Untrimmed retail MKDS USA ROM (`AMCE-00`) | `mkds/extract/baserom_mkds_usa.nds` | `33,554,432` B (`SHA-1: 691e00d9a5dd80b04f80cc7559503e8b06848785`) |
| Nintendo DS ARM7 BIOS (emulator tests only) | `BIOSNDS7.ROM` | `16,384` B |
| Nintendo DS ARM9 BIOS (emulator tests only) | `BIOSNDS9.ROM` | `4,096` B |
| Nintendo DS Firmware (emulator tests only) | `FW600DF0.BIN` | `262,144` B |

*(If your MKDS USA dump is trimmed to `25,562,150` B with `SHA-1: d9c2cc22c9e5ab385ba479770d5c09705e6d143f`, pad `0xFF` bytes to `33,554,432` B at `mkds/extract/baserom_mkds_usa.nds` to reproduce the canonical retail untrimmed SHA-1.)*

---

## 2. Reproducible Containerized Toolchain (Podman)

All compilation (`wibo`, Metrowerks `mwccarm`/`mwldarm`, `dsd`, `gcc` unit tests, `D`-archive generation, MKDS ROM linking, and the headless `melonDS` multi-instance harness) runs inside a self-contained Podman container built from `Containerfile`:

| Component | Pinned Version / Commit |
|---|---|
| Base OS | `docker.io/library/ubuntu:24.04` |
| `wibo` | `420a75b` (built from `tools/build_wibo.sh`, 53-bit x87 precision) |
| `dsd` | `v0.12.1` |
| `objdiff-cli` | `v3.8.2` |
| Metrowerks `mwccarm` / `mwldarm` | `2.0/sp2p2` (`dsd` toolchain bundle) |
| `gcc` / `g++` | `13.3.0` (Ubuntu `24.04`) |
| `melonDS` (in-container clone) | `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` (`/opt/melonds-src`, interpreter core) |

---

## 3. Step-by-Step Build & Verification Commands

Run all commands from the `mkds/` repository root:

### 3.1 Build the Podman Image
```sh
./scripts/build_simple32_podman.sh build-image
```

### 3.2 Extract & Verify Unmodified Retail Baseline (First-Time Setup)
If `extract/usa/` has not been extracted yet (on a clean checkout prior to applying patches):
```sh
podman run --rm -v "$(pwd)/..:/work:Z" -w /work/mkds localhost/mkds-simple32:latest \
    bash -c "python3 tools/configure.py usa && ninja && ninja check"
```

### 3.3 Build Patched Host ROM, 15 `D`-Archives, Unit Tests & `dlp_harness`
```sh
./scripts/build_simple32_podman.sh build
```
This executes `scripts/container_build_all.sh` inside Podman, which:
1. Compiles `build/dlp_harness/dlp_harness` against `/opt/melonds-build/src/libcore.a`.
2. Compiles and runs `tests/test_simple32_logic.c` (`32` sequential starting courses, `64`x wrap-around transitions, `100` random shuffle seeds, Simple-mode Battle 3-stage preservation, and Normal Multi-Card VS unlock gating).
3. Runs `scripts/stage_stock_cloneboot.py` to stage the untouched retail Cloneboot payload (`mb_child_usa.bin`, `1,114,304` B) and stock `ARM9` overlays (`mb_child_ov0..3.bin`, `mb_child_ovt.bin`) into gitignored `extract/usa/files/data/Boot/`.
4. Runs `scripts/build_simple32_d_archives.py` to build 15 gitignored `<stem>D.carc` and `<stem>DTex.carc` archives under `extract/usa/files/data/Course/`, verifying lossless LZ10 round-trip decompression and `mCourseBufferHeap` (`0x4B000`) headroom `>= +9,088` B on every track.
5. Links `build/usa/mkds_usa.nds` (`33,554,432` B) with zero linker warnings and exports `build/usa/host_nm.txt`.

### 3.4 Run Static 32-Course Archive & Heap Audit
```sh
./scripts/build_simple32_podman.sh audit
```
Writes `reports/simple32_static_audit.csv` and `reports/simple32_static_audit.json`, asserting all 32 `gCupCourseIdTable` entries resolve to existing archives and fit within `0x4B000` (`307,200` B) after wireless `RomArchiveBlock` FAT redirection.

### 3.5 Run Full 32-Track Download Play Matrix & Regression Suite
```sh
./scripts/build_simple32_podman.sh validate
```
Executes `scripts/run_simple32_validation.py` inside Podman, running:
- Cold-boot Download Play RSA-1024 verification + byte-for-byte comparison of live `host_transmitted_cloneboot.bin` against retail reference (`0` byte differences across `1,114,304` B).
- All **32 retail tracks** in canonical `gCupCourseIdTable` order (`build/tests/matrix/track_01_cup0_slot0` .. `track_32_cup7_slot3`).
- Mandatory regressions (`Sequential VS` 4th->5th race boundary, `Random VS` 32-track permutation, `2-Guest Simple Mode`, `Simple-Mode Battle`, `Normal Multi-Card VS`, `Single-Player GP`, and `Single-Player Time Trial` `.sav` persistence).
- §9 diagnostic proof (`raw_desert_overflow` vs `downscaled_desert_3laps`) empirically ruling out host-side asset, decompression, heap, and block-transfer limits on the stock signed guest.
- Outputs `reports/simple32_course_matrix.csv` and `build/tests/validation_results.json`.

---

## 4. Modified & Added Files

- **Host ROM Production Patches**:
  - `src/Main/Unknown/code_021189f4.c`: Enable all 8 cups (`cupIndex < 8`) in `GetSelectCupGridCupEnabled()` for `SelectCupGrid` selector `case 3` (`Simple` mode) only.
  - `src/Main/Unknown/code_02045874.c`: Expand `BuildCourseList()` in Simple-mode VS (`case 1` sequential and `case 2` random when `isVersus != 0`) to all 8 cups / 32 tracks (`data_02154108`) regardless of host save unlocks, without altering `data_0217aa00->mUnk_0454 & 0x40`, Simple-mode Battle (`3` stages), or Normal Multi-Card / Single-Player VS unlock checks.
  - `src/Main/Unknown/code_02008880.c` & `src/Main/Unknown/code_02008570.c`: Serve untouched retail signed Cloneboot image (`/data/Boot/mb_child_usa.bin` + live BIOS-decrypted `0x02000000..0x02000800` Key1 Secure Area) so shifted Host `ARM9` code never invalidates the guest RSA-1024 signature.
  - `src/Main/Unknown/code_0204bca0.c`: Serve untouched stock `ARM9` overlays (`mb_child_ov0..3.bin`) and redirect wireless `RomArchiveBlock` FAT entries of the 15 over-budget courses to `<stem>D.carc` / `<stem>DTex.carc` for Download Play guests while keeping full-resolution NitroFS files on Host.
  - `libs/NitroSDK/os/src/code_0200eddc.c`, `tools/lcf_force_active.py`, `config/usa/arm9/lcf_edits.txt`: Linker-shift safety (`OV003_BSS_END` arena base and `append_data` placement).
- **Build, Asset, Audit & Test Tooling**:
  - `Containerfile`, `scripts/build_simple32_podman.sh`, `scripts/container_build_all.sh`, `scripts/container_build_rom.sh`
  - `scripts/stage_stock_cloneboot.py`, `scripts/build_simple32_d_archives.py`, `scripts/simple32_course_audit.py`, `scripts/run_simple32_validation.py`
  - `tests/test_simple32_logic.c`
  - `scripts/dlp_harness/` (`CMakeLists.txt`, `PlatformImpl.cpp`, `dlp_harness.cpp`)
- **Reports & Evidence**:
  - `reports/simple32_findings.md` (Diagnostic report + §9 Blocker & Revised-Plan Request)
  - `reports/simple32_course_matrix.csv` & `reports/simple32_static_audit.csv`
