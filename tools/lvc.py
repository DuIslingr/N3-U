#!/usr/bin/env python3
"""Parse N.O.V.A. 3 level files (.lvc) and the game object library (gol.bin).

Both are big-endian CMemoryStream "DICT" files:

* u32 "DICT", u32 offset of the string table, u8 "wide strings" flag; data
  starts at byte 9. The string table is u32 count, then count x (u32 length,
  bytes). In the data, a string is a u32 index into that table.
* .lvc (CLevel::LoadRawLvl / LoadNextObject): u16 'NV', u16 version 3,
  u32 object count, then objects. Each object is u32 type followed by that
  type's fields; there are no lengths, so every reader must consume exactly
  what the game reads. Types below 0x3a.. are level structure (zones,
  portals, nav data, spawn points...); any other type is a game object whose
  layout comes from gol.bin.
* gol.bin (CGameObjectManager::Load): u16 'GO', u16 version 3, u32 type
  count, u32 offsets; each entry is u32 type id, u32 parent count + parent
  ids, u16 component count, string name, then (u16 component id, u16 flag)
  pairs. A game object in a level stores, in component-id order, the data of
  the components whose flag is 0 (plus a few "config" components that are
  always stored).

Readers are ported from the game's loaders; see tools/ghidra_scripts and
work/decomp for the decompiled sources.
"""

import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lvc_specs import COMPONENT_CLASSES, SPECS, TEMPLATE_SPECS  # noqa: E402


class LvcError(Exception):
    pass


class Stream:
    def __init__(self, data):
        if data[:4] != b"DICT":
            raise LvcError("not a DICT stream")
        self.data = data
        table = struct.unpack_from(">I", data, 4)[0]
        self.wide = data[8] != 0
        if self.wide:
            raise LvcError("wide-string streams not supported")
        self.strings = []
        n = struct.unpack_from(">I", data, table)[0]
        p = table + 4
        for _ in range(n):
            length = struct.unpack_from(">I", data, p)[0]
            self.strings.append(data[p + 4:p + 4 + length].decode("latin1"))
            p += 4 + length
        self.end = table
        self.pos = 9

    def _unpack(self, fmt, size):
        if self.pos + size > self.end:
            raise LvcError(f"read past end of data at {self.pos:#x}")
        v = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += size
        return v

    def i(self):
        return self._unpack(">i", 4)

    def f(self):
        return self._unpack(">f", 4)

    def h(self):
        return self._unpack(">h", 2)

    def H(self):
        return self._unpack(">H", 2)

    def c(self):
        return self._unpack(">b", 1)

    def s(self):
        idx = self.i()
        if idx == -1:
            return ""       # GetStringC returns an empty string for missing indices
        if not 0 <= idx < len(self.strings):
            raise LvcError(f"string index {idx} out of range at {self.pos - 4:#x}")
        return self.strings[idx]

    def v3(self):
        return (self.f(), self.f(), self.f())

    def skip(self, n):
        if self.pos + n > self.end:
            raise LvcError("skip past end of data")
        self.pos += n

    def spec(self, spec):
        """Read a flat field list: i f h H c s v (vec3), and [ ... ] for an
        int-count-prefixed repeated group. Returns the values as a list."""
        tokens = spec.replace("[", " [ ").replace("]", " ] ").split()
        out, _ = self._spec(tokens, 0)
        return out

    def _spec(self, tokens, k):
        out = []
        while k < len(tokens):
            t = tokens[k]
            if t == "]":
                return out, k
            if t == "[":
                n = self.i()
                if n < 0 or n > 100000:
                    raise LvcError(f"implausible count {n} at {self.pos - 4:#x}")
                end = k
                items = []
                for _ in range(n):
                    item, end = self._spec(tokens, k + 1)
                    items.append(item)
                if n == 0:
                    depth, end = 0, k + 1
                    while not (tokens[end] == "]" and depth == 0):
                        depth += {"[": 1, "]": -1}.get(tokens[end], 0)
                        end += 1
                out.append(items)
                k = end + 1
                continue
            out.append(getattr(self, t)())
            k += 1
        return out, k


# ---------------------------------------------------------------------- shared blocks

COMP_COLLISION = 0x19                     # CCollisionComponent: c c c c c s(col model) [s i] i(triangles)
BASE = "i f f f f f f f f f c c"          # CComponentBase::Load: id, pos, rot, scale, 2 flags
MESH = "s i c c c c c c c c c c c"        # CComponentMesh::Load: model name, int, 11 flags


@dataclass
class Transform:
    id: int
    position: tuple
    rotation: tuple     # degrees
    scale: tuple


def read_base(st):
    v = st.spec(BASE)
    return Transform(v[0], tuple(v[1:4]), tuple(v[4:7]), tuple(v[7:10]))


def read_mesh(st):
    return st.spec(MESH)[0]


# ---------------------------------------------------------------------- level structure types

def read_level_properties(st):
    return {"props": st.spec("i c c c c f f f f f [c c c c f f f] f f f f [f] c c c c f f f f s c c c [s] s "
                             "c c c c c c c c s s i s s f f")}


def read_nav_mesh(st):
    base = read_base(st)
    nverts = st.h()
    verts = [st.v3() for _ in range(max(nverts, 0))]
    st.c()
    npolys = st.h()
    polys = [st.spec("h h h h h h c c") for _ in range(max(npolys, 0))]
    nlinks = st.H()
    for _ in range(nlinks):
        st.spec("h c f f f i")
    st.v3()
    st.v3()
    st.H()
    st.H()
    nnodes = st.H()
    for _ in range(nnodes):
        st.spec("H H f f i H H")
    return {"base": base, "vertices": verts, "polygons": polys}


def read_portal(st):
    return {"base": read_base(st), "fields": st.spec("i i c"), "plane": st.spec("f f i c c c c")}


def read_skybox(st):
    return {"model": st.spec("s i c")[0]}


def read_zone(st):
    base = read_base(st)
    fields = st.spec("i c c c c c f f f [c c c c c f f]")
    model = st.s()
    st.i()      # CMemoryStream::Read(int&) into a local
    return {"base": base, "fields": fields, "model": model}


def read_spawn_point(st):
    return {"base": read_base(st), "fields": st.spec("c c c s i i i i i i i")}


def read_waypoint(st):
    return {"fields": st.spec("i c f f f i c c c c c s f c")}


def read_nav_space(st):
    st.spec("i f f f")
    st.v3()
    nx, ny, nz = st.i(), st.i(), st.i()
    st.f()
    st.skip(((nx & 0xFFFF) * (ny & 0xFFFF) * (nz & 0xFFFF) + 3) >> 2)
    n = st.H()
    for _ in range(n):
        st.spec("h h h i")
    return {}


def read_meta_zone(st):
    base = read_base(st)
    fields = st.spec("[i] i")
    model = st.s()
    st.i()
    return {"base": base, "fields": fields, "model": model}


LEVEL_TYPES = {
    0x3A: ("level_properties", read_level_properties),
    0x3B: ("nav_mesh", read_nav_mesh),
    0x3E9: ("nav_mesh", read_nav_mesh),
    0x3C: ("portal", read_portal),
    0x3D: ("skybox", read_skybox),
    0x3E: ("zone", read_zone),
    0x6C: ("spawn_point", read_spawn_point),
    0x6D: ("waypoint", read_waypoint),
    0x7E9: ("waypoint", read_waypoint),
    0x6E: ("zone_end", lambda st: {}),
    0x71: ("nav_space", read_nav_space),
    0x401: ("ai_group", lambda st: {"fields": st.spec("i c c")}),
    0x421: ("ai_region", lambda st: {"base": read_base(st)}),
    0xA23: ("nav_point", lambda st: {"fields": st.spec("i f f f c i")}),
    0xA25: ("patrol_point", lambda st: {"fields": st.spec("i f f f [i]")}),
    0xBB9: ("link", lambda st: {"fields": st.spec("i i i")}),
    0xBBA: ("meta_zone", read_meta_zone),
    0xBC1: ("occluder", lambda st: {"base": read_base(st), "model": read_mesh(st), "fields": st.spec("f")}),
    0x1011: ("irradiance_volume", lambda st: {"base": read_base(st), "model": read_mesh(st),
                                              "fields": st.spec("c f c f f f")}),
}


# ---------------------------------------------------------------------- game object components

# Components read from the level whatever their flag (CreateObject "config" cases).
ALWAYS = {
    0x12: "f c [i i]",
    0x22: "c c",
    0x24: "c",
    0x2D: "s [s]",
    0x4C: "i i c c c c",
    0x4F: "i",
    0x51: "i",
    0x56: "i",
    0x58: "i",
    0x60: "i c",
    0x72: "i i c",
    0xAE: "s i s c c",
}

# Components read only when their flag is 0 (per-instance data).
PER_INSTANCE = {
    0x00: BASE,
    0x01: MESH,
    0xB3: "[f f f f f f c c c c]",
}


# Hand corrections where the generated spec is wrong (checked against the decompiled code).
SPEC_OVERRIDES = {
    # The decompiler wraps the second list's loop in an if/else, hiding that it is a
    # list of lists; the converter emitted "i [ i i s c ]".
    "CComponentCinematic::Load": "i c f c c c c s c [ i [ i i i i ] ] [ [ i i s c ] ] [ [ i i s ] ] "
                                 "[ s [ i i f f f f f f ] ] [ <CContainerTracksAnimatedObject::Load> ]",
}


def expand_spec(name, seen=()):
    """Spec of loader `name` with nested <X::Load> calls inlined."""
    spec = SPEC_OVERRIDES.get(name, SPECS.get(name))
    if spec is None:
        raise LvcError(f"no spec for {name}")
    if name in seen:
        raise LvcError(f"recursive spec {name}")
    return re.sub(r"<([^>]+)>", lambda m: expand_spec(m.group(1), seen + (name,)), spec)


def component_spec(comp):
    """Spec for a default-path component (CComponentFactory class Load), or '' if none."""
    cls = COMPONENT_CLASSES.get(comp)
    if cls is None:
        return ""   # the factory returns no component, so nothing is read
    return expand_spec(f"{cls}::Load")


@dataclass
class GolType:
    id: int
    name: str
    parents: list
    components: list    # (component id, flag)
    templates: dict = field(default_factory=dict)   # component id -> template values

    def template_model(self):
        """Mesh name from this type's own CComponentMesh template, if any."""
        t = self.templates.get(0x01)
        return t[0] if t else ""


def template_spec(comp):
    spec = TEMPLATE_SPECS.get(comp)
    if spec is None:
        raise LvcError(f"no template reader for component {comp:#x}")
    return re.sub(r"<([^>]+)>", lambda m: expand_spec(m.group(1)), spec)


def load_gol(path, templates=True):
    """Parse gol.bin. With `templates`, also read every component's template data
    (GenerateComponentTemplate) and check each entry ends where the next begins."""
    st = Stream(Path(path).read_bytes())
    if st.h() != 0x474F or st.h() != 3:
        raise LvcError("not a version-3 GO library")
    n = st.i()
    offsets = [st.i() for _ in range(n)]
    ends = dict(zip(sorted(offsets), sorted(offsets)[1:] + [st.end]))
    types = {}
    for off in offsets:
        st.pos = off
        tid = st.i()
        parents = [st.i() for _ in range(st.i())]
        ncomp = st.h()
        name = st.s().strip()
        comps = [(st.h(), st.h()) for _ in range(ncomp)]
        gt = GolType(tid, name, parents, comps)
        if templates:
            try:
                for comp, _flag in comps:       # templates follow in listed order
                    gt.templates[comp] = st.spec(template_spec(comp))
                if st.pos != ends[off]:
                    raise LvcError(f"templates end at {st.pos:#x}, next entry starts at {ends[off]:#x}")
            except LvcError as ex:
                # Menu-only components (0x69-0x71) and library entries of level-structure
                # types don't validate yet; keep the type, without template data.
                gt.templates = {}
                gt.template_error = str(ex)
        types[tid] = gt
    return types


def resolved_template(gtype, gol, comp, seen=()):
    """Template values for `comp`, from the type itself or the nearest parent that has it."""
    if comp in gtype.templates:
        return gtype.templates[comp]
    for parent in gtype.parents:
        if parent in gol and parent not in seen:
            t = resolved_template(gol[parent], gol, comp, seen + (gtype.id,))
            if t is not None:
                return t
    return None


@dataclass
class LevelObject:
    offset: int
    type: int
    kind: str
    name: str = ""
    data: dict = field(default_factory=dict)


def resolved_components(gtype, gol, seen=()):
    """Component (id, flag) list including inherited ones; the child's entries win."""
    comps = {}
    for parent in gtype.parents:
        if parent in gol and parent not in seen:
            comps.update(resolved_components(gol[parent], gol, seen + (gtype.id,)))
    comps.update(dict(gtype.components))
    return sorted(comps.items())


def read_game_object(st, gtype, gol):
    data = {}
    for comp, flag in resolved_components(gtype, gol):
        if comp in ALWAYS:
            data[comp] = st.spec(ALWAYS[comp])
        elif flag != 0:
            continue
        elif comp == 0x00:
            data["base"] = read_base(st)
        elif comp == 0x01:
            data["model"] = read_mesh(st)
        elif comp in PER_INSTANCE:
            data[comp] = st.spec(PER_INSTANCE[comp])
        else:
            try:
                spec = component_spec(comp)
            except LvcError as ex:
                raise LvcError(f"component {comp:#x} of {gtype.name!r}: {ex}") from None
            if spec:
                data[comp] = st.spec(spec)
    return data


class Level:
    def __init__(self, path, gol):
        self.path = Path(path)
        st = self.stream = Stream(self.path.read_bytes())
        if st.h() != 0x4E56:
            raise LvcError("missing 'NV' tag")
        if st.h() != 3:
            raise LvcError("unsupported level version")
        self.count = st.i()
        self.objects = []
        for _ in range(self.count):
            start = st.pos
            t = st.i()
            if t == -1:
                break
            try:
                self._read_object(st, start, t, gol)
            except LvcError as ex:
                what = gol[t].name if t in gol else LEVEL_TYPES.get(t, ("?",))[0]
                prev = self.objects[-1] if self.objects else None
                after = f"{prev.kind}:{prev.name or hex(prev.type)}@{prev.offset:#x}" if prev else "start"
                raise LvcError(f"in {what!r} (type {t:#x} @ {start:#x}): {ex} [after {after}]") from None
        if st.pos + 4 <= st.end and st.i() != -1:
            raise LvcError(f"missing end marker at {st.pos - 4:#x}")
        self.trailing = st.end - st.pos

    def _read_object(self, st, start, t, gol):
        if True:
            if t in LEVEL_TYPES:
                kind, reader = LEVEL_TYPES[t]
                self.objects.append(LevelObject(start, t, kind, data=reader(st)))
            elif t in gol:
                g = gol[t]
                self.objects.append(LevelObject(start, t, "object", g.name, read_game_object(st, g, gol)))
            else:
                raise LvcError(f"unknown object type {t:#x} at {start:#x}")


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("levels", nargs="+")
    ap.add_argument("--gol", default="work/extracted/levels/gol.bin")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    gol = load_gol(args.gol)
    failed = 0
    for path in args.levels:
        try:
            lvl = Level(path, gol)
        except LvcError as ex:
            print(f"FAIL  {path}: {ex}")
            failed += 1
            continue
        # Reaching the -1 terminator exactly at the string table is the real check; the
        # header count is an editor-side figure that can exceed the stored objects.
        status = "OK  " if lvl.trailing == 0 else "WARN"
        print(f"{status}  {path}: {len(lvl.objects)} objects (header says {lvl.count}), "
              f"{lvl.trailing} bytes left")
        if args.verbose:
            for o in lvl.objects:
                print(f"    {o.offset:#08x} {o.type:#06x} {o.kind:18} {o.name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
