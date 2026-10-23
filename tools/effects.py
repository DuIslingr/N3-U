#!/usr/bin/env python3
"""Parse N.O.V.A. 3's particle effect library (levels/effects.bin).

Layout (CEffectsManager::Deserialize / GetEffectStream):

* u32 version (0x20014), u32 ?, u32 effect count (little-endian), then per effect:
  u32 name length, name, u32 offset into the DICT stream below.
* u32, u32, then a big-endian CMemoryStream "DICT" stream holding the effects.

Each effect (CPSEffect::Deserialize): string name, int emitter count, emitters
(spark::CEmitterInstance::Deserialize, stream version 0x20014):

  s name, s effect file (.bdae; empty: a built-in material), s material/effect,
  s technique,
  i parameter count, i mask; per parameter: s name, i kind, i type, then a texture
      name (kind 2), an int (type 1), a float (type 5) or an int,
  i shape (0 point, 1 box, 2 sphere, 3 disc, 4 cylinder) + its fields,
  s model (mesh particles),
  c, f f f, i i i, i i i i i, i i i, c c c, i, f f f f f, i, i (colour), i (colour),
  i i i i, c c c i, i frame count + per frame 6 floats, c, i, s,
  i sound count + per sound (s, i, i, i, c),
  optional colour / size / direction curves (c flag, then i keys, per key i time + 4/2/3 values),
  i force-field count + per field i type (1 attraction, 2 gravity, 3 rotation, 4 spin) + fields,
  influence curves: 3 x (i keys, keys x (i, i)).

Field meanings are filled in by tools/effects.py --dump as they are understood.
"""

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lvc import Stream  # noqa: E402

SHAPES = {0: "point", 1: "box", 2: "sphere", 3: "disc", 4: "cylinder"}
SHAPE_FLOATS = {0: 3, 1: 6, 2: 5, 3: 5, 4: 6}
FORCE_FIELDS = {1: ("attraction", "f f f f c c c c f f"), 2: ("gravity", "f f f i i"),
                3: ("rotation", "f f f f f f f f f i i"), 4: ("spin", "i i i i")}


@dataclass
class Emitter:
    name: str
    effect_file: str
    material: str
    technique: str
    params: dict = field(default_factory=dict)
    shape: str = ""
    shape_data: tuple = ()
    model: str = ""
    fields: list = field(default_factory=list)     # the fixed block after the model
    frames: list = field(default_factory=list)
    sounds: list = field(default_factory=list)
    color_keys: list = field(default_factory=list)
    size_keys: list = field(default_factory=list)
    dir_keys: list = field(default_factory=list)
    force_fields: list = field(default_factory=list)
    influence: list = field(default_factory=list)


@dataclass
class Effect:
    name: str
    emitters: list


def read_spec(st, spec):
    out = []
    for t in spec.split():
        out.append({"i": st.i, "f": st.f, "c": st.c, "s": st.s}[t]())
    return out


def read_emitter(st):
    e = Emitter(st.s(), st.s(), st.s(), st.s())
    nparams, e.params["_mask"] = st.i(), st.i()
    for _ in range(nparams):
        name, kind, typ = st.s(), st.i(), st.i()
        if kind == 2:
            e.params[name] = st.s()
        elif typ == 5:
            e.params[name] = st.f()
        else:
            e.params[name] = st.i()
    shape = st.i()
    e.shape = SHAPES.get(shape, str(shape))
    e.shape_data = tuple(st.f() for _ in range(SHAPE_FLOATS.get(shape, 0)))
    e.model = st.s()
    e.fields = read_spec(st, "c f f f i i i i i i i i i i i c c c i f f f f f i i i i i i i c c c i")
    nframes = st.i()
    e.frames = [tuple(st.f() for _ in range(6)) for _ in range(nframes)]
    e.fields += read_spec(st, "c i s")
    for _ in range(st.i()):
        e.sounds.append(read_spec(st, "s i i i c"))
    if st.c() == 1:
        e.color_keys = [read_spec(st, "i f f f f") for _ in range(st.i())]
    if st.c() == 1:
        e.size_keys = [read_spec(st, "i f f") for _ in range(st.i())]
    if st.c() == 1:
        e.dir_keys = [read_spec(st, "i f f f") for _ in range(st.i())]
    for _ in range(st.i()):
        t = st.i()
        name, spec = FORCE_FIELDS[t]
        e.force_fields.append((name, read_spec(st, spec)))
    for _ in range(3):
        e.influence.append([read_spec(st, "i f") for _ in range(st.i())])
    return e


def load(path):
    data = Path(path).read_bytes()
    version, _, count = struct.unpack_from("<III", data, 0)
    p = 12
    table = []
    for _ in range(count):
        (n,) = struct.unpack_from("<I", data, p)
        name = data[p + 4:p + 4 + n].decode("latin-1")
        (off,) = struct.unpack_from("<I", data, p + 4 + n)
        table.append((name, off))
        p += 8 + n
    dict_start = data.index(b"DICT", p)
    st = Stream(data[dict_start:])
    effects = {}
    ends = [off for _, off in table[1:]] + [None]
    for (name, off), end in zip(table, ends):
        st.pos = off
        ename = st.s()
        emitters = [read_emitter(st) for _ in range(st.i())]
        effects[name] = Effect(ename, emitters)
        if end is not None and st.pos != end:
            raise ValueError(f"{name}: ended at {st.pos}, next effect at {end}")
    return version, effects


def main(argv):
    path = argv[0] if argv else "work/extracted/levels/effects.bin"
    version, effects = load(path)
    print(f"version {version:#x}: {len(effects)} effects, "
          f"{sum(len(e.emitters) for e in effects.values())} emitters")
    if "--dump" in argv:
        for name, e in effects.items():
            print(name)
            for em in e.emitters:
                print("  ", em)


if __name__ == "__main__":
    main(sys.argv[1:])
