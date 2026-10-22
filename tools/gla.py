#!/usr/bin/env python3
"""List, test and extract N.O.V.A. 3 .gla archives.

The game uses two archive formats under the .gla extension.

Scrambled ZIP (CScrambledZipReader) - most archives:

* A ZIP archive whose headers are scrambled. Each region (a local file header,
  the central directory, or an end record with no central directory before it)
  starts with a replacement signature instead of the ZIP one: b"SB" + u16 for
  local headers, b"SDIR" for the central directory, b"SEND" for a bare end record.
* The bytes after that signature are XORed with the archive's file name
  (e.g. b"lut3.gla"), the key index restarting at 0 right after the signature.
  The central directory and the end record that follows it form one region.
* File data is not scrambled: it is stored or raw deflate, as in a normal ZIP.

Custom pak (CCustomPakReader) - *_stream.gla, sounds_hi*.gla, sprites.gla, strings.gla:

* Big-endian header: u32 unknown, u32 data_start, u32 name_table, u32 count.
* `count` entries of four big-endian u32: offset, size, name_offset, flags.
* Name table from name_table to data_start: NUL-terminated names, each
  encrypted with DECODE_XOR32 (seed 0x3857A, restarted per name).
* File data is uncompressed. If flags & 1 it is encrypted with DECODE_XOR32
  (same seed, restarted per file).
"""

import argparse
import mmap
import os
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

EOCD_SIZE = 22
CD_ENTRY_SIZE = 46
LOCAL_HEADER_SIZE = 30

SIG_LOCAL = b"SB"
SIG_CD = b"SDIR"
SIG_EOCD = b"SEND"
ZIP_CD = b"PK\x01\x02"
ZIP_EOCD = b"PK\x05\x06"

METHOD_STORED = 0
METHOD_DEFLATE = 8

PAK_HEADER_SIZE = 16
PAK_ENTRY_SIZE = 16
PAK_FLAG_ENCRYPTED = 1
PAK_SEED = 0x3857A

LCG_A = 0x19660D
LCG_C = 0x3C6EF35F
U32 = 0xFFFFFFFF


class GlaError(Exception):
    pass


@dataclass
class Entry:
    name: str
    size: int
    stored_size: int
    method: str        # "store", "defl", "pak", "pak+enc"
    offset: int        # ZIP: local header offset; pak: data offset
    crc: int = None    # ZIP only


def xor(data, key, phase=0):
    """XOR `data` with the repeating `key`, starting at key index `phase`."""
    n = len(key)
    rep = (key[phase % n:] + key[:phase % n]) * (len(data) // n + 1)
    return bytes(a ^ b for a, b in zip(data, rep))


def decode_xor32(buf, seed=PAK_SEED):
    """Port of the game's DECODE_XOR32: an LCG-keyed rotate+XOR over 32-bit words.

    The 1-3 trailing bytes are decoded with getRand(n), which continues the same
    LCG and returns (state >> 16) % n.
    """
    out = bytearray(len(buf))
    s = seed
    n4 = len(buf) // 4 * 4
    for i in range(0, n4, 4):
        s1 = (s * LCG_A + LCG_C) & U32
        s2 = (s1 * LCG_A + LCG_C) & U32
        rot = (s2 & 0x1FFFFF) >> 16
        if not s1 & 0x10000:
            rot = 32 - rot
        rot &= 31
        s3 = (s2 * LCG_A + LCG_C) & U32
        s4 = (s3 * LCG_A + LCG_C) & U32
        s5 = (s4 * LCG_A + LCG_C) & U32
        s = (s5 * LCG_A + LCG_C) & U32
        w = int.from_bytes(buf[i:i + 4], "little")
        if rot:
            w = ((w >> rot) | (w << (32 - rot))) & U32
        k = (((s3 >> 16) & 0xFF) << 24) | (s4 & 0xFF0000) | (((s5 >> 16) & 0xFF) << 8) | ((s >> 16) & 0xFF)
        out[i:i + 4] = (w ^ k).to_bytes(4, "little")

    rest = len(buf) - n4
    if rest:
        def rand(n):
            nonlocal s
            s = (s * LCG_A + LCG_C) & U32
            return (s >> 16) % n

        bits = 8 * rest
        mask = (1 << bits) - 1
        left = rand(2) == 0
        n = rand(bits)
        v = int.from_bytes(buf[n4:], "little")
        if left:
            v = ((v >> (bits - n)) | (v << n)) & mask
        else:
            v = ((v << (bits - n)) | (v >> n)) & mask
        k = 0
        for shift in range(bits - 8, -1, -8):  # key bytes are drawn high to low
            k |= rand(256) << shift
        out[n4:] = (v ^ k).to_bytes(rest, "little")
    return bytes(out)


class _Archive:
    def __init__(self, path, data):
        self.path = Path(path)
        self._data = data
        self.entries = []

    def read(self, entry, verify=True):
        raise NotImplementedError


class ScrambledZip(_Archive):
    def __init__(self, path, data, key=None):
        super().__init__(path, data)
        self.key = (key or self.path.name).encode()
        self.entries = self._read_central_directory()

    def _find_eocd(self):
        """Return (offset, decoded end record)."""
        d = self._data
        if len(d) < EOCD_SIZE:
            raise GlaError("file too small for an end record")
        # Search backwards to allow for an archive comment (max 64 KiB).
        lo = max(0, len(d) - EOCD_SIZE - 0xFFFF)
        for off in range(len(d) - EOCD_SIZE, lo - 1, -1):
            raw = d[off:off + EOCD_SIZE]
            if raw[:4] == SIG_EOCD:
                return off, ZIP_EOCD + xor(raw[4:], self.key)
            # Otherwise the end record continues the central directory's XOR
            # stream, whose phase we don't know yet: try them all.
            for phase in range(len(self.key)):
                dec = xor(raw, self.key, phase)
                if dec[:4] == ZIP_EOCD:
                    comment_len = struct.unpack_from("<H", dec, 20)[0]
                    if off + EOCD_SIZE + comment_len == len(d):
                        return off, dec
        raise GlaError("end record not found (wrong key?)")

    def _read_central_directory(self):
        eocd_off, eocd = self._find_eocd()
        count, cd_size, cd_off = struct.unpack_from("<HII", eocd, 10)
        if count == 0:
            return []
        if cd_off + cd_size > eocd_off:
            raise GlaError("central directory overlaps end record")
        raw = self._data[cd_off:cd_off + cd_size]
        if raw[:4] != SIG_CD:
            raise GlaError(f"bad central directory signature {raw[:4]!r}")
        cd = ZIP_CD + xor(raw[4:], self.key)

        entries = []
        pos = 0
        for i in range(count):
            if cd[pos:pos + 4] != ZIP_CD:
                raise GlaError(f"bad central directory entry {i} at +{pos}")
            (method,) = struct.unpack_from("<H", cd, pos + 10)
            crc, csize, usize, name_len, extra_len, comment_len = struct.unpack_from("<IIIHHH", cd, pos + 16)
            (local_off,) = struct.unpack_from("<I", cd, pos + 42)
            name = cd[pos + CD_ENTRY_SIZE:pos + CD_ENTRY_SIZE + name_len].decode("utf-8", "replace")
            method_name = {METHOD_STORED: "store", METHOD_DEFLATE: "defl"}.get(method, str(method))
            entries.append(Entry(name, usize, csize, method_name, local_off, crc))
            pos += CD_ENTRY_SIZE + name_len + extra_len + comment_len
        return entries

    def _data_offset(self, entry):
        off = entry.offset
        raw = self._data[off:off + LOCAL_HEADER_SIZE]
        if raw[:2] != SIG_LOCAL:
            raise GlaError(f"{entry.name}: bad local header signature {raw[:2]!r}")
        hdr = xor(raw[4:], self.key)
        name_len, extra_len = struct.unpack_from("<HH", hdr, 22)
        return off + LOCAL_HEADER_SIZE + name_len + extra_len

    def read(self, entry, verify=True):
        start = self._data_offset(entry)
        blob = self._data[start:start + entry.stored_size]
        if entry.method == "store":
            out = bytes(blob)
        elif entry.method == "defl":
            out = zlib.decompress(blob, -15)
        else:
            raise GlaError(f"{entry.name}: unsupported compression method {entry.method}")
        if verify:
            if len(out) != entry.size:
                raise GlaError(f"{entry.name}: size {len(out)} != {entry.size}")
            if zlib.crc32(out) != entry.crc:
                raise GlaError(f"{entry.name}: CRC mismatch")
        return out


class CustomPak(_Archive):
    def __init__(self, path, data):
        super().__init__(path, data)
        d = self._data
        _, data_start, name_table, count = struct.unpack_from(">4I", d, 0)
        names = d[name_table:data_start]
        raw = [struct.unpack_from(">4I", d, PAK_HEADER_SIZE + PAK_ENTRY_SIZE * i) for i in range(count)]
        for i, (offset, size, name_off, flags) in enumerate(raw):
            # Each name runs up to the NUL before the next name (or the table end).
            end = raw[i + 1][2] - 1 if i + 1 < count else len(names) - 1
            name = decode_xor32(names[name_off:end]).decode("utf-8", "replace")
            if offset + size > len(d):
                raise GlaError(f"{name}: data runs past end of file")
            method = "pak+enc" if flags & PAK_FLAG_ENCRYPTED else "pak"
            self.entries.append(Entry(name, size, size, method, offset))

    @staticmethod
    def looks_like(data):
        if len(data) < PAK_HEADER_SIZE:
            return False
        _, data_start, name_table, count = struct.unpack_from(">4I", data, 0)
        return (count > 0 and name_table == PAK_HEADER_SIZE + PAK_ENTRY_SIZE * count
                and name_table <= data_start <= len(data))

    def read(self, entry, verify=True):
        blob = self._data[entry.offset:entry.offset + entry.size]
        return decode_xor32(blob) if entry.method == "pak+enc" else bytes(blob)


class GlaArchive:
    """Open either archive format; use as a context manager."""

    def __init__(self, path, key=None):
        self._file = open(path, "rb")
        size = os.fstat(self._file.fileno()).st_size
        self._map = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ) if size else b""
        try:
            if self._map[:2] == SIG_LOCAL or self._map[:4] == SIG_EOCD:
                self._impl = ScrambledZip(path, self._map, key)
            elif CustomPak.looks_like(self._map):
                self._impl = CustomPak(path, self._map)
            else:
                raise GlaError("unrecognized archive format")
        except Exception:
            self.close()
            raise
        self.format = "zip" if isinstance(self._impl, ScrambledZip) else "pak"
        self.entries = self._impl.entries

    def read(self, entry, verify=True):
        return self._impl.read(entry, verify)

    def close(self):
        if isinstance(self._map, mmap.mmap):
            self._map.close()
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def safe_relpath(name):
    """Map an archive member name to a relative path that stays inside the output dir."""
    parts = [p for p in PurePosixPath(name.replace("\\", "/")).parts if p not in ("", ".", "/")]
    if not parts or any(p == ".." for p in parts):
        raise GlaError(f"refusing unsafe member name {name!r}")
    return Path(*parts)


def cmd_list(args):
    for path in args.archives:
        with GlaArchive(path, args.key) as gla:
            print(f"{path}: {gla.format}, {len(gla.entries)} files")
            for e in gla.entries:
                print(f"  {e.size:>12} {e.stored_size:>12} {e.method:>7}  {e.name}")
    return 0


def cmd_test(args):
    failed = 0
    for path in args.archives:
        try:
            with GlaArchive(path, args.key) as gla:
                bad = 0
                for e in gla.entries:
                    try:
                        gla.read(e)
                    except (GlaError, zlib.error) as ex:
                        print(f"  {path}: {ex}")
                        bad += 1
                status = "OK" if not bad else f"{bad} BAD"
                print(f"{status:>7}  {gla.format}  {len(gla.entries):>6} files  {path}")
                failed += bool(bad)
        except GlaError as ex:
            print(f"  ERROR  {path}: {ex}")
            failed += 1
    return 1 if failed else 0


def cmd_extract(args):
    out_root = Path(args.output)
    for path in args.archives:
        with GlaArchive(path, args.key) as gla:
            dest = out_root / Path(path).stem if len(args.archives) > 1 or args.per_archive else out_root
            for e in gla.entries:
                target = dest / safe_relpath(e.name)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(gla.read(e))
            print(f"{len(gla.entries):>6} files  {path} -> {dest}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", help="XOR key for scrambled ZIPs (default: the archive's file name)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list archive contents")
    p.add_argument("archives", nargs="+")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("test", help="read every file (and check CRCs where the format has them)")
    p.add_argument("archives", nargs="+")
    p.set_defaults(func=cmd_test)

    p = sub.add_parser("extract", help="extract archives")
    p.add_argument("archives", nargs="+")
    p.add_argument("-o", "--output", required=True, help="output directory")
    p.add_argument("--per-archive", action="store_true",
                   help="put files in <output>/<archive name>/ even for a single archive")
    p.set_defaults(func=cmd_extract)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except GlaError as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
