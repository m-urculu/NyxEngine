#!/usr/bin/env python3
"""planet.py - procedural planet mesh generator for Nyx.

Builds an icosphere, displaces it with multi-octave value noise into terrain,
clamps everything below sea level to a flat ocean, and writes per-vertex biome
colours (ocean / beach / plains / hills / rock / snow + polar caps). Output is a
Wavefront OBJ with the 'v x y z r g b' colour extension -- Nyx's OBJ loader reads
those, so the biomes show up lit + shadowed with no material setup. Rivers/erosion
are left out for now.

Pure standard library (no numpy). Run from a terminal:

    python planet.py                 # random seed, writes into ../assets/models/Planet/
    python planet.py --seed 42       # reproducible
    python planet.py --subdiv 6      # more detail (20 * 4^subdiv triangles)
    python planet.py --out world.obj # custom output path

Then drag the generated .obj from the content browser into the scene.
"""

import argparse
import math
import os
import random

# ----------------------------------------------------------------------------- noise
def make_perm(seed):
    p = list(range(256))
    random.Random(seed).shuffle(p)
    return p + p  # length 512, so we can index without masking the second lookup

def _fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)

def _lerp(a, b, t):
    return a + (b - a) * t

def value_noise(perm, x, y, z):
    """Seeded 3D value noise in roughly [-1, 1]."""
    xi, yi, zi = math.floor(x), math.floor(y), math.floor(z)
    xf, yf, zf = x - xi, y - yi, z - zi
    u, v, w = _fade(xf), _fade(yf), _fade(zf)

    def h(i, j, k):
        return perm[(perm[(perm[i & 255] + (j & 255)) & 255] + (k & 255)) & 255] / 127.5 - 1.0

    c000 = h(xi,   yi,   zi);   c100 = h(xi+1, yi,   zi)
    c010 = h(xi,   yi+1, zi);   c110 = h(xi+1, yi+1, zi)
    c001 = h(xi,   yi,   zi+1); c101 = h(xi+1, yi,   zi+1)
    c011 = h(xi,   yi+1, zi+1); c111 = h(xi+1, yi+1, zi+1)
    x00 = _lerp(c000, c100, u); x10 = _lerp(c010, c110, u)
    x01 = _lerp(c001, c101, u); x11 = _lerp(c011, c111, u)
    return _lerp(_lerp(x00, x10, v), _lerp(x01, x11, v), w)

def fbm(perm, x, y, z, octaves=6, lac=2.0, gain=0.5):
    total, amp, freq, norm = 0.0, 0.5, 1.0, 0.0
    for _ in range(octaves):
        total += amp * value_noise(perm, x * freq, y * freq, z * freq)
        norm += amp
        amp *= gain
        freq *= lac
    return total / norm

# ----------------------------------------------------------------------------- math
def normalize(v):
    m = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) or 1.0
    return (v[0]/m, v[1]/m, v[2]/m)

def lerp3(a, b, t):
    t = max(0.0, min(1.0, t))
    return (a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t)

# ----------------------------------------------------------------------------- icosphere
def icosphere(subdiv):
    t = (1.0 + 5.0 ** 0.5) / 2.0
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0),
             (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
             (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    verts = [normalize(v) for v in verts]
    faces = [(0,11,5),(0,5,1),(0,1,7),(0,7,10),(0,10,11),
             (1,5,9),(5,11,4),(11,10,2),(10,7,6),(7,1,8),
             (3,9,4),(3,4,2),(3,2,6),(3,6,8),(3,8,9),
             (4,9,5),(2,4,11),(6,2,10),(8,6,7),(9,8,1)]
    for _ in range(subdiv):
        cache = {}
        def mid(a, b):
            key = (a, b) if a < b else (b, a)
            if key in cache:
                return cache[key]
            m = normalize(((verts[a][0]+verts[b][0])*0.5,
                           (verts[a][1]+verts[b][1])*0.5,
                           (verts[a][2]+verts[b][2])*0.5))
            verts.append(m)
            cache[key] = len(verts) - 1
            return cache[key]
        nf = []
        for a, b, c in faces:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = nf
    return verts, faces

# ----------------------------------------------------------------------------- planet
def generate(seed, subdiv, radius):
    perm = make_perm(seed)
    off = random.Random(seed ^ 0x9E3779B9).uniform(-1000, 1000)
    noff = (off, off * 1.7 + 11.0, off * 0.3 - 7.0)

    dirs, faces = icosphere(subdiv)

    base_freq, octaves = 1.7, 7
    sea_level, land_height = -0.06, 0.085

    deep    = (0.03, 0.09, 0.27); shallow = (0.10, 0.36, 0.52)
    beach   = (0.78, 0.71, 0.50); plains  = (0.27, 0.52, 0.24)
    hills   = (0.32, 0.40, 0.20); rock    = (0.40, 0.38, 0.36)
    snow    = (0.93, 0.94, 0.96)

    positions, colors = [], []
    for d in dirs:
        e = fbm(perm, d[0]*base_freq + noff[0], d[1]*base_freq + noff[1], d[2]*base_freq + noff[2], octaves)
        if e < sea_level:
            r = radius
            depth = max(0.0, min(1.0, (sea_level - e) / 0.5))
            col = lerp3(shallow, deep, depth)
        else:
            land = (e - sea_level) / (1.0 - sea_level)
            shaped = land ** 1.5
            r = radius * (1.0 + shaped * land_height)
            h = shaped
            if   h < 0.04: col = beach
            elif h < 0.30: col = lerp3(plains, hills, (h - 0.04) / 0.26)
            elif h < 0.60: col = lerp3(hills,  rock,  (h - 0.30) / 0.30)
            else:          col = lerp3(rock,   snow,  (h - 0.60) / 0.40)
        lat = abs(d[1])
        if lat > 0.82:
            col = lerp3(col, snow, (lat - 0.82) / 0.18)
        positions.append((d[0]*r, d[1]*r, d[2]*r))
        colors.append(col)

    # Smooth normals: accumulate (area-weighted) face normals per vertex.
    normals = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in faces:
        p0, p1, p2 = positions[a], positions[b], positions[c]
        e1 = (p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2])
        e2 = (p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2])
        fn = (e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0])
        for i in (a, b, c):
            normals[i][0] += fn[0]; normals[i][1] += fn[1]; normals[i][2] += fn[2]
    normals = [normalize(tuple(n)) for n in normals]
    return positions, normals, colors, faces

def write_obj(path, positions, normals, colors, faces, seed):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write(f"# Nyx procgen planet  seed={seed}  verts={len(positions)}  tris={len(faces)}\n")
        for p, c in zip(positions, colors):
            f.write(f"v {p[0]:.5f} {p[1]:.5f} {p[2]:.5f} {c[0]:.4f} {c[1]:.4f} {c[2]:.4f}\n")
        for n in normals:
            f.write(f"vn {n[0]:.5f} {n[1]:.5f} {n[2]:.5f}\n")
        for a, b, c in faces:                      # OBJ is 1-indexed; v and vn share index
            f.write(f"f {a+1}//{a+1} {b+1}//{b+1} {c+1}//{c+1}\n")

def main():
    ap = argparse.ArgumentParser(description="Generate a procgen planet OBJ for Nyx.")
    ap.add_argument("--seed", type=int, default=random.randint(0, 2**31 - 1))
    ap.add_argument("--subdiv", type=int, default=5, help="icosphere detail: 20*4^subdiv tris (5 = ~20k)")
    ap.add_argument("--radius", type=float, default=1.0, help="sea-level radius (scale the entity in-engine to taste)")
    ap.add_argument("--out", default=None, help="output .obj path")
    a = ap.parse_args()
    a.subdiv = max(1, min(7, a.subdiv))

    # Default output: <project>/assets/models/Planet/planet_<seed>.obj
    # (the script lives in <project>/procgen/, so the project root is one level up).
    if a.out is None:
        proj = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        a.out = os.path.join(proj, "assets", "models", "Planet", f"planet_{a.seed}.obj")

    print(f"Generating planet: seed={a.seed} subdiv={a.subdiv} radius={a.radius}")
    pos, nrm, col, faces = generate(a.seed, a.subdiv, a.radius)
    write_obj(a.out, pos, nrm, col, faces, a.seed)
    print(f"Wrote {len(pos)} verts / {len(faces)} tris -> {a.out}")

if __name__ == "__main__":
    main()
