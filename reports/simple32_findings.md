# Mario Kart DS Simple-Mode 32-Track Investigation, Implementation & §9 Blocker Report

- **Repository / Branch**: `SMarioMan/mkds` — `feature/simple-mode-32-tracks` (base commit `031ab8bf36c1060a6d4cfca8742f2ef035a5f2df`)
- **Target Region / Revision**: USA (`AMCE-00`)
- **User-Supplied Base ROM**:
  - Trimmed input dump (`25,562,150` B): `SHA-1: d9c2cc22c9e5ab385ba479770d5c09705e6d143f`
  - Canonical untrimmed 32 MiB image (`33,554,432` B): `SHA-1: 691e00d9a5dd80b04f80cc7559503e8b06848785` (`ninja check` exact baseline match)
- **Containerized Build & Test Environment**: Podman `localhost/mkds-simple32:latest` (`Containerfile`: Ubuntu 24.04, `wibo 420a75b`, `dsd v0.12.1`, `objdiff-cli v3.8.2`, Metrowerks `mwccarm`/`mwldarm 2.0/sp2p2`, `gcc 13.3.0`, headless `melonDS 906e9ebb27da8c6a715cd7abab4abfe8a8d29427`)

---

## 1. Acceptance Criteria & Evidence-Tier Scorecard

Per the Implementation Brief, evidence tiers are strictly separated into `code reviewed`, `build passed`, `emulator tested`, and `physical-hardware verified`. Physical Nintendo DS hardware was unavailable in this automated container environment and is explicitly reported as **`NOT TESTED`**.

| Criterion | Status | Highest Evidence Tier | Summary of Evidence |
|---|---|---|---|
| **1. Normal host boot, Simple-mode discovery, and identical signed guest download** | **PASS** | `emulator tested` (hardware: `NOT TESTED`) | Rebuilt shifted Host ROM (`33,554,432` B) boots cleanly, advertises Simple-mode Download Play, and transmits a `1,114,304`-byte Cloneboot image with **0 byte differences** (`SHA-256: f044f74ec045b96ab8982f9b2a7765fb18b462633627364c55b3c8c41c6b3008`) vs retail MKDS USA; unpatched DS Firmware verifies the RSA-1024 trailer and boots MKDS (`*(u16*)0x027ffc40 == 2`). |
| **2. Host Simple-mode UI visibly enables all 8 cups / 32 standard tracks with accurate names, icons & IDs** | **PASS** | `emulator tested` | `GetSelectCupGridCupEnabled()` (`case 3`) enables cups `0..7` (`cups_enabled = [1,1,1,1,1,1,1,1]` even on a blank save). Verified across all 32 tracks in `SelectCupGrid` (`02_cup_select.png`) and `SelectCupCourse` (`03_course_select.png`), matching `gCupCourseIdTable` 32/32. |
| **3. Host and unmodified signed guest(s) enter, complete 3 laps, and exit races on all 32 tracks** | **BLOCKED (8/32 PASS, 24/32 FAIL)** | `emulator tested` + `code reviewed` | All **8** Mushroom + Shell Cup tracks complete 3-lap Simple-mode races + `Next Course` transitions with unmodified guests. All **24** tracks in Flower, Star, Special, Banana, Leaf, and Lightning Cups fail with deterministic `COURSE_DESYNC` because the RSA-signed guest `ARM9` binary computes its own `courseId` locally from `SelectCupGrid` (`mMode == 3`, locked to `{0, 4}`). |
| **4. Sequential & Random VS course lists include all 32 tracks without reverting to 8-track pool** | **HOST PASS / GUEST BLOCKED** | `emulator tested` + `unit tested` | Host `BuildCourseList()` constructs full 32-track sequential & random lists (`mCourseList[0..31]` + `0` terminator; verified across 32 starts, 64x wrap-around, and 100 PRNG seeds in `tests/test_simple32_logic.c`). Unmodified guest `BuildCourseList()` remains locked to `data_021540c0 = {0, 4}`, diverging on Race #2 (`4th -> 5th` boundary: Host `27` vs Guest `10`) in Sequential VS and Race #1 (Host `35` vs Guest `11`) in Random VS. |
| **5. Zero regressions in Multi-Card VS, Single-Player GP/TT, Battle mode, save data, and original 8 Simple tracks** | **PASS** | `emulator tested` + `unit tested` | Verified in Podman `melonDS`: Normal Multi-Card VS (`6/6 PASS`), Single-Player GP (`6/6 PASS`), Single-Player Time Trial + 256 KiB `.sav` persistence (`6/6 PASS`, `21,942` non-`0xFF` bytes saved), Simple-Mode Battle (`6/6 PASS`, 3-stage pool `[44, 47, 43]` preserved), 2-Guest Simple Mode (`boot`, `lobby`, `xfer` `PASS`), and all 8 original Simple-mode tracks. |

---

## 2. Signed Downloadable Guest Preservation Proof

Because patching `ARM9` code shifts host `ARM9` sections relative to the retail binary covered by Nintendo's RSA-1024 Wireless Multiboot signature at ROM offset `0x01860B9E`, our build pipeline (`scripts/stage_stock_cloneboot.py` + `src/Main/Unknown/code_02008880.c`, `code_02008570.c`, `code_0204bca0.c`) stages the untouched retail Cloneboot payload (`mb_child_usa.bin`) and stock `ARM9` overlays (`mb_child_ov0..3.bin`, `mb_child_ovt.bin`), copying the live BIOS-decrypted Key1 Secure Area (`0x02000000..0x02000800`) into `rangeTable->mUnk_30[1].mData` at runtime.

| Artifact | Byte Size | SHA-256 Digest | Result |
|---|---:|---|---|
| Retail MKDS USA live Cloneboot payload (`retail_reference_cloneboot.bin`) | `1,114,304` | `f044f74ec045b96ab8982f9b2a7765fb18b462633627364c55b3c8c41c6b3008` | Reference |
| Patched Host live Cloneboot payload (`host_transmitted_cloneboot.bin`) | `1,114,304` | `f044f74ec045b96ab8982f9b2a7765fb18b462633627364c55b3c8c41c6b3008` | **EXACT MATCH (`0` byte diffs)** |
| RSA-1024 Wireless Multiboot signature trailer (`+0x160`, `136` B) | `136` | `953e20fae2812a630cf160a4152845b355df10090f32f8759fe930c65e4710df` | **EXACT MATCH** |
| Downloaded DS header in Guest RAM (`0x027FFE00..0x027FFF60`) | `352` | `bd086e3f3c3247ab39f32b7dacec6811736ef904bc7443976200135c105de548` | **EXACT MATCH (`bootType == 2`)** |

---

## 3. Host-Side Implementation Summary

1. **Simple-Mode Cup Grid Unlock (`src/Main/Unknown/code_021189f4.c`, `GetSelectCupGridCupEnabled`)**:
   - Changed `SelectCupGrid` selector `case 3` (`SELECT_CUP_GRID_MODE_MULTI_SIMPLE`) from `if (cupIndex == 0 || cupIndex == 4)` to `if (cupIndex < 8)`.
   - Left `case 0`, `1`, `2` (Single-Player GP/TT/VS) and `case 4` (Normal Multi-Card VS) untouched.
2. **Simple-Mode 32-Track Sequential & Random Course Lists (`src/Main/Unknown/code_02045874.c`, `BuildCourseList`)**:
   - Preserved `data_0217aa00->mUnk_0454 & 0x40` completely untouched.
   - In both `case 1` (Sequential) and `case 2` (Random) `if (isVersus != 0)` branches, set `m = 8`, `t = data_02154108` (`{0,1,2,3,4,5,6,7}`), and `if (simpleMode || func_02046648(t[p]) != FALSE)` so Simple-mode VS populates all 32 `gCupCourseIdTable` course IDs into `gRaceSetup->mCourseList[0..31]` with `mCourseList[32] == 0` (`0x21`-byte allocation preserved without overflow), even when the host save has not unlocked Star/Special/Leaf/Lightning Cups.
   - Simple-mode Battle (`isVersus == 0`) continues using `n = 3, t = data_021540c8` (`{44, 47, 43}`).
3. **Linker-Shift & Main-RAM Arena Safety (`libs/NitroSDK/os/src/code_0200eddc.c`, `tools/lcf_force_active.py`, `config/usa/arm9/lcf_edits.txt`)**:
   - Updated `OS_GetInitArenaLo(OS_ARENA_MAIN)` from a hardcoded `0x021da300` literal to `(void *) OV003_BSS_END` (`0x021da500` after host patches) so host `ARM9` growth never overlaps the initial Main-RAM heap arena.
   - Added `append_data` support in `tools/lcf_force_active.py` so new constants in patched `ARM9` translation units link cleanly before `ARM9_DATA_END` with zero `mwldarm` warnings.

---

## 4. 32-Course Archive, Heap & Block-Transfer Audit + Empirical `D`-Archive Solution

### 4.1 Stock Download Play Guest Resource Limits
Tracing `InitBlockEntries(heap, 1)` (`src/Main/Unknown/code_0204f240.c`), `func_02039e08` (`src/Main/Unknown/code_02039e08.c`), `RequestMPState16` (`src/Main/Unknown/code_0205928c.c`), and `RequestRaceArchives` / `LoadArchiveSlot` (`src/Main/Unknown/code_02047104.c`) established the exact guest resource constraints:
- **Pending file transfer queue (`AddPendingFile`)**: `12` slots; `RequestRaceArchives` queues `6` files per race (`6/12` slots free).
- **Guest compressed transfer buffer (`mCompressedBuffer`)**: `0x4B000` (`307,200` B); largest compressed course archive on the retail ROM is `rainbow_course.carc` at `202,463` B (`OK`).
- **Guest course buffer frame heap (`mCourseBufferHeap`)**: `0x4B000` (`307,200` B). During `MPState 16`, this frame heap simultaneously holds **uncompressed** `<stem>.carc` (`mBuffers[6]`) plus **compressed** `<stem>Tex.carc` (`mBuffers[7]`), requiring `0x20 + align32(uncomp(.carc)) + align32(comp(Tex.carc)) <= 307,200` B.

### 4.2 Static Audit & Automatic `D`-Archive Redirection (`scripts/simple32_course_audit.py` & `scripts/build_simple32_d_archives.py`)
Running `scripts/simple32_course_audit.py` against the retail USA ROM proved that **15 of the 24 locked-cup tracks** have `d_flag == 0` in `data_02154228`, lack retail `D` archives, and **overflow `mCourseBufferHeap` (`0x4B000`)** by `5,152` B (`pinball_course`) up to `81,408` B (`desert_course`).

To solve this completely on the host side without modifying the guest executable:
1. `scripts/build_simple32_d_archives.py` follows Nintendo's retail `mansion_courseD` / `cross_courseD` layout:
   - Preserves `course_collision.kcl`, `course_map.nkm`, `course_model.nsbtp`, and `/MapObj/*` byte-for-byte while stripping single-player `/MissionRun/*.nkm`, `/course_model_V.nsbmd`, and `/course_model*.nsbta`.
   - Strips `/course_model_V.nsbtx`, 2x-subsamples `Comp4x4` (`fmt == 5`) textures in `/course_model.nsbtx` while updating `mExtraParam` + material `TEXMTX_USE` / `GX_TEXGEN_TEXCOORD` flags so NitroSystem G3D (`bindMdlTex_Internal_`) hardware-scales UVs via `mMagW`/`mMagH`, and block-quantizes `/Map2D/local.NCGR` 8bpp tiles in-place.
2. `CreateRomArchiveBlock()` (`src/Main/Unknown/code_0204bca0.c`) redirects the `[startOffset, endOffset]` entries of those 15 `<stem>.carc` and `<stem>Tex.carc` files **only inside the wireless `RomArchiveBlock` FAT** sent to Download Play guests (`noFile != FALSE`), so the Host continues loading full-resolution retail archives locally while guests transparently receive `0x4B000`-safe `D` archives (`+9,088` B to `+69,984` B headroom across all 15 tracks; `0` static overflows across all 32 tracks in `reports/simple32_static_audit.csv`).

### 4.3 Live Runtime Proof Ruling Out Host Asset, Decompression, Heap & Transfer Limits
To verify this empirically on the **unmodified RSA-signed guest executable** inside `melonDS`, `scripts/run_simple32_validation.py` ran two controlled wireless FAT-redirection diagnostics (`build/tests/diag_heap_proof/`):
- **`diag_raw_desert_overflow`** (redirecting `cross_courseD` to raw retail `desert_course.carc` `260,976` B uncomp + `desert_courseTex.carc` `127,570` B comp = `388,608` B): Guest `mCourseBufferHeap` dropped to `46,160` B free after allocating `desert_course.carc`, causing the subsequent `127,570`-byte `desert_courseTex.carc` allocation to return `NULL` and **fail `MPState 16` (`asset_transfer = FAIL`)**.
- **`diag_downscaled_desert_3laps`** (redirecting `cross_courseD` to our generated `desert_courseD.carc` `237,520` B uncomp + `desert_courseDTex.carc` `45,840` B comp = `283,424` B): **Passed `MPState 16` transfer, LZ10 decompression, `ov000` `RaceScene` load, and `GO!` countdown (`asset_transfer = PASS`, `race_countdown = PASS`, live `min_guest_course_heap_free = 23,760` B)**, rendering 3D `Desert Hills` geometry and downsampled `Comp4x4` textures simultaneously on both Host and the unmodified signed Guest (`build/tests/diag_heap_proof/downscaled_desert_3laps/04_race_start_go.png`).

---

## 5. Mandatory Regression Suite Results (`melonDS 906e9ebb` in Podman)

| Test Case | Mode / Args | Result | Key Telemetry & Evidence (`build/tests/regressions/`) |
|---|---|---|---|
| **C Logic Unit Tests** | `tests/test_simple32_logic.c` | **PASS** | `32` sequential starts, `64` consecutive race wrap-arounds (`31 -> 0`), `100` PRNG random seeds (exact 32-track permutations, `mCourseList[32] == 0`, guard canaries intact), Battle `[44, 47, 43]`, Normal VS unlock gating, `mUnk_0454 & 0x40` preserved. |
| **Sequential VS (4th -> 5th Race Boundary)** | `simple-vs`, `--course-list-mode 1`, start `Luigi's Mansion` (`18`), `--races 2` | **Race #1 PASS -> Boundary `COURSE_DESYNC`** | Host `course_list_snapshot` contains all 32 tracks + `0`; Race #1 (`18`) completed all 3 laps (`frame 30,540`); on Race #2, Host loaded `Desert Hills` (`hCid = 27`) while unmodified Guest wrapped to `SNES Mario Circuit 1` (`gCid = 10`). |
| **Random VS Pool** | `simple-vs`, `--course-list-mode 2` | **Host 32-Pool / Guest `COURSE_DESYNC`** | Host shuffled all 32 tracks (`64` PRNG calls -> `hCid = 35`, `SNES Donut Plains 1`), while unmodified Guest shuffled 8 tracks (`16` PRNG calls -> `gCid = 11`, `N64 Moo Moo Farm`). |
| **2-Guest Simple Mode** | `simple-vs`, `--guests 2` (`3` DS consoles) | **Boot + Lobby + Asset Transfer PASS** | 1 Host + 2 unmodified Download Play Guests completed RSA-1024 boot, joined lobby, selected `Figure-8 Circuit` (`20`), and transferred course archives (`min_guest_course_heap_free = 42,152` B). |
| **Simple-Mode Battle** | `simple-battle`, `--course-list-mode 1` | **6/6 PASS** | Preserved 3-stage Simple-mode Battle pool (`course_list_snapshot = [47, 43, 44, 0]`), entered `Tart Top` (`stage 47`, `min_guest_course_heap_free = 50,156` B), verified active battle gameplay (`05_battle_active.png`). |
| **Normal Multi-Card VS** | `normal-vs` (`2` cartridge DS consoles) | **6/6 PASS** | Preserved standard participant cup unlock gating (`cups_enabled = [1, 1, 0, 0, 1, 1, 0, 0]`) and completed full 3-lap wireless race + transition on `Figure-8 Circuit` (`20`). |
| **Single-Player Grand Prix** | `single-gp` (`50cc` Mushroom Cup) | **6/6 PASS** | Preserved standard backup save cup unlock gating (`cups_enabled = [1, 1, 0, 0, 1, 1, 0, 0]`) and completed full 3-lap race. |
| **Single-Player Time Trial + Save** | `single-tt` + `--save-file mkds_tt.sav` | **6/6 PASS** | Completed 3-lap Time Trial on `SNES Mario Circuit 1` (`10`) and persisted valid `262,144`-byte EEPROM save (`21,942` non-`0xFF` bytes written). |

---

## 6. §9 Blocker and Revised-Plan Request

Per **Section 9 (Stop / Escalate Rule)** of the Implementation Brief, because the unmodified Nintendo-signed MKDS Download Play guest executable lacks essential course-selection synchronization for tracks outside Mushroom and Shell Cups—and any remedy requires modifying the guest executable—this workstream stops at the verified host-ROM-only boundary and submits the following **Blocker and Revised-Plan Request**.

### 6.1 Exact Courses Affected and Earliest Failure Point
- **Passing courses (`8 / 32`)**: All 8 courses in **Mushroom Cup (`cup 0`: IDs `20, 22, 31, 18`)** and **Shell Cup (`cup 4`: IDs `10, 11, 13, 14`)** pass all 6 stages (`boot_download`, `lobby_selection`, `asset_transfer_and_course_sync`, `race_countdown`, `full_race_results`, `next_race_transition` = `PASS` in `reports/simple32_course_matrix.csv`).
- **Blocked courses (`24 / 32`)**: Every course in **Flower Cup (`cup 1`: `27, 28, 33, 24`)**, **Star Cup (`cup 2`: `30, 17, 25, 19`)**, **Special Cup (`cup 3`: `34, 26, 32, 29`)**, **Banana Cup (`cup 5`: `35, 16, 12, 9`)**, **Leaf Cup (`cup 6`: `15, 36, 37, 38`)**, and **Lightning Cup (`cup 7`: `39, 23, 40, 1`)**.
- **Earliest failure point by mode**:
  1. **`Choose` mode (`mCourseListMode == 0`)**: Host and Guest both pass Download Play RSA boot (`boot_download = PASS`), and Host visibly enables and selects the target cup and course (`lobby_selection = PASS`). However, during `SelectCupGrid` navigation, Guest ignores Host's D-Pad/touch inputs onto disabled cups `{1, 2, 3, 5, 6, 7}`, remaining on Mushroom Cup (`{20, 22, 31, 18}` for Nitro cups `1..3`) or Shell Cup (`{10, 11, 13, 14}` for Retro cups `5..7`). Both consoles then enter `MPState 16` requesting **different courses** (`hCid` = locked track vs `gCid` = Mushroom/Shell track -> `COURSE_DESYNC`).
  2. **`Sequential` mode (`mCourseListMode == 1`)**: Races `1..4` (if started in Mushroom Cup) succeed, then diverge at the **4th -> 5th race transition** (`Host` advances to `Desert Hills` `27`, while `Guest` wraps to `SNES Mario Circuit 1` `10`). Starting on any locked cup diverges immediately on **Race #1**.
  3. **`Random` mode (`mCourseListMode == 2`)**: Diverges immediately on **Race #1** (`Host` performs `64` PRNG calls across `32` courses -> `hCid = 35`, while `Guest` performs `16` PRNG calls across `8` courses -> `gCid = 11`).

### 6.2 Why Pure Host-Side NitroFS FAT Swapping Cannot Work Around the Blocker
In `diag_downscaled_desert_3laps`, dynamically pointing `cross_courseD.carc` / `cross_courseDTex.carc` at `desert_courseD.carc` / `desert_courseDTex.carc` proved that the stock guest can transfer, decompress, and render downscaled assets for any track. However, dynamic NitroFS path-swapping behind the 8 stock filenames **cannot** satisfy the 32-track goal because the guest's `gRaceSetup->mCourseId` remains one of the 8 stock IDs (`{20, 22, 31, 18, 10, 11, 13, 14}`), which corrupts:
1. **Course audio & music sets**: `LoadCourseSoundSet(data_0217aa00->mCourseId)` (`src/Main/Unknown/code_0205928c.c:333`) loads the sound/music bank for Guest's local `mCourseId` (causing missing object SFX or wrong BGM).
2. **Course intro banners & results UI**: `GetCourseMessageIndex()` displays the wrong track name on Guest.
3. **Course-ID-gated gameplay, physics, cameras, and stage objects** across Guest `ITCM` and `ARM9`: e.g., `COURSE_ID_RAINBOW` (`29`) loop/corkscrew gravity handling (`src/ITCM/code_01ff9598.c:114`), `COURSE_ID_PINBALL` (`33`) pinball bumpers/slot triggers, `COURSE_ID_AIRSHIP` (`19`), `COURSE_ID_KOOPA` (`32`), `COURSE_ID_OLD_KOOPA_AGB` (`12`), and `COURSE_ID_OLD_SKY_AGB` (`40`).

### 6.3 Exact Guest Executable Symbols, USA Addresses & Call Chains Causing the Lock
1. **Absence of Wireless `courseId` / `mCourseList` Packets in Local Multiplayer**:
   - Unlike Nintendo WFC (`WiFiSession` / `TallyPickedCourse` in `src/Main/Unknown/code_02120610.c`), Local Wireless Multiplayer (`data_021759d0->mUnk_20 == 1` on Host, `2` on Guest) **never transmits a selected `courseId` or `mCourseList` packet from Host to Guest**.
   - Instead, `func_020cc744` (`src/Main/Unknown/code_020cc744.c`) replicates the Host's raw controller buttons and touch coordinates into `data_02175628[8]` (`UNK_02175628_INDEX_8`), and both consoles step `MenuScene` (`ov002`) in lockstep to compute `gRaceSetup->mNextHeader.mCourseId` locally. (`RequestMPState16` at `src/Main/Unknown/code_0205928c.c:216-225, 293` reads Guest's own `gRaceSetup->mCourseId` into Guest's local `data_0217aa00->mCourseId`.)
2. **Permanent Simple-Mode Flag on Guest (`0x027FFC40 == 2`)**:
   - `func_0204ad44` (USA `0x0204AD44`, `src/Main/Unknown/code_0204ad44.c:99-101`): at boot, checks `*(u16 *) 0x027ffc40 == 2` (set by DS Firmware Download Play) and sets `data_021759d0->mUnk_1c_2 = 1`.
   - `func_0204aa28` (USA `0x0204AA28`, `src/Main/Unknown/code_0204aa04.c:11-17`): the sole setter of `mUnk_1c_2` after boot immediately returns without modifying `mUnk_1c_2` whenever `*(const u16 *) 0x027ffc40 == 2u`.
3. **Guest `Choose`-Mode Cup Lock (RSA-Signed Static `ARM9`)**:
   - `InitCurrentMenuScreen` (`src/Main/Unknown/code_020cb618.c:66-72`, `ov002`) calls `func_020cae38()` (`return data_021759d0->mUnk_1c_2;`) and initializes `SelectCupGrid` with `mMode = 3`.
   - `GetSelectCupGridCupEnabled` (USA `0x02118BF8`, `src/Main/Unknown/code_021189f4.c:484-488`, **inside RSA-1024 signed static `ARM9`**): for `case 3`, enables only `cupIndex == 0 || cupIndex == 4` (`mCupsEnabled = {1, 0, 0, 0, 1, 0, 0, 0}`).
   - `AdvanceSelectCupGridState3` (USA `0x02117EBC`, `src/Main/Unknown/code_021173e4.c:723-795`, **inside RSA-1024 signed static `ARM9`**): guards every touch rectangle (`line 725`) and every D-Pad transition (`lines 748, 761, 774, 787`) with `mCupsEnabled[...] != 0`.
4. **Guest `Sequential` / `Random` Pool Lock (RSA-Signed Static `ARM9`)**:
   - `func_0204df90` (`src/Main/Unknown/code_0204df90.c:301-305`, **static `ARM9`**): when `*(u16 *) 0x027ffc40 == 2`, sets `data_0217aa00->mUnk_0454 |= 0x40` in Guest RAM.
   - `BuildCourseList` (USA `0x02045A4C`, `src/Main/Unknown/code_02045874.c:131-228`, **inside RSA-1024 signed static `ARM9`**): evaluates `f = (data_0217aa00->mUnk_0454 & 0x40) != 0` (`1` on Guest), forcing `m = 2` and `t = data_021540c0` (`{0, 4}`).

### 6.4 Minimal Guest-Side Patch Required vs What Remains Host-Only
If a revised plan authorizes delivering a modified guest executable:
- **Guest-side change (minimal — 2 functions / ~6 Thumb instructions)**: Apply the **exact same two source edits** already implemented on the Host in `GetSelectCupGridCupEnabled()` (`src/Main/Unknown/code_021189f4.c:485`, `cupIndex < 8`) and `BuildCourseList()` (`src/Main/Unknown/code_02045874.c:142-156, 205-213`, `m = 8, t = data_02154108, simpleMode || func_02046648(...)`) to the guest `ARM9` image! Because Host and Guest already step `SelectCupGrid`, `SelectCupCourse`, and `BuildCourseList` identically from replicated inputs + synchronized PRNG seed, no custom wireless packets are needed.
- **Remains 100% Host-only (already complete in this branch)**: All 15 generated `<stem>D.carc` / `<stem>DTex.carc` archives (`scripts/build_simple32_d_archives.py`) and the wireless `RomArchiveBlock` FAT redirection in `CreateRomArchiveBlock()` (`src/Main/Unknown/code_0204bca0.c`) remain purely on the Host—no guest heap resizing or `data_02154228` edits are required.

### 6.5 Revised-Plan Delivery Options & Feasibility Questions to Resolve Before Approval
Because stock retail DS Firmware rejects any modified MKDS Cloneboot binary whose RSA-1024 signature fails against Nintendo's public key, a separate revised plan must choose an authorized delivery mechanism:

1. **Option A — HaxxStation / *DS Download Station Volume 1* Stage-1 Exploit (Unmodified Guest Consoles)**:
   - **Mechanism**: Host first advertises a Nintendo-signed *DS Download Station Volume 1* (`NTR-NTRJ` / `dspatch`) multiboot container whose signed stage-1 loader exploits Download Station archive parsing to jump into an unsigned stage-2 MKDS guest binary (`ARM9` patched with the 6-instruction Simple-32 unlock), then switches to MKDS Simple-mode Stage-2 wireless hosting (`GGID 0x00400110`) to serve `RomArchiveBlock` and race traffic.
   - **Open Feasibility Questions for Revised Plan**:
     1. *Single-cartridge two-stage transition*: Can a single DS cartridge ROM cleanly run the HaxxStation multiboot server first and then transition into MKDS's `WM` Stage-2 parent state (`hMp = 8 -> 3 -> 4 -> 15`), or does the guest's `0x027FFC40` / `0x027FFE00` boot-header state need specific `GGID` / `TGID` / MAC fields populated by the stage-1 stub so MKDS `func_0204df90` auto-reconnects to the same host without rebooting?
     2. *Clean-room & user-input requirements*: Because *DS Download Station Volume 1* is a separate copyrighted Nintendo ROM, the build system must accept it as a second local user-provided input ROM (never committed or distributed).
2. **Option B — Custom Firmware / FlashMe / Unlaunch Guest Consoles**:
   - **Mechanism**: Omit HaxxStation and serve the patched MKDS Cloneboot directly from the MKDS Host ROM (`SKIP_STOCK_CLONEBOOT`), which works on guests running RSA-bypassing firmware (`FlashMe` on DS/DS Lite, `Unlaunch`/CFW on DSi/3DS) while stock unmodded DS/DS Lite firmware rejects the download.
