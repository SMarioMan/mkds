# MKDS Simple-Mode 32-Track HaxxStation — Physical Hardware Results

**Target ROM:** `mkds-simple32-haxxstation-host.nds` (`SHA-256: 60e0a6d04a1852b526440b8f224be49dc5d1ca20474a5bd9144796f668a65cf9`; *Mario Kart DS* USA `AMCE-00` + embedded DS Download Station Vol. 1 USA Stage-1 + unsigned modified MKDS Stage-2)  
**Hardware Verification Status:** **`NOT TESTED`** *(no physical DS / DS Lite / flashcart attached to this automated Linux container environment)*

> **Honest Evidence Tier Notice:** All automated cold-boot RSA-1024 validation, Stage-2 HaxxStation exploit execution, native MKDS wireless handoff, wireless `RomArchiveBlock` / `D`-archive streaming, 32-course 3-lap races, Next-Race transitions, and `1..7`-guest (up to 8-player) cohort multicast / barrier tests in this repository carry the **`emulator tested`** tier (`melonDS 906e9ebb` with unmodified DS ARM9/ARM7 BIOS and `FW600DF0.BIN` Download Play firmware). Emulator results do **not** imply physical flashcart kernel compatibility or real-world 802.11 RF timing acceptance.

---

## 1. Hardware Acceptance Matrix (Pending Physical Run)

| Hardware Checkpoint | Host Console | Guest Console(s) | Status | Notes / Expected Observation |
| --- | --- | --- | --- | --- |
| Flashcart Slot-1 Cold Boot | DS / DS Lite + Slot-1 flashcart | — | `NOT TESTED` | Host ARM9 (`0x02000000..0x02004000`), Secure Area, ARM7 (`0x02380000`), and header CRC16 preserved via `dsd rom build` (no Secure-Area code cave). |
| Stage-1 DS Download Play RSA | Flashcart Host in `Simple Mode -> Create Group` | Stock, unmodified DS / DS Lite (no FlashMe / Unlaunch) | `NOT TESTED` | Guest sees *Mario Kart DS* listing, downloads untouched `306,824`-byte Nintendo-signed `ds_demo_client.srl` (`SHA-256: deaec313...`), passes stock firmware RSA-1024 check, and boots Download Station menu. |
| Stage-2 Unsigned MKDS Delivery | Flashcart Host (`HS_PHASE_STATION_APP_SERVE`, `16 x 8 KiB` LRU cache + Port `15` cohort multicast) | 1–7 stock DS / DS Lite guests | `NOT TESTED` | Each guest taps *Mario Kart DS* entry on Station menu, downloads `rom0000d` (`1,139,264` B, `SHA-256: f0802cef...`), waits at party-wide `0xFFFF` execution barrier until all joined guests reach `100%`, then launches modified MKDS client. |
| Native MKDS Rejoin & 8-Cup UI | Flashcart Host (`GGID = 0x25`) | 1–7 stock DS / DS Lite running Stage-2 MKDS | `NOT TESTED` | Host & guest(s) rejoin on `(savedTgid + 7) & 0x7FFF`, transfer wireless `RomArchiveBlock` + `ov000..ov003` (`HMAC-SHA1` verified), and display all 8 cups enabled in Simple VS. |
| Locked-Cup Race (Desert Hills, `courseId = 27`) | Flashcart Host | Stock DS / DS Lite guest(s) | `NOT TESTED` | All consoles load `courseId = 27` (`desert_courseD.carc` + `desert_courseDTex.carc`), play Desert Hills music/physics/UI, complete 3 laps, show matching results, and enter Race #2 without rebooting. |
| Sequential (`18 -> 27`) & Random 32-Course Pools | Flashcart Host | Stock DS / DS Lite guest(s) | `NOT TESTED` | Sequential advances from Luigi's Mansion (`18`) to Desert Hills (`27`); Random shuffles identical 32-course `mCourseList` on host and guest(s). |
| Full 8-Player Party (`1` Host + `7` Guests) | Flashcart Host | 7 stock DS / DS Lite guests (`AID 1..7`) | `NOT TESTED` | Verified in `melonDS` across simultaneous & staggered Station taps (`Desert Hills` & `Rainbow Road`, `participantMask = 0xFF`, `activeKarts = 8`); pending physical 8-console RF verification. |

---

## 2. Physical Hardware Test Procedure (For Console Owner)

1. Build `build/haxxstation/mkds-simple32-haxxstation-host.nds` (`SHA-256: 60e0a6d04a1852b526440b8f224be49dc5d1ca20474a5bd9144796f668a65cf9`) locally inside the reproducible Podman container using your own untrimmed retail *Mario Kart DS* USA (`SHA-1: 691e00d9a5dd80b04f80cc7559503e8b06848785`) and *DS Download Station — Volume 1* USA (`SHA-1: f18b55f3e1259c03e10d0ecb549693b42905ceb5`) dumps (see `README-haxxstation.md`).
2. Copy `mkds-simple32-haxxstation-host.nds` onto a Slot-1 DS flashcart microSD card and insert into the Host DS / DS Lite. Ensure standard DS-mode cartridge boot (disable any flashcart "Download Play patch" cheat/hook if the kernel offers one, so the ROM's own HaxxStation server runs unmodified).
3. Power on `1..7` completely stock, unmodified DS / DS Lite guest consoles with no Slot-1/Slot-2 modifications and select **DS Download Play** from the system menu.
4. On the Host DS, select **Multiplayer -> Simple -> Create Group**.
5. On each Guest DS, select **Mario Kart DS** and confirm **Yes** to join the lobby.
6. Once all desired guests (`1..7`) appear in the Host's member list, press **A / START** on the Host to cut off entry and transmit Stage 1.
7. Each Guest DS authenticates and boots into the **DS Download Station** menu showing **Mario Kart DS — Simple Mode (32 Tracks)**. Tap the entry and confirm download on each Guest DS (no synchronized button timing is required; late-tapping guests catch up into the Port `15` multicast cohort and early guests wait at `100%` until the last guest finishes downloading).
8. All consoles automatically transition into the native *Mario Kart DS* Simple-mode setup screen with all **8 cups / 32 courses** unlocked.
