# MKDS HaxxStation Stage-1 / Stage-2 Guest Memory Map Audit

**Target:** Mario Kart DS USA (`AMCE-00`) unsigned Stage-2 guest launched via DS Download Station Vol. 1 USA (`NTRJ`).

## 1. Stage-2 Loader (`ds_demo_client.srl` + `dspatch` Exploit Wrapper)

| Region | Start | End (excl.) | Size | Owner / Safety Proof |
| --- | --- | --- | --- | --- |
| Temporary `rom0000d` download buffer (`r4`) | `~0x02004800` | `~0x02120000` | `1,139,264` B (`0x116240`) | Allocated in low MainRAM by `ds_demo_client.srl` (`< 0x02320000`); never collides with running Station client (`0x02320000..0x0235E700`). |
| Station Stage-1 relocated client | `0x02320000` | `0x0235E700` | `255,744` B | Runs Stage-2 download & launcher; `0x02332C40` (`rsa_GetDecodedHash`) overwritten by `0x100`-byte `dspatch` `exploitData`. |
| Forward ARM9 copy (`exploitData`) | `0x02000000` | `0x020E8D18` | `953,624` B | Copies `r4 + 0x4000` forward to `0x02000000` (`dst <= src`, non-destructive); ends below `r4 + arm7_off`, well below `0x02320000`. |
| Direct ARM7 copy (no `arm7Fix` shim) | `0x02380000` | `0x023A89A4` | `166,308` B | `SubRamAddress = 0x02380000 < 0x03000000`, so `dspatch` omits `arm7Fix`; Station copies MKDS ARM7 directly to `0x02380000`. |

## 2. Final MKDS Stage-2 Guest Runtime Memory Map

| Region | Start | End (excl.) | Size | Purpose / Invariant |
| --- | --- | --- | --- | --- |
| Decrypted Secure Area & SWI veneers | `0x02000000` | `0x02000800` | `2,048` B | Plaintext Thumb BIOS `SWI` veneers (`0x0200007E..0x0200079A`). |
| `Entry`, `MIi_UncompressBackward`, `BuildInfo` | `0x02000800` | `0x02001000` | `2,048` B | `AutoloadCallback` (`0x02000A58`) branches to `0x02001000`. |
| Guest-only child/GGID stub + zeroed Logo cave | `0x02001000` | `0x02003000` | `8,192` B | 36-byte stub at `0x02001000` adapts `GGID -> 0x25`; rest zeroed like retail Cloneboot. |
| Decoded MKDS Static `ARM9` (`.text`/`.rodata`/`.data`) | `0x02003000` | `0x02171EE0` | `1,502,944` B | Contains Simple-32 `GetSelectCupGridCupEnabled` & `BuildCourseList` + updated OVT HMAC (`data_0216f2dc`). *(Autoload tail up to `0x02179F78` copied to ITCM/DTCM before `.bss` clear.)* |
| Static `ARM9` `.bss` | `0x02171EE0` | `0x02183080` | `70,048` B | Cleared and cache-flushed by `Entry`. |
| `OV000` | `0x02183080` | `0x021BA720` | `226,976` B | Wireless overlay (`HMAC-SHA1` verified) |
| `OV001` | `0x02183080` | `0x021CCFE0` | `302,944` B | Wireless overlay (`HMAC-SHA1` verified) |
| `OV002` | `0x02183080` | `0x021D30A0` | `327,712` B | Wireless overlay (`HMAC-SHA1` verified) |
| `OV003` | `0x021BA720` | `0x021DCEC0` | `141,216` B | Wireless overlay (`HMAC-SHA1` verified) |
| MainRAM Heap Arena (`ArenaLo .. ArenaHi`) | `0x021DCEC0` | `0x023FE000` | `2,232,640` B | Includes `0x4B000` (`307,200` B) `mCourseBufferHeap` for race `.carc` / `D`-archives. |
| ARM9 ITCM | `0x01FF8000` | `0x01FFFFA0` | `32,672` B | Autoloaded by `func_020009e0`. |
| ARM9 DTCM + IRQ/SVC/SYS Stacks | `0x027E0000` | `0x027E4000` | `16,384` B | `.dtcm` data at `0x027E0000..0x027E00E0`; stacks at top of DTCM. |
| Uncached Shared Boot / Parent `WMBssDesc` | `0x027FFC40` | `0x027FFC80` | `64` B | `bootType == 2` at `0x027FFC40`; parent MAC, `GGID = 0x25`, `TGID`, RF channel at `0x027FFC42..0x027FFC7E`. |
| Downloaded NDS Header | `0x027FFE00` | `0x027FFF60` | `352` B | Copied from `rom0000d[0..0x160]` (`MARIOKARTDS` / `AMCE`). |
