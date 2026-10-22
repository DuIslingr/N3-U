#!/usr/bin/env python3
"""Parse N.O.V.A. 3 .bdae files (Glitch engine "BRES" resource images).

A .bdae is a memory image of the engine's glitch::collada::SCollada structures:

* Header (little-endian u32s): "BRES", 0xFEFF BOM, header size, file size,
  relocation count (+0x10), relocation table offset (+0x18), string region
  (+0x1c..+0x20), root struct offset (+0x20), buffer table offset (+0x2c).
* Relocation table: offsets of every u32 that holds a pointer. Each such u32
  stores a file offset; the engine adds the load address. (Entry 0 is the
  table's own pointer at +0x18.)
* Collections in the root are (count, pointer) pairs; strings are pointers to
  NUL-terminated text.
* Vertex and index buffers live at the end of the file. Each starts with a u32
  (seen: 1) before the data; the struct pointers point at that prefix.

Struct layouts below were read from the engine (CColladaDatabase getters,
CColladaFactory::createGeometry, scene::CMeshBuffer, setMaterialParameter,
CResFileManager::postLoadProcess) and checked against the data.
"""

import struct
from dataclasses import dataclass, field

MAGIC = b"BRES"

# Root (glitch::collada::SCollada) collection offsets: (count, pointer, stride)
ROOT_ANIMATIONS = (0x24, 0x28, 0x20)
ROOT_IMAGES = (0x68, 0x6C, 0x10)
ROOT_EFFECTS = (0x70, 0x74, 0x104)
ROOT_MATERIALS = (0x78, 0x7C, 0x24)
ROOT_GEOMETRIES = (0x84, 0x88, 0x10)
ROOT_CONTROLLERS = (0x90, 0x94, 0x0C)
ROOT_VISUAL_SCENES = (0xB8, 0xBC, 0x10)

NODE_SIZE = 0x50
SUBMESH_SIZE = 0x38
MATERIAL_PARAM_SIZE = 0x18
SAMPLER_SIZE = 0x14

# Vertex attribute component types (SMesh +0x18 array)
COMP_S8 = 0
COMP_U8 = 1
COMP_FLOAT = 6
COMP_FORMATS = {0: ("b", 1), 1: ("B", 1), 2: ("h", 2), 3: ("H", 2), 4: ("i", 4), 5: ("I", 4), 6: ("f", 4)}

# Material parameter types that hold samplers (setMaterialParameter cases 0xc..0x10)
PARAM_SAMPLER_TYPES = range(0x0C, 0x11)

# Node instance types (SNode +0x40/+0x44 array of (type, pointer)). The instance
# struct starts {0, url, 0, ...}; controller instances name a skin ("#...-skin").
INSTANCE_CAMERA = 1
INSTANCE_CONTROLLER = 2
INSTANCE_GEOMETRY = 3
INSTANCE_LIGHT = 4


class BdaeError(Exception):
    pass


class Bres:
    """Random access to a BRES image with pointer and string helpers."""

    def __init__(self, data):
        if data[:4] != MAGIC:
            raise BdaeError(f"not a BRES file (magic {data[:4]!r})")
        self.data = data
        self.size = struct.unpack_from("<I", data, 0x0C)[0]
        count, table = struct.unpack_from("<I", data, 0x10)[0], struct.unpack_from("<I", data, 0x18)[0]
        self.relocs = set(struct.unpack_from(f"<{count}I", data, table))
        self.root = self.u32(0x20)

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def i32(self, off):
        return struct.unpack_from("<i", self.data, off)[0]

    def f32(self, off, n=1):
        v = struct.unpack_from(f"<{n}f", self.data, off)
        return v[0] if n == 1 else v

    def is_ptr(self, off):
        return off in self.relocs

    def ptr(self, off):
        """Pointer stored at `off`, or None for a null pointer."""
        v = self.u32(off)
        return v if v and off in self.relocs else None

    def cstr(self, off):
        """String pointed to by the pointer at `off`."""
        p = self.ptr(off)
        if p is None:
            return None
        end = self.data.index(b"\0", p)
        return self.data[p:end].decode("utf-8", "replace")

    def array(self, base, spec):
        """Yield element offsets of a (count, pointer, stride) collection in the struct at `base`."""
        count_off, ptr_off, stride = spec
        n = self.i32(base + count_off)
        p = self.ptr(base + ptr_off)
        if n <= 0 or p is None:
            return []
        return [p + i * stride for i in range(n)]


@dataclass
class Image:
    id: str
    name: str
    filename: str


@dataclass
class Material:
    id: str
    name: str
    effect_file: str
    effect_id: str
    textures: dict = field(default_factory=dict)   # parameter name -> image index
    params: dict = field(default_factory=dict)     # parameter name -> (type, raw values)


@dataclass
class Attribute:
    offset: int
    comp_type: int
    comps: int
    stride: int


@dataclass
class Submesh:
    prim_type: int
    material: str
    slots: bytes            # SMeshBuffer +0x0c..+0x1c attribute slot indices (0xff = unused)
    min_index: int
    max_index: int
    index_count: int
    indices_off: int        # offset of the index data (after the u32 prefix)


@dataclass
class Mesh:
    interleaved: int
    vertex_count: int
    attributes: list
    vertices_off: int       # offset of the vertex data (after the u32 prefix)
    submeshes: list
    bbox: tuple


GEOMETRY_MESH = 0
GEOMETRY_SPLINE = 1     # camera/AI paths; struct not decoded yet


@dataclass
class Geometry:
    id: str
    name: str
    kind: int
    mesh: Mesh              # None unless kind == GEOMETRY_MESH


@dataclass
class Node:
    offset: int
    id: str
    name: str
    translation: tuple
    rotation: tuple         # quaternion (x, y, z, w)
    scale: tuple
    children: list
    instances: list         # (type, offset of instance struct)


# Submesh slot positions (bytes at SMeshBuffer +0x0c..+0x1b)
SLOT_POSITION = 0x0
SLOT_NORMAL = 0x1
SLOT_COLOR = 0x2
SLOT_TEXCOORD0 = 0x4      # +0x10..+0x13: texcoord sets 0-3
SLOT_TANGENT0 = 0x8       # +0x14..+0x17 (stripped by the engine on drivers without them)
SLOT_BINORMAL0 = 0xC      # +0x18..+0x1b


class Bdae:
    def __init__(self, data):
        self.bres = Bres(data)
        b = self.bres
        r = b.root
        self.images = [Image(b.cstr(o), b.cstr(o + 4), b.cstr(o + 8)) for o in b.array(r, ROOT_IMAGES)]
        self.materials = [self._material(o) for o in b.array(r, ROOT_MATERIALS)]
        self.geometries = []
        for o in b.array(r, ROOT_GEOMETRIES):
            kind = b.u32(o + 8)
            mesh = self._mesh(b.ptr(o + 0xC)) if kind == GEOMETRY_MESH else None
            self.geometries.append(Geometry(b.cstr(o), b.cstr(o + 4), kind, mesh))
        self.controllers = b.array(r, ROOT_CONTROLLERS)
        self.visual_scenes = []
        for o in b.array(r, ROOT_VISUAL_SCENES):
            nodes = [self._node(n) for n in b.array(o, (0x08, 0x0C, NODE_SIZE))]
            self.visual_scenes.append((b.cstr(o), b.cstr(o + 4), nodes))
        self.on_demand_meshes = b.i32(r + 0x80)

    def _material(self, o):
        b = self.bres
        m = Material(b.cstr(o), b.cstr(o + 4), b.cstr(o + 8), b.cstr(o + 0xC))
        for p in b.array(o, (0x10, 0x14, MATERIAL_PARAM_SIZE)):
            name, ptype, count = b.cstr(p), b.u32(p + 8), b.u32(p + 0xC)
            value = b.ptr(p + 0x14)
            if ptype in PARAM_SAMPLER_TYPES and value is not None:
                image_ref = b.ptr(value)
                idx = b.i32(image_ref) if image_ref is not None else -1
                if idx >= 0:
                    m.textures[name] = idx
            m.params[name] = (ptype, count, value)
        return m

    def _mesh(self, o):
        b = self.bres
        if o is None:
            return None
        interleaved, vcount = b.u32(o), b.u32(o + 4)
        strides = [b.u32(x) for x in b.array(o, (0x08, 0x0C, 4))]
        offsets = [b.u32(x) for x in b.array(o, (0x10, 0x14, 4))]
        types = [b.u32(x) for x in b.array(o, (0x18, 0x1C, 4))]
        comps = [b.u32(x) for x in b.array(o, (0x20, 0x24, 4))]
        attrs = []
        for i in range(len(offsets)):
            stride = strides[0] if interleaved else strides[i]
            attrs.append(Attribute(offsets[i], types[i], comps[i], stride))
        vbuf = b.ptr(o + 0x30)
        subs = []
        for s in b.array(o, (0x38, 0x3C, SUBMESH_SIZE)):
            ibuf = b.ptr(s + 0x2C)
            subs.append(Submesh(b.u32(s), b.cstr(s + 4), b.data[s + 0xC:s + 0x1C],
                                b.u32(s + 0x20), b.u32(s + 0x24), b.u32(s + 0x28),
                                ibuf + 4 if ibuf is not None else None))
        bbox = b.f32(o + 0x40, 6)
        return Mesh(interleaved, vcount, attrs, vbuf + 4 if vbuf is not None else None, subs, bbox)

    def _node(self, o):
        b = self.bres
        children = [self._node(c) for c in b.array(o, (0x38, 0x3C, NODE_SIZE))]
        instances = [(b.u32(i), b.ptr(i + 4)) for i in b.array(o, (0x40, 0x44, 8))]
        return Node(o, b.cstr(o), b.cstr(o + 4), b.f32(o + 0x0C, 3), b.f32(o + 0x18, 4), b.f32(o + 0x28, 3),
                    children, instances)

    # ------------------------------------------------------------------ data access

    def read_attribute(self, mesh, slot_attr):
        """Return a list of tuples for attribute index `slot_attr` of `mesh`."""
        a = mesh.attributes[slot_attr]
        fmt, size = COMP_FORMATS[a.comp_type]
        st = struct.Struct("<" + fmt * a.comps)
        base = mesh.vertices_off + a.offset
        return [st.unpack_from(self.bres.data, base + v * a.stride) for v in range(mesh.vertex_count)]

    def read_indices(self, sub):
        wide = sub.max_index - sub.min_index >= 0xFFFF
        fmt = "I" if wide else "H"
        return list(struct.unpack_from(f"<{sub.index_count}{fmt}", self.bres.data, sub.indices_off))


def load(path):
    with open(path, "rb") as f:
        return Bdae(f.read())
