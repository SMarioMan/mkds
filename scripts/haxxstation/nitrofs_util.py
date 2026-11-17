#!/usr/bin/env python3
"""Shared Nitro / NDS / backward-LZ77 / NitroSDK-HMAC utilities for MKDS HaxxStation."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

EXPECTED_MKDS_USA_SHA1 = "691e00d9a5dd80b04f80cc7559503e8b06848785"
EXPECTED_STATION_USA_SHA1 = "f18b55f3e1259c03e10d0ecb549693b42905ceb5"
EXPECTED_STATION_SRL_SHA256 = "deaec313106e34d8290b4c9c6e3405b8bb2eaec1db6c2cad38ee2771aef1ca2b"
EXPECTED_STATION_SGN_SHA256 = "118e179b7bdc26110444988d879f7baff9042c2111fc9cf6dd2485639794d1ff"

# MKDS USA NitroSDK overlay HMAC key (*data_02161768 -> 0x02152b80 retail / 0x02154124 decomp, 64 bytes)
MKDS_OVERLAY_HMAC_KEY = bytes.fromhex(
    "2106c0deba98ce3fa692e39d46f2ed01"
    "76e3cc08562363facad4ecdf9a627834"
    "8f6d633cfe22ca9220889723d2cfaec2"
    "32678dfeca836498acfd3e3787465824"
)

_CRC16_TABLE: list[int] = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = ((_c >> 1) ^ 0xA001) if (_c & 1) else (_c >> 1)
    _CRC16_TABLE.append(_c)


def crc16_modbus(data: bytes | bytearray | memoryview) -> int:
    """Compute Nintendo DS standard CRC16 (init 0xFFFF, reflected poly 0xA001)."""
    crc = 0xFFFF
    for b in data:
        crc = ((crc >> 8) ^ _CRC16_TABLE[(crc ^ b) & 0xFF]) & 0xFFFF
    return crc


def sha1_bytes(data: bytes | bytearray | memoryview) -> str:
    return hashlib.sha1(data).hexdigest()


def sha256_bytes(data: bytes | bytearray | memoryview) -> str:
    return hashlib.sha256(data).hexdigest()


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def list_nitrofs_files(rom: bytes | bytearray) -> dict[str, tuple[int, int, int]]:
    """Return mapping `path_without_leading_slash -> (file_id, start_off, end_off)`."""
    fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", rom, 0x40)
    if fnt_sz == 0:
        return {}
    out: dict[str, tuple[int, int, int]] = {}

    def walk(dir_id: int, prefix: str) -> None:
        sub_off, first_fid = struct.unpack_from("<IH", rom, fnt_off + (dir_id & 0xFFF) * 8)
        p = fnt_off + sub_off
        fid = first_fid
        while p < fnt_off + fnt_sz:
            t = rom[p]
            p += 1
            if t == 0:
                break
            is_dir = (t & 0x80) != 0
            nlen = t & 0x7F
            name = bytes(rom[p : p + nlen]).decode("ascii")
            p += nlen
            full = f"{prefix}/{name}" if prefix else name
            if is_dir:
                sub_id = struct.unpack_from("<H", rom, p)[0]
                p += 2
                walk(sub_id, full)
            else:
                s_off, e_off = struct.unpack_from("<II", rom, fat_off + fid * 8)
                out[full] = (fid, s_off, e_off)
                fid += 1

    walk(0xF000, "")
    return out


def read_nitrofs_file(rom: bytes | bytearray, rel_path: str) -> bytes:
    key = rel_path.strip("/")
    files = list_nitrofs_files(rom)
    if key not in files:
        raise KeyError(f"NitroFS file not found: {rel_path!r}")
    _, s_off, e_off = files[key]
    return bytes(rom[s_off:e_off])


def make_banner_v1(title_lines: list[str], icon_tiles: bytes | None = None, icon_pltt: bytes | None = None) -> bytes:
    """Create a valid 0x840-byte NDS BannerV1 with computed CRC16."""
    if icon_tiles is None:
        # Simple 32x32 4bpp checkerboard/border icon (512 bytes)
        tiles = bytearray(512)
        for ty in range(4):
            for tx in range(4):
                t_idx = ty * 4 + tx
                for py in range(8):
                    gy = ty * 8 + py
                    row = bytearray(4)
                    for px in range(4):
                        gx0 = tx * 8 + px * 2
                        gx1 = gx0 + 1
                        c0 = 2 if (gx0 in (0, 31) or gy in (0, 31)) else (3 if ((gx0 ^ gy) & 4) else 1)
                        c1 = 2 if (gx1 in (0, 31) or gy in (0, 31)) else (3 if ((gx1 ^ gy) & 4) else 1)
                        row[px] = (c1 << 4) | c0
                    tiles[t_idx * 32 + py * 4 : t_idx * 32 + (py + 1) * 4] = row
        icon_tiles = bytes(tiles)
    if icon_pltt is None:
        pltt = bytearray(32)
        struct.pack_into("<HHHH", pltt, 0, 0x0000, 0x2842, 0x7FFF, 0x03E0)
        icon_pltt = bytes(pltt)

    assert len(icon_tiles) == 512 and len(icon_pltt) == 32
    text = "\n".join(title_lines)[:127]
    utf16_str = text.ljust(128, "\x00").encode("utf-16-le")
    assert len(utf16_str) == 256

    body = icon_tiles + icon_pltt + (utf16_str * 6)
    assert len(body) == 2080
    banner_crc = crc16_modbus(body)
    hdr = struct.pack("<BBH28s", 1, 0, banner_crc, b"\x00" * 28)
    return hdr + body


def pack_executable_only_nds(
    *,
    game_name: str,
    game_code: str,
    maker_code: str,
    arm9_bytes: bytes,
    arm9_entry: int,
    arm9_ram: int,
    arm7_bytes: bytes,
    arm7_entry: int,
    arm7_ram: int,
    logo_156: bytes,
    banner_bytes: bytes | None = None,
    rom_param_a: bytes = b"\x00\x60\x58\x00\xf8\x08\x18\x00",
    rom_param_b: bytes = b"\x1e\x05",
    main_autoload_done: int = 0,
    sub_autoload_done: int = 0,
    nitro_footer: bytes = b"",
    reserved_b_override: bytes | None = None,
) -> bytes:
    """Pack a clean, valid executable-only NDS file compatible with `dspatch` (`NDS.cs`)."""
    assert len(logo_156) == 0x9C
    if nitro_footer:
        assert len(nitro_footer) == 12

    buf = bytearray(0x4000)  # Header + empty secure prefix region

    arm9_off = len(buf)
    arm9_size = len(arm9_bytes)
    buf.extend(arm9_bytes)
    if nitro_footer:
        buf.extend(nitro_footer)
    else:
        # Ensure at least 4 zero bytes follow MainRom so dspatch's NitroFooter probe never reads SubRom/EOF!
        buf.extend(b"\x00\x00\x00\x00")
    while len(buf) % 0x200 != 0:
        buf.append(0xFF)

    arm7_off = len(buf)
    arm7_size = len(arm7_bytes)
    buf.extend(arm7_bytes)
    while len(buf) % 0x200 != 0:
        buf.append(0xFF)

    # Minimal valid root FNT: 1 DirectoryTableEntry (start=8, firstFile=0, totalDirs=1) + 1 EndOfDir (0x00)
    fnt_off = len(buf)
    fnt_bytes = struct.pack("<IHHB", 8, 0, 1, 0)
    fnt_size = len(fnt_bytes)
    buf.extend(fnt_bytes)
    while len(buf) % 0x200 != 0:
        buf.append(0xFF)

    fat_off = len(buf)
    fat_size = 0

    banner_off = 0
    if banner_bytes is not None:
        assert len(banner_bytes) == 0x840
        banner_off = len(buf)
        buf.extend(banner_bytes)
        while len(buf) % 0x200 != 0:
            buf.append(0xFF)

    rom_size = len(buf)
    cap = 1 << 17
    dev_size = 0
    while cap < rom_size:
        cap <<= 1
        dev_size += 1

    hdr = bytearray(0x160)
    hdr[0x00:0x0C] = game_name.encode("ascii")[:12].ljust(12, b"\x00")
    hdr[0x0C:0x10] = game_code.encode("ascii")[:4].ljust(4, b"\x00")
    hdr[0x10:0x12] = maker_code.encode("ascii")[:2].ljust(2, b"\x00")
    hdr[0x14] = dev_size

    struct.pack_into(
        "<8I",
        hdr,
        0x20,
        arm9_off,
        arm9_entry,
        arm9_ram,
        arm9_size,
        arm7_off,
        arm7_entry,
        arm7_ram,
        arm7_size,
    )
    struct.pack_into("<8I", hdr, 0x40, fnt_off, fnt_size, fat_off, fat_size, 0, 0, 0, 0)
    hdr[0x60:0x68] = rom_param_a
    struct.pack_into("<IH", hdr, 0x68, banner_off, 0x0000)
    hdr[0x6E:0x70] = rom_param_b
    struct.pack_into("<II", hdr, 0x70, main_autoload_done, sub_autoload_done)
    struct.pack_into("<II", hdr, 0x80, rom_size, 0x4000)
    if reserved_b_override is not None:
        assert len(reserved_b_override) == 0x38
        hdr[0x88:0xC0] = reserved_b_override

    hdr[0xC0:0x15C] = logo_156
    logo_crc = crc16_modbus(logo_156)
    struct.pack_into("<H", hdr, 0x15C, logo_crc)
    hdr_crc = crc16_modbus(hdr[:0x15E])
    struct.pack_into("<H", hdr, 0x15E, hdr_crc)

    buf[0x000:0x160] = hdr
    return bytes(buf)


def uncompress_backward(data: bytes | bytearray) -> bytes:
    """Exact implementation of NitroSDK `MIi_UncompressBackward` (`code_02000920.c`)."""
    if len(data) < 8:
        raise ValueError("Compressed blob too short for backward LZ77 footer")
    size_and_hdr, top_delta = struct.unpack_from("<II", data, len(data) - 8)
    comp_bottom = len(data)
    out_len = comp_bottom + top_delta
    hdr_size = (size_and_hdr >> 24) & 0xFF
    comp_len = size_and_hdr & 0x00FFFFFF
    comp_start = comp_bottom - comp_len

    out = bytearray(out_len)
    out[:comp_start] = data[:comp_start]

    src = comp_bottom - hdr_size
    dst = out_len
    while src > comp_start:
        src -= 1
        flags = data[src]
        for _ in range(8):
            if src <= comp_start:
                break
            if (flags & 0x80) == 0:
                src -= 1
                dst -= 1
                out[dst] = data[src]
            else:
                b1 = data[src - 1]
                b2 = data[src - 2]
                src -= 2
                length = (b1 >> 4) + 3
                disp = (((b1 & 0x0F) << 8) | b2) + 2
                for _k in range(length):
                    b = out[dst + disp]
                    dst -= 1
                    out[dst] = b
            flags = (flags << 1) & 0xFF
    return bytes(out)


def uncompress_backward_inplace(data: bytes | bytearray) -> bytes:
    """Exact in-place simulation of ARM9 `func_02000934` (`MIi_UncompressBackward`) in shared RAM.

    Verifies that the write cursor `r2` never crosses below the read cursor `r3` (`r2 >= r3`) at any
    instruction during backward decompression.
    """
    if len(data) < 8:
        raise ValueError("Compressed blob too short for backward LZ77 footer")
    size_and_hdr, top_delta = struct.unpack_from("<II", data, len(data) - 8)
    r0 = len(data)
    r2 = r0 + top_delta
    mem = bytearray(r2)
    mem[:r0] = data
    r3 = r0 - ((size_and_hdr >> 24) & 0xFF)
    r1 = r0 - (size_and_hdr & 0x00FFFFFF)
    if r2 < r3:
        raise AssertionError(f"Initial in-place backward LZ77 overlap: r2=0x{r2:X} < r3=0x{r3:X}")
    while r3 > r1:
        r3 -= 1
        r5 = mem[r3]
        for _ in range(8):
            if (r5 & 0x80) == 0:
                r3 -= 1
                b = mem[r3]
                r2 -= 1
                if r2 < r3:
                    raise AssertionError(f"In-place backward LZ77 overlap on literal: r2=0x{r2:X} < r3=0x{r3:X}")
                mem[r2] = b
            else:
                r3 -= 1
                ip = mem[r3]
                r3 -= 1
                r7 = mem[r3]
                disp = (((ip & 0x0F) << 8) | r7) + 2
                cnt = (ip >> 4) + 3
                for _k in range(cnt):
                    b = mem[r2 + disp]
                    r2 -= 1
                    if r2 < r3:
                        raise AssertionError(f"In-place backward LZ77 overlap on match: r2=0x{r2:X} < r3=0x{r3:X}")
                    mem[r2] = b
            r5 = (r5 << 1) & 0xFF
            if r3 <= r1:
                break
    return bytes(mem)


def compress_backward(raw: bytes | bytearray, uncomp_prefix_len: int = 0x4000) -> bytes:
    """Compress `raw` in NitroSDK `compstatic` backward-LZ77 format (`MIi_UncompressBackward`).

    Selects the exact stop boundary `actual_prefix_len >= uncomp_prefix_len` maximizing cumulative
    compression savings `pos - len(tokens)` so in-place ARM9 decompression guarantees `r2 >= r3` at
    every byte, verified by `uncompress_backward_inplace`.
    """
    total_raw_len = len(raw)
    body = bytes(raw[uncomp_prefix_len:])
    n = len(body)
    if n == 0:
        raise ValueError("Nothing to compress after prefix")

    # Reverse body so backward compression from `total_raw_len` down to `uncomp_prefix_len` becomes forward LZ77
    rev = body[::-1]

    head = [-1] * 65536
    prev = [-1] * n

    def hash3(pos: int) -> int:
        return ((rev[pos] << 8) ^ (rev[pos + 1] << 4) ^ rev[pos + 2]) & 0xFFFF

    def insert_pos(pos: int) -> None:
        if pos + 3 <= n:
            h = hash3(pos)
            prev[pos] = head[h]
            head[h] = pos

    tokens = bytearray()
    pos = 0
    best_savings = -1
    best_pos = 0
    best_tok_len = 0
    best_flag_idx = 0
    best_flags = 0

    while pos < n:
        flag_idx = len(tokens)
        tokens.append(0)
        flags = 0
        for bit in range(8):
            if pos >= n:
                break
            best_len = 0
            best_disp = 0
            max_len = min(18, n - pos)
            if max_len >= 3 and pos >= 3:
                h = hash3(pos)
                cand = head[h]
                steps = 0
                min_cand = max(0, pos - 4098)
                while cand >= min_cand and steps < 96:
                    d = pos - cand
                    if d >= 3 and rev[cand + best_len] == rev[pos + best_len]:
                        ml = 0
                        while ml < max_len and rev[cand + ml] == rev[pos + ml]:
                            ml += 1
                        if ml > best_len:
                            best_len = ml
                            best_disp = d
                            if best_len == max_len:
                                break
                    cand = prev[cand]
                    steps += 1

            if best_len >= 3:
                flags |= 0x80 >> bit
                raw12 = best_disp - 3
                assert 0 <= raw12 <= 0xFFF
                b1 = ((best_len - 3) << 4) | ((raw12 >> 8) & 0x0F)
                b2 = raw12 & 0xFF
                tokens.append(b1)
                tokens.append(b2)
                for k in range(best_len):
                    insert_pos(pos + k)
                pos += best_len
            else:
                tokens.append(rev[pos])
                insert_pos(pos)
                pos += 1

            savings = pos - len(tokens)
            if savings >= best_savings:
                best_savings = savings
                best_pos = pos
                best_tok_len = len(tokens)
                best_flag_idx = flag_idx
                best_flags = flags

        tokens[flag_idx] = flags

    if best_savings <= 8:
        raise AssertionError("Compressed ARM9 did not achieve positive in-place savings!")

    del tokens[best_tok_len:]
    tokens[best_flag_idx] = best_flags
    actual_prefix_len = total_raw_len - best_pos
    assert actual_prefix_len >= uncomp_prefix_len
    prefix = bytes(raw[:actual_prefix_len])

    # Reverse `tokens` so backward decoder reads from end toward `comp_start == actual_prefix_len`
    comp_stream = bytes(tokens[::-1])

    # Align end to 4 bytes with 0xFF padding before 8-byte footer (`hdr_size` includes padding + 8-byte footer)
    total_before_pad = len(prefix) + len(comp_stream)
    pad_len = (4 - (total_before_pad % 4)) % 4
    hdr_size = pad_len + 8
    assert hdr_size <= 0xFF
    comp_bottom = total_before_pad + hdr_size
    comp_len = len(comp_stream) + hdr_size
    assert comp_len < (1 << 24)
    top_delta = total_raw_len - comp_bottom
    assert top_delta > 0, "Compressed ARM9 grew larger than uncompressed input!"

    footer = struct.pack("<II", (hdr_size << 24) | comp_len, top_delta & 0xFFFFFFFF)
    result = prefix + comp_stream + (b"\xFF" * pad_len) + footer

    # Mandatory in-place round-trip self-test (catches any r2 < r3 overlap)!
    roundtrip = uncompress_backward_inplace(result)
    if roundtrip != bytes(raw):
        raise AssertionError("compress_backward in-place round-trip mismatch!")
    return result


def _sha1_compress_block(h: list[int], block64: bytes | bytearray, mask_ovt_mode: bool) -> None:
    b = bytearray(block64)
    if mask_ovt_mode:
        # Exact NitroSDK `DGTi_Hash2ProcessMessageBlock` (`hash2.c:12-27`) when `DGT_OverlayTableMode == 1`:
        # zeros bytes +0x18..+0x1B (word 6 = mFileId) and +0x38..+0x3B (word 14 = mFileId / bit-length hi!)
        b[0x18:0x1C] = b"\x00\x00\x00\x00"
        b[0x38:0x3C] = b"\x00\x00\x00\x00"

    w = list(struct.unpack(">16I", b))
    for i in range(16, 80):
        v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]
        w.append(((v << 1) | (v >> 31)) & 0xFFFFFFFF)

    a, bb, c, d, e = h
    for i in range(80):
        if i < 20:
            f = (bb & c) | ((~bb) & d)
            k = 0x5A827999
        elif i < 40:
            f = bb ^ c ^ d
            k = 0x6ED9EBA1
        elif i < 60:
            f = (bb & c) | (bb & d) | (c & d)
            k = 0x8F1BBCDC
        else:
            f = bb ^ c ^ d
            k = 0xCA62C1D6
        temp = (((a << 5) | (a >> 27)) + f + e + k + w[i]) & 0xFFFFFFFF
        e = d
        d = c
        c = ((bb << 30) | (bb >> 2)) & 0xFFFFFFFF
        bb = a
        a = temp

    h[0] = (h[0] + a) & 0xFFFFFFFF
    h[1] = (h[1] + bb) & 0xFFFFFFFF
    h[2] = (h[2] + c) & 0xFFFFFFFF
    h[3] = (h[3] + d) & 0xFFFFFFFF
    h[4] = (h[4] + e) & 0xFFFFFFFF


def _nitrosdk_sha1(data: bytes, mask_ovt_mode: bool) -> bytes:
    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]
    msg = bytearray(data)
    bit_len = len(msg) * 8
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0x00)
    msg.extend(struct.pack(">Q", bit_len))
    for off in range(0, len(msg), 64):
        _sha1_compress_block(h, msg[off : off + 64], mask_ovt_mode)
    return struct.pack(">5I", *h)


def nitrosdk_hmac_sha1(data: bytes, key64: bytes, *, ovt_mode: bool = False) -> bytes:
    """Exact NitroSDK `DGT_Hash2CalcHmac` (`hash2.c`), including `DGT_SetOverlayTableMode(1)` quirk."""
    assert len(key64) == 64
    ipad = bytes(b ^ 0x36 for b in key64)
    opad = bytes(b ^ 0x5C for b in key64)
    inner = _nitrosdk_sha1(ipad + data, mask_ovt_mode=ovt_mode)
    return _nitrosdk_sha1(opad + inner, mask_ovt_mode=ovt_mode)
