"""Irradiance volume files (<level>_<object id>.irr): spherical-harmonic light probe grids.

    "MIhS", i32 version (3; the game ignores other versions), i32 volume count
    per volume: 6 x f32 AABB (relative to the owning level object), f32 grid spacing,
                f32 fade distance, i32 set count,
                then per set nx*ny*nz points of 0x84 bytes, index x + nx * (y + ny * z),
                n = round((max - min) / spacing) + 1 per axis
    per point:  27 x f32 order-2 SH coefficients (R 0-8, G 9-17, B 18-26),
                3 x f32 peak direction, 3 x f32 peak intensity

From glitch::irradiance::CIrradianceManager::load / CIrradianceVolume::fromDataStream.
"""
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

POINT_SIZE = 0x84


class IrrError(Exception):
    pass


@dataclass
class Volume:
    min: tuple
    max: tuple
    spacing: float
    fade: float
    dims: tuple
    sets: list = field(default_factory=list)   # per set: list of 33-float tuples


def load(path_or_bytes, offset=(0.0, 0.0, 0.0)):
    data = Path(path_or_bytes).read_bytes() if isinstance(path_or_bytes, (str, Path)) else path_or_bytes
    magic, version, count = struct.unpack_from("<4sii", data, 0)
    if magic != b"MIhS":
        raise IrrError(f"bad magic {magic!r}")
    if version != 3:
        return []
    o, volumes = 12, []
    for _ in range(count):
        box = struct.unpack_from("<6f", data, o)
        spacing, fade, nsets = struct.unpack_from("<ffi", data, o + 24)
        o += 36
        dims = tuple(int((box[3 + k] - box[k]) / spacing + 0.5) + 1 for k in range(3))
        n = dims[0] * dims[1] * dims[2]
        v = Volume(tuple(box[k] + offset[k] for k in range(3)), tuple(box[3 + k] + offset[k] for k in range(3)),
                   spacing, fade, dims)
        for _ in range(nsets):
            if o + n * POINT_SIZE > len(data):
                raise IrrError("truncated volume")
            v.sets.append([struct.unpack_from("<33f", data, o + i * POINT_SIZE) for i in range(n)])
            o += n * POINT_SIZE
        volumes.append(v)
    if o != len(data):
        raise IrrError(f"{len(data) - o} trailing bytes")
    return volumes


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    failed = 0
    for f in args.files:
        try:
            vols = load(f)
        except (IrrError, struct.error) as ex:
            print(f"FAIL {f}: {ex}")
            failed += 1
            continue
        pts = sum(len(s) for v in vols for s in v.sets)
        print(f"OK   {Path(f).name}: {len(vols)} volumes, {pts} points")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
