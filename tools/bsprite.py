#!/usr/bin/env python3
"""Gameloft sprite sheets (.bsprite, CSprite::LoadSpriteData).

Little-endian. u16 magic 0xA9D2, u16 flags, u16 images, u16 modules, u16 frame modules,
u16 frames, u16 anim frames, u16 anims, u16 extra; the images' name lengths (u8 each), then their names; then
columns per module (u8 image, u8 flags, u32 ?, s16 x, s16 y, u16 w, u16 h), per frame
module (u16 module, u8 flags, s16 x, s16 y), per frame (u8 count, u16 first, u8 ?),
per anim frame (u16 frame, u16 time, s16 x, s16 y), per anim (u16, u8, u8, u16, u16),
extra (u8, u16).
"""
import struct
import sys
from dataclasses import dataclass, field


@dataclass
class Sprite:
    images: list
    modules: list = field(default_factory=list)    # (image, flags, x, y, w, h)
    fmodules: list = field(default_factory=list)   # (module, flags, ox, oy)
    frames: list = field(default_factory=list)     # (first fmodule, count)


def load(data):
    p = 0

    def take(fmt, n):
        nonlocal p
        size = struct.calcsize("<" + fmt)
        vals = struct.unpack_from(f"<{n}{fmt}", data, p)
        p += size * n
        return list(vals)

    magic, flags, ni, nm, nf, nfr, na, nan, nx = take("H", 9)
    if magic != 0xA9D2:
        raise ValueError("not a bsprite")
    lengths = take("B", ni)   # all name lengths first, then the names
    images = []
    for n in lengths:
        images.append(data[p:p + n].decode("latin-1"))
        p += n
    s = Sprite(images)
    img, mfl = take("B", nm), take("B", nm)
    take("I", nm)
    mx, my, mw, mh = take("h", nm), take("h", nm), take("H", nm), take("H", nm)
    s.modules = list(zip(img, mfl, mx, my, mw, mh))
    fm, ff, fx, fy = take("H", nf), take("B", nf), take("h", nf), take("h", nf)
    s.fmodules = list(zip(fm, ff, fx, fy))
    cnt, first = take("B", nfr), take("H", nfr)
    take("B", nfr)
    s.frames = list(zip(first, cnt))
    return s, p, len(data)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        s, used, size = load(open(path, "rb").read())
        print(path, s.images, f"{len(s.modules)} modules, {len(s.fmodules)} fmodules, {len(s.frames)} frames; read {used}/{size}")
        for i, m in enumerate(s.modules): print("  module", i, m)
        for i, f in enumerate(s.frames): print("  frame", i, f, [s.fmodules[k] for k in range(f[0], f[0] + f[1])])
