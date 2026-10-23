#!/usr/bin/env python3
"""Convert N.O.V.A. 3 .bdae models to binary glTF (.glb) with embedded PNG textures.

Exports the first visual scene's node hierarchy, static meshes, skinned meshes
(joints, weights, inverse bind matrices) and materials. Textures are looked up
by name in the extracted data (see --data) and decoded with textures.py.

Material mapping (preview quality, not the game's shaders):
  DiffuseTexture  -> baseColorTexture (TEXCOORD_0)
  NormalTexture   -> normalTexture
  LightmapTexture -> occlusionTexture on TEXCOORD_1 (baked lighting, approximate)
"""

import argparse
import io
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bdae  # noqa: E402
import bdae_anim  # noqa: E402
import textures  # noqa: E402

GL_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT, GL_FLOAT = 5120, 5121, 5123, 5125, 5126
GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER = 34962, 34963

TEXTURE_EXT_PREFERENCE = (".etc", ".pvr", ".pvr2", ".png", ".tga", ".jpg")
ZUP_TO_YUP = [-0.7071067811865476, 0.0, 0.0, 0.7071067811865476]  # -90 degrees about X

BLEND_EFFECT_HINTS = ("glass", "alpha", "additive", "coronas", "glow", "modulate", "hologram", "sun")
VERTEX_COLOR_EFFECT_HINTS = ("vc",)


def load_bdae_bytes(path):
    data = Path(path).read_bytes()
    if data[:4] == b"PK\x03\x04":
        # Effect-style files ship as a ZIP holding the COLLADA source plus binaries
        # for each endianness/quantization; the engine reads the little-endian one.
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = z.namelist()
            pick = next((n for n in ("little_endian_not_quantized.bdae", "little_endian_quantized.bdae")
                         if n in names), None)
            if pick is None:
                raise bdae.BdaeError(f"ZIP without a little-endian .bdae ({names})")
            data = z.read(pick)
    return data


class TextureIndex:
    """Find decoded game textures by image file name."""

    def __init__(self, roots):
        self.by_stem = {}
        for root in roots:
            for p in Path(root).rglob("*"):
                if p.suffix.lower() in TEXTURE_EXT_PREFERENCE:
                    self.by_stem.setdefault(p.stem.lower(), []).append(p)
        self._cache = {}

    def find(self, filename):
        if not filename:
            return None
        stem = Path(filename.replace("\\", "/")).stem.lower()
        cands = self.by_stem.get(stem)
        if not cands:
            return None
        return min(cands, key=lambda p: TEXTURE_EXT_PREFERENCE.index(p.suffix.lower()))

    def png(self, path):
        """Return (png bytes, has_alpha) for a texture file, cached."""
        if path not in self._cache:
            if path.suffix.lower() in (".png", ".tga", ".jpg"):
                img = np.asarray(Image.open(path).convert("RGBA"))
            else:
                img = textures.load_texture(path, path.name)
            buf = io.BytesIO()
            Image.fromarray(img, "RGBA").save(buf, "PNG", optimize=False)
            self._cache[path] = (buf.getvalue(), bool((img[..., 3] < 250).any()))
        return self._cache[path]


class GltfBuilder:
    def __init__(self):
        self.json = {"asset": {"version": "2.0", "generator": "nova3 bdae2gltf"},
                     "scenes": [{"nodes": []}], "scene": 0, "nodes": [], "meshes": [],
                     "materials": [], "textures": [], "images": [], "samplers": [],
                     "accessors": [], "bufferViews": [], "buffers": [], "skins": []}
        self.bin = bytearray()

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.json["bufferViews"].append(view)
        return len(self.json["bufferViews"]) - 1

    def accessor(self, arr, gltf_type, component, normalized=False, target=None, minmax=False):
        arr = np.ascontiguousarray(arr)
        acc = {"bufferView": self._view(arr.tobytes(), target), "componentType": component,
               "count": int(arr.shape[0]), "type": gltf_type}
        if normalized:
            acc["normalized"] = True
        if minmax:
            flat = arr.reshape(arr.shape[0], -1)
            acc["min"] = flat.min(0).tolist()
            acc["max"] = flat.max(0).tolist()
        self.json["accessors"].append(acc)
        return len(self.json["accessors"]) - 1

    def image(self, png):
        self.json["images"].append({"bufferView": self._view(png), "mimeType": "image/png"})
        return len(self.json["images"]) - 1

    def glb(self):
        j = {k: v for k, v in self.json.items() if v != []}
        while len(self.bin) % 4:
            self.bin.append(0)
        j["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(j, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        out = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(js) + 8 + len(self.bin))
        out += struct.pack("<I4s", len(js), b"JSON") + js
        out += struct.pack("<I4s", len(self.bin), b"BIN\0") + bytes(self.bin)
        return out


class Exporter:
    def __init__(self, model, tex_index, warn, builder=None, texture_map=None, skins=True):
        """`builder` and `texture_map` may be shared between exporters to put several
        models in one glTF (textures are then stored once). `skins=False` exports
        skinned meshes as static bind-pose meshes."""
        self.m = model
        self.b = model.bres
        self.tex = tex_index
        self.warn = warn
        self.g = builder or GltfBuilder()
        self.use_skins = skins
        self.material_map = {}      # bdae material id -> glTF material index
        self.texture_map = {} if texture_map is None else texture_map   # texture path -> glTF texture
        self.mesh_map = {}          # (geometry id, skin offset) -> glTF mesh index
        self.node_by_sid = {}
        self.node_by_id = {}
        self.scene_nodes = {}       # node id -> bdae.Node (rest pose for animation)
        self.geometries = {g.id: g for g in model.geometries}
        self.materials = {mt.id: mt for mt in model.materials}
        self.controllers = {self.b.cstr(c + 4): self.b.ptr(c + 8) for c in model.controllers}

    # ------------------------------------------------------------------ vertex data

    def _attr_array(self, mesh, attr_index, first, count):
        a = mesh.attributes[attr_index]
        fmt, size = bdae.COMP_FORMATS[a.comp_type]
        dtype = np.dtype("<" + fmt)
        start = mesh.vertices_off + a.offset + first * a.stride
        return np.ndarray((count, a.comps), dtype, self.b.data, start, (a.stride, size)).copy()

    def _skin_arrays(self, skin, first, count, vertex_count):
        b = self.b
        wp = b.ptr(skin + 0x58) + 4   # skip the buffer's u32 prefix
        # 4 x u8 joint index + up to 4 x f32 weight: the buffer's length in words (+0x54)
        # over the vertex count says how many (3 in Boreas_Thug_A's LOD0, else 4).
        nweights = min(max(b.u32(skin + 0x54) // max(vertex_count, 1) - 1, 1), 4)
        rec = 4 + 4 * nweights
        raw = np.frombuffer(b.data, np.uint8, (first + count) * rec, wp)[first * rec:].reshape(count, rec)
        joints = raw[:, :4].copy()
        weights = np.zeros((count, 4), np.float32)
        weights[:, :nweights] = raw[:, 4:].copy().view("<f4").reshape(count, nweights)
        weights = np.where(weights > 0, weights, 0).astype(np.float32)
        joints[weights == 0] = 0
        sums = weights.sum(1, keepdims=True)
        weights = np.divide(weights, sums, out=np.zeros_like(weights), where=sums > 0)
        weights[sums[:, 0] == 0, 0] = 1.0
        return joints, weights

    # ------------------------------------------------------------------ materials

    def _texture(self, image_index):
        if image_index is None or image_index >= len(self.m.images):
            return None
        img = self.m.images[image_index]
        path = self.tex.find(img.filename) or self.tex.find(img.name) or self.tex.find(img.id)
        if path is None:
            self.warn(f"texture not found: {img.filename}")
            return None
        if path not in self.texture_map:
            try:
                png, has_alpha = self.tex.png(path)
            except Exception as ex:   # a broken texture shouldn't sink the whole model
                self.warn(f"texture {path.name}: {ex}")
                return None
            g = self.g.json
            if not g["samplers"]:
                g["samplers"].append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})
            g["textures"].append({"source": self.g.image(png), "sampler": 0, "name": path.stem})
            self.texture_map[path] = (len(g["textures"]) - 1, has_alpha)
        return self.texture_map[path]

    def _material(self, mat_id):
        if mat_id in self.material_map:
            return self.material_map[mat_id]
        mt = self.materials.get(mat_id)
        mat = {"name": mat_id or "default",
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.8}}
        effect = ((mt.effect_file or "") if mt else "").lower()
        if mt:
            diffuse = self._texture(mt.textures.get("DiffuseTexture", mt.textures.get("DiffuseMap")))
            if diffuse:
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": diffuse[0]}
                if any(h in effect for h in BLEND_EFFECT_HINTS):
                    mat["alphaMode"] = "BLEND"
            normal = self._texture(mt.textures.get("NormalTexture"))
            if normal:
                mat["normalTexture"] = {"index": normal[0]}
            lightmap = self._texture(mt.textures.get("LightmapTexture"))
            if lightmap:
                mat["occlusionTexture"] = {"index": lightmap[0], "texCoord": 1}
            mat["extras"] = {"effect": mt.effect_file, "effect_id": mt.effect_id}
        self.g.json["materials"].append(mat)
        self.material_map[mat_id] = len(self.g.json["materials"]) - 1
        return self.material_map[mat_id]

    # ------------------------------------------------------------------ meshes

    def _mesh(self, geom_id, skin=None):
        key = (geom_id, skin)
        if key in self.mesh_map:
            return self.mesh_map[key]
        geom = self.geometries.get(geom_id)
        if geom is not None and geom.kind == bdae.GEOMETRY_SPLINE:
            return None   # paths, not renderable geometry
        if geom is None or geom.mesh is None or not geom.mesh.attributes:
            self.warn(f"geometry {geom_id!r} missing or empty")
            return None
        mesh = geom.mesh
        prims = []
        for sub in mesh.submeshes:
            if sub.prim_type != 0:
                self.warn(f"{geom_id}: skipping submesh with primitive type {sub.prim_type}")
                continue
            if sub.indices_off is None or sub.index_count == 0:
                continue
            first, count = sub.min_index, sub.max_index - sub.min_index + 1
            if first + count > mesh.vertex_count:
                self.warn(f"{geom_id}: submesh vertex range past end of buffer")
                continue
            slots = sub.slots
            attrs = {}
            pos = self._attr_array(mesh, slots[bdae.SLOT_POSITION], first, count).astype(np.float32)
            attrs["POSITION"] = self.g.accessor(pos, "VEC3", GL_FLOAT, target=GL_ARRAY_BUFFER, minmax=True)
            if slots[bdae.SLOT_NORMAL] != 0xFF:
                n = self._attr_array(mesh, slots[bdae.SLOT_NORMAL], first, count)[:, :3].astype(np.float32)
                length = np.linalg.norm(n, axis=1, keepdims=True)
                n = np.divide(n, length, out=np.tile([0, 0, 1], (count, 1)).astype(np.float32), where=length > 0)
                attrs["NORMAL"] = self.g.accessor(n.astype(np.float32), "VEC3", GL_FLOAT, target=GL_ARRAY_BUFFER)
            mt = self.materials.get(sub.material)
            effect = ((mt.effect_file or "") if mt else "").lower()
            if slots[bdae.SLOT_COLOR] != 0xFF and any(h in effect for h in VERTEX_COLOR_EFFECT_HINTS):
                c = self._attr_array(mesh, slots[bdae.SLOT_COLOR], first, count).astype(np.uint8)
                attrs["COLOR_0"] = self.g.accessor(c, "VEC4", GL_UNSIGNED_BYTE, normalized=True,
                                                   target=GL_ARRAY_BUFFER)
            for t in range(4):
                slot = slots[bdae.SLOT_TEXCOORD0 + t]
                if slot == 0xFF:
                    break
                uv = self._attr_array(mesh, slot, first, count).astype(np.float32)
                attrs[f"TEXCOORD_{t}"] = self.g.accessor(uv, "VEC2", GL_FLOAT, target=GL_ARRAY_BUFFER)
            if skin is not None:
                joints, weights = self._skin_arrays(skin, first, count, mesh.vertex_count)
                attrs["JOINTS_0"] = self.g.accessor(joints, "VEC4", GL_UNSIGNED_BYTE, target=GL_ARRAY_BUFFER)
                attrs["WEIGHTS_0"] = self.g.accessor(weights, "VEC4", GL_FLOAT, target=GL_ARRAY_BUFFER)
            idx = np.array(self.m.read_indices(sub), dtype=np.uint32)
            if idx.size and idx.max() >= count:
                self.warn(f"{geom_id}: index out of range")
                continue
            if count <= 0xFFFF:
                ind = self.g.accessor(idx.astype(np.uint16), "SCALAR", GL_UNSIGNED_SHORT, target=GL_ELEMENT_ARRAY_BUFFER)
            else:
                ind = self.g.accessor(idx, "SCALAR", GL_UNSIGNED_INT, target=GL_ELEMENT_ARRAY_BUFFER)
            prims.append({"attributes": attrs, "indices": ind, "material": self._material(sub.material)})
        if not prims:
            return None
        self.g.json["meshes"].append({"name": geom.name or geom_id, "primitives": prims})
        self.mesh_map[key] = len(self.g.json["meshes"]) - 1
        return self.mesh_map[key]

    def _skin(self, skin, mesh_node):
        b = self.b
        joints = [b.cstr(o) for o in b.array(skin, (0x4C, 0x50, 4))]
        nodes = []
        for name in joints:
            n = self.node_by_sid.get(name, self.node_by_id.get(name))
            if n is None:
                self.warn(f"skin joint {name!r} has no node")
                return None
            nodes.append(n)
        nmat = b.u32(skin) // 16
        if nmat != len(joints):
            self.warn(f"skin has {nmat} matrices for {len(joints)} joints")
            return None
        ibm = np.frombuffer(b.data, "<f4", nmat * 16, b.ptr(skin + 4)).reshape(nmat, 16)  # column-major already
        acc = self.g.accessor(ibm.astype(np.float32), "MAT4", GL_FLOAT)
        self.g.json["skins"].append({"joints": nodes, "inverseBindMatrices": acc})
        return len(self.g.json["skins"]) - 1

    # ------------------------------------------------------------------ nodes

    def _add_nodes(self, node):
        g = self.g.json
        entry = {"name": node.name or node.id}
        if any(node.translation):
            entry["translation"] = list(node.translation)
        if tuple(node.rotation) != (0.0, 0.0, 0.0, 1.0):
            entry["rotation"] = list(node.rotation)
        if tuple(node.scale) != (1.0, 1.0, 1.0):
            entry["scale"] = list(node.scale)
        g["nodes"].append(entry)
        index = len(g["nodes"]) - 1
        sid = self.b.cstr(node.offset + 8)
        if sid:
            self.node_by_sid[sid] = index
        self.node_by_id[node.id] = index
        self.scene_nodes[node.id] = node
        kids = [self._add_nodes(c) for c in node.children]
        if kids:
            g["nodes"][index]["children"] = kids
        node.gltf_index = index
        return index

    def _attach(self, node):
        b = self.b
        g = self.g.json
        for itype, ip in node.instances:
            url = (b.cstr(ip + 4) or "").lstrip("#")
            if itype == bdae.INSTANCE_GEOMETRY:
                mesh, skin_index = self._mesh(url), None
            elif itype == bdae.INSTANCE_CONTROLLER:
                skin = self.controllers.get(url)
                if skin is None:
                    self.warn(f"controller {url!r} not found")
                    continue
                source = (b.cstr(skin + 0x48) or "").lstrip("#")
                if self.use_skins:
                    mesh = self._mesh(source, skin)
                    skin_index = self._skin(skin, node) if mesh is not None else None
                else:
                    mesh, skin_index = self._mesh(source), None
            else:
                continue
            if mesh is None:
                continue
            target = node.gltf_index
            if "mesh" in g["nodes"][target]:
                # A node can carry several instances; give extras their own child node.
                g["nodes"].append({"name": f"{url}"})
                child = len(g["nodes"]) - 1
                g["nodes"][target].setdefault("children", []).append(child)
                target = child
            g["nodes"][target]["mesh"] = mesh
            if skin_index is not None:
                g["nodes"][target]["skin"] = skin_index
        for c in node.children:
            self._attach(c)

    # ------------------------------------------------------------------ animation

    def add_animations(self, library, clip_filter=None):
        """Add each clip of a bdae_anim.AnimationLibrary as a glTF animation."""
        by_node = {}
        for tr in library.tracks:
            if tr.property[0] is not None:
                by_node.setdefault(tr.target, []).append(tr)
        missing = sorted(t for t in by_node if t not in self.node_by_id)
        if missing:
            self.warn(f"{len(missing)} animated nodes not in this model (e.g. {missing[0]})")
        g = self.g.json
        added = 0
        clips = library.clips
        if not clips and library.tracks:
            # Cutscenes and animated props have no clip table: play the whole timeline.
            clips = [bdae_anim.Clip("animation", min(t.times[0] for t in library.tracks),
                                    max(t.times[-1] for t in library.tracks))]
        for clip in clips:
            if clip_filter and not clip_filter.search(clip.name):
                continue
            if clip.end <= clip.start:
                continue
            samplers, channels = [], []
            for target, tracks in by_node.items():
                node = self.scene_nodes.get(target)
                if node is None:
                    continue
                rest = {"translation": node.translation, "rotation": node.rotation, "scale": node.scale}
                for prop in ("translation", "rotation", "scale"):
                    mine = [tr for tr in tracks if tr.property[0] == prop]
                    if not mine:
                        continue
                    t = bdae_anim.clip_times(mine, clip)
                    vals = bdae_anim.sample_property(mine, prop, rest[prop], t).astype(np.float32)
                    secs = ((t - clip.start) / 1000.0).astype(np.float32)
                    inp = self.g.accessor(secs[:, None], "SCALAR", GL_FLOAT, minmax=True)
                    out = self.g.accessor(vals, "VEC4" if prop == "rotation" else "VEC3", GL_FLOAT)
                    samplers.append({"input": inp, "output": out, "interpolation": "LINEAR"})
                    channels.append({"sampler": len(samplers) - 1,
                                     "target": {"node": self.node_by_id[target], "path": prop}})
            if channels:
                g.setdefault("animations", []).append({"name": clip.name, "samplers": samplers, "channels": channels})
                added += 1
        return added

    def instantiate(self):
        """Add one copy of the model's node tree; return its top-level node indices.

        Can be called repeatedly (e.g. for every placement of a prop in a level);
        meshes, materials and textures are shared between the copies."""
        g = self.g.json
        top = []
        if self.m.visual_scenes:
            _, _, nodes = self.m.visual_scenes[0]
            top = [self._add_nodes(n) for n in nodes]
            for n in nodes:
                self._attach(n)
        else:
            # No scene: place every geometry at the origin.
            for geom in self.m.geometries:
                if geom.kind != bdae.GEOMETRY_MESH:
                    continue
                mesh = self._mesh(geom.id)
                if mesh is not None:
                    g["nodes"].append({"name": geom.name or geom.id, "mesh": mesh})
                    top.append(len(g["nodes"]) - 1)
        return top

    def run(self, zup_to_yup=True, animations=(), clip_filter=None):
        g = self.g.json
        g["nodes"].append({"name": "root", "rotation": ZUP_TO_YUP} if zup_to_yup else {"name": "root"})
        g["scenes"][0]["nodes"] = [0]
        top = self.instantiate()
        if top:
            g["nodes"][0]["children"] = top
        for library in animations:
            self.add_animations(library, clip_filter)
        return self.g.glb()


def convert(src, dest, tex_index, animations=(), clip_filter=None):
    warnings = []
    model = bdae.Bdae(load_bdae_bytes(src))
    if not any(g.kind == bdae.GEOMETRY_MESH for g in model.geometries):
        return 0, []   # materials, animations, effects or paths only
    own = bdae_anim.AnimationLibrary(model.bres)
    if own.tracks:
        animations = [own, *animations]   # props, doors and vehicles carry their own animation
    glb = Exporter(model, tex_index, warnings.append).run(animations=animations, clip_filter=clip_filter)
    Path(dest).parent.mkdir(parents=True, exist_ok=True)
    Path(dest).write_bytes(glb)
    return len(glb), sorted(set(warnings))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="+", help=".bdae files")
    ap.add_argument("-o", "--output", help="output .glb (single input) or directory")
    ap.add_argument("--data", action="append", default=[],
                    help="directory to search for textures (repeatable; default: the inputs' grandparent)")
    ap.add_argument("--anim", action="append", default=[],
                    help="animation .bdae whose clips to add (repeatable; tracks bind to node ids)")
    ap.add_argument("--clips", help="only add clips whose name matches this regex")
    args = ap.parse_args(argv)

    roots = args.data or sorted({str(Path(p).resolve().parent.parent) for p in args.inputs})
    tex_index = TextureIndex(roots)
    libraries = [bdae_anim.AnimationLibrary(load_bdae_bytes(a)) for a in args.anim]
    clip_filter = re.compile(args.clips) if args.clips else None
    failed = 0
    for src in map(Path, args.inputs):
        if args.output and len(args.inputs) == 1 and args.output.endswith(".glb"):
            dest = Path(args.output)
        else:
            dest = Path(args.output or src.parent) / (src.stem + ".glb")
        try:
            size, warnings = convert(src, dest, tex_index, libraries, clip_filter)
        except (bdae.BdaeError, ValueError, KeyError, IndexError, struct.error) as ex:
            print(f"error: {src}: {type(ex).__name__}: {ex}", file=sys.stderr)
            failed += 1
            continue
        status = f"{size:>10} bytes" if size else "   skipped"
        print(f"{status}  {src.name}" + (f"  ({len(warnings)} warnings)" if warnings else ""))
        for w in warnings:
            print(f"      - {w}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
