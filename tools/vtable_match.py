#!/usr/bin/env python3
"""Dump the C++ vtables of the Android libNOVA3_neon.so (named by its _ZTV symbols) and of
the stripped iOS Mach-O executable (found by shape) for tools/match_symbols.py.

Android: each "_ZTV<class>" symbol's size gives its slots after the offset-to-top and
typeinfo words. iOS has no RTTI for the game's classes, so its vtables are found as runs
of pointers to function entries (ExportFeatures' list) after two zero words.

Output JSON: {"android": [[name, [slot entries...]], ...], "ios": [[address, [...]], ...]};
entries are Ghidra addresses (8 hex digits; the Android image sits at +0x10000 there),
null for a slot without a function (pure virtuals, imports).

Usage: vtable_match.py libNOVA3_neon.so nova3_ios ios_features.jsonl out.json
"""

import json
import struct
import subprocess
import sys

GHIDRA_ANDROID_BASE = 0x10000


def elf_loads(path):
    out = subprocess.run(["readelf", "-lW", path], capture_output=True, text=True).stdout
    return [(int(p[2], 16), int(p[1], 16), int(p[4], 16)) for p in (l.split() for l in out.splitlines()) if p and p[0] == "LOAD"]


def macho_segments(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    p, segs = 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, p)
        if cmd == 1:
            name = data[p + 8:p + 24].split(b"\0")[0].decode()
            vm, vs, fo, fs = struct.unpack_from("<IIII", data, p + 24)
            segs.append((name, vm, fo, fs))
        p += size
    return segs


def main():
    apath, ipath, feats, out = sys.argv[1:5]
    adata, idata = open(apath, "rb").read(), open(ipath, "rb").read()
    loads = elf_loads(apath)

    def aword(va):
        for v, o, n in loads:
            if v <= va < v + n and va - v + 4 <= n:
                return struct.unpack_from("<I", adata, va - v + o)[0]
        return None

    android = []
    syms = subprocess.run(["readelf", "-Ws", apath], capture_output=True, text=True).stdout
    for line in syms.splitlines():
        p = line.split()
        if len(p) < 8 or not p[7].startswith("_ZTV"):
            continue
        vt, size = int(p[1], 16), int(p[2])
        slots = []
        for k in range((size - 8) // 4):
            w = aword(vt + 8 + 4 * k)
            slots.append(f"{(w & ~1) + GHIDRA_ANDROID_BASE:08x}" if w else None)
        if slots:
            android.append([p[7], slots])

    entries = set()
    for line in open(feats, encoding="utf-8"):
        e = int(json.loads(line)["entry"], 16)
        entries.add(e), entries.add(e | 1)
    ios = []
    segs = macho_segments(idata)
    text = next(((vm, vm + fs) for name, vm, fo, fs in segs if name == "__TEXT"), (0, 0))

    def code(w):   # a pointer into the code (Thumb functions carry bit 0)
        return w in entries or (w & 1 and text[0] + 0x1000 <= (w & ~1) < text[1]) or (w & ~1) in entries

    for name, vm, fo, fs in segs:
        if name != "__DATA":
            continue
        words = struct.unpack_from(f"<{fs // 4}I", idata, fo)
        k = 2
        while k < len(words):
            if words[k - 2] == 0 and words[k - 1] == 0 and code(words[k]):
                start, slots = k, []
                while k < len(words) and (code(words[k]) or words[k] == 0):
                    if words[k] == 0 and (k + 1 >= len(words) or words[k + 1] == 0):
                        break   # the next vtable's header
                    slots.append(f"{words[k] & ~1:08x}" if words[k] else None)
                    k += 1
                ios.append([f"{vm + 4 * start:08x}", slots])
            else:
                k += 1
    json.dump({"android": android, "ios": ios}, open(out, "w"))
    print(f"{len(android)} Android vtables, {len(ios)} iOS ones", file=sys.stderr)


if __name__ == "__main__":
    main()
