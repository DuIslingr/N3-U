#!/usr/bin/env python3
"""Read N.O.V.A. 3 collision files (.col).

A .col is a big-endian CMemoryStream "DICT" file (see lvc.py) loaded by
CCollisionComponent::Load: the level names "<room>_col.bdae", and the game
swaps the extension for ".col". Layout:

    u32 group count
    per group: string name, u32 triangle count,
               per triangle: 9 floats (three vertices, world space) + u8 material

The material byte is a collision material id; CPhysicsManager::isColMatInvisible
decides which ones block movement but are not drawn.
"""

import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lvc import LvcError, Stream  # noqa: E402

TRIANGLE = np.dtype([("v", ">f4", (3, 3)), ("material", "u1")])


@dataclass
class CollisionGroup:
    name: str
    vertices: np.ndarray    # (n, 3, 3) float32
    materials: np.ndarray   # (n,) uint8


def load_col(path):
    st = Stream(Path(path).read_bytes())
    groups = []
    for _ in range(st.i()):
        name = st.s()
        n = st.i()
        if n < 0 or st.pos + n * TRIANGLE.itemsize > st.end:
            raise LvcError(f"{path}: bad triangle count {n}")
        tris = np.frombuffer(st.data, TRIANGLE, n, st.pos)
        st.pos += n * TRIANGLE.itemsize
        groups.append(CollisionGroup(name, tris["v"].astype(np.float32), tris["material"].copy()))
    if st.pos != st.end:
        raise LvcError(f"{path}: {st.end - st.pos} bytes left after the last group")
    return groups


def _quat_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def bdae_triangles(model):
    """World-space (n, 3, 3) triangles of every geometry instance in a parsed bdae.Bdae.

    The game falls back to this when a collision component names a model with no
    .col file: the stored triangle count then equals the model's triangle count."""
    import bdae
    b = model.bres
    geoms = {g.id: g for g in model.geometries if g.mesh is not None}
    out = []

    def mesh_tris(mesh):
        pos = np.array(model.read_attribute(mesh, 0), np.float64)
        for sub in mesh.submeshes:
            if sub.prim_type != 0 or sub.indices_off is None:
                continue
            idx = np.array(model.read_indices(sub), np.int64) + sub.min_index
            if len(idx) % 3 == 0 and idx.size and idx.max() < len(pos):
                out_local.append(pos[idx].reshape(-1, 3, 3))

    def walk(node, parent):
        m = np.eye(4)
        m[:3, :3] = _quat_matrix(node.rotation) @ np.diag(node.scale)
        m[:3, 3] = node.translation
        world = parent @ m
        for itype, ip in node.instances:
            if itype != bdae.INSTANCE_GEOMETRY:
                continue
            g = geoms.get((b.cstr(ip + 4) or "").lstrip("#"))
            if g is None:
                continue
            out_local.clear()
            mesh_tris(g.mesh)
            for t in out_local:
                out.append(t @ world[:3, :3].T + world[:3, 3])
        for c in node.children:
            walk(c, world)

    out_local = []
    if model.visual_scenes:
        for n in model.visual_scenes[0][2]:
            walk(n, np.eye(4))
    else:
        for g in geoms.values():
            out_local.clear()
            mesh_tris(g.mesh)
            out.extend(out_local)
    return np.concatenate(out).astype(np.float32) if out else np.zeros((0, 3, 3), np.float32)


def find_collision(name, col_files, bdae_files):
    """Triangles for a collision component's model name, as the game resolves it:
    "<name>.col" if it exists, else the named model's own triangles. Returns
    ((n, 3, 3) triangles, (n,) materials, source) or None if neither exists."""
    stem = name.lower()
    if stem.endswith(".bdae"):
        stem = stem[:-5]
    path = col_files.get(stem + ".col")
    if path is not None:
        groups = load_col(path)
        return (np.concatenate([g.vertices for g in groups]),
                np.concatenate([g.materials for g in groups]), path.name)
    path = bdae_files.get(stem + ".bdae")
    if path is not None:
        import bdae
        import bdae2gltf
        tris = bdae_triangles(bdae.Bdae(bdae2gltf.load_bdae_bytes(path)))
        return tris, np.full(len(tris), 255, np.uint8), path.name
    return None


def main(argv=None):
    import argparse
    from collections import Counter
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    mats, tris, failed = Counter(), 0, 0
    for p in args.files:
        try:
            for g in load_col(p):
                tris += len(g.materials)
                mats.update(g.materials.tolist())
        except LvcError as ex:
            print(f"FAIL {ex}")
            failed += 1
    print(f"{len(args.files) - failed}/{len(args.files)} files, {tris} triangles; materials: {sorted(mats.items())}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
