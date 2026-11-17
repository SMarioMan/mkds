# MKDS USA (`AMCE-00`) Bootstrap, Wireless & Guest State Inventory

**Target ROM:** Mario Kart DS USA (`AMCE-00`, untrimmed 32 MiB `SHA-1: 691e00d9a5dd80b04f80cc7559503e8b06848785`)  
**Station Stage-1 ROM:** DS Download Station Volume 1 USA (`NTRJ`, 16 MiB `SHA-1: f18b55f3e1259c03e10d0ecb549693b42905ceb5`)  
**Repository / Branch:** `SMarioMan/mkds`, `feature/simple-mode-32-tracks` (Phase 1 commit `c1680030331c78c671a091e0d008ef6ace2e426a` atop `031ab8bf36c1060a6d4cfca8742f2ef035a5f2df`)  
**Status:** Source-verified and binary-verified against `config/usa/arm9/symbols.txt`, `build/usa/host_nm.txt`, `extract/baserom_mkds_usa.nds`, and `DS Download Station - Volume 1 (USA) (Demo) (Wi-Fi Kiosk).nds`.

---

## 1. USA Symbol Address Table (Retail vs Phase 1 `c1680030` vs Final HaxxStation `c8a38668`)

> **Important:** Linking decompiled/patched static `ARM9` (`Main`) shifts symbols after modified translation units, and shifts `OV000..OV003` base addresses from `0x021804C0` (retail) to `0x02182D60` (`OV003_BSS_END = 0x021DCBA0` in the final HaxxStation build). All runtime hooks, `nm`-driven harness probes, and guest builds resolve symbol addresses from `build/usa/host_nm.txt` rather than hardcoding retail addresses.

| Role / Target | Symbol | Source File | Retail USA | Phase 1 (`c1680030`) | Final HaxxStation (`c8a38668`) |
| --- | --- | --- | --- | --- | --- |
| ARM9 CRT0 entry | `Entry` | `src/Main/Unknown/code_02000800.c:10` | `0x02000800` | `0x02000800` | `0x02000800` |
| CRT0 module params | `BuildInfo` | `src/Main/Unknown/code_02000b4c.s` | `0x02000B4C` | `0x02000B4C` | `0x02000B4C` |
| CRT0 autoload hook | `AutoloadCallback` | `src/Main/Unknown/code_02000a58.c:3` | `0x02000A58` | `0x02000A58` | `0x02000A58` |
| Game `main` | `main` | `src/Main/Unknown/code_02003000.c` | `0x02003000` | `0x02003000` | `0x02003000` |
| Simple-mode cup gate | `GetSelectCupGridCupEnabled` | `src/Main/Unknown/code_021189f4.c:320` | `0x021189F4` | `0x02118BBC` | `0x0211A05C` |
| VS / Battle course pools | `BuildCourseList` | `src/Main/Unknown/code_02045874.c:180` | `0x02046024` | `0x020460CC` | `0x02046134` |
| 8x4 Cup -> `courseId` table | `gCupCourseIdTable` | `src/Main/Unknown/code_02045874.c` | `0x02154128` | `0x02154308` | `0x021557A8` |
| 2-cup Simple index table `[0, 4]` | `data_021540c0` | `src/Main/Unknown/code_02045874.c` | `0x021540C0` | `0x021542A0` | `0x02155740` |
| 8-cup VS index table `[0..7]` | `data_02154108` | `src/Main/Unknown/code_02045874.c` | `0x02154108` | `0x021542E8` | `0x02155788` |
| Boot-mode / owner init | `func_0204ad44` | `src/Main/Unknown/code_0204ad44.c:51` | `0x0204AD44` | `0x0204ADC8` | `0x0204AE30` |
| Child flag write guard | `func_0204aa28` | `src/Main/Unknown/code_0204aa04.c:5` | `0x0204AA28` | `0x0204AAAC` | `0x0204AB14` |
| Global system/menu state | `data_021759d0` | `include/Main/Unknown/unk_02175000.h` | `0x021759B0` | `0x02176590` | `0x02178250` |
| HaxxStation host state | `gHaxxStationHost` | `src/Main/HaxxStation.inc.c:29` | *(n/a)* | *(n/a)* | `0x0217191C` |
| DLP sound init buffer size | `GetSndInitBufferSize` | `src/Main/Unknown/code_02105550.c:261` | `0x02108844` | `0x02108A0C` | `0x02109DE4` |
| Child retained-BssDesc setup | `func_0204df90` | `src/Main/Unknown/code_0204df90.c:434` | `0x0204DF90` | `0x0204E158` | `0x0204F51C` |
| Simple wireless init step | `func_0204e1a8` | `src/Main/Unknown/code_0204df90.c:335` | `0x0204E1A8` | `0x0204E370` | `0x0204F740` |
| Simple parent MB init | `func_0204e568` | `src/Main/Unknown/code_0204df90.c:149` | `0x0204E568` | `0x0204E730` | `0x0204FB00` |
| Simple parent MB callback | `func_0204e6e8` | `src/Main/Unknown/code_0204df90.c:46` | `0x0204E6E8` | `0x0204E8B0` | `0x0204FC8C` |
| Simple parent MB allocs | `func_0204e928` | `src/Main/Unknown/code_0204df90.c:21` | `0x0204E928` | `0x0204EAF0` | `0x0204FEDC` |
| MB start sending (`0x18`) | `func_0204e128` | `src/Main/Unknown/code_0204df90.c:358` | `0x0204E128` | `0x0204E2F0` | `0x0204F6C0` |
| MB boot request (`0x19`) | `func_0204e0bc` | `src/Main/Unknown/code_0204df90.c:381` | `0x0204E0BC` | `0x0204E284` | `0x0204F654` |
| MB end (`0x1b`) | `func_0204e090` | `src/Main/Unknown/code_0204df90.c:400` | `0x0204E090` | `0x0204E258` | `0x0204F61C` |
| MB register child image | `func_0204ea3c` | `src/Main/Unknown/code_0204ea14.c:12` | `0x0204EA3C` | `0x0204EC04` | `0x0204FFF4` |
| Host MB -> Native handoff | `func_0204db6c` | `src/Main/Unknown/code_0204db48.c:167` | `0x0204DB6C` | `0x0204DD34` | `0x0204F0E4` |
| Host child-rejoin barrier | `func_0204dc8c` | `src/Main/Unknown/code_0204db48.c:146` | `0x0204DC8C` | `0x0204DE54` | `0x0204F218` |
| Child native connect driver | `func_0204dd24` | `src/Main/Unknown/code_0204db48.c:17` | `0x0204DD24` | `0x0204DEEC` | `0x0204F2B0` |
| `ConnectionWork *` global | `data_0217aa00` | `include/Main/Unknown/unk_0217a000.h` | `0x0217A9E0` | `0x0217B5C0` | `0x0217D280` |
| Menu setup commit / lockstep | `func_020cc744` | `src/Main/Unknown/code_020cc744.c:21` | `0x020CC744` | `0x020CC90C` | `0x020CDD64` |
| Replicated pad slots `[8]` | `data_02175628` | `include/Main/Unknown/unk_02175000.h` | `0x02175608` | `0x021761E8` | `0x02177EA8` |
| Native parent param builder | `FillParentParameter` | `src/Main/Unknown/code_0204f814.c:158` | `0x0204FB40` | `0x0204FD08` | `0x02051160` |
| Native parent event & PRNG | `StartParentCallback` | `src/Main/Unknown/code_020529b8.c:590` | `0x020529B8` | `0x02052B80` | `0x02053FD8` |
| Native child connect & PRNG | `StartConnectCallback` | `src/Main/Unknown/code_02051658.c:379` | `0x02051AD4` | `0x02051C9C` | `0x020530F4` |
| Race archive state trigger | `RequestMPState16` | `src/Main/Unknown/code_0205928c.c:216` | `0x0205950C` | `0x020596D4` | `0x0205AB2C` |
| MB segment size (`MB_GetSegmentLength`) | `func_02008880` | `src/Main/Unknown/code_02008880.c:22` | `0x02008880` | `0x020088F8` | `0x02008938` |
| MB segment reader (`MB_ReadSegment`) | `func_02008570` | `src/Main/Unknown/code_02008570.c:23` | `0x02008570` | `0x02008570` | `0x020085A0` |
| MB register file (`MB_RegisterFile`) | `func_02008310` | `src/Main/Unknown/code_02007ec4.c:11` | `0x02008310` | `0x02008310` | `0x02008340` |
| Read retained parent BssDesc | `func_02007ec4` | `src/Main/Unknown/code_02007ec4.c:280` | `0x02007EC4` | `0x02007EC4` | `0x02007EF4` |
| MB parent event dispatcher | `func_020079c4` | `src/Main/Unknown/code_02007624.c:77` | `0x020079C4` | `0x020079C4` | `0x020079E8` |
| MB parent MP receive | `func_02007698` | `src/Main/Unknown/code_02007624.c:208` | `0x02007698` | `0x02007698` | `0x020076AC` |
| MB parent MP send pump | `func_020071b4` | `src/Main/Unknown/code_020071b4.c:94` | `0x020071B4` | `0x020071B4` | `0x020071B4` |
| MB control sender | `SendParentMessage` | `src/Main/Unknown/code_020075f8.c:8` | `0x020075F8` | `0x020075F8` | `0x020075F8` |
| MB init (`MB_Init`) | `InitWirelessWork` | `src/Main/Unknown/code_02009928.c:559` | `0x02009FF4` | `0x0200A09C` | `0x0200A104` |
| MB comm size (`MB_SetParentCommParam`) | `SetMPLimits` | `src/Main/Unknown/code_02009928.c:639` | `0x02009F38` | `0x02009FE0` | `0x0200A048` |
| MB start parent (`MB_StartParent`) | `func_02009cc8` | `src/Main/Unknown/code_02009928.c:724` | `0x02009CC8` | `0x02009D70` | `0x02009DD8` |
| MB end (`MB_End`) | `func_02009c04` | `src/Main/Unknown/code_02009928.c:783` | `0x02009C04` | `0x02009CAC` | `0x02009D14` |
| WM port callback registration | `WM_SetPortCallback` | `libs/NitroSDK/wm/src/code_02142eac.c:630` | `0x0214358C` | `0x02143750` | `0x02144BF0` |
| WM port async send | `WM_SetMPDataToPortEx` | `libs/NitroSDK/wm/src/code_02142eac.c:158` | `0x02144048` | `0x0214420C` | `0x021456AC` |
| WM entry enable/disable | `WM_SetEntry` | `libs/NitroSDK/wm/src/code_021453ec.c` | `0x021453EC` | `0x021455B0` | `0x02146A50` |
| WM beacon GameInfo update | `WM_SetGameInfo` | `libs/NitroSDK/wm/src/code_021453ec.c` | `0x021455A0` | `0x02145764` | `0x02146C04` |
| Wireless FAT/FNT/OVT builder | `CreateRomArchiveBlock` | `src/Main/Unknown/code_0204bca0.c:58` | `0x0204D6F4` | `0x0204D778` | `0x0204D7E0` |
| Child wireless FS mount | `ReloadRomArchiveFromBlock` | `src/Main/Unknown/code_0204bca0.c:255` | `0x0204D610` | `0x0204D694` | `0x0204D6FC` |
| Overlay table HMAC verify | `FS_AttachOverlayTable` | `libs/NitroSDK/fs/src/code_02004dc0.c:326` | `0x020050C0` | `0x020050C0` | `0x020050C0` |
| Overlay binary HMAC verify | `FS_StartOverlay` | `libs/NitroSDK/fs/src/code_02004dc0.c:198` | `0x02005314` | `0x02005314` | `0x02005314` |
| Overlay HMAC key pointer | `data_02161768` | `libs/NitroSDK/fs/src/code_02004dc0.c:187` | `0x02161748` | `0x02161928` | `0x02162DE8` |
| 20-byte OVT HMAC digest | `data_0216f2dc` | `libs/NitroSDK/fs/src/code_02004dc0.c:332` | `0x0216F2BC` | `0x0216F49C` | `0x0217093C` |
| 4x20-byte overlay HMAC table | `OverlaySignatures` | `libs/NitroSDK/fs/src/code_02004dc0.c:209` | `0x0216F2D0` | `0x0216F4B0` | `0x02170950` |
| `MBGameRegistry` (`ggid=0x25`) | `data_0215472c` | `src/Main/Unknown/code_0204ea14.c:33` | `0x0215472C` | `0x0215490C` | `0x02155DAC` |
| `WMParentParam` template (`ggid=0x25`) | `data_02154788` | `src/Main/Unknown/code_0204f814.c:168` | `0x02154788` | `0x02154968` | `0x02155E08` |
| Main RAM ArenaLo init | `OS_GetInitArenaLo` | `libs/NitroSDK/os/src/code_0200eddc.c` | `0x0200EF4C` | `0x0200EFF4` | `0x0200F05C` |

*(Note: Retail `0x02118BF8` / `0x02045A4C` cited in earlier high-level notes correspond to interior switch labels / adjacent functions in `code_021189f4.c` and `code_02045874.c`; the actual function entry points in `config/usa/arm9/symbols.txt` are `0x021189F4` (`GetSelectCupGridCupEnabled`) and `0x02046024` (`BuildCourseList`).)*

---

## 2. Detailed Proofs by Target (§3 Table)

### 2.1 Guest Simple-Mode Menu (`GetSelectCupGridCupEnabled`)
- **Location:** `src/Main/Unknown/code_021189f4.c:320` (Thumb, feature addr `0x02118BBC`).
- **Behavior:** Switches on `data_0217df20->mMode` (`0..4`). Only `case 3` governs Simple-mode VS cup selection.
- **Verified Disassembly (`0x02118C94..0x02118C9A` in decoded `ARM9`):**
  ```armasm
  02118c94: 2f08  cmp  r7, #8       ; cupIndex < 8 (was retail cmp r7, #0 / cmp r7, #4)
  02118c96: d22b  bcs  0x02118cf0
  02118c98: 2401  movs r4, #1
  02118c9a: e029  b    0x02118cf0
  ```
- **Requirement for HaxxStation:** Because `GetSelectCupGridCupEnabled` resides in static `ARM9` (`Main`), building the HaxxStation Stage-2 MKDS guest from the modified `Main` image places these exact Thumb bytes (`08 2f 2b d2 01 24`) into the guest's executed static `ARM9`.

### 2.2 Guest VS Course Pools (`BuildCourseList`)
- **Location:** `src/Main/Unknown/code_02045874.c:180` (ARM, feature addr `0x020460CC`).
- **Behavior:** Reads `simpleMode = ((data_0217aa00->mUnk_0454 & 0x40) != 0)` without clearing or mutating `mUnk_0454`.
  - `case 1` (In-Order) & `case 2` (Random) when `isVersus != 0`: uses `m = 8` cups, `t = data_02154108` (`[0..7]`), and `if (simpleMode || func_02046648(t[p]) != FALSE)` to populate up to `32` courses into `gRaceSetup->mCourseList` (`0x21` bytes, keeping index `32` terminator `32`).
  - `isVersus == 0` (Battle): untouched (`simpleMode ? [44, 47, 43] : all 6 battle stages`).
- **Verified Disassembly (`0x020461C4..0x02046298` in decoded `ARM9`):**
  - `0x020461c4: e3540000 cmp r4, #0; 13a0a001 movne sl, #1` (`simpleMode`)
  - `0x02046218: e3500020 cmp r0, #32` (search across all 32 tracks)
  - `0x02046230: e35a0000 cmp sl, #0; 1a000003 bne 0x02046248` (bypass host save-unlock check in `simpleMode`)
  - `0x02046284: e3580008 cmp r8, #8` (wrap across 8 cups) and `0x02046294: e3550020 cmp r5, #32` (emit 32 entries).

### 2.3 Downloaded Boot Flag (`0x027FFC40`) & Early Startup
- **CRT0 (`Entry`, `code_02000800.c:10`):**
  - Sets SVC/IRQ/SYS stacks in DTCM (`0x027E0000..0x027E3FFF`), zeroes DTCM, palette (`0x05000000`), OAM (`0x07000000`), unpacks backward-LZ static `ARM9` (`func_02000934`), copies autoload blocks (`0x01FF8000` ITCM `0x7FA0` B, `0x027E0000` DTCM `0xE0` B), zeroes static `.bss` (`0x0216FF00..0x021810A0`), writes `0` to `0x027FFF9C`, and jumps to `main` (`0x02003000`).
  - **Crucial finding:** `Entry` does **not** overwrite `0x027FFC40..0x027FFC7E` or `0x027FFE00..0x027FFF5F`.
- **Where `0x027FFC40` is read:**
  1. `FSi_InitRom` (`libs/NitroSDK/fs/src/code_02004dc0.c:403`): called during `main` init (`FS_Init(3)`). When `*(u16*)0x027FFC40 == 2`, attaches dummy empty `"rom"` archive instead of reading local Slot-1 cartridge.
  2. `func_0204ad44` (`src/Main/Unknown/code_0204ad44.c:99`): sets `data_021759d0->mUnk_1c_2 = (*(u16*)0x027FFC40 == 2) ? 1 : 0`.
  3. `func_0204aa28` (`src/Main/Unknown/code_0204aa04.c:11`): refuses to overwrite `mUnk_1c_2` when `*(u16*)0x027FFC40 == 2`.
  4. Sound, save/backup (`code_02056d54.c:16`), overlay auth (`FS_StartOverlay`), UI scenes (`SelectCupM.c`, `SelectCupCourse.c`), and `WLMenuScene` all branch on `*(u16*)0x027FFC40 == 2`.
- **HaxxStation behavior:** DS Firmware writes `*(u16*)0x027FFC40 = 2` and the `0x3C`-byte `MBParam` parent `WMBssDesc` at `0x027FFC42..0x027FFC7E` when booting Stage-1 `ds_demo_client.srl`. The Station client preserves `0x027FFC40..0x027FFC7E` when launching `rom0000d` (to be empirically confirmed in Gate M1 via `marker-probe.nds`).

### 2.4 Simple Wireless Flag (`mUnk_0454 & 0x40`), GGID, TGID & Retained Parent Descriptor
- **Proven Wireless Identities:**
  - **Station Stage-1 / Stage-2 App GGID:** `0x00400120` (required by signed `mb/ds_demo_client.srl`).
  - **Native MKDS USA GGID (both Stage-1 MB and Stage-2 Gameplay):** **`0x00000025`** (`0x25`), proven in `code_0204df90.c:159,197`, `code_02051658.c:262,273,581`, `data_0215472c[5]`, and `data_02154788[+0x08]`. *(Note: `0x00400110` from the earlier external note is NOT MKDS's GGID; MKDS uses `0x00000025`.)*
- **How Guest Sets `mUnk_0454 |= 0x40` & Connects Without Scanning:**
  1. `func_0204e204` (`code_0204df90.c:301-311`): when `*(u16*)0x027FFC40 == 2`, sets `data_0217aa00->mUnk_0454 |= 0x40` and calls `Initialize()` (`WM_Initialize`, installing `MPDataPortCallback` on WM port 15).
  2. `func_0204dd24` (`code_0204db48.c:100-127`): calls `func_0204df90()` -> `func_02007ec4(mUnk_0024, 0x1bc, 0x36, 0, 0)`:
     - Copies `0x3C` bytes from `0x027FFC42` into `ScanBlock *mUnk_0024`:
       - `0x027FFC46..0x027FFC4B`: parent 6-byte `BSSID` (MAC)
       - `0x027FFC4E..0x027FFC51`: 32-bit `GGID` (`mUnk_0c | (mUnk_0e << 16)` -> `mUnk_44`)
       - `0x027FFC52..0x027FFC53`: 16-bit Stage-1 `TGID` (`mUnk_10` -> `mUnk_48`)
       - `0x027FFC78`: RF `channel`
     - `func_0204df90` then computes `mUnk_0024->mUnk_48 = (mUnk_0024->mUnk_48 + 7) & 0x7FFF`!
     - Calls `StartRomArchiveTransfer()` (`StartRomArchive(0, 4, 0x154, ...)` on WM port 4), then `StartConnect(NULL, ...)` -> `WM_StartConnectEx` **directly** to `mUnk_0024` (with `ssid[0x14] |= 1` marking a downloaded child so Host sets `mUnk_0457 |= (1 << aid)`).
- **Minimal Guest Pre-CRT0 / Startup Adaptation Required:**
  - Because Stage-1 advertised `GGID = 0x00400120` and `TGID = mUnk_1690` on `channel = mUnk_0088`, `0x027FFC42` on the Guest already has the Host's exact MAC, RF channel, and `TGID = mUnk_1690`, and `*(u16*)0x027FFC40 == 2`.
  - Only `*(u16*)0x027FFC4E = 0x0025; *(u16*)0x027FFC50 = 0x0000;` is needed so `func_02007ec4` builds `mUnk_0024` with MKDS's `GGID = 0x00000025` while preserving MAC, channel, and `(TGID + 7) & 0x7FFF`!

### 2.5 Menu Lockstep & Shared PRNG (`func_020cc744`, `StepDataSharing`, `mUnk_1ee8`)
- **PRNG Synchronization (`StartParentCallback` `code_020529b8.c:606-617` & `StartConnectCallback` `code_02051658.c:392-403`):**
  - Host sets `parentParameter->mUnk_0c = (mUnk_1690 + 7) & 0x7FFF` in `FillParentParameter` (`code_0204f814.c:179`).
  - Guest computes `mUnk_0024->mUnk_48 = (retained_tgid + 7) & 0x7FFF` in `func_0204df90`.
  - Both seed `data_0217aa00->mUnk_1ee8.mValue = (u32)(v * 3 | (v << 16))` (`v == (mUnk_1690 + 7) & 0x7FFF`). Keeping `TGID == mUnk_1690` across the Station bootstrap guarantees **bit-identical** Random-mode course pool shuffling on Host and all Guests!
- **Input Lockstep (`StepDataSharing` / `UpdateDataSharing`, WM port 12):**
  - `EnterMPState12` / `UpdateMPState12` calls `WM_StartDataSharing(mUnk_15bc, port=12, aidBitmap=0xFF, dataLength=8, doubleMode=1)`.
  - Host pad keys (`GetPadKeys()`) in slot `0` are replicated to `data_02175628[0]` on every console, driving identical menu cursor navigation and `func_020cc744()` execution.

### 2.6 Native Race Setup (`RequestMPState16`)
- **Location:** `src/Main/Unknown/code_0205928c.c:216` (`0x020596D4`).
- Each console independently passes its locally computed `gRaceSetup->mCourseId` and `mMode` into `RequestMPState16(courseId, mode)`. Guest calls `RequestRaceArchives(courseId, mode)` (`code_0205928c.c:159`) and streams the course/kart/UI archives over WM port 4 before transitioning `16 -> 17 -> 20 -> 15`.

### 2.7 Retail MKDS Multiboot Child vs Signed Station First Stage
- `func_02008880(FSFile *f)` (`MB_GetSegmentLength`) and `func_02008570(FSFile *f, void *buf, u32 len)` (`MB_ReadSegment`) **natively support** passing a non-`NULL` `FSFile *` opened on an untouched signed `.srl` file!
- For `mb/ds_demo_client.srl` (`306,824` B, `SHA-256: deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b`):
  - `ARM9`: `off=0x4000`, `entry=0x02004800`, `ram=0x02004000..0x02022B5C`, `size=0x1EB5C` (`< 0x022C0000`, passes `func_02008188` / `func_02007ff0` bounds check).
  - `ARM7`: `off=0x22C00`, `entry=0x02380000`, `ram=0x02380000..0x023A7E6C`, `size=0x27E6C` (`<= 0x40000` and `< 0x023FE800`, passes `func_02008188` / `func_02007f38` bounds check).
  - `romSize = 0x4AE00`; trailing `0x88` bytes at `0x4AE00..0x4AE88` are byte-for-byte identical to `mb/ds_demo_client.sgn[0..0x88]`.
  - Required MB buffer size (`func_02008880(&srlFile)`) = `0x1EB5C + 0x27E6C + 0x268 = 0x46C30` (`289,840` B), which fits entirely inside `WLMenuScene`'s existing `0x80000` (`524,288` B) `mUnk_1658` allocation without needing background worker streaming or Secure-Area patching!

### 2.8 Parent WM/MB State Machine & Handoff Boundaries
- **Stage-1 MB Lobby (`WLMenuScene`):**
  - `func_0204e568`: allocates/clears `mUnk_165c` (`0xC000` B: `gWirelessWork` at `+0x0000`, `data_0216fdac` at `+0x1E00`, `gWMSysBuf` at `+0x9B20`), picks `mUnk_1690 = func_02052c5c(1)` (TGID `0x8xxx`), calls `InitWirelessWork(..., ggid, tgid, 2)`, `SetMPLimits(0x1FE, 7)`, `func_02009cc8(channel)`, `func_0204ea3c()`.
  - Children join (`mUnk_1696 |= 1 << aid`, `mUnk_1699++`).
- **Host START Press (`func_0204db6c` in `code_0204db48.c:167-225`):**
  - `mUnk_000c == 22`: `func_0204e128()` -> `mUnk_000c = 24`, sends Stage-1 SRL until `mUnk_1697 == mUnk_1696`.
  - `mUnk_000c == 24`: `func_0204e0bc()` -> `mUnk_000c = 25`, sends `MB_CommBootRequest`; MKDS's NitroSDK 3.0 `func_020071b4` keeps `SendParentMessage(5, ...)` active past `BOOTREQ_ACCEPTED` (`kind == 11`) until actual WM disconnect (`eventCode == 1` in `func_020079c4`, setting `mUnk_1695 |= 1 << aid` and `mUnk_1698 |= 1 << aid`).
  - **[HaxxStation Insertion Point at `mUnk_000c == 25`]:** Snapshot `expectedRosterCount = popcount(mUnk_1695)` (`1..7`), `savedBootMask = mUnk_1695`, `savedTgid = mUnk_1690`, `savedChannel = mUnk_0088`; run `func_02009c04()` to close Stage-1 MB cleanly, start Station App parent (`GGID = 0x00400120`, `TGID = savedTgid`, `channel = savedChannel`), serve ports `13`/`14`/`15` + port `1` Station heartbeat (`{0, 0, 0x48, 0, 2, 0}`), enforce party-wide execution barrier before `0xFFFF`, then restore `GGID = 0x25` + `mUnk_1695 = savedBootMask` and run `func_0204e090()` (`mUnk_000c = 27`).
  - `mUnk_000c == 27 -> 36 -> 3 -> 5 -> 6 -> 11`: Native MKDS runs `StartRomArchiveTransfer()` -> `SetLifeTime(0x14)` -> `FillParentParameter(1)` -> `SetParentParameter()` -> `StartParent(NULL)` -> `CompactMaskBits(mUnk_1695)` -> `StartMP()` -> `func_0204dc8c()` (30s rejoin window) -> `func_02059878()` (`hMp`: `7 -> 8 -> 9 -> 10 -> 6 -> 1 -> 2 -> 3 -> 4 -> 12 -> 5 -> 15`).

### 2.9 File / Block Transfer (`CreateRomArchiveBlock`, Port 4)
- MKDS uses its own `RomArchiveTransfer` service (`code_0204bca0.c`) on WM port `4` (`0x154`-byte parent packets), **not** NitroSDK WFS.
- `CreateRomArchiveBlock(NULL, 0)` builds a serialized `[positionOffset, FAT, FNT, ARM9_OVT, ARM7_OVT]` block from the Host's `0x027FFE00` header and applies the 30 `sChildDArchiveRedirects` entries (`15` courses x `{.carc, Tex.carc}`).
- **Critical Phase 2 distinction:** In Phase 1 (`stock_cloneboot` mode), `CreateRomArchiveBlock` replaced `ARM9_OVT` with retail `mb_child_ovt.bin` (`mb_child_ov0..3.bin`) because the stock guest ran retail `Main`. In Phase 2 (`haxxstation` mode), the guest runs our newly linked `Main` (`OV000..OV003` at `0x021810A0..0x021DAEE0`), so `CreateRomArchiveBlock` **must** emit the Host's own `ARM9_OVT` (`0x027FFE50`) and serve the Host's compiled `ov000..ov003`! (Gated cleanly by `MKDS_BOOTSTRAP_MODE`.)
- Note on Guest `0x027FFE00` header: `FSi_InitRom` on a downloaded child (`*(u16*)0x027FFC40 == 2`) ignores `0x027FFE40..0x027FFE5F`, and `ReloadRomArchiveFromBlock` dynamically allocates/mounts the exact tables sent by the Host. Preserving `0x40..0x60` in `rom0000d` after `dspatch` is optional/benign as long as `dspatch`'s `0x20..0x3F` fields are never touched and `0x15E` CRC16 is recomputed.

### 2.10 Guest Overlays, HMAC Authentication & Memory Map
- **Overlay Authentication (`libs/NitroSDK/fs/src/code_02004dc0.c` & `libs/NitroSDK/dgt/src/hash2.c`):**
  1. `FS_StartOverlay` (`code_02004dc0.c:205-217`): when `*(u16*)0x027FFC40 == 2`, verifies each compressed overlay `ov000..ov003` against `OverlaySignatures[id]` using `HMAC-SHA1` with the 64-byte key at `data_02152b80`. `dsd rom build` **already updates** `OverlaySignatures[0..3]` in `mkds_usa.nds`.
  2. `FS_AttachOverlayTable` (`code_02004dc0.c:330-335`): called during `ReloadRomArchiveFromBlock` on the guest! Calls `DGT_SetOverlayTableMode(1)` and verifies `ARM9_OVT` (`128` B) against `data_0216f2dc` (`0x0216F49C`).
     - **Critical discovery:** `dsd rom build` does **not** update `data_0216f2dc` (leaving retail `b6d4714dd72681ad1977037099ab2e6fd1f79bcb`), AND `DGT_SetOverlayTableMode(1)` zeroes bytes `+0x18..+0x1B` and `+0x38..+0x3B` of **every** 64-byte SHA-1 block (including `ipad`, `opad`, and padding blocks!). `build_mkds_client.py` computes this exact NitroSDK `DGTi_Hash2ProcessMessageBlock` digest (verified bit-for-bit against retail `b6d4714d...`) and updates `data_0216f2dc` in the unsigned guest `ARM9`, keeping full overlay authentication enabled!
- **Stage-2 Loader & Guest RAM Map (`dspatch` + `ds_demo_client.srl`):**
  - `ds_demo_client.srl` runs its Stage-2 code in `0x02320000..0x0235E700` (with download state at `0x023FE840`) and downloads `rom0000d` (`~1.13 MiB`) into Main RAM buffer `r4` (`< 0x02150000`).
  - `dspatch` sets `rom0000d` header `ARM9` to `off=0x180, load=0x02332C40, size=0x100`, overwriting `rsa_GetDecodedHash` with `exploitData`.
  - `exploitData` copies the real MKDS Guest `ARM9` (`~0xE7910` B) forward from `r4 + 0x4000` to `0x02000000..0x020E7910` (non-destructive forward `MI_CpuCopy8` since `dst <= src`, ending well below `r4 + arm7offset` `>= 0x020EBA00`), flushes caches, and returns `1` (`TRUE`).
  - Because MKDS's `ARM7` has `SubRamAddress = 0x02380000` (`< 0x03000000`), `dspatch` does **not** prepend `arm7Fix`; the Station launcher copies MKDS's `ARM7` (`0x289A4` B) directly to `0x02380000..0x023A89A4`, copies `rom0000d[0..0x160]` to `0x027FFE00`, and jumps ARM9 to `0x02000800` and ARM7 to `0x02380000`.
  - All 18 BIOS `SWI` veneers in `0x0200007E..0x0200079A` (`VBlankIntrWait`, `Div`, `Mod`, `CpuSet`, `LZ77UnCompReadNormalWrite8bit`, etc.) are already decrypted plaintext Thumb instructions in `build/usa/mkds_usa.nds[0x4000..0x4800]`.

### 2.11 Empirical Stage-2 Invariants Proven During Bringup
1. **In-Place Backward LZ77 (`MIi_UncompressBackward` / `func_02000934`) Cursor Safety:**
   - ARM9 CRT0 unpacks static `ARM9` **in-place** in `0x02000000` RAM (`r2` = backward write cursor, `r3` = backward read cursor). If incompressible literals sit immediately above a forced `0x02004000` split, `r2` dips below `r3` by 1 byte and clobbers `0x02003FFC..0x02004000` inside `FS_UnloadArchive` (causing a delayed Data Abort during `ReloadRomArchiveFromBlock`).
   - `scripts/haxxstation/nitrofs_util.py` (`compress_backward` + `uncompress_backward_inplace`) selects the exact `compstatic` maximum-savings prefix boundary `>= 0x4000` and asserts `r2 >= r3` at every instruction at build time.
2. **Overlay HMAC-SHA1 Key Pointer Indirection (`*data_02161768`):**
   - In `libs/NitroSDK/fs/src/code_02004dc0.c:187`, `data_02161768` (`0x02162D68`) is a `const u8 *` pointer (`-> 0x02154124`) to the 64-byte NitroSDK HMAC key (`2106c0deba98ce3f...`). `build_mkds_client.py` dereferences `*syms["data_02161768"]` and verifies all 4 overlay signatures (`OverlaySignatures[0..3]`) plus the `DGT_SetOverlayTableMode(1)` OVT digest (`data_0216f2dc` at `0x021708BC`).
3. **Download Play `SeqArc(1)` Stripped Map-Object SFX Guards:**
   - On `*(u16*)0x027FFC40 == 2`, `LoadSndDataFromFile` (`code_0210e33c.c:296`) swaps stripped `SeqArc(1)` into `SeqArc(0)` to fit the Download Play `SndHeap`. Map-object SFX IDs unique to the 24 non-Simple-8 tracks (e.g., Desert Hills Pokey `soundId = 0x195`) have `mOffset == -1` in `SeqArc(1)`, causing `NNS_SndArcGetSeqArcParam()` / `NNS_SndArcGetSeqInfo()` to return `NULL`.
   - Adding `NULL` guards at the four unchecked callers (`code_0210bcf4.c:161`, `code_0210c1b0.c:116`, `code_0210a67c.c:462`, `code_0210d740.c:62`) prevents ARM9 Data Aborts across all 32 tracks without expanding the guest `SndHeap`.
4. **Download Play `SndHeap` Capacity (`GetSndInitBufferSize(2) = 0xb8000`):**
   - Retail `GetSndInitBufferSize(2)` (`0xaf000`, `code_02105550.c:262`) leaves `~57.6 KiB` free in `data_0217d130` after `LoadSndDataFromFile` — sized for the 8 original Simple-mode tracks (`<= 57,024` B). Exactly five non-Simple-8 tracks require `59,872..82,368` B (`Mario Circuit` `59,872` B, `Bowser Castle` `71,232` B, `Shroom Ridge` `73,344` B, `GCN Mushroom Bridge` `76,192` B, and `Rainbow Road` `82,368` B). Raising `GetSndInitBufferSize(2)` by `+36 KiB` (`0xb8000`, still `64 KiB` smaller than normal cartridge `0xc8000`) provides `~94.5 KiB` in `data_0217d130` while preserving `> 300 KiB` of free guest `ExpHeap` MainRAM.
