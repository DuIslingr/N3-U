#!/usr/bin/env python3
"""Measure how closely each level's nav meshes sit on its collision geometry (needs scipy). Run from the repo root."""
import sys, glob, pathlib, numpy as np
sys.path.insert(0, 'tools')
from scipy.spatial import cKDTree
import lvc, col, level2gltf

def rotmat(rx, ry, rz):
    x, y, z, w = level2gltf.euler_deg_to_quat(rx, ry, rz)
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])

def samples(tris, spacing=0.4):
    """Points on triangles roughly every `spacing` metres (barycentric grid)."""
    out = [tris.reshape(-1, 3)]
    e = np.maximum(np.linalg.norm(tris[:, 1] - tris[:, 0], axis=1), np.linalg.norm(tris[:, 2] - tris[:, 0], axis=1))
    n = np.clip((e / spacing).astype(int), 1, 60)
    for k in np.unique(n):
        T = tris[n == k]
        for i in range(k + 1):
            for j in range(k + 1 - i):
                a, b = i / k, j / k
                out.append(T[:, 0] * (1 - a - b) + T[:, 1] * a + T[:, 2] * b)
    return np.concatenate(out)

gol = lvc.load_gol('work/extracted/levels/gol.bin')
cols = {p.name.lower(): p for p in pathlib.Path('work/extracted').rglob('*.col')}
bdaes = {p.name.lower(): p for p in pathlib.Path('work/extracted').rglob('*.bdae')}
cache = {}
allfrac = []
for path in sorted(glob.glob('work/extracted/levels/*.lvc')):
    try: L = lvc.Level(path, gol)
    except lvc.LvcError: continue
    navs = [o for o in L.objects if o.kind == 'nav_mesh']
    if not navs: continue
    pts = []
    for name, tf, obj in level2gltf.collision_placements(L, gol):
        key = name.lower()
        if key not in cache:
            found = col.find_collision(name, cols, bdaes)
            cache[key] = samples(found[0].astype(np.float64)) if found is not None and len(found[0]) else None
        if cache[key] is None: continue
        P = cache[key] * np.array(tf.scale)
        pts.append(P @ rotmat(*tf.rotation).T + np.array(tf.position))
    tree = cKDTree(np.concatenate(pts))
    V = np.concatenate([np.array(o.data['vertices']) for o in navs])
    d, _ = tree.query(V)
    frac = np.mean(d < 0.3); allfrac.append((frac, len(V)))
    print(f'{pathlib.Path(path).stem:32} {len(navs):3} nav meshes {len(V):6} verts  median {np.median(d):.2f} m  <0.3m: {frac*100:5.1f}%  >2m: {np.mean(d > 2)*100:4.1f}%')
w = sum(f * n for f, n in allfrac) / sum(n for f, n in allfrac)
print(f'overall: {w*100:.1f}% of nav-mesh vertices within 0.3 m of collision geometry')
