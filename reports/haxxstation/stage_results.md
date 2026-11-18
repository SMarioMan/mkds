# MKDS Simple-Mode 32-Track HaxxStation — Gate & Stage Results (`M0..M7`)

**Date:** 17 November 2026  
**Target Cartridge:** *Mario Kart DS* USA (`AMCE-00`)  
**Stage-1 Donor:** *DS Download Station — Volume 1* USA (`NTRJ`)  
**Working Branch:** `feature/simple-mode-32-tracks`  
**Build & Test Container:** `localhost/mkds-simple32:latest` (rootless Podman; `melonDS 906e9ebb` with unmodified DS BIOS + `FW600DF0.BIN` Download Play firmware RSA-1024)

---

## 1. Milestone Gate Summary & Evidence Tiers

| Gate | Scope | Status | Evidence Tier | Primary Evidence |
| --- | --- | --- | --- | --- |
| **M0** | Reproducible Podman environment, Simple32 & 8-player HaxxStation logic unit tests (`test_simple32_logic`), `stock_cloneboot` baseline | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `Containerfile`, `tests/test_simple32_logic.c`, `build/haxxstation/{m0_figure8,m0_desert}/summary.json` |
| **M1** | Untouched Station Stage-1 RSA audit + unsigned executable-only `marker-probe.nds` (`0x50584148` `"HAXP"`) cold-booted via stock DS firmware | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `scripts/haxxstation/{audit_station.py,build_probe.py}`, `reports/haxxstation/build_manifest.json` |
| **M2** | Unsigned modified MKDS Stage-2 executable-only guest (`mkds-simple32-guest.nds`) wrapped via `dspatch`, launched from stock firmware (`bootType == 2`, `mUnk_1c_2 == 1`, `simple0x40 == 1`, `GGID = 0x25`) | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `scripts/haxxstation/build_mkds_client.py`, `reports/haxxstation/memory_map.md`, `build/haxxstation/m2_mkds_guest/summary.json` |
| **M3** | Embedded Station Stage-1 + Stage-2 server inside single MKDS Host ROM (`WLMenuScene` `16 x 8 KiB` non-evicting LRU NitroFS streamer, RF channel `{1, 7, 13}`, Port `15` cohort multicast + `0xFFFF` execution barrier for `1..7` guests) | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `include/Main/HaxxStation.h`, `src/Main/HaxxStation.inc.c`, `scripts/haxxstation/build_host.py` |
| **M4** | Native MKDS wireless handoff (`(savedTgid + 7) & 0x7FFF`, `GGID = 0x25`), `ARM9_OVT` + `ov000..ov003` `HMAC-SHA1`, wireless `RomArchiveBlock` (`WM` port `4`) | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `reports/haxxstation/mkds_boot_inventory.md`, `reports/haxxstation/phase_trace.jsonl` |
| **M5** | End-to-end locked-cup Simple-mode race on Desert Hills (`courseId = 27`): 8-cup UI, `desert_courseD.carc` + `DTex.carc` transfer, 3 laps, results, Next Race | **`PASS`** | `code reviewed`, `build passed`, `emulator tested` | `build/haxxstation/matrix/track_05_cup1_slot0/summary.json` |
| **M6** | Single deterministic two-pass Host ROM build (`build_all.py`) with strict input `SHA-1` checks, byte-identical run-to-run output, and zero tracked commercial assets | **`PASS`** | `code reviewed`, `build passed` | `scripts/haxxstation/build_all.py`, `reports/haxxstation/build_manifest.json` |
| **M7 (Emulator)** | **32/32 retail race tracks** cold-booted through all 8 stages + **13/13 multi-guest (`1..7` guests / up to 8 players) & regression checks** (Sequential `18 -> 27`, Random 1-race & 2-race `34 -> 28`, 2-Guest simultaneous & 4.0s staggered tap barrier, 4-Guest DK Pass, **7-Guest / 8-Player simultaneous Desert Hills, 7-Guest staggered tap barrier, 7-Guest Rainbow Road**, Simple Battle, Normal Multi-Card VS, Single-Player GP, Single-Player TT + `.sav`) | **`PASS` (`32/32` + `13/13`)** | `emulator tested` | `reports/haxxstation/course_matrix.csv`, `reports/haxxstation/phase_trace.jsonl`, `build/haxxstation/validation_bundle.json` |
| **M7 (Hardware)** | Flashcart Host DS + `1..7` unmodified stock DS / DS Lite Download Play guest(s) on physical RF | **`NOT TESTED`** | **`NOT TESTED`** | `reports/haxxstation/hardware_results.md` |

---

## 2. Cryptographic & Binary Integrity Summary

| Artifact / Input | Size (Bytes) | Digest | Verification Note |
| --- | ---: | --- | --- |
| Retail *Mario Kart DS* USA (`AMCE-00`, untrimmed input) | `33,554,432` | `SHA-1: 691e00d9a5dd80b04f80cc7559503e8b06848785` | Verified before extraction in `build_all.py` |
| Retail *DS Download Station — Vol. 1* USA (`NTRJ` input) | `16,777,216` | `SHA-1: f18b55f3e1259c03e10d0ecb549693b42905ceb5` | Verified in `audit_station.py` & `build_all.py` |
| Untouched Stage-1 `mb/ds_demo_client.srl` (`/haxx/station_client.srl`, FID `616`) | `306,824` | `SHA-256: deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b` | `signed_stage1_byte_identical == true` (`136`-byte `ac` RSA-1024 trailer intact) |
| Untouched Stage-1 `mb/ds_demo_client.sgn` | `155` | `SHA-256: 118e179b7bdc26110444988d879f7baff9042c2111fc9cf6dd2485639794d1ff` | Preserved byte-for-byte |
| Stage-1 Probe `marker-probe.nds` (`rom0000d` `22,080` B) | `22,528` | `SHA-256: 996a57eaa11b6afc0cd4b2f66f6e329971f0781f142ac8ade1b057e367039958` | Unsigned ARM9+ARM7 marker probe (`"HAXP"` at `0x027FF000`; wrapped `rom0000d` `SHA-256: 483d5326...`) |
| Stage-2 Unsigned MKDS Guest `mkds-simple32-guest.nds` | `1,139,712` | `SHA-256: ca40dd9cfe66062e28a71431f99be81ceed01ce980dea52256d0a0905313c526` | Executable-only modified MKDS guest before `dspatch` wrapping |
| Decoded Stage-2 Guest ARM9 (`0x02000000..0x02179F78`) | `1,548,152` | `SHA-256: 5caf4203c5b43cf2cf13efa553fa732067848fa2d9192c464958cdbb9f3cfe4d` | Contains 8-cup `GetSelectCupGridCupEnabled`, 32-track `BuildCourseList`, `SndHeap = 0xb8000`, `SeqArc` guards, `AutoloadCallback` stub (`0x02001000`), and `ARM9_OVT` HMAC (`0x02170bbc`) |
| Embedded `/haxx/station_demomenu.bin` (FID `617`) | `3,980` | `SHA-256: 9decaa127485de1d7b9cfe2668b6e1847396f0d97323a70d68e142eb48b454fb` | Generated by pinned `Gericom/dspatch` (`32856c4b...`) |
| Embedded `/haxx/station_rom0000d.bin` (FID `618`) | `1,139,264` | `SHA-256: f0802cef5d7aeb01f8879771aa60e357976abc1a9a86321aafd8e84c0840ace7` | HaxxStation Stage-2 exploit payload (`header CRC16 = 0x6217`) |
| Final Deterministic Host ROM `mkds-simple32-haxxstation-host.nds` | `33,554,432` | **`SHA-256: 60e0a6d04a1852b526440b8f224be49dc5d1ca20474a5bd9144796f668a65cf9`** | Verified byte-for-byte identical across consecutive clean rebuilds (`649` FAT entries; all `606` retail file IDs & `30` `D`-archive redirects preserved) |

---

## 3. Technical Root Causes Identified & Resolved

1. **HaxxStation In-Place `MIi_UncompressBackward` Cursor Safety (`scripts/haxxstation/build_mkds_client.py`):**
   - `dspatch`'s Stage-2 loader copies the compressed ARM9 payload directly to `arm9_ram_address` (`0x02000000`) and calls `MIi_UncompressBackward` in place. Compressing the entire `1,548,152`-byte MKDS ARM9 caused the backward write cursor to overtake the read cursor.
   - Keeping the leading ARM9 prefix uncompressed and BLZ-compressing only the safe trailing tail produces a `953,624`-byte module (`total rom0000d = 1,139,264` B, safely inside Station's heap budget) with strictly positive cursor margin at every step.
2. **NitroSDK Overlay Table (`ARM9_OVT`) & Overlay `0..3` `HMAC-SHA1` Authentication (`build_mkds_client.py`, `src/Main/Unknown/code_0204bca0.c`):**
   - Download Play guests verify the wireless `128`-byte `ARM9_OVT` via `FS_LoadOverlayInfo` against a 20-byte `HMAC-SHA1` digest pointed to via double indirection (`0x02170bbc`), and verify `ov000..ov003` against `sOverlaySignatures` via `PMi_TryLoadOverlay`.
   - `build_mkds_client.py` patches the guest's `20`-byte `ARM9_OVT` digest (`e8764c66b21355b554fc313c135d7a7b8de93aa0`), and `CreateRomArchiveBlock` serves the rebuilt `ov000..ov003` binaries (`/haxx/mb_child_ov0..3.bin`) with their exact NitroSDK `HMAC-SHA1` digests (`0f1d23e7...`, `d52c2969...`, `c0b0fd3e...`, `96acd164...`) without disabling any NitroSDK security check.
3. **Post-Exploit Boot Identity & `(savedTgid + 7) & 0x7FFF` Native Wireless Rejoin (`build_mkds_client.py`, `src/Main/HaxxStation.inc.c`):**
   - Because `ds_demo_client.srl` writes `GGID = 0x00400120` into `WMDownloadParam` (`0x027FFC4E`) during Stage 1, a 36-byte ARM9 `AutoloadCallback` stub (`0x02000A58 -> 0x02001000`) restores `GGID = 0x00000025` while preserving `*(u16*)0x027FFC40 == 2` (`bootType = MULTIBOOT`), parent MAC, RF channel, and `TGID`.
   - On the host, `HaxxStation_StepBootstrap` cleanly shuts down the Station parent after all joined guests receive `0xFFFF` and transitions into MKDS's native parent session on `(savedTgid + 7) & 0x7FFF` (`GGID = 0x25`), matching the guest's `StartScan` filter.
4. **8-Player (`1` Host + `7` DLP Clients) Stage-2 Delivery, `WMPortSendCallback.sentBitmap` Fix & Cohort Multicast (`src/Main/HaxxStation.inc.c`):**
   - **Critical `AID 4..7` Callback Fix:** In `WMPortSendCallback` (`Unk02143d64`), `+0x0a` (`mUnk_08_2`) holds the `WM` `port` (`14` / `15` = `0x000E` / `0x000F`, bits `1..3` only), whereas **`+0x10` (`mUnk_10`) holds `sentBitmap`** (`AID 1..7` = bits `1..7`). Reading `cb->mUnk_10` unblocks Stage-2 ACK progression for `AID 4..7` (5–8 players).
   - **Non-Evicting `16 x 8 KiB` (`128 KiB`) NitroFS LRU Cache:** `16` pages of `8 KiB` guarantee at least `2` pages (`current` + conditional straddle prefetch) per peer across all `7` concurrent DLP clients with zero cache thrashing even under arbitrary staggered download offsets.
   - **Port `15` Cohort Multicast & `0xFFFF` Execution Barrier:** `HaxxStation_PumpTx` groups all `WMi_GetStatusAddress()->mMpReadyBitmap` peers sharing the minimum `rom0000d` byte offset into a single `WM_SetMPDataToPortEx` broadcast on Port `15` (`1.14 MiB` sent once instead of `7x`), catches up staggered stragglers automatically until they merge into the cohort, and holds the `0xFFFF` execution marker behind a party-wide barrier until every joined guest reaches `100%`.
5. **Download Play `SndHeap` Sizing (`GetSndInitBufferSize(2) = 0xb8000`) & Course `SeqArc(1)` Guards (`src/Main/Unknown/code_0210*.c`):**
   - Retail `GetSndInitBufferSize(2)` allocated `0xaf000` (`716,800` B), leaving `~57.6 KiB` free in `data_0217d130` — `2..25 KiB` short for Mario Circuit (`59,872` B), Bowser Castle (`71,232` B), Shroom Ridge (`73,344` B), GCN Mushroom Bridge (`76,192` B), and Rainbow Road (`82,368` B).
   - Raising `GetSndInitBufferSize(2)` by `+36 KiB` to `0xb8000` (`753,664` B) gives `~94.5 KiB` sound-heap headroom while keeping positive `ExpHeap` margin (`31,852..147,120` B free during active 8-player races). `NULL` guards around `NNS_SndArcGetSeqArcParam` and `NNS_SndArcGetSeqInfo` prevent null dereferences when Download Play omits `SeqArc(1)`.
6. **Multi-Console `melonDS` `LocalMP` Synchronization (`scripts/dlp_harness/{PlatformImpl.cpp,dlp_harness.cpp}`):**
   - Aligned `0x0010` Association Response `USTimestamp` to each guest's `ARM9Timestamp` curve (`& ~7ULL`) and gated `gPumpGuestsCb()` `PreBeacon`/CMD wake headroom (`360,448` SysCycles in `1,024`-cycle / `30.5 us` steps) on `W_AIDFull` (`0x02A`, preserved across NitroSDK inter-beacon power-save) and `!Harness_HasGuestReplied(c)` (`len > 0 && aid > 0`), eliminating both 8-player `SetMPFrequency(0)` clock skew and `WM_StepDataSharing` double-mode token loss.

---

## 4. Complete 32-Course Cold-Boot Validation Matrix (`32/32 PASS`)

Every course below was cold-booted from DS Firmware Download Play -> untouched signed Station Stage-1 RSA -> Station menu -> unsigned modified MKDS Stage-2 -> native MKDS wireless rejoin -> 8-cup UI -> `MPState16` wireless `RomArchiveBlock` transfer -> countdown -> 3-lap race completion -> results -> `Next Course` transition (`hCid == gCid` on all 32 tracks; full machine-readable rows in `reports/haxxstation/course_matrix.csv`):

| Seq | Cup | Slot | Course ID | Track Name | Effective Guest `.carc` | Min Guest `mCourseBufferHeap` Free | Status |
| ---: | --- | :---: | :---: | --- | --- | ---: | :---: |
| 1 | Mushroom Cup | 0 | `20` | Figure-8 Circuit | `cross_courseD.carc` | `42,152` B | `PASS` |
| 2 | Mushroom Cup | 1 | `22` | Yoshi Falls | `bank_course.carc` | `32,972` B | `PASS` |
| 3 | Mushroom Cup | 2 | `31` | Cheep Cheep Beach | `beach_courseD.carc` | `14,748` B | `PASS` |
| 4 | Mushroom Cup | 3 | `18` | Luigi's Mansion | `mansion_courseD.carc` | `33,512` B | `PASS` |
| 5 | Flower Cup | 0 | `27` | Desert Hills | `desert_courseD.carc` | `23,760` B | `PASS` |
| 6 | Flower Cup | 1 | `28` | Delfino Square | `town_courseD.carc` | `18,440` B | `PASS` |
| 7 | Flower Cup | 2 | `33` | Waluigi Pinball | `pinball_courseD.carc` | `46,020` B | `PASS` |
| 8 | Flower Cup | 3 | `24` | Shroom Ridge | `ridge_courseD.carc` | `69,968` B | `PASS` |
| 9 | Star Cup | 0 | `30` | DK Pass | `snow_courseD.carc` | `63,380` B | `PASS` |
| 10 | Star Cup | 1 | `17` | Tick-Tock Clock | `clock_course.carc` | `49,652` B | `PASS` |
| 11 | Star Cup | 2 | `25` | Mario Circuit | `mario_courseD.carc` | `18,052` B | `PASS` |
| 12 | Star Cup | 3 | `19` | Airship Fortress | `airship_courseD.carc` | `52,136` B | `PASS` |
| 13 | Special Cup | 0 | `34` | Wario Stadium | `stadium_courseD.carc` | `34,160` B | `PASS` |
| 14 | Special Cup | 1 | `26` | Peach Gardens | `garden_courseD.carc` | `55,040` B | `PASS` |
| 15 | Special Cup | 2 | `32` | Bowser Castle | `koopa_courseD.carc` | `53,840` B | `PASS` |
| 16 | Special Cup | 3 | `29` | Rainbow Road | `rainbow_courseD.carc` | `9,464` B | `PASS` |
| 17 | Shell Cup | 0 | `10` | SNES Mario Circuit 1 | `old_mario_sfc.carc` | `176,136` B | `PASS` |
| 18 | Shell Cup | 1 | `11` | N64 Moo Moo Farm | `old_momo_64D.carc` | `62,580` B | `PASS` |
| 19 | Shell Cup | 2 | `13` | GBA Peach Circuit | `old_peach_agb.carc` | `84,744` B | `PASS` |
| 20 | Shell Cup | 3 | `14` | GCN Luigi Circuit | `old_luigi_gcD.carc` | `47,392` B | `PASS` |
| 21 | Banana Cup | 0 | `35` | SNES Donut Plains 1 | `old_donut_sfcD.carc` | `35,320` B | `PASS` |
| 22 | Banana Cup | 1 | `16` | N64 Frappe Snowland | `old_frappe_64.carc` | `36,220` B | `PASS` |
| 23 | Banana Cup | 2 | `12` | GBA Bowser Castle 2 | `old_koopa_agb.carc` | `108,264` B | `PASS` |
| 24 | Banana Cup | 3 | `9` | GCN Baby Park | `old_baby_gc.carc` | `74,800` B | `PASS` |
| 25 | Leaf Cup | 0 | `15` | SNES Koopa Beach 2 | `old_noko_sfc.carc` | `16,564` B | `PASS` |
| 26 | Leaf Cup | 1 | `36` | N64 Choco Mountain | `old_choco_64.carc` | `7,760` B | `PASS` |
| 27 | Leaf Cup | 2 | `37` | GBA Luigi Circuit | `old_luigi_agbD.carc` | `23,528` B | `PASS` |
| 28 | Leaf Cup | 3 | `38` | GCN Mushroom Bridge | `old_kinoko_gcD.carc` | `14,984` B | `PASS` |
| 29 | Lightning Cup | 0 | `39` | SNES Choco Island 2 | `old_choco_sfc.carc` | `92,124` B | `PASS` |
| 30 | Lightning Cup | 1 | `23` | N64 Banshee Boardwalk | `old_hyudoro_64.carc` | `91,564` B | `PASS` |
| 31 | Lightning Cup | 2 | `40` | GBA Sky Garden | `old_sky_agb.carc` | `97,944` B | `PASS` |
| 32 | Lightning Cup | 3 | `1` | GCN Yoshi Circuit | `old_yoshi_gcD.carc` | `9,084` B | `PASS` |

---

## 5. Multi-Guest (`1..7` DLP Clients / Up to 8 Players) & Regression Suite Summary (`13/13 PASS`)

| Test ID | Configuration | Observed Result | Evidence Tier | Status |
| --- | --- | --- | --- | :---: |
| `sequential_vs_boundary_18_to_27` | Simple VS `In Order`, starting at slot 4 (`Luigi's Mansion`, `18`) for 2 consecutive races + Next Race | Completed `observed_race_course_ids == [18, 27]` (`Luigi's Mansion -> Desert Hills`) and queued `28` (`Delfino Square`) without reverting to 8-track pool (`53,067` frames) | `emulator tested` | `PASS` |
| `random_vs_32pool_run1` | Simple VS `Random`, 1 race + Next Race | Host & Guest built identical 32-course permutation `snap[:32] == gsnap[:32]` (`[34, 28, 23, 29, 35, ...]`, `len(set) == 32`), raced `Wario Stadium` (`34`), queued `28` (`34,764` frames) | `emulator tested` | `PASS` |
| `random_vs_32pool_run2` | Simple VS `Random`, **2 consecutive races** + Next Race | Completed `observed_race_course_ids == [34, 28]` (`Wario Stadium -> Delfino Square`, matching `snap[:2]`) and queued `23` (`N64 Banshee Boardwalk`) (`57,819` frames) | `emulator tested` | `PASS` |
| `multi_guest_2_desert_hills` | **1 Host + 2 unmodified Download Play Guests**, simultaneous Stage-1 & Stage-2 boot on `Desert Hills` (`27`) | Both guests authenticated Stage 1, downloaded `rom0000d` via Port `15` cohort multicast, rejoined native MKDS (`participantMask = 0x07`), completed 3 laps on `Desert Hills`, and entered Next Race (`27,408` frames) | `emulator tested` | `PASS` |
| `multi_guest_2_staggered_tap_barrier` | **1 Host + 2 unmodified Download Play Guests**, `--guest-tap-delay 240` (`4.0s` staggered human tap offset) on `Desert Hills` (`27`) | Guest 1 waited cleanly while Guest 2 caught up via minimum-offset multicast; `0`-frame launch spread at `0xFFFF` barrier (`min_guest_course_heap_free = 23,760` B, `min_guest_exp_heap_free = 108,968` B, `27,699` frames) | `emulator tested` | `PASS` |
| `multi_guest_4_dk_pass` | **1 Host + 4 unmodified Download Play Guests** (5 players, crossing `AID 4` boundary) on `DK Pass` (`30`) | All 4 guests authenticated Stage 1, passed `0xFFFF` barrier (`launchSpread = 0`), rejoined native MKDS (`participantMask = 0x1F`, `activeKarts = 5`), raced 3 laps on `DK Pass` (`min_guest_course_heap_free = 63,380` B), and entered Next Race (`22,097` frames) | `emulator tested` | `PASS` |
| `multi_guest_7_desert_hills` | **8 Players (`1` Host + `7` unmodified Download Play Guests)**, simultaneous Stage-1 & Stage-2 boot on `Desert Hills` (`27`) | All 7 guests authenticated Stage 1, downloaded `rom0000d` with zero NitroFS cache eviction, launched simultaneously (`spread = 0`), rejoined native MKDS (`participantMask = 0xFF`, `activeKarts = 8`), completed 3 laps (`min_guest_course_heap_free = 23,760` B, `min_guest_exp_heap_free = 31,852` B), and returned to `SELECT CUP` (`18,749` frames) | `emulator tested` | `PASS` |
| `multi_guest_7_staggered_tap_barrier` | **8 Players (`1` Host + `7` unmodified Download Play Guests)**, `--guest-tap-delay 60` (`1.0s` per-console staggered Station menu taps, `6.0s` first-to-last spread) on `Desert Hills` (`27`) | Staggered stragglers caught up and merged into Port `15` cohort multicast; all 7 guests held at `0xFFFF` barrier (`spread = 0`), rejoined native MKDS (`participantMask = 0xFF`, `activeKarts = 8`), completed 3 laps (`min_guest_course_heap_free = 23,760` B, `min_guest_exp_heap_free = 31,852` B), and entered Next Race (`19,167` frames) | `emulator tested` | `PASS` |
| `multi_guest_7_rainbow_road` | **8 Players (`1` Host + `7` unmodified Download Play Guests)** on `Rainbow Road` (`29`, tightest unlocked `D`-archive + largest course `SndHeap` load) | All 8 consoles (`participantMask = 0xFF`, `activeKarts = 8`) loaded `rainbow_courseD.carc` + `DTex.carc` (`min_guest_course_heap_free = 9,464` B, `min_guest_exp_heap_free = 53,784` B), completed 3 laps, displayed 8-player results, and entered Next Race (`22,377` frames) | `emulator tested` | `PASS` |
| `simple_battle_pool` | Simple-mode Battle (`In Order`) | Preserved retail 3-stage Simple Battle pool `[43, 47, 44]`; entered active battle gameplay on `Nintendo DS` (`43`, `min_guest_course_heap_free = 132,052` B, `8,894` frames) | `emulator tested` | `PASS` |
| `normal_multicard_vs` | Multi-Card VS (2 cartridges, `Figure-8 Circuit`, `20`) | Joined native Multi-Card session, completed 3-lap race, results, and Next-Race transition (`12,837` frames) | `emulator tested` | `PASS` |
| `single_player_gp` | Single-Player Grand Prix (`Mushroom Cup`, `Figure-8 Circuit`, `20`) | Completed intro camera, 3-lap GP race, and GP results (`12,898` frames) | `emulator tested` | `PASS` |
| `single_player_tt_save` | Single-Player Time Trial (`SNES Mario Circuit 1`, `10`) + EEPROM save | Completed 3-lap Time Trial and wrote valid `262,144`-byte (`256 KiB`) `.sav` (`21,942` non-`0xFF` bytes, `6,662` frames) | `emulator tested` | `PASS` |
