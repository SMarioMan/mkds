# Mario Kart DS — Simple Mode 32-Track HaxxStation Build & Operations Guide

Unlock all **8 cups / 32 standard retail race tracks** in *Mario Kart DS* USA (`AMCE-00`) **Simple mode** (single-cartridge Download Play) for **2 to 8 players (`1` Host + `1..7` completely unmodified Nintendo DS / DS Lite guest consoles)**.

No Nintendo ROMs, signed binaries, BIOS/firmware dumps, or commercial assets are stored in this repository. All ROMs and intermediates are generated locally inside a reproducible Podman container from two user-supplied retail dumps.

---

## 1. Architecture Summary

Retail *Mario Kart DS* Simple mode transmits a static RSA-signed guest executable whose ARM9 code hardcodes a 2-cup / 8-track Simple-mode pool (`Mushroom` + `Shell` Cups) in `GetSelectCupGridCupEnabled` and `BuildCourseList`. Because MKDS replicates host pad inputs rather than sending a selected `courseId` packet, an unmodified retail guest diverges on the other 24 tracks.

This branch implements an **embedded HaxxStation two-stage bootstrap** inside a single playable *Mario Kart DS* Host ROM:

1. **Stage 1 (Firmware RSA-1024 Authenticated):** When the host opens a Simple-mode group (`Simple -> Create Group`), it advertises Station `GGID = 0x00400120` on MKDS's chosen `TGID` and RF channel (`{1, 7, 13}`), and serves the **byte-for-byte unmodified** Nintendo-signed *DS Download Station — Volume 1* client (`mb/ds_demo_client.srl`, `306,824` B, `SHA-256: deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b`). Stock DS/DS Lite firmware validates its original RSA-1024 signature and boots the Station client on all `1..7` guests.
2. **Stage 2 (Unsigned Modified MKDS Guest via `dspatch` + 7-Client Cohort Multicast):** Inside `WLMenuScene`, `src/Main/HaxxStation.inc.c` runs the Station application server (`WM` ports `13`, `14`, `15` + port `1` Station keepalive) using a bounded, non-evicting `16 x 8 KiB` (`128 KiB`) LRU NitroFS page cache, serving `demomenu` (`3,980` B) and `rom0000d` (`1,139,264` B — an executable-only modified MKDS guest wrapped by `Gericom/dspatch` commit `32856c4b68c8e45d90fe135d5555a4596fbd2955`). Port `15` `rom0000d` chunks are broadcast via **minimum-offset cohort multicast** (`WMi_GetStatusAddress()->mMpReadyBitmap`), and a **party-wide execution barrier** holds the `0xFFFF` launch marker until every joined guest (`1..7`, `AID 1..7`) finishes downloading `rom0000d`.
3. **Native MKDS Wireless Rejoin & Asset Streaming:** On launch, a 36-byte guest `AutoloadCallback` stub (`0x02001000`) restores `GGID = 0x00000025` at `0x027FFC4E` while preserving `*(u16*)0x027FFC40 == 2` (`bootType = MULTIBOOT`), parent MAC, RF channel, and `TGID`. Host and all `1..7` guests transition into MKDS's native `(TGID + 7) & 0x7FFF` wireless session, seed identical PRNG state (`mUnk_1ee8`), verify `ov000..ov003` and `ARM9_OVT` via NitroSDK `HMAC-SHA1`, mount the wireless `RomArchiveBlock` (`WM` port `4`) with 15 generated reduced `D` course archives (`30` `.carc` files under `/haxx/Course/`), and run all 32 tracks across Choose, In-Order (Sequential), and Random VS modes.

---

## 2. Required User-Provided Local Inputs

Place your own clean retail dumps outside git tracking (for example in the parent workspace directory `/path/to/workspace/`):

| Input File | Required Size | Required `SHA-1` | Notes |
| --- | --- | --- | --- |
| *Mario Kart DS* (USA, `AMCE-00`, untrimmed) | `33,554,432` B | `691e00d9a5dd80b04f80cc7559503e8b06848785` | Strict check enforced by `build_all.py` |
| *DS Download Station — Volume 1* (USA, `NTRJ`) | `16,777,216` B | `f18b55f3e1259c03e10d0ecb549693b42905ceb5` | Source of untouched `mb/ds_demo_client.srl` & `.sgn` |
| `BIOSNDS9.ROM`, `BIOSNDS7.ROM`, `FW600DF0.BIN` | `4 KiB` / `16 KiB` / `256 KiB` | *(stock DS BIOS + firmware)* | Required for `arm7_bios.bin` build step & `melonDS` cold-boot RSA validation |

---

## 3. Reproducible Container Build (Podman)

All builds and emulator validations run inside `Containerfile` using rootless **Podman**.

### 3.1 Build the Toolchain & Emulator Container Image

From `/path/to/workspace/mkds`:

```bash
podman build -t localhost/mkds-simple32:latest -f Containerfile .
```

### 3.2 Build the Single-Cartridge HaxxStation Host ROM

Assuming `/path/to/workspace` contains your MKDS USA ROM, Download Station Vol. 1 USA ROM, and `BIOSNDS7.ROM`:

```bash
podman run --rm \
  -v /path/to/workspace:/work:Z \
  -w /work/mkds \
  localhost/mkds-simple32:latest \
  python3 scripts/haxxstation/build_all.py \
    --mkds-rom "/work/mkds/extract/baserom_mkds_usa.nds" \
    --station-rom "/work/DS Download Station - Volume 1 (USA) (Demo) (Wi-Fi Kiosk).nds" \
    --region usa \
    --guests 7 \
    --out build/haxxstation/mkds-simple32-haxxstation-host.nds
```

**Deterministic Output (`build/haxxstation/mkds-simple32-haxxstation-host.nds`):**
- Size: `33,554,432` bytes (`32 MiB`)
- `SHA-256`: `60e0a6d04a1852b526440b8f224be49dc5d1ca20474a5bd9144796f668a65cf9`
- Embedded untouched Stage-1 `station_client.srl` `SHA-256`: `deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b`
- Embedded Stage-2 `station_rom0000d.bin` `SHA-256`: `f0802cef5d7aeb01f8879771aa60e357976abc1a9a86321aafd8e84c0840ace7`
- Machine-readable audit manifest: `reports/haxxstation/build_manifest.json`

*(Note: `--guests 1..7` records target party metadata in `build_manifest.json`; the generated Host ROM dynamically detects `1..7` connected Download Play guests at runtime from the Stage-1 boot mask.)*

### 3.3 Compile-Time Fallback Switch (`stock_cloneboot` Mode)

`include/Main/HaxxStation.h` defines `MKDS_BOOTSTRAP_MODE`:
- `MKDS_BOOTSTRAP_HAXXSTATION` (`1`, default): Two-stage signed Station Stage 1 + unsigned 32-track MKDS Stage 2.
- `MKDS_BOOTSTRAP_STOCK_CLONEBOOT` (`0`): Reverts wireless bootstrap and `CreateRomArchiveBlock` OVT behavior to Phase 1's untouched retail MKDS Cloneboot path for A/B regression comparison.

---

## 4. Running the Automated Validation Suite

Inside the Podman container, build `dlp_harness` (`melonDS 906e9ebb` with unmodified DS firmware RSA-1024 boot) and run the full 32-track + `1..7`-guest (up to 8-player) + regression suite (`32/32 courses PASS, 13/13 regressions PASS`):

```bash
podman run --rm \
  -v /path/to/workspace:/work:Z \
  -w /work/mkds \
  localhost/mkds-simple32:latest \
  bash -lc '
    gcc -O2 -Wall -Wextra -Werror tests/test_simple32_logic.c -o build/test_simple32_logic && ./build/test_simple32_logic
    cmake -S scripts/dlp_harness -B build/dlp_harness -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/dlp_harness -j4
    python3 scripts/haxxstation/run_haxxstation_validation.py --workers 4 --resume
  '
```

Generated validation reports:
- `reports/haxxstation/course_matrix.csv` — All 32 retail tracks cold-booted through Stage-1 RSA, Stage-2 launch, native rejoin, 3-lap race, results, and Next-Race transition.
- `reports/haxxstation/phase_trace.jsonl` — Structured wireless state-machine trace across every bootstrap boundary (including 2-guest and 7-guest / 8-player runs).
- `reports/haxxstation/stage_results.md` — Gate-by-gate (`M0..M7`) results, 8-player scaling root causes resolved, and evidence tiers.
- `reports/haxxstation/memory_map.md` — Stage-1 / Stage-2 / MKDS runtime MainRAM, overlay, ITCM, and DTCM map.
- `reports/haxxstation/hardware_results.md` — Physical DS / DS Lite acceptance checklist (`NOT TESTED` in container environment).

---

## 5. Console Operational Directions (Step-by-Step)

1. **Host Setup:** Copy `mkds-simple32-haxxstation-host.nds` onto a Slot-1 flashcart microSD card, insert it into the Host DS / DS Lite, and boot *Mario Kart DS* normally (ensure any flashcart "Download Play patch" option is turned **off** so the ROM's built-in HaxxStation bootstrap runs cleanly).
2. **Open Simple Lobby (Host):** Navigate to **Multiplayer -> Simple -> Create Group**. The Host waits at `CURRENT RACERS... 1/8`.
3. **Join Stage-1 Download Play (Guests `1..7`):**
   - Power on `1..7` unmodified DS / DS Lite consoles (no cartridge required, stock firmware).
   - Tap **DS Download Play** on the DS system menu.
   - Select **Mario Kart DS** (`Simple Mode (32 Tracks)`) and choose **Yes** to download.
4. **Start Stage-1 Transfer (Host):** Once all desired guests (`1..7`) appear in the Host's member list, press **A** (or **START**) on the Host to cut off entry.
5. **Stage-1 Download Station Menu Tap (Guests — 1 Extra Confirmation Step):**
   - Each Guest DS validates Nintendo's RSA signature and boots into the **DS Download Station** screen showing **Mario Kart DS — Simple Mode (32 Tracks)** while the Host displays `PLEASE WAIT...`.
   - On **each Guest DS**, press **A** (or tap the entry) and confirm **Yes** to download the Stage-2 game client (`~1.14 MiB`).
   - **No synchronized button timing is required:** late-tapping guests catch up into the Host's Port `15` multicast cohort automatically, and guests that reach `100%` first wait at the Host's `0xFFFF` execution barrier until the last guest finishes downloading.
6. **Automatic Native MKDS Handoff & Racing:**
   - Once all joined guests reach `100%`, every Guest DS boots directly into *Mario Kart DS* and automatically rejoins the Host (`participantMask` up to `0xFF` for all 8 players).
   - Configure VS rules (`Choose`, `In Order`, or `Random`) on the Host: all **8 cups and 32 standard tracks** are unlocked and selectable on both Host and Guest consoles, and consecutive races proceed directly via **Next Course** without re-downloading.

---

## 6. Known Operational Notes & Troubleshooting

| Symptom / Stage | Cause & Check |
| --- | --- |
| Guest sits on `DS Download Station` screen while Host says `PLEASE WAIT...` | Expected Stage-1 menu step: press **A -> Left (Yes) -> A** on every Guest DS to start the Stage-2 download. In multi-guest (`2..7` guests) play, early guests wait at `100%` until all joined guests finish downloading. |
| Guest rejects Stage-1 download on DS Firmware screen | Verify `signed_stage1_srl_sha256` in `reports/haxxstation/build_manifest.json` equals `deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b`, and disable any flashcart kernel "Download Play cheat/patch" feature that mutates MB headers. |
| Build fails with `SHA-1 mismatch` | Ensure the MKDS USA ROM is the untrimmed `33,554,432`-byte `AMCE-00` dump (`691e00d9...`) and the Station ROM is the unmodified `16,777,216`-byte Volume 1 USA dump (`f18b55f3...`). |
| Physical hardware status | Emulator validation (`melonDS 906e9ebb` with real BIOS/firmware RSA across `32/32` tracks and `1..7` guests / up to 8 players) is complete; physical flashcart + stock DS/DS Lite RF testing remains marked **`NOT TESTED`** in `reports/haxxstation/hardware_results.md` until executed on real consoles. |
