#!/usr/bin/env python3
"""Decode N.O.V.A. 3 textures (PVR v2 containers: PVRTC, ETC1, RGBA8888, RGB565, I8) to PNG.

Texture files:
* .pvr / .pvr2 - a PVR v2 file (.pvr2 is the half-resolution variant).
* .etc         - either a PVR v2 file, or a ZIP holding "rgb.pvr" + "alpha.pvr"
                 (ETC1 has no alpha, so alpha is stored as a second grey texture).
"""

import argparse
import io
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

PVR2_HEADER = struct.Struct("<11I4sI")  # size, h, w, mips, flags, data size, bpp, r/g/b/a masks, "PVR!", surfaces
PVR2_MAGIC = b"PVR!"

FMT_RGBA8888 = 0x12
FMT_RGB565 = 0x13
FMT_I8 = 0x16
FMT_PVRTC2 = 0x18
FMT_PVRTC4 = 0x19
FMT_ETC1 = 0x36

FLAG_TWIDDLED = 0x200
FLAG_FLIPPED = 0x10000


class TextureError(Exception):
    pass


# --------------------------------------------------------------------------- ETC1

_ETC1_TABLES = np.array([[2, 8], [5, 17], [9, 29], [13, 42], [18, 60], [24, 80], [33, 106], [47, 183]], np.int32)


def decode_etc1(data, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blocks = np.frombuffer(data, ">u8", bw * bh).reshape(bh, bw)
    hi = (blocks >> np.uint64(32)).astype(np.uint32).astype(np.int32)
    lo = (blocks & np.uint64(0xFFFFFFFF)).astype(np.uint32)

    diff = (hi >> 1) & 1
    flip = hi & 1
    cw = np.stack([(hi >> 5) & 7, (hi >> 2) & 7], -1)  # table codeword per subblock

    def bits(shift, n):
        return (hi >> shift) & ((1 << n) - 1)

    # Base colors for both subblocks, shape (bh, bw, 2, 3).
    base = np.empty(hi.shape + (2, 3), np.int32)
    for c, shift in enumerate((27, 19, 11)):  # R, G, B in differential layout
        c5 = bits(shift, 5)
        d = bits(shift - 3, 3)
        d = np.where(d >= 4, d - 8, d)
        c5b = c5 + d
        diff_c1 = (c5 << 3) | (c5 >> 2)
        diff_c2 = ((c5b & 31) << 3) | ((c5b & 31) >> 2)
        ind_c1 = bits(shift + 1, 4) * 17
        ind_c2 = bits(shift - 3, 4) * 17
        base[..., 0, c] = np.where(diff == 1, diff_c1, ind_c1)
        base[..., 1, c] = np.where(diff == 1, diff_c2, ind_c2)

    # Per-pixel (x, y) within the block; pixel bit index is x*4 + y.
    ys, xs = np.mgrid[0:4, 0:4]
    bit = (xs * 4 + ys).astype(np.uint32)
    lsb = (lo[..., None, None] >> bit) & 1
    msb = (lo[..., None, None] >> (bit + 16)) & 1
    idx = (msb << 1 | lsb).astype(np.int32)  # 0:+a 1:+b 2:-a 3:-b

    sub = np.where(flip[..., None, None] == 1, ys >= 2, xs >= 2).astype(np.int32)  # (bh, bw, 4, 4)
    table = np.take_along_axis(cw, sub.reshape(bh, bw, 16), -1).reshape(bh, bw, 4, 4)
    mag = _ETC1_TABLES[table, idx & 1]
    delta = np.where(idx >= 2, -mag, mag)

    base_px = np.where(sub[..., None] == 1, base[:, :, None, None, 1, :], base[:, :, None, None, 0, :])
    rgb = np.clip(base_px + delta[..., None], 0, 255).astype(np.uint8)  # (bh, bw, 4y, 4x, 3)

    img = rgb.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)[:h, :w]
    alpha = np.full(img.shape[:2] + (1,), 255, np.uint8)
    return np.concatenate([img, alpha], -1)


# --------------------------------------------------------------------------- PVRTC

def _twiddle_indices(nx, ny):
    """Word index for each (y, x) word position (PVRTDecompress TwiddleUV)."""
    ys, xs = np.mgrid[0:ny, 0:nx].astype(np.int64)
    min_dim = min(nx, ny)
    out = np.zeros_like(xs)
    shift = 0
    bit = 1
    while bit < min_dim:
        out |= ((ys & bit) != 0).astype(np.int64) << (2 * shift)
        out |= ((xs & bit) != 0).astype(np.int64) << (2 * shift + 1)
        bit <<= 1
        shift += 1
    rest = (xs if ny < nx else ys) >> shift
    return out | (rest << (2 * shift))


def _pvrtc_colors(cd):
    """Return (colorA, colorB) as int32 arrays (..., 4) with 5-bit RGB and 4-bit alpha."""
    a = np.empty(cd.shape + (4,), np.int32)
    b = np.empty(cd.shape + (4,), np.int32)
    opaque_a = (cd & 0x8000) != 0
    a[..., 0] = np.where(opaque_a, (cd & 0x7C00) >> 10, ((cd & 0xF00) >> 7) | ((cd & 0xF00) >> 11))
    a[..., 1] = np.where(opaque_a, (cd & 0x3E0) >> 5, ((cd & 0xF0) >> 3) | ((cd & 0xF0) >> 7))
    a[..., 2] = np.where(opaque_a, (cd & 0x1E) | ((cd & 0x1E) >> 4), ((cd & 0xE) << 1) | ((cd & 0xE) >> 2))
    a[..., 3] = np.where(opaque_a, 0xF, (cd & 0x7000) >> 11)
    opaque_b = (cd & 0x80000000) != 0
    b[..., 0] = np.where(opaque_b, (cd & 0x7C000000) >> 26, ((cd & 0xF000000) >> 23) | ((cd & 0xF000000) >> 27))
    b[..., 1] = np.where(opaque_b, (cd & 0x3E00000) >> 21, ((cd & 0xF00000) >> 19) | ((cd & 0xF00000) >> 23))
    b[..., 2] = np.where(opaque_b, (cd & 0x1F0000) >> 16, ((cd & 0xF0000) >> 15) | ((cd & 0xF0000) >> 19))
    b[..., 3] = np.where(opaque_b, 0xF, (cd & 0x70000000) >> 27)
    return a, b


def decode_pvrtc(data, w, h, bpp):
    """PVRTC 2/4bpp decoder following PowerVR's reference PVRTDecompress."""
    ww = 8 if bpp == 2 else 4   # word (block) width in pixels
    wh = 4
    nx, ny = max(w // ww, 2), max(h // wh, 2)
    W, H = nx * ww, ny * wh

    words = np.frombuffer(data, "<u4", nx * ny * 2).reshape(-1, 2).astype(np.int64)
    order = _twiddle_indices(nx, ny)
    mod_bits = words[order, 0]   # (ny, nx)
    color = words[order, 1]
    col_a, col_b = _pvrtc_colors(color)

    py, px = np.mgrid[0:H, 0:W]
    # Colors: bilinear between the 4 words whose centres surround the pixel.
    sx, sy = px - ww // 2, py - wh // 2
    x0, y0 = np.floor_divide(sx, ww), np.floor_divide(sy, wh)
    fx, fy = sx - x0 * ww, sy - y0 * wh
    x0 %= nx
    y0 %= ny
    x1, y1 = (x0 + 1) % nx, (y0 + 1) % ny
    wP = ((wh - fy) * (ww - fx))[..., None]
    wQ = ((wh - fy) * fx)[..., None]
    wR = (fy * (ww - fx))[..., None]
    wS = (fy * fx)[..., None]

    def interp(c):
        r = c[y0, x0] * wP + c[y0, x1] * wQ + c[y1, x0] * wR + c[y1, x1] * wS
        out = np.empty_like(r)
        if bpp == 2:
            out[..., :3] = (r[..., :3] >> 7) + (r[..., :3] >> 2)
            out[..., 3] = (r[..., 3] >> 5) + (r[..., 3] >> 1)
        else:
            out[..., :3] = (r[..., :3] >> 6) + (r[..., :3] >> 1)
            out[..., 3] = (r[..., 3] >> 4) + r[..., 3]
        return out

    ca, cb = interp(col_a), interp(col_b)

    # Modulation, from the word that contains the pixel.
    bx, by = px // ww, py // wh
    lx, ly = px % ww, py % wh
    mode = (color[by, bx] & 1).astype(np.int32)
    bits = mod_bits[by, bx]
    punch = np.zeros((H, W), bool)

    if bpp == 4:
        v = ((bits >> (2 * (ly * 4 + lx))) & 3).astype(np.int32)
        mod = np.where(mode == 0, np.array([0, 3, 5, 8])[v], np.array([0, 4, 4, 8])[v])
        punch = (mode == 1) & (v == 2)
    else:
        rep = np.array([0, 3, 5, 8])
        # Per-word unpacked 2-bit values ("stored" values for interpolated mode,
        # 0/3 for direct 1bpp mode) and per-word sub-mode (0 direct, 1 HV, 2 H, 3 V).
        mb = mod_bits.copy()
        word_mode = (color & 1).astype(np.int32)
        hv_only = (word_mode == 1) & ((mb & 1) != 0)
        sub_mode = np.where(word_mode == 0, 0, np.where(hv_only, np.where((mb >> 20) & 1, 3, 2), 1))
        centre = np.where((mb >> 21) & 1, mb | (1 << 20), mb & ~(1 << 20))
        mb = np.where(hv_only, centre, mb)
        mb = np.where(word_mode == 1, np.where(mb & 2, mb | 1, mb & ~1), mb)

        # Direct mode: one bit per pixel, index y*8 + x.
        direct_v = np.where((bits >> (ly * 8 + lx)) & 1, 3, 0)
        # Interpolated mode: 2 bits per stored pixel, stored pixels in raster order
        # where (x ^ y) & 1 == 0, i.e. index (y*8 + x) // 2.
        mbp = mb[by, bx]
        stored_v = (mbp >> (2 * ((ly * 8 + lx) // 2))) & 3
        value = np.where(mode == 0, direct_v, stored_v).astype(np.int32)   # (H, W)
        smode = sub_mode[by, bx]

        def nb(dx, dy):
            return rep[value[(py + dy) % H, (px + dx) % W]]

        stored = ((lx ^ ly) & 1) == 0
        hv = (nb(0, -1) + nb(0, 1) + nb(-1, 0) + nb(1, 0) + 2) // 4
        ho = (nb(-1, 0) + nb(1, 0) + 1) // 2
        vo = (nb(0, -1) + nb(0, 1) + 1) // 2
        interp_mod = np.select([stored, smode == 1, smode == 2], [rep[value], hv, ho], vo)
        mod = np.where(smode == 0, rep[value], interp_mod)

    out = (ca * (8 - mod[..., None]) + cb * mod[..., None]) // 8
    out[..., 3] = np.where(punch, 0, out[..., 3])
    return np.clip(out, 0, 255).astype(np.uint8)[:h, :w]


# --------------------------------------------------------------------------- uncompressed

def decode_rgba8888(data, w, h):
    return np.frombuffer(data, np.uint8, w * h * 4).reshape(h, w, 4).copy()


def decode_rgb565(data, w, h):
    v = np.frombuffer(data, "<u2", w * h).reshape(h, w).astype(np.uint32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    out = np.empty((h, w, 4), np.uint8)
    out[..., 0] = (r << 3) | (r >> 2)
    out[..., 1] = (g << 2) | (g >> 4)
    out[..., 2] = (b << 3) | (b >> 2)
    out[..., 3] = 255
    return out


def decode_i8(data, w, h):
    v = np.frombuffer(data, np.uint8, w * h).reshape(h, w)
    out = np.empty((h, w, 4), np.uint8)
    out[..., :3] = v[..., None]
    out[..., 3] = 255
    return out


def _untwiddle(img):
    """Undo Morton ordering of an uncompressed texture."""
    h, w = img.shape[:2]
    order = _twiddle_indices(w, h).reshape(-1)
    flat = img.reshape(h * w, -1)
    return flat[order].reshape(img.shape)


# --------------------------------------------------------------------------- containers

def decode_pvr2(buf):
    """Decode the top mip level of a PVR v2 file to an (h, w, 4) uint8 RGBA array."""
    if len(buf) < PVR2_HEADER.size:
        raise TextureError("file too small")
    hsize, h, w, mips, flags, dsize, bpp, *_masks, magic, surfaces = PVR2_HEADER.unpack_from(buf)
    if magic != PVR2_MAGIC:
        raise TextureError(f"not a PVR v2 file (magic {magic!r})")
    fmt = flags & 0xFF
    data = buf[hsize:]
    if fmt == FMT_ETC1:
        img = decode_etc1(data, w, h)
    elif fmt in (FMT_PVRTC4, FMT_PVRTC2):
        img = decode_pvrtc(data, w, h, 4 if fmt == FMT_PVRTC4 else 2)
    elif fmt == FMT_RGBA8888:
        img = decode_rgba8888(data, w, h)
    elif fmt == FMT_RGB565:
        img = decode_rgb565(data, w, h)
    elif fmt == FMT_I8:
        img = decode_i8(data, w, h)
    else:
        raise TextureError(f"unsupported PVR pixel format {fmt:#x}")
    if flags & FLAG_TWIDDLED and fmt not in (FMT_PVRTC4, FMT_PVRTC2, FMT_ETC1):
        img = _untwiddle(img)
    if flags & FLAG_FLIPPED:
        img = img[::-1].copy()
    return img


def load_texture(path_or_bytes, name="texture"):
    """Decode a .pvr/.pvr2/.etc texture to an (h, w, 4) uint8 RGBA array."""
    buf = Path(path_or_bytes).read_bytes() if isinstance(path_or_bytes, (str, Path)) else path_or_bytes
    if buf[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(buf)) as z:
            names = set(z.namelist())
            if "rgb.pvr" not in names:
                raise TextureError(f"{name}: ETC archive without rgb.pvr ({sorted(names)})")
            img = decode_pvr2(z.read("rgb.pvr"))
            if "alpha.pvr" in names:
                alpha = decode_pvr2(z.read("alpha.pvr"))
                if alpha.shape[:2] != img.shape[:2]:
                    raise TextureError(f"{name}: alpha size {alpha.shape[:2]} != rgb {img.shape[:2]}")
                img[..., 3] = alpha[..., 1]
            return img
    return decode_pvr2(buf)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="+", help=".pvr/.pvr2/.etc files")
    ap.add_argument("-o", "--output", help="output directory (default: next to each input)")
    args = ap.parse_args(argv)

    failed = 0
    for src in map(Path, args.inputs):
        dest = (Path(args.output) if args.output else src.parent) / (src.name + ".png")
        try:
            img = load_texture(src, src.name)
        except (TextureError, ValueError, zipfile.BadZipFile) as ex:
            print(f"error: {src}: {ex}", file=sys.stderr)
            failed += 1
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(img, "RGBA").save(dest)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
