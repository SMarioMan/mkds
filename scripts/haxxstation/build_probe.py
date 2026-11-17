#!/usr/bin/env python3
"""Build a standalone, zero-commercial-code DS homebrew probe (`marker-probe.nds`) for Gate M1.

Compiles tiny dual-CPU bare-metal ARM9 + ARM7 binaries with `arm-none-eabi-gcc`:
  - ARM9 (`0x02004000`, entry `0x02004000`):
      1. Captures initial DS boot / multiboot parent state from `0x027FFC40..0x027FFC7F`
         into uncached shared MainRAM `0x027FF000..0x027FF03F` (`magic = 0x50584148` = `"HAXP"`).
      2. Initializes 2D Engine A (top screen) and 2D Engine B (bottom screen) in Mode 0
         BG0 4bpp text mode using an embedded 8x8 ASCII bitmap font, rendering large
         unmistakable banner text:
             `MKDS HAXX PROBE`
             `STAGE 2 UNSIGNED OK`
         along with live hex readouts of `BOOT_TYPE`, `GGID`, `TGID`, `CHAN`, and `ARM7_ALIVE`.
  - ARM7 (`0x02380000`, entry `0x02380000`):
      Initializes ARM7 stacks, polls `REG_VCOUNT` VBlank transitions, and increments
      `*(vu32*)0x023FF014` (`== *(vu32*)0x027FF014` on ARM9) every frame to prove dual-CPU liveness.
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nitrofs_util import (  # noqa: E402
    EXPECTED_STATION_USA_SHA1,
    make_banner_v1,
    pack_executable_only_nds,
    sha1_file,
    sha256_file,
)

ARM7_ASM = r"""
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    /* Disable ARM7 master interrupts while setting stacks */
    ldr     r0, =0x04000208
    mov     r1, #0
    str     r1, [r0]            /* REG_IME = 0 */
    str     r1, [r0, #8]        /* REG_IE  = 0 */
    mvn     r2, #0
    str     r2, [r0, #12]       /* REG_IF  = ~0 */

    /* IRQ mode stack */
    mrs     r0, cpsr
    bic     r1, r0, #0x1F
    orr     r1, r1, #0x92
    msr     cpsr_c, r1
    ldr     sp, =0x0380FF80

    /* SVC mode stack */
    bic     r1, r0, #0x1F
    orr     r1, r1, #0x93
    msr     cpsr_c, r1
    ldr     sp, =0x0380FFC0

    /* System mode stack */
    bic     r1, r0, #0x1F
    orr     r1, r1, #0x9F
    msr     cpsr_c, r1
    ldr     sp, =0x0380FD00

    /* Write ARM7 ready magic (`0x374D5241` = "ARM7") and loop incrementing heartbeat on VBlank */
    ldr     r4, =0x023FF010
    ldr     r5, =0x374D5241
    str     r5, [r4, #0]
    mov     r6, #0
    str     r6, [r4, #4]

    ldr     r7, =0x04000006     /* REG_VCOUNT */
1:
    ldrh    r0, [r7]
    cmp     r0, #192
    beq     1b
2:
    ldrh    r0, [r7]
    cmp     r0, #192
    bne     2b
    add     r6, r6, #1
    str     r6, [r4, #4]        /* *(vu32*)0x023FF014 = arm7_frame_counter */
    b       1b
"""

ARM9_ASM = r"""
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    /* Disable ARM9 IME while initializing */
    ldr     r0, =0x04000208
    mov     r1, #0
    str     r1, [r0]
    str     r1, [r0, #8]
    mvn     r2, #0
    str     r2, [r0, #12]

    /* Clean & flush D-Cache, drain write buffer, invalidate I-Cache */
    mov     r0, #0
1:  mov     r1, #0
2:  orr     r2, r0, r1
    mcr     p15, 0, r2, c7, c14, 2
    add     r1, r1, #0x20
    cmp     r1, #0x1000
    blo     2b
    add     r0, r0, #0x40000000
    cmp     r0, #0
    bne     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    mcr     p15, 0, r0, c7, c5, 0

    /* System mode stack in DTCM */
    mrs     r0, cpsr
    bic     r1, r0, #0x1F
    orr     r1, r1, #0x9F
    msr     cpsr_c, r1
    ldr     sp, =0x027E3F00

    bl      probe_main
3:  b       3b
"""

ARM9_C = r"""
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef volatile u8        vu8;
typedef volatile u16       vu16;
typedef volatile u32       vu32;

#define REG_POWCNT1     (*(vu16*)0x04000304)
#define REG_VRAMCNT_A   (*(vu8*)0x04000240)
#define REG_VRAMCNT_C   (*(vu8*)0x04000242)
#define REG_DISPCNT     (*(vu32*)0x04000000)
#define REG_BG0CNT      (*(vu16*)0x04000008)
#define REG_DISPCNT_SUB (*(vu32*)0x04001000)
#define REG_BG0CNT_SUB  (*(vu16*)0x04001008)
#define REG_VCOUNT      (*(vu16*)0x04000006)

#define BG_PALETTE      ((vu16*)0x05000000)
#define BG_PALETTE_SUB  ((vu16*)0x05000400)
#define BG_GFX          ((vu32*)0x06008000)
#define BG_MAP          ((vu16*)0x06000000)
#define BG_GFX_SUB      ((vu32*)0x06208000)
#define BG_MAP_SUB      ((vu16*)0x06200000)

/* Minimal 8x8 1bpp font for ' ', '0'-'9', 'A'-'Z', ':', '=', '-' */
static u8 glyph_row(char c, int row) {
    static const u8 digits[10][8] = {
        {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0 */
        {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
        {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00}, /* 2 */
        {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
        {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, /* 4 */
        {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
        {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
        {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, /* 7 */
        {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
        {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    };
    static const u8 alpha[26][8] = {
        {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* A */
        {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* B */
        {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* C */
        {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* D */
        {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* E */
        {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
        {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, /* G */
        {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* H */
        {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
        {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, /* J */
        {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* K */
        {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* L */
        {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* M */
        {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, /* N */
        {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* O */
        {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* P */
        {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, /* Q */
        {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, /* R */
        {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* S */
        {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
        {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* U */
        {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* V */
        {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
        {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* X */
        {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* Y */
        {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* Z */
    };
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    if (c >= 'A' && c <= 'Z') return alpha[c - 'A'][row];
    if (c == ':') return (row == 2 || row == 5) ? 0x18 : 0x00;
    if (c == '=') return (row == 2 || row == 4) ? 0x7E : 0x00;
    if (c == '-') return (row == 3) ? 0x7E : 0x00;
    return 0x00;
}

static void upload_font(vu32 *gfx_base) {
    for (int ch = 0; ch < 96; ++ch) {
        char c = (char)(ch + 32);
        for (int r = 0; r < 8; ++r) {
            u8 bits = glyph_row(c, r);
            u32 word = 0;
            for (int x = 0; x < 8; ++x) {
                u32 pix = (bits & (0x80 >> x)) ? 1u : 0u;
                word |= (pix << (x * 4));
            }
            gfx_base[ch * 8 + r] = word;
        }
    }
}

static void draw_str(vu16 *map, int x, int y, const char *s, int pal) {
    for (int i = 0; s[i] != '\0' && (x + i) < 32; ++i) {
        int ch = (unsigned char)s[i] - 32;
        if (ch < 0 || ch >= 96) ch = 0;
        map[y * 32 + x + i] = (u16)(ch | (pal << 12));
    }
}

static void fmt_hex(char *out, u32 val, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; --i) {
        out[i] = hex[val & 0xF];
        val >>= 4;
    }
    out[digits] = '\0';
}

void probe_main(void) {
    /* Capture DS boot state into uncached shared MainRAM 0x027FF000 (= MainRAM[0x3FF000]) */
    vu32 *diag = (vu32*)0x027FF000;
    u16 boot_type = *(vu16*)0x027FFC40;
    u32 ggid = (u32)(*(vu16*)0x027FFC4E) | ((u32)(*(vu16*)0x027FFC50) << 16);
    u16 tgid = *(vu16*)0x027FFC52;
    u8  chan = *(vu8*)0x027FFC78;
    u32 mac_lo = (u32)(*(vu16*)0x027FFC46) | ((u32)(*(vu16*)0x027FFC48) << 16);
    u16 mac_hi = *(vu16*)0x027FFC4A;

    diag[0] = 0x50584148u; /* "HAXP" magic */
    diag[1] = (u32)boot_type;
    diag[2] = ggid;
    diag[3] = ((u32)chan << 16) | (u32)tgid;
    diag[6] = mac_lo;
    diag[7] = (u32)mac_hi;
    diag[8] = 0;

    /* Turn on both LCDs + 2D Engine A & B */
    REG_POWCNT1 = 0x8203;
    REG_VRAMCNT_A = 0x81; /* Engine A BG VRAM @ 0x06000000 */
    REG_VRAMCNT_C = 0x84; /* Engine B BG VRAM @ 0x06200000 */

    REG_DISPCNT     = 0x00010100u; /* Mode 0, BG0 enabled */
    REG_BG0CNT      = 0x0004u;     /* Map base 0 (0x06000000), Tile base 1 (0x06004000? Wait: tile base unit is 16KB -> bit 2 = 1 -> 0x06004000; bit 3 = 1 -> 0x06008000!) */
    REG_BG0CNT      = 0x0008u;     /* 4bpp, map @ +0x0000, tiles @ +0x8000 */
    REG_DISPCNT_SUB = 0x00010100u;
    REG_BG0CNT_SUB  = 0x0008u;

    /* Palettes: 0=navy bg + bright yellow text, 1=navy bg + bright green text, 2=navy bg + white text */
    BG_PALETTE[0] = 0x2842;     /* dark blue */
    BG_PALETTE[1] = 0x03FF;     /* yellow */
    BG_PALETTE[17] = 0x03E0;    /* green */
    BG_PALETTE[33] = 0x7FFF;    /* white */

    BG_PALETTE_SUB[0] = 0x1084; /* dark slate */
    BG_PALETTE_SUB[1] = 0x03FF;
    BG_PALETTE_SUB[17] = 0x03E0;
    BG_PALETTE_SUB[33] = 0x7FFF;

    for (int i = 0; i < 32 * 24; ++i) {
        BG_MAP[i] = 0;
        BG_MAP_SUB[i] = 0;
    }
    upload_font(BG_GFX);
    upload_font(BG_GFX_SUB);

    draw_str(BG_MAP, 4, 3,  "========================", 2);
    draw_str(BG_MAP, 8, 5,  "MKDS HAXX PROBE", 0);
    draw_str(BG_MAP, 6, 7,  "STAGE 2 UNSIGNED OK", 1);
    draw_str(BG_MAP, 4, 9,  "========================", 2);

    char buf[24];
    draw_str(BG_MAP, 5, 12, "BOOT TYPE:", 2);
    fmt_hex(buf, boot_type, 4);
    draw_str(BG_MAP, 16, 12, buf, 1);

    draw_str(BG_MAP, 5, 14, "ST1 GGID :", 2);
    fmt_hex(buf, ggid, 8);
    draw_str(BG_MAP, 16, 14, buf, 1);

    draw_str(BG_MAP, 5, 16, "ST1 TGID :", 2);
    fmt_hex(buf, tgid, 4);
    draw_str(BG_MAP, 16, 16, buf, 1);

    draw_str(BG_MAP, 5, 18, "RF CHAN  :", 2);
    fmt_hex(buf, chan, 2);
    draw_str(BG_MAP, 16, 18, buf, 1);

    draw_str(BG_MAP_SUB, 8, 6,  "MKDS HAXX PROBE", 0);
    draw_str(BG_MAP_SUB, 5, 9,  "ARM7 TICKS:", 2);
    draw_str(BG_MAP_SUB, 5, 11, "ARM9 TICKS:", 2);

    u32 arm9_ticks = 0;
    for (;;) {
        while (REG_VCOUNT != 192) {}
        while (REG_VCOUNT == 192) {}
        arm9_ticks++;
        diag[8] = arm9_ticks;
        fmt_hex(buf, diag[5], 8);
        draw_str(BG_MAP_SUB, 17, 9, buf, 1);
        fmt_hex(buf, arm9_ticks, 8);
        draw_str(BG_MAP_SUB, 17, 11, buf, 1);
    }
}
"""

ARM7_LD = r"""
ENTRY(_start)
SECTIONS {
    . = 0x02380000;
    .text : { *(.text.start) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss  : { *(.bss*) *(COMMON) }
}
"""

ARM9_LD = r"""
ENTRY(_start)
SECTIONS {
    . = 0x02004000;
    .text : { *(.text.start) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss  : { *(.bss*) *(COMMON) }
}
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--station-rom",
        type=Path,
        required=True,
        help="Path to DS Download Station Vol. 1 USA ROM (used strictly to copy 156-byte header Nintendo logo)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("build/haxxstation/marker-probe.nds"),
        help="Output path for standalone unsigned marker-probe.nds",
    )
    args = ap.parse_args()

    if not args.station_rom.is_file():
        print(f"ERROR: Station ROM not found: {args.station_rom}", file=sys.stderr)
        return 1
    st_sha1 = sha1_file(args.station_rom)
    if st_sha1 != EXPECTED_STATION_USA_SHA1:
        print(
            f"ERROR: Station ROM SHA-1 mismatch: got {st_sha1}, expected {EXPECTED_STATION_USA_SHA1}",
            file=sys.stderr,
        )
        return 1

    with args.station_rom.open("rb") as f:
        f.seek(0xC0)
        logo_156 = f.read(0x9C)

    work_dir = args.out.parent / "probe_obj"
    work_dir.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    (work_dir / "arm7.S").write_text(ARM7_ASM)
    (work_dir / "arm7.ld").write_text(ARM7_LD)
    (work_dir / "arm9_start.S").write_text(ARM9_ASM)
    (work_dir / "arm9_main.c").write_text(ARM9_C)
    (work_dir / "arm9.ld").write_text(ARM9_LD)

    subprocess.run(
        [
            "arm-none-eabi-gcc",
            "-mcpu=arm7tdmi",
            "-marm",
            "-nostdlib",
            "-Wl,--build-id=none",
            "-T",
            str(work_dir / "arm7.ld"),
            str(work_dir / "arm7.S"),
            "-o",
            str(work_dir / "arm7.elf"),
        ],
        check=True,
    )
    subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", str(work_dir / "arm7.elf"), str(work_dir / "arm7.bin")],
        check=True,
    )

    subprocess.run(
        [
            "arm-none-eabi-gcc",
            "-mcpu=arm946e-s",
            "-marm",
            "-O2",
            "-ffreestanding",
            "-nostdlib",
            "-Wl,--build-id=none",
            "-T",
            str(work_dir / "arm9.ld"),
            str(work_dir / "arm9_start.S"),
            str(work_dir / "arm9_main.c"),
            "-o",
            str(work_dir / "arm9.elf"),
        ],
        check=True,
    )
    subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", str(work_dir / "arm9.elf"), str(work_dir / "arm9.bin")],
        check=True,
    )

    arm9_bytes = (work_dir / "arm9.bin").read_bytes()
    if len(arm9_bytes) % 4 != 0:
        arm9_bytes += b"\x00" * (4 - (len(arm9_bytes) % 4))
    arm7_bytes = (work_dir / "arm7.bin").read_bytes()
    if len(arm7_bytes) % 4 != 0:
        arm7_bytes += b"\x00" * (4 - (len(arm7_bytes) % 4))
    banner_bytes = make_banner_v1(["MKDS HAXX PROBE", "Unsigned Stage-2 Marker Test"])

    nds_bytes = pack_executable_only_nds(
        game_name="MKDSHAXXPRB",
        game_code="AHXP",
        maker_code="01",
        arm9_bytes=arm9_bytes,
        arm9_entry=0x02004000,
        arm9_ram=0x02004000,
        arm7_bytes=arm7_bytes,
        arm7_entry=0x02380000,
        arm7_ram=0x02380000,
        logo_156=logo_156,
        banner_bytes=banner_bytes,
    )
    args.out.write_bytes(nds_bytes)

    meta = {
        "probe_nds": str(args.out),
        "probe_size": len(nds_bytes),
        "probe_sha256": sha256_file(args.out),
        "arm9_entry": "0x02004000",
        "arm9_ram": "0x02004000",
        "arm9_size": len(arm9_bytes),
        "arm7_entry": "0x02380000",
        "arm7_ram": "0x02380000",
        "arm7_size": len(arm7_bytes),
        "diag_uncached_addr": "0x027FF000",
        "diag_mainram_offset": "0x003FF000",
        "diag_magic": "0x50584148",
    }
    meta_path = args.out.with_suffix(".json")
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
