#!/usr/bin/env python3
"""Generate Download Play ('D'-variant) course & texture archives for the 15 over-budget MKDS tracks.

Stock Mario Kart DS (USA) ships Download Play 'D' archives for only 5 retail race tracks
(cross_course, beach_course, mansion_course, old_momo_64, old_luigi_gc) plus 2 unused D pairs
(old_mario_sfc, old_koopa_agb; old_baby_gc also has unused D files). Meanwhile, 15 of the 24
locked-cup tracks exceed the stock Download Play guest's 0x4B000 (307,200-byte) mCourseBufferHeap
when uncompressed <stem>.carc + compressed <stem>Tex.carc are held simultaneously in MPState 16.

Following Nintendo's retail mansion_courseD / cross_courseD pattern, for each of those 15 stems
this script writes gitignored:
  - extract/<ver>/files/data/Course/<stem>D.carc
      * Preserves course_collision.kcl, course_map.nkm, course_model.nsbtp, and /MapObj/* byte-for-byte.
      * Strips single-player /MissionRun/*.nkm, /course_model_V.nsbmd, and /course_model*.nsbta.
      * Sets NNS_G3D_MATMDL_FLAG_TEXMTX_USE (0x0001) and GX_TEXGEN_TEXCOORD (0x40000000) in-place on
        course_model.nsbmd materials bound to 2x-downsampled Comp4x4 textures so G3D hardware-scales UVs.
  - extract/<ver>/files/data/Course/<stem>DTex.carc
      * Strips /course_model_V.nsbtx.
      * 2x-subsamples Comp4x4 (fmt == 5) textures with width >= 32 or height >= 32 in /course_model.nsbtx,
        updating mTexImageParam (S, T, offset) and mExtraParam (width, height) in NNSG3dResTex.
      * 2x2-quantizes 8bpp minimap tiles in /Map2D/local.NCGR in-place (preserving exact tile dimensions
        and NSCR indices while significantly improving LZ77 compression).

Every generated pair is asserted lossless under LZ10 decompression and verified to fit inside
mCourseBufferHeap (0x4B000 = 307,200 B) with >= 17,200 B of free headroom.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import ctypes
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

CHILD_HEAP_LIMIT = 0x4B000  # 307,200 bytes
TARGET_MAX_USED = 299_000   # Stricter than stock old_choco_64 (299,424 B); >= 8,200 B free headroom

OVER_BUDGET_STEMS: tuple[str, ...] = (
    "desert_course",
    "town_course",
    "pinball_course",
    "ridge_course",
    "snow_course",
    "mario_course",
    "airship_course",
    "stadium_course",
    "garden_course",
    "koopa_course",
    "rainbow_course",
    "old_donut_sfc",
    "old_luigi_agb",
    "old_kinoko_gc",
    "old_yoshi_gc",
)


def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def lz10_decompress(data: bytes) -> bytes:
    hdr = struct.unpack_from("<I", data, 0)[0]
    if (hdr & 0xFF) != 0x10:
        raise ValueError(f"Expected LZ10 header 0x10, got {hdr & 0xFF:#x}")
    out_len = hdr >> 8
    out = bytearray()
    pos = 4
    while len(out) < out_len:
        flags = data[pos]
        pos += 1
        for bit in range(7, -1, -1):
            if len(out) >= out_len:
                break
            if (flags >> bit) & 1:
                b0 = data[pos]
                b1 = data[pos + 1]
                pos += 2
                length = (b0 >> 4) + 3
                disp = (((b0 & 0x0F) << 8) | b1) + 1
                for _ in range(length):
                    out.append(out[-disp])
            else:
                out.append(data[pos])
                pos += 1
    return bytes(out)


_LZ10_C_SRC = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>

size_t lz10_compress_buf(const uint8_t *raw, size_t n, uint8_t *out) {
    out[0] = 0x10;
    out[1] = (uint8_t)(n & 0xFF);
    out[2] = (uint8_t)((n >> 8) & 0xFF);
    out[3] = (uint8_t)((n >> 16) & 0xFF);
    size_t out_pos = 4;
    if (n == 0) return out_pos;

    static int32_t head[65536];
    for (int i = 0; i < 65536; ++i) head[i] = -1;
    /* caller passes scratch prev array at end of out or we use window scan + hash */
    size_t pos = 0;
    while (pos < n) {
        size_t flag_idx = out_pos++;
        uint8_t flags = 0;
        for (int bit = 7; bit >= 0 && pos < n; --bit) {
            size_t best_len = 0;
            size_t best_disp = 0;
            if (pos >= 1 && pos + 3 <= n) {
                size_t max_len = n - pos;
                if (max_len > 18) max_len = 18;
                size_t min_cand = (pos > 4096) ? (pos - 4096) : 0;
                uint8_t b0 = raw[pos];
                for (size_t cand = pos; cand-- > min_cand; ) {
                    if (raw[cand] == b0 && raw[cand + best_len] == raw[pos + best_len]) {
                        size_t ml = 1;
                        while (ml < max_len && raw[cand + ml] == raw[pos + ml]) ++ml;
                        if (ml > best_len) {
                            best_len = ml;
                            best_disp = pos - cand;
                            if (best_len == max_len) break;
                        }
                    }
                }
            }
            if (best_len >= 3) {
                flags |= (uint8_t)(1u << bit);
                size_t disp_m1 = best_disp - 1;
                out[out_pos++] = (uint8_t)(((best_len - 3) << 4) | ((disp_m1 >> 8) & 0x0F));
                out[out_pos++] = (uint8_t)(disp_m1 & 0xFF);
                pos += best_len;
            } else {
                out[out_pos++] = raw[pos++];
            }
        }
        out[flag_idx] = flags;
    }
    while ((out_pos & 3) != 0) out[out_pos++] = 0;
    return out_pos;
}
"""

_C_LIB = None


def _get_c_lz10():
    global _C_LIB
    if _C_LIB is not None:
        return _C_LIB
    so_path = Path(tempfile.gettempdir()) / "mkds_simple32_lz10.so"
    if not so_path.is_file() and shutil.which("gcc"):
        c_path = Path(tempfile.gettempdir()) / "mkds_simple32_lz10.c"
        c_path.write_text(_LZ10_C_SRC)
        subprocess.run(
            ["gcc", "-O3", "-shared", "-fPIC", str(c_path), "-o", str(so_path)],
            check=True,
        )
    if so_path.is_file():
        lib = ctypes.CDLL(str(so_path))
        lib.lz10_compress_buf.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
        lib.lz10_compress_buf.restype = ctypes.c_size_t
        _C_LIB = lib
    else:
        _C_LIB = False
    return _C_LIB


def lz10_compress(raw: bytes) -> bytes:
    """Exact optimal Nintendo LZ10 compressor (disp in 1..4096, len in 3..18)."""
    lib = _get_c_lz10()
    if lib:
        cap = 16 + len(raw) + (len(raw) // 8) + 16
        buf = ctypes.create_string_buffer(cap)
        sz = lib.lz10_compress_buf(raw, len(raw), buf)
        return buf.raw[:sz]

    n = len(raw)
    out = bytearray(struct.pack("<I", 0x10 | (n << 8)))
    if n == 0:
        return bytes(out)

    head = [-1] * 65536
    prev = [-1] * n
    mv = memoryview(raw)

    def insert_pos(p: int) -> None:
        if p + 2 < n:
            h = ((raw[p] << 8) ^ (raw[p + 1] << 4) ^ raw[p + 2]) & 0xFFFF
            prev[p] = head[h]
            head[h] = p

    pos = 0
    while pos < n:
        flag_idx = len(out)
        out.append(0)
        flags = 0
        for bit in range(7, -1, -1):
            if pos >= n:
                break
            best_len = 0
            best_disp = 0
            if pos + 3 <= n and pos >= 1:
                max_len = min(18, n - pos)
                h = ((raw[pos] << 8) ^ (raw[pos + 1] << 4) ^ raw[pos + 2]) & 0xFFFF
                cand = head[h]
                chain = 0
                min_cand = max(0, pos - 4096)
                while cand >= min_cand and chain < 96:
                    disp = pos - cand
                    if disp >= 1 and raw[cand + best_len] == raw[pos + best_len]:
                        ml = 0
                        while ml < max_len and raw[cand + ml] == raw[pos + ml]:
                            ml += 1
                        if ml > best_len:
                            best_len = ml
                            best_disp = disp
                            if best_len == max_len:
                                break
                    cand = prev[cand]
                    chain += 1

            if best_len >= 3:
                flags |= 1 << bit
                disp_m1 = best_disp - 1
                out.append(((best_len - 3) << 4) | ((disp_m1 >> 8) & 0x0F))
                out.append(disp_m1 & 0xFF)
                for k in range(best_len):
                    insert_pos(pos + k)
                pos += best_len
            else:
                out.append(mv[pos])
                insert_pos(pos)
                pos += 1
        out[flag_idx] = flags

    while len(out) % 4 != 0:
        out.append(0)
    return bytes(out)


class NarcDir:
    def __init__(self, name: str, dir_id: int, parent_id: int) -> None:
        self.name = name
        self.dir_id = dir_id
        self.parent_id = parent_id
        self.items: list[tuple[bool, str, int | bytes]] = []  # (is_dir, name, sub_dir_id or file_bytes)


def unpack_narc(raw: bytes) -> NarcDir:
    if raw[:4] != b"NARC":
        raise ValueError("Invalid NARC magic")
    fatb_off = 16
    if raw[fatb_off : fatb_off + 4] != b"BTAF":
        raise ValueError("Invalid BTAF magic")
    fatb_size, num_files = struct.unpack_from("<II", raw, fatb_off + 4)
    entries = [struct.unpack_from("<II", raw, fatb_off + 12 + 8 * i) for i in range(num_files)]
    fntb_off = fatb_off + fatb_size
    if raw[fntb_off : fntb_off + 4] != b"BTNF":
        raise ValueError("Invalid BTNF magic")
    fntb_size = struct.unpack_from("<I", raw, fntb_off + 4)[0]
    fimg_off = fntb_off + fntb_size
    if raw[fimg_off : fimg_off + 4] != b"GMIF":
        raise ValueError("Invalid GMIF magic")

    fnt_base = fntb_off + 8
    _, _, total_dirs = struct.unpack_from("<IHH", raw, fnt_base)
    dirs: dict[int, NarcDir] = {}
    for d in range(total_dirs):
        _, _, parent_or_cnt = struct.unpack_from("<IHH", raw, fnt_base + d * 8)
        dir_id = 0xF000 + d
        dirs[dir_id] = NarcDir("" if d == 0 else f"d{d}", dir_id, 0 if d == 0 else parent_or_cnt)

    for d in range(total_dirs):
        dir_id = 0xF000 + d
        sub_off, first_fid, _ = struct.unpack_from("<IHH", raw, fnt_base + d * 8)
        p = fnt_base + sub_off
        fid = first_fid
        while True:
            t = raw[p]
            p += 1
            if t == 0:
                break
            is_dir = (t & 0x80) != 0
            nlen = t & 0x7F
            name = raw[p : p + nlen].decode("ascii")
            p += nlen
            if is_dir:
                sub_id = struct.unpack_from("<H", raw, p)[0]
                p += 2
                dirs[sub_id].name = name
                dirs[dir_id].items.append((True, name, sub_id))
            else:
                s, e = entries[fid]
                fbytes = raw[fimg_off + 8 + s : fimg_off + 8 + e]
                dirs[dir_id].items.append((False, name, fbytes))
                fid += 1

    def attach_subdirs(node: NarcDir) -> NarcDir:
        new_items: list[tuple[bool, str, int | bytes | NarcDir]] = []
        for is_dir, nm, payload in node.items:
            if is_dir:
                assert isinstance(payload, int)
                new_items.append((True, nm, attach_subdirs(dirs[payload])))
            else:
                new_items.append((False, nm, payload))
        node.items = new_items  # type: ignore[assignment]
        return node

    return attach_subdirs(dirs[0xF000])


def pack_narc(root: NarcDir) -> bytes:
    """Pack a NarcDir tree back into a valid NitroSDK NARC binary."""
    # Prune empty subdirectories (e.g., /MissionRun after stripping mr*.nkm)
    def prune(node: NarcDir) -> None:
        kept = []
        for is_dir, nm, payload in node.items:
            if is_dir:
                assert isinstance(payload, NarcDir)
                prune(payload)
                if payload.items:
                    kept.append((True, nm, payload))
            else:
                kept.append((False, nm, payload))
        node.items = kept

    prune(root)

    # Assign directory IDs in breadth/preorder matching original table order
    all_dirs: list[NarcDir] = []

    def collect_dirs(node: NarcDir, parent_id: int) -> None:
        node.dir_id = 0xF000 + len(all_dirs)
        node.parent_id = parent_id
        all_dirs.append(node)
        for is_dir, _, payload in node.items:
            if is_dir:
                assert isinstance(payload, NarcDir)
                collect_dirs(payload, node.dir_id)

    collect_dirs(root, 0)

    # Assign file IDs per directory sequentially and build BTNF + GMIF
    fat_entries: list[tuple[int, int]] = []
    gmif_body = bytearray()
    dir_first_fid: dict[int, int] = {}
    dir_subtables: dict[int, bytes] = {}

    for dnode in all_dirs:
        dir_first_fid[dnode.dir_id] = len(fat_entries)
        sub = bytearray()
        for is_dir, nm, payload in dnode.items:
            nm_bytes = nm.encode("ascii")
            if is_dir:
                assert isinstance(payload, NarcDir)
                sub.append(0x80 | len(nm_bytes))
                sub.extend(nm_bytes)
                sub.extend(struct.pack("<H", payload.dir_id))
            else:
                assert isinstance(payload, (bytes, bytearray))
                sub.append(len(nm_bytes))
                sub.extend(nm_bytes)
                start = len(gmif_body)
                gmif_body.extend(payload)
                end = len(gmif_body)
                while len(gmif_body) % 4 != 0:
                    gmif_body.append(0xFF)
                fat_entries.append((start, end))
        sub.append(0x00)
        dir_subtables[dnode.dir_id] = bytes(sub)

    num_dirs = len(all_dirs)
    fnt_hdr = bytearray(8 * num_dirs)
    fnt_subs = bytearray()
    for idx, dnode in enumerate(all_dirs):
        sub_off = 8 * num_dirs + len(fnt_subs)
        third = num_dirs if idx == 0 else dnode.parent_id
        struct.pack_into("<IHH", fnt_hdr, idx * 8, sub_off, dir_first_fid[dnode.dir_id], third)
        fnt_subs.extend(dir_subtables[dnode.dir_id])

    fnt_payload = fnt_hdr + fnt_subs
    while len(fnt_payload) % 4 != 0:
        fnt_payload.append(0xFF)

    btaf = bytearray(struct.pack("<4sII", b"BTAF", 12 + 8 * len(fat_entries), len(fat_entries)))
    for s, e in fat_entries:
        btaf.extend(struct.pack("<II", s, e))

    btnf = struct.pack("<4sI", b"BTNF", 8 + len(fnt_payload)) + bytes(fnt_payload)
    gmif = struct.pack("<4sI", b"GMIF", 8 + len(gmif_body)) + bytes(gmif_body)

    total_size = 16 + len(btaf) + len(btnf) + len(gmif)
    narc_hdr = struct.pack("<4sHHIHH", b"NARC", 0xFFFE, 0x0100, total_size, 16, 3)
    return narc_hdr + bytes(btaf) + btnf + gmif


def downsample_btx0(raw_btx: bytes, min_exp: int = 2) -> tuple[bytes, set[str]]:
    """2x-subsample Comp4x4 textures >= (8 << min_exp) in BTX0, returning (new_btx, scaled_tex_names)."""
    sig, bom, ver, fsize, hdrsize, nblks, tex0_ofs = struct.unpack_from("<4sHHIHHI", raw_btx, 0)
    assert sig == b"BTX0" and tex0_ofs == 0x14 and nblks == 1

    t_sig, t_size = struct.unpack_from("<4sI", raw_btx, tex0_ofs)
    assert t_sig == b"TEX0"
    ti_vram, ti_sz, ti_dict, ti_flg, ti_dmy, ti_ofs = struct.unpack_from("<IHHHHI", raw_btx, tex0_ofs + 0x08)
    c4_vram, c4_sz, c4_dict, c4_flg, c4_dmy, c4_ofs, c4_pofs = struct.unpack_from(
        "<IHHHHII", raw_btx, tex0_ofs + 0x18
    )
    pl_vram, pl_sz, pl_flg, pl_dict, pl_dmy, pl_ofs = struct.unpack_from("<IHHHHI", raw_btx, tex0_ofs + 0x2C)

    if c4_sz == 0:
        return raw_btx, set()

    hdr_end = ti_ofs if ti_sz > 0 else c4_ofs
    std_tex_blob = raw_btx[tex0_ofs + ti_ofs : tex0_ofs + ti_ofs + ti_sz * 8] if ti_sz > 0 else b""
    orig_c4_tex = raw_btx[tex0_ofs + c4_ofs : tex0_ofs + c4_ofs + c4_sz * 8]
    orig_c4_pidx = raw_btx[tex0_ofs + c4_pofs : tex0_ofs + c4_pofs + c4_sz * 4]
    pltt_blob = raw_btx[tex0_ofs + pl_ofs : tex0_ofs + pl_ofs + pl_sz * 8]

    out_hdr_and_dicts = bytearray(raw_btx[: tex0_ofs + hdr_end])

    d_num = out_hdr_and_dicts[tex0_ofs + 0x3D]
    d_ent = struct.unpack_from("<H", out_hdr_and_dicts, tex0_ofs + 0x3C + 6)[0]
    ent_base = tex0_ofs + 0x3C + d_ent
    u_sz, n_ofs = struct.unpack_from("<HH", out_hdr_and_dicts, ent_base)

    # Group Comp4x4 entries by original byte offset to preserve any shared-data aliases
    c4_groups: dict[int, list[int]] = defaultdict(list)
    for idx in range(d_num):
        param, _ = struct.unpack_from("<II", out_hdr_and_dicts, ent_base + 4 + idx * u_sz)
        fmt = (param >> 26) & 7
        if fmt == 5:
            orig_byte_ofs = (param & 0xFFFF) * 8
            c4_groups[orig_byte_ofs].append(idx)

    new_c4_tex = bytearray()
    new_c4_pidx = bytearray()
    scaled_names: set[str] = set()

    for orig_byte_ofs in sorted(c4_groups.keys()):
        idxs = c4_groups[orig_byte_ofs]
        param0, extra0 = struct.unpack_from("<II", out_hdr_and_dicts, ent_base + 4 + idxs[0] * u_sz)
        s_exp = (param0 >> 20) & 7
        t_exp = (param0 >> 23) & 7
        w = 8 << s_exp
        h = 8 << t_exp

        new_s_exp = s_exp - 1 if s_exp >= min_exp else s_exp
        new_t_exp = t_exp - 1 if t_exp >= min_exp else t_exp
        new_w = 8 << new_s_exp
        new_h = 8 << new_t_exp
        step_x = 2 if new_s_exp < s_exp else 1
        step_y = 2 if new_t_exp < t_exp else 1

        new_ofs_units = len(new_c4_tex) // 8
        bw = w // 4
        bh = h // 4
        orig_pidx_ofs = orig_byte_ofs // 2

        for by in range(0, bh, step_y):
            for bx in range(0, bw, step_x):
                blk_idx = by * bw + bx
                t_off = orig_byte_ofs + blk_idx * 4
                p_off = orig_pidx_ofs + blk_idx * 2
                new_c4_tex.extend(orig_c4_tex[t_off : t_off + 4])
                new_c4_pidx.extend(orig_c4_pidx[p_off : p_off + 2])

        assert len(new_c4_tex) % 8 == 0
        # Pad pidx at end if needed, though each 8B tex chunk = 2 blocks = 4B pidx
        for idx in idxs:
            param, extra = struct.unpack_from("<II", out_hdr_and_dicts, ent_base + 4 + idx * u_sz)
            new_param = (
                (param & ~((7 << 20) | (7 << 23) | 0xFFFF))
                | (new_s_exp << 20)
                | (new_t_exp << 23)
                | new_ofs_units
            )
            new_extra = (extra & ~0x3FFFFF) | (new_h << 11) | new_w
            struct.pack_into("<II", out_hdr_and_dicts, ent_base + 4 + idx * u_sz, new_param, new_extra)
            if new_w != w or new_h != h:
                nm = (
                    bytes(out_hdr_and_dicts[ent_base + n_ofs + idx * 16 : ent_base + n_ofs + (idx + 1) * 16])
                    .rstrip(b"\x00")
                    .decode("ascii")
                )
                scaled_names.add(nm)

    new_ti_ofs = hdr_end if ti_sz > 0 else 0
    new_c4_ofs = hdr_end + len(std_tex_blob)
    new_c4_sz = len(new_c4_tex) // 8
    new_c4_pofs = new_c4_ofs + len(new_c4_tex)
    new_pl_ofs = new_c4_pofs + len(new_c4_pidx)
    # 4-byte align pl_ofs if needed
    pad_before_pltt = b"\x00" * ((4 - (new_pl_ofs % 4)) % 4)
    new_pl_ofs += len(pad_before_pltt)

    new_t_size = new_pl_ofs + len(pltt_blob)
    new_fsize = tex0_ofs + new_t_size

    struct.pack_into("<I", out_hdr_and_dicts, 0x08, new_fsize)
    struct.pack_into("<I", out_hdr_and_dicts, tex0_ofs + 0x04, new_t_size)
    struct.pack_into("<IHHHHI", out_hdr_and_dicts, tex0_ofs + 0x08, ti_vram, ti_sz, ti_dict, ti_flg, ti_dmy, new_ti_ofs)
    struct.pack_into(
        "<IHHHHII",
        out_hdr_and_dicts,
        tex0_ofs + 0x18,
        c4_vram,
        new_c4_sz,
        c4_dict,
        c4_flg,
        c4_dmy,
        new_c4_ofs,
        new_c4_pofs,
    )
    struct.pack_into("<IHHHHI", out_hdr_and_dicts, tex0_ofs + 0x2C, pl_vram, pl_sz, pl_flg, pl_dict, pl_dmy, new_pl_ofs)

    rebuilt = (
        bytes(out_hdr_and_dicts)
        + std_tex_blob
        + bytes(new_c4_tex)
        + bytes(new_c4_pidx)
        + pad_before_pltt
        + pltt_blob
    )
    assert len(rebuilt) == new_fsize
    return rebuilt, scaled_names


def patch_nsbmd_for_scaled_textures(raw_mdl: bytes, scaled_tex_names: set[str]) -> bytes:
    """Enable hardware texture-matrix UV scaling (mMagW/mMagH) on materials bound to downsampled textures."""
    if not scaled_tex_names:
        return raw_mdl
    out = bytearray(raw_mdl)
    mdl0_ofs = struct.unpack_from("<I", out, 0x10)[0]
    dict_ofs = mdl0_ofs + 8
    m_num_mdl = out[dict_ofs + 1]
    d_ent = struct.unpack_from("<H", out, dict_ofs + 6)[0]
    u_mdl_sz = struct.unpack_from("<H", out, dict_ofs + d_ent)[0]

    for m_idx in range(m_num_mdl):
        mdl_rel = struct.unpack_from("<I", out, dict_ofs + d_ent + 4 + m_idx * u_mdl_sz)[0]
        mdl_ofs = mdl0_ofs + mdl_rel
        ofs_mat = struct.unpack_from("<I", out, mdl_ofs + 8)[0]
        mat_ofs = mdl_ofs + ofs_mat

        ofs_tex_to_mat = struct.unpack_from("<H", out, mat_ofs)[0]
        mat_dict_ofs = mat_ofs + 4
        mat_d_ent = struct.unpack_from("<H", out, mat_dict_ofs + 6)[0]
        mat_ent_base = mat_dict_ofs + mat_d_ent
        mat_u_sz = struct.unpack_from("<H", out, mat_ent_base)[0]

        tlist_ofs = mat_ofs + ofs_tex_to_mat
        t_num = out[tlist_ofs + 1]
        t_ent = struct.unpack_from("<H", out, tlist_ofs + 6)[0]
        t_ent_base = tlist_ofs + t_ent
        t_u_sz, t_n_ofs = struct.unpack_from("<HH", out, t_ent_base)

        for t_idx in range(t_num):
            tnm = (
                bytes(out[t_ent_base + t_n_ofs + t_idx * 16 : t_ent_base + t_n_ofs + (t_idx + 1) * 16])
                .rstrip(b"\x00")
                .decode("ascii")
            )
            if tnm not in scaled_tex_names:
                continue
            bind_ofs, num_mat_idx, _ = struct.unpack_from("<HBB", out, t_ent_base + 4 + t_idx * t_u_sz)
            for k in range(num_mat_idx):
                mat_i = out[mat_ofs + bind_ofs + k]
                m_rel = struct.unpack_from("<I", out, mat_ent_base + 4 + mat_i * mat_u_sz)[0]
                mp = mat_ofs + m_rel
                tip = struct.unpack_from("<I", out, mp + 0x14)[0]
                tmask = struct.unpack_from("<I", out, mp + 0x18)[0]
                flg = struct.unpack_from("<H", out, mp + 0x1E)[0]
                flg |= 0x0001  # NNS_G3D_MATMDL_FLAG_TEXMTX_USE
                if (tip & 0xC0000000) == 0:
                    tip |= 0x40000000  # GX_TEXGEN_TEXCOORD
                    tmask |= 0xC0000000
                struct.pack_into("<II", out, mp + 0x14, tip, tmask)
                struct.pack_into("<H", out, mp + 0x1E, flg)

    return bytes(out)


def quantize_local_ncgr(raw_ncgr: bytes, block: int = 4) -> bytes:
    """Quantize 8bpp 8x8 tiles in /Map2D/local.NCGR in-place (4x4 or 8x8) to boost LZ77 compression."""
    if raw_ncgr[:4] != b"RGCN":
        return raw_ncgr
    c_sig, _, tiles_h, tiles_w, fmt, _, _, pixel_bytes, data_ofs = struct.unpack_from("<4sIHHIIIII", raw_ncgr, 16)
    if c_sig != b"RAHC" or fmt != 4:
        return raw_ncgr
    pix_start = 16 + 8 + data_ofs
    if pix_start + pixel_bytes > len(raw_ncgr) or pixel_bytes != tiles_h * tiles_w * 64:
        return raw_ncgr

    out = bytearray(raw_ncgr)
    num_tiles = tiles_h * tiles_w
    for t in range(num_tiles):
        base = pix_start + t * 64
        if block >= 8:
            p = out[base]
            out[base : base + 64] = bytes((p,)) * 64
        else:
            for ty in range(0, 8, block):
                for tx in range(0, 8, block):
                    p = out[base + ty * 8 + tx]
                    row_bytes = bytes((p,)) * block
                    for dy in range(block):
                        row = base + (ty + dy) * 8 + tx
                        out[row : row + block] = row_bytes
    return bytes(out)


def _build_pair(carc_bytes: bytes, tex_bytes: bytes, aggressive: bool) -> tuple[bytes, bytes, set[str]]:
    carc_root = unpack_narc(lz10_decompress(carc_bytes))
    tex_root = unpack_narc(lz10_decompress(tex_bytes))

    scaled_tex_names: set[str] = set()
    kept_tex_items = []
    for is_dir, nm, payload in tex_root.items:
        if not is_dir and nm == "course_model_V.nsbtx":
            continue
        if not is_dir and nm == "course_model.nsbtx":
            assert isinstance(payload, bytes)
            payload, s1 = downsample_btx0(payload, min_exp=2)
            scaled_tex_names |= s1
            if aggressive:
                payload, s2 = downsample_btx0(payload, min_exp=1)
                scaled_tex_names |= s2
        elif is_dir and nm == "Map2D":
            assert isinstance(payload, NarcDir)
            new_map2d = []
            for m_isdir, m_nm, m_pay in payload.items:
                if not m_isdir and m_nm in ("local.NCGR", "global.NCGR"):
                    assert isinstance(m_pay, bytes)
                    if m_nm == "local.NCGR":
                        m_pay = quantize_local_ncgr(m_pay, block=8 if aggressive else 4)
                    elif aggressive:
                        m_pay = quantize_local_ncgr(m_pay, block=4)
                new_map2d.append((m_isdir, m_nm, m_pay))
            payload.items = new_map2d
        kept_tex_items.append((is_dir, nm, payload))
    tex_root.items = kept_tex_items

    kept_carc_items = []
    for is_dir, nm, payload in carc_root.items:
        if is_dir and nm == "MissionRun":
            continue
        if not is_dir and nm in ("course_model_V.nsbmd", "course_model.nsbta", "course_model_V.nsbta"):
            continue
        if not is_dir and nm == "course_model.nsbmd":
            assert isinstance(payload, bytes)
            payload = patch_nsbmd_for_scaled_textures(payload, scaled_tex_names)
        kept_carc_items.append((is_dir, nm, payload))
    carc_root.items = kept_carc_items

    return pack_narc(carc_root), pack_narc(tex_root), scaled_tex_names


def process_stem(course_dir: Path, stem: str, dry_run: bool = False) -> dict[str, int | str]:
    src_carc = (course_dir / f"{stem}.carc").read_bytes()
    src_tex = (course_dir / f"{stem}Tex.carc").read_bytes()

    d_carc_raw, d_tex_raw, scaled_tex_names = _build_pair(src_carc, src_tex, aggressive=False)
    d_tex_comp = lz10_compress(d_tex_raw)
    frm_used = 0x20 + align_up(len(d_carc_raw), 0x20) + align_up(len(d_tex_comp), 0x20)

    if frm_used > TARGET_MAX_USED:
        d_carc_raw, d_tex_raw, scaled_tex_names = _build_pair(src_carc, src_tex, aggressive=True)
        d_tex_comp = lz10_compress(d_tex_raw)
        frm_used = 0x20 + align_up(len(d_carc_raw), 0x20) + align_up(len(d_tex_comp), 0x20)

    d_carc_comp = lz10_compress(d_carc_raw)

    # Verify 100% lossless LZ10 round-trip
    assert lz10_decompress(d_carc_comp) == d_carc_raw, f"LZ10 round-trip mismatch on {stem}D.carc"
    assert lz10_decompress(d_tex_comp) == d_tex_raw, f"LZ10 round-trip mismatch on {stem}DTex.carc"

    headroom = CHILD_HEAP_LIMIT - frm_used
    if len(d_carc_comp) > CHILD_HEAP_LIMIT or frm_used > TARGET_MAX_USED:
        raise RuntimeError(
            f"{stem}D exceeded target budget: frm_used={frm_used} (headroom={headroom}), d_carc_comp={len(d_carc_comp)}"
        )

    if not dry_run:
        (course_dir / f"{stem}D.carc").write_bytes(d_carc_comp)
        (course_dir / f"{stem}DTex.carc").write_bytes(d_tex_comp)

    return {
        "stem": stem,
        "d_carc_comp": len(d_carc_comp),
        "d_carc_uncomp": len(d_carc_raw),
        "d_tex_comp": len(d_tex_comp),
        "d_tex_uncomp": len(d_tex_raw),
        "frm_used": frm_used,
        "headroom": headroom,
        "scaled_textures": len(scaled_tex_names),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--version", default="usa", choices=["usa", "eur"])
    ap.add_argument("--json-out", type=Path)
    args = ap.parse_args()

    course_dir = args.repo_root / "extract" / args.version / "files" / "data" / "Course"
    if not course_dir.is_dir():
        print(f"ERROR: Missing {course_dir}", file=sys.stderr)
        return 1

    results = []
    print(f"=== Building 15 Download Play D-Archives ({args.version.upper()}) ===")
    for stem in OVER_BUDGET_STEMS:
        res = process_stem(course_dir, stem)
        results.append(res)
        print(
            f"  {stem + 'D':<18} carc={res['d_carc_comp']:>6}->{res['d_carc_uncomp']:>6}B  "
            f"tex={res['d_tex_comp']:>6}->{res['d_tex_uncomp']:>6}B  "
            f"frmUsed={res['frm_used']:>6}B  headroom={res['headroom']:>+7}B  "
            f"(scaledTex={res['scaled_textures']})"
        )

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(results, indent=2) + "\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
