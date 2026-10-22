#!/usr/bin/env python3
"""Assemble a N.O.V.A. 3 level (.lvc) into one binary glTF (.glb).

Every game object whose mesh component is stored per instance (room geometry,
props, doors, consoles, placed characters) is converted from its .bdae and
placed at the object's CComponentBase transform. Each model is converted once;
placements share its meshes, materials and textures.

Room geometry is authored in world space and placed at the origin, so the
result shows the level as laid out by the designers. Objects whose mesh comes
from the shared object library (most enemies and pickups) use their type's
template from gol.bin; characters appear in their bind pose.
"""

import argparse
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import bdae  # noqa: E402
import bdae2gltf  # noqa: E402
import col  # noqa: E402
import lvc  # noqa: E402


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def euler_deg_to_quat(rx, ry, rz):
    """Glitch/Irrlicht setRotationDegrees order: X, then Y, then Z (q = qz * qy * qx), as (x, y, z, w)."""
    def axis(i, deg):
        h = math.radians(deg) / 2
        q = [0.0, 0.0, 0.0, math.cos(h)]
        q[i] = math.sin(h)
        return tuple(q)
    return _qmul(_qmul(axis(2, rz), axis(1, ry)), axis(0, rx))


# Editor-only stand-ins for invisible objects (trigger boxes, sound emitters).
HELPER_MODELS = ("helpers_box", "staticmeshplaceholder")


def placements(level, gol=None, library=True):
    """(model name, Transform, object name) for every visible placed object.

    The mesh and transform come from the level when stored per instance, otherwise
    (with `library`) from the object type's template in gol.bin or its parents."""
    out = []
    for o in level.objects:
        if o.kind != "object":
            continue
        model, base = o.data.get("model"), o.data.get("base")
        if gol is not None and library and o.type in gol:
            g = gol[o.type]
            if not model:
                t = lvc.resolved_template(g, gol, 0x01)
                model = t[0] if t else ""
            if base is None:
                t = lvc.resolved_template(g, gol, 0x00)
                if t:
                    base = lvc.Transform(t[0], tuple(t[1:4]), tuple(t[4:7]), tuple(t[7:10]))
        if model and base is not None and not any(h in model.lower() for h in HELPER_MODELS):
            out.append((model, base, o.name))
    return out


def collision_placements(level, gol):
    """(collision model name, Transform, object name) for objects with a collision component.

    Room collision is authored in world space at an identity transform; prop collision
    is local to the object, so applying the object's transform is right for both."""
    out = []
    for o in level.objects:
        if o.kind != "object" or o.type not in gol:
            continue
        data = o.data.get(lvc.COMP_COLLISION) or lvc.resolved_template(gol[o.type], gol, lvc.COMP_COLLISION)
        base = o.data.get("base")
        if base is None:
            t = lvc.resolved_template(gol[o.type], gol, 0x00)
            base = lvc.Transform(t[0], tuple(t[1:4]), tuple(t[4:7]), tuple(t[7:10])) if t else None
        if data and base is not None and isinstance(data[5], str) and data[5]:
            out.append((data[5], base, o.name))
    return out


# Surface colours for collision materials (ids index CComponentCollisionTypesInfos).
INVISIBLE_MATERIALS = (18, 19)      # CPhysicsManager::isColMatInvisible


def _flat_material(g, name, rgba, blend=False):
    mat = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(rgba), "metallicFactor": 0.0,
                                                  "roughnessFactor": 1.0}, "doubleSided": True}
    if blend:
        mat["alphaMode"] = "BLEND"
    g["materials"].append(mat)
    return len(g["materials"]) - 1


def _triangle_mesh(builder, name, tris, material):
    """Add a glTF mesh for (n, 3, 3) triangles; returns the mesh index."""
    pos = np.ascontiguousarray(tris.reshape(-1, 3), np.float32)
    idx = np.arange(len(pos), dtype=np.uint32)
    prim = {"attributes": {"POSITION": builder.accessor(pos, "VEC3", bdae2gltf.GL_FLOAT,
                                                        target=bdae2gltf.GL_ARRAY_BUFFER, minmax=True)},
            "indices": builder.accessor(idx, "SCALAR", bdae2gltf.GL_UNSIGNED_INT,
                                        target=bdae2gltf.GL_ELEMENT_ARRAY_BUFFER),
            "material": material}
    builder.json["meshes"].append({"name": name, "primitives": [prim]})
    return len(builder.json["meshes"]) - 1


def add_collision_layer(builder, parent_children, level, gol, data_root):
    """Collision triangles (.col files, or the named model's own triangles when the game
    falls back to it): solid surfaces grey, invisible blockers red."""
    g = builder.json
    solid = _flat_material(g, "collision", (0.55, 0.6, 0.65, 0.35), blend=True)
    invisible = _flat_material(g, "collision_invisible", (0.9, 0.15, 0.1, 0.45), blend=True)
    cols = {p.name.lower(): p for p in Path(data_root).rglob("*.col")}
    bdaes = {p.name.lower(): p for p in Path(data_root).rglob("*.bdae")}
    meshes, layer, missing = {}, [], Counter()
    for name, tf, obj in collision_placements(level, gol):
        key = name.lower()
        if key in missing:
            missing[key] += 1
            continue
        if key not in meshes:
            found = col.find_collision(name, cols, bdaes)
            if found is None or not len(found[0]):
                missing[key] += 1
                continue
            tris, mats, _source = found
            inv = np.isin(mats, INVISIBLE_MATERIALS)
            prims = []
            for sel, m in ((~inv, solid), (inv, invisible)):
                if sel.any():
                    prims.append(g["meshes"][_triangle_mesh(builder, key, tris[sel], m)]["primitives"][0])
                    g["meshes"].pop()
            g["meshes"].append({"name": key, "primitives": prims})
            meshes[key] = len(g["meshes"]) - 1
        g["nodes"].append(_transform_node(f"col:{obj}", tf, mesh=meshes[key]))
        layer.append(len(g["nodes"]) - 1)
    g["nodes"].append({"name": "collision", "children": layer})
    parent_children.append(len(g["nodes"]) - 1)
    return len(meshes), len(layer), missing


def add_navmesh_layer(builder, parent_children, level):
    """Nav meshes (world-space triangles) in translucent green."""
    g = builder.json
    mat = _flat_material(g, "navmesh", (0.1, 0.9, 0.3, 0.5), blend=True)
    layer = []
    for i, o in enumerate(o for o in level.objects if o.kind == "nav_mesh"):
        V = np.array(o.data["vertices"], np.float32)
        tri_idx = np.array([p[:3] for p in o.data["polygons"]], np.int64)
        if not len(V) or not len(tri_idx):
            continue
        tris = V[tri_idx] + np.array([0, 0, 0.05], np.float32)     # lift slightly above the floor
        g["nodes"].append({"name": f"navmesh_{i}", "mesh": _triangle_mesh(builder, f"navmesh_{i}", tris, mat)})
        layer.append(len(g["nodes"]) - 1)
    g["nodes"].append({"name": "navigation", "children": layer})
    parent_children.append(len(g["nodes"]) - 1)
    return len(layer)


def _transform_node(name, tf, mesh=None):
    node = {"name": name}
    if any(tf.position):
        node["translation"] = list(tf.position)
    if any(tf.rotation):
        node["rotation"] = list(euler_deg_to_quat(*tf.rotation))
    if tuple(tf.scale) != (1.0, 1.0, 1.0):
        node["scale"] = list(tf.scale)
    if mesh is not None:
        node["mesh"] = mesh
    return node


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("level", help=".lvc file")
    ap.add_argument("-o", "--output", required=True, help="output .glb")
    ap.add_argument("--gol", default="work/extracted/levels/gol.bin")
    ap.add_argument("--data", default="work/extracted", help="extracted game data (models and textures)")
    ap.add_argument("--skip", default="", help="comma-separated substrings of object or model names to leave out")
    ap.add_argument("--no-library", action="store_true",
                    help="only place objects whose mesh is stored in the level itself")
    ap.add_argument("--no-models", action="store_true", help="leave out the visible models")
    ap.add_argument("--collision", action="store_true", help="add the collision geometry (.col) as a layer")
    ap.add_argument("--navmesh", action="store_true", help="add the AI navigation meshes as a layer")
    args = ap.parse_args(argv)

    gol = lvc.load_gol(args.gol)
    level = lvc.Level(args.level, gol)
    skip = [s for s in args.skip.split(",") if s]
    places = [] if args.no_models else [p for p in placements(level, gol, not args.no_library) if not any(s.lower() in (p[2] + " " + p[0]).lower() for s in skip)]

    models = {}
    for p in Path(args.data).rglob("*.bdae"):
        models.setdefault(p.name.lower(), p)

    tex_index = bdae2gltf.TextureIndex([args.data])
    builder = bdae2gltf.GltfBuilder()
    texture_map = {}
    warnings = []
    g = builder.json
    g["nodes"].append({"name": Path(args.level).stem, "rotation": bdae2gltf.ZUP_TO_YUP})
    g["scenes"][0]["nodes"] = [0]
    root_children = []

    exporters, missing, failed = {}, Counter(), Counter()
    by_model = defaultdict(list)
    for model, tf, name in places:
        by_model[model.lower()].append((tf, name))
    for model, insts in sorted(by_model.items()):
        path = models.get(model)
        if path is None:
            missing[model] += len(insts)
            continue
        try:
            data = bdae2gltf.load_bdae_bytes(path)
            exp = bdae2gltf.Exporter(bdae.Bdae(data), tex_index, warnings.append,
                                     builder=builder, texture_map=texture_map, skins=False)
        except (bdae.BdaeError, ValueError, KeyError, IndexError) as ex:
            failed[f"{model}: {ex}"] += len(insts)
            continue
        exporters[model] = exp
        for tf, name in insts:
            g["nodes"].append(_transform_node(f"{name}:{model}", tf))
            index = len(g["nodes"]) - 1
            kids = exp.instantiate()
            if kids:
                g["nodes"][index]["children"] = kids
            root_children.append(index)
    extra = []
    if args.collision:
        n_models, n_places, col_missing = add_collision_layer(builder, root_children, level, gol, args.data)
        extra.append(f"collision: {n_places} placements of {n_models} .col files")
        extra += [f"  missing collision: {m} ({n})" for m, n in col_missing.most_common()]
    if args.navmesh:
        extra.append(f"navigation: {add_navmesh_layer(builder, root_children, level)} nav meshes")
    g["nodes"][0]["children"] = root_children

    out = builder.glb()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_bytes(out)
    print(f"{args.output}: {len(places)} placements of {len(by_model)} models, "
          f"{len(g['meshes'])} meshes, {len(g['images'])} textures, {len(out) / 1e6:.1f} MB")
    for line in extra:
        print(f"  {line}")
    for m, n in missing.most_common():
        print(f"  missing model: {m} ({n} placements)")
    for m, n in failed.most_common():
        print(f"  failed model: {m} ({n} placements)")
    for w in sorted(set(warnings))[:20]:
        print(f"  warning: {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
