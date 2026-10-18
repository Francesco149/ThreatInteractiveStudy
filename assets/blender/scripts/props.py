"""Props: street lamps, benches, the fountain with a gilded armillary sphere, market stall,
crates/barrels, cafe terrace (bistro tables, chairs, bottles, glasses, parasol), handcart,
garden windmill, hanging inn sign, fire barrel, bollards, bins, wall spot housings, neon sign.

Repeated props are built once and placed as linked duplicates (shared glTF meshes)."""
import math
import random

import bpy
from mathutils import Matrix, Vector

from common import MeshBuilder, box, empty, get_coll, lathe, place, set_props, tube

LAMP_HEIGHT = 3.65      # light / bulb centre above the base


# ------------------------------------------------------------------ helpers


def ring(mb, center, radius, thickness, mat, axis="z", segs=24, sides=6):
    """Torus-like ring built with a closed tube."""
    cx, cy, cz = center
    pts = []
    for k in range(segs + 1):
        a = 2 * math.pi * k / segs
        if axis == "z":
            pts.append((cx + radius * math.cos(a), cy + radius * math.sin(a), cz))
        elif axis == "x":
            pts.append((cx, cy + radius * math.cos(a), cz + radius * math.sin(a)))
        else:
            pts.append((cx + radius * math.cos(a), cy, cz + radius * math.sin(a)))
    tube(mb, pts, [thickness] * len(pts), mat, sides=sides)


def sphere(mb, center, r, mat, segs=12, rings=8, tile=None):
    prof = [(math.sin(math.pi * k / rings) * r, -math.cos(math.pi * k / rings) * r) for k in range(rings + 1)]
    prof[0] = (0.0, -r)
    prof[-1] = (0.0, r)
    lathe(mb, prof, mat, segs=segs, center=center, tile=tile)


# ------------------------------------------------------------------- meshes


def lamp_post_mesh():
    mb = MeshBuilder()
    prof = [(0.0, 0.0), (0.2, 0.0), (0.2, 0.1), (0.16, 0.13), (0.15, 0.4), (0.11, 0.46), (0.085, 0.6),
            (0.075, 0.75), (0.06, 1.0), (0.05, 3.1), (0.07, 3.18), (0.07, 3.24), (0.05, 3.3), (0.0, 3.3)]
    lathe(mb, prof, "paint_iron_green", segs=14)
    ring(mb, (0, 0, 0.78), 0.085, 0.02, "paint_iron_green", segs=14)
    # lantern: tapered glass box between z0 and z1
    z0, z1 = 3.32, 3.95
    b, t = 0.13, 0.21
    lathe(mb, [(0.0, 3.28), (0.16, 3.28), (0.16, z0)], "paint_iron_green", segs=4, angle0=math.pi / 4)
    corners0 = [Vector((b * math.cos(math.pi / 4 + k * math.pi / 2) * 1.414, b * math.sin(math.pi / 4 + k * math.pi / 2) * 1.414, z0)) for k in range(4)]
    corners1 = [Vector((t * math.cos(math.pi / 4 + k * math.pi / 2) * 1.414, t * math.sin(math.pi / 4 + k * math.pi / 2) * 1.414, z1)) for k in range(4)]
    for k in range(4):
        a0, a1, b1, b0 = corners0[k], corners0[(k + 1) % 4], corners1[(k + 1) % 4], corners1[k]
        out = ((a0 + a1) / 2) - Vector((0, 0, z0))
        mb.poly([a0, a1, b1, b0], "glass_clear", uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], facing=out)
        tube(mb, [a0, b0], [0.012, 0.012], "paint_iron_green", sides=4)
    # roof cap + finial
    lathe(mb, [(0.0, z1 - 0.01), (0.33, z1 - 0.01), (0.33, z1 + 0.03), (0.12, z1 + 0.22), (0.04, z1 + 0.3),
               (0.0, z1 + 0.3)], "paint_iron_green", segs=4, angle0=math.pi / 4)
    sphere(mb, (0, 0, z1 + 0.36), 0.05, "gold", segs=8, rings=6)
    # bulb
    sphere(mb, (0, 0, LAMP_HEIGHT), 0.07, "lamp_bulb", segs=10, rings=6)
    lathe(mb, [(0.03, z0), (0.03, LAMP_HEIGHT - 0.06)], "gold", segs=6)
    return mb


def bench_mesh():
    mb = MeshBuilder()
    L = 1.8
    for k in range(5):     # seat slats
        y0 = -0.22 + k * 0.095
        box(mb, (-L / 2, y0, 0.43), (L / 2, y0 + 0.075, 0.46), "wood")
    for k in range(3):     # back slats (tilted)
        z0 = 0.55 + k * 0.13
        y = 0.27 + (z0 - 0.5) * 0.18
        box(mb, (-L / 2, y, z0), (L / 2, y + 0.03, z0 + 0.09), "wood")
    for x in (-L / 2 + 0.15, L / 2 - 0.15):
        box(mb, (x - 0.03, -0.25, 0.0), (x + 0.03, -0.19, 0.43), "paint_iron_green")
        box(mb, (x - 0.03, 0.2, 0.0), (x + 0.03, 0.26, 0.43), "paint_iron_green")
        box(mb, (x - 0.03, -0.27, 0.38), (x + 0.03, 0.28, 0.43), "paint_iron_green")
        tube(mb, [(x, 0.24, 0.4), (x, 0.3, 0.6), (x, 0.36, 0.95)], [0.025, 0.025, 0.025], "paint_iron_green", sides=5)
        tube(mb, [(x, -0.25, 0.43), (x, -0.27, 0.62), (x, -0.05, 0.66), (x, 0.3, 0.66)], [0.022] * 4,
             "paint_iron_green", sides=5)
    return mb


def fountain_mesh():
    mb = MeshBuilder()
    R, th, H = 4.0, 0.38, 0.62
    n = 8
    off = math.pi / 8
    outer = [Vector((R * math.cos(off + 2 * math.pi * k / n), R * math.sin(off + 2 * math.pi * k / n), 0)) for k in range(n)]
    inner = [v * ((R - th) / R) for v in outer]
    cop = [v * ((R + 0.1) / R) for v in outer]
    copi = [v * ((R - th - 0.06) / R) for v in outer]
    for k in range(n):
        a, b = outer[k], outer[(k + 1) % n]
        mid = (a + b) / 2
        mb.poly([a, b, b + Vector((0, 0, H)), a + Vector((0, 0, H))], "stone", facing=mid)
        ai, bi = inner[k], inner[(k + 1) % n]
        mb.poly([ai + Vector((0, 0, 0.1)), bi + Vector((0, 0, 0.1)), bi + Vector((0, 0, H)), ai + Vector((0, 0, H))], "stone",
                facing=-mid)
        # coping: top, outer lip, inner lip, underside
        ca, cb, cia, cib = cop[k], cop[(k + 1) % n], copi[k], copi[(k + 1) % n]
        zt = H + 0.12
        mb.poly([ca + Vector((0, 0, zt)), cb + Vector((0, 0, zt)), cib + Vector((0, 0, zt)), cia + Vector((0, 0, zt))], "stone",
                facing=(0, 0, 1))
        mb.poly([ca + Vector((0, 0, H)), cb + Vector((0, 0, H)), cb + Vector((0, 0, zt)), ca + Vector((0, 0, zt))], "stone", facing=mid)
        mb.poly([cia + Vector((0, 0, H)), cib + Vector((0, 0, H)), cib + Vector((0, 0, zt)), cia + Vector((0, 0, zt))], "stone",
                facing=-mid)
        mb.poly([ca + Vector((0, 0, H)), cb + Vector((0, 0, H)), b + Vector((0, 0, H)), a + Vector((0, 0, H))], "stone",
                facing=(0, 0, -1))
        mb.poly([cia + Vector((0, 0, H)), cib + Vector((0, 0, H)), bi + Vector((0, 0, H)), ai + Vector((0, 0, H))], "stone",
                facing=(0, 0, -1))
    mb.poly([v + Vector((0, 0, 0.1)) for v in inner], "stone_grey", facing=(0, 0, 1))
    mb.poly([v + Vector((0, 0, 0.47)) for v in inner], "water", facing=(0, 0, 1), tile=3.0)
    # central column with two bowls
    lathe(mb, [(0.95, 0.1), (0.95, 0.45), (0.75, 0.55), (0.42, 0.75), (0.34, 0.9), (0.3, 1.4), (0.36, 1.5),
               (0.2, 1.62), (0.2, 1.7)], "stone", segs=24)
    lathe(mb, [(0.2, 1.7), (0.6, 1.72), (1.25, 1.85), (1.5, 2.02), (1.52, 2.1), (1.4, 2.12), (1.2, 2.0),
               (0.25, 1.9)], "stone", segs=28)
    lathe(mb, [(0.0, 2.0), (1.38, 2.0)], "water", segs=28, tile=3.0)
    lathe(mb, [(0.18, 1.9), (0.14, 2.3), (0.17, 2.4), (0.12, 2.55), (0.12, 2.65)], "stone", segs=16)
    lathe(mb, [(0.12, 2.65), (0.45, 2.68), (0.75, 2.82), (0.8, 2.9), (0.72, 2.92), (0.55, 2.85), (0.15, 2.8)],
          "stone", segs=24)
    lathe(mb, [(0.0, 2.86), (0.7, 2.86)], "water", segs=24, tile=3.0)
    lathe(mb, [(0.15, 2.8), (0.1, 3.0), (0.06, 3.1), (0.06, 3.2), (0.0, 3.2)], "gold", segs=12)
    # gilded armillary sphere ornament
    c = (0, 0, 3.62)
    ring(mb, c, 0.36, 0.022, "gold", axis="z", segs=28)
    ring(mb, c, 0.36, 0.022, "gold", axis="x", segs=28)
    ring(mb, c, 0.36, 0.022, "gold", axis="y", segs=28)
    mb.push(Matrix.Translation(c) @ Matrix.Rotation(math.radians(23.5), 4, "Y"))
    ring(mb, (0, 0, 0), 0.31, 0.03, "gold", axis="z", segs=28)
    tube(mb, [(0, 0, -0.46), (0, 0, 0.46)], [0.012, 0.012], "gold", sides=6)
    mb.pop()
    sphere(mb, c, 0.09, "gold", segs=14, rings=8)
    # spouts
    for k in range(4):
        a = k * math.pi / 2
        d = Vector((math.cos(a), math.sin(a), 0))
        tube(mb, [d * 0.3 + Vector((0, 0, 1.2)), d * 0.55 + Vector((0, 0, 1.18)), d * 0.62 + Vector((0, 0, 1.1))],
             [0.03, 0.03, 0.025], "gold", sides=6)
    return mb


def crate_mesh(w=0.6, d=0.4, h=0.3, open_top=True):
    mb = MeshBuilder()
    t = 0.02
    for side in range(2):
        y = -d / 2 if side == 0 else d / 2 - t
        for k in range(3):
            z0 = k * h / 3 + 0.005
            box(mb, (-w / 2, y, z0), (w / 2, y + t, z0 + h / 3 - 0.02), "wood")
    for side in range(2):
        x = -w / 2 if side == 0 else w / 2 - t
        for k in range(3):
            z0 = k * h / 3 + 0.005
            box(mb, (x, -d / 2 + t, z0), (x + t, d / 2 - t, z0 + h / 3 - 0.02), "wood")
    box(mb, (-w / 2 + t, -d / 2 + t, 0.0), (w / 2 - t, d / 2 - t, 0.02), "wood")
    for sx in (-1, 1):
        for sy in (-1, 1):
            box(mb, (sx * w / 2 - (0.04 if sx > 0 else 0) - 0.0, sy * d / 2 - (0.04 if sy > 0 else 0), 0),
                (sx * w / 2 + (0.0 if sx > 0 else 0.04), sy * d / 2 + (0.0 if sy > 0 else 0.04), h), "wood_dark")
    if not open_top:
        box(mb, (-w / 2, -d / 2, h), (w / 2, d / 2, h + 0.02), "wood")
    return mb


def produce_crate_mesh(fruit, rng):
    mb = crate_mesh()
    r = {"fruit_red": 0.04, "fruit_orange": 0.042, "fruit_green": 0.075, "fruit_yellow": 0.045}[fruit]
    nx, ny = int(0.54 / (2 * r)), int(0.34 / (2 * r))
    for i in range(nx):
        for j in range(ny):
            for layer in range(2 if r < 0.06 else 1):
                x = -0.27 + r + i * 2 * r + rng.uniform(-0.005, 0.005) + layer * r
                y = -0.17 + r + j * 2 * r + rng.uniform(-0.005, 0.005) + layer * r
                if layer and (x > 0.27 - r or y > 0.17 - r):
                    continue
                z = 0.2 + r + layer * r * 1.5
                sphere(mb, (x, y, z), r * rng.uniform(0.9, 1.05), fruit, segs=8, rings=5)
    return mb


def barrel_mesh():
    mb = MeshBuilder()
    H, rb, re = 0.9, 0.31, 0.26
    prof = []
    for k in range(9):
        z = H * k / 8
        r = re + (rb - re) * math.sin(math.pi * z / H)
        prof.append((r, z))
    lathe(mb, prof, "wood", segs=20, tile=1.2)
    lathe(mb, [(re - 0.02, H - 0.03), (0.0, H - 0.03)], "wood_dark", segs=20)
    lathe(mb, [(0.0, 0.03), (re - 0.02, 0.03)], "wood_dark", segs=20)
    lathe(mb, [(re - 0.02, H - 0.03), (re, H)], "wood_dark", segs=20)
    for z in (0.08, 0.28, 0.62, 0.82):
        r = re + (rb - re) * math.sin(math.pi * z / H) + 0.006
        lathe(mb, [(r, z - 0.025), (r + 0.004, z), (r, z + 0.025)], "paint_iron_red", segs=20)
    return mb


def bistro_table_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.24, 0.0), (0.24, 0.025), (0.06, 0.06), (0.03, 0.12), (0.025, 0.7), (0.05, 0.72),
               (0.05, 0.73)], "paint_iron_green", segs=12)
    lathe(mb, [(0.0, 0.73), (0.36, 0.73), (0.36, 0.76), (0.0, 0.76)], "marble", segs=24, tile=0.8)
    return mb


def chair_mesh():
    mb = MeshBuilder()
    s = 0.21
    for sx in (-1, 1):
        for sy in (-1, 1):
            top = 0.46 if sy < 0 else 0.85
            tube(mb, [(sx * s, sy * s, 0.0), (sx * s * 0.95, sy * s * 0.95, 0.45), (sx * s * 0.9, sy * s * 1.1, top)][:2 if sy < 0 else 3],
                 [0.012] * (2 if sy < 0 else 3), "paint_iron_green", sides=4)
    lathe(mb, [(0.0, 0.44), (0.23, 0.44), (0.23, 0.47), (0.0, 0.47)], "wood", segs=16, tile=0.6)
    tube(mb, [(-0.19, 0.23, 0.6), (0.0, 0.26, 0.62), (0.19, 0.23, 0.6)], [0.012] * 3, "paint_iron_green", sides=4)
    for z in (0.7, 0.82):
        box(mb, (-0.19, 0.22, z), (0.19, 0.25, z + 0.06), "wood")
    return mb


def bottle_mesh(mat):
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.036, 0.0), (0.038, 0.01), (0.038, 0.19), (0.03, 0.22), (0.014, 0.25),
               (0.013, 0.31), (0.016, 0.315), (0.016, 0.325), (0.0, 0.325)], mat, segs=12, tile=0.3)
    return mb


def wine_glass_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.035, 0.0), (0.035, 0.004), (0.005, 0.01), (0.004, 0.09), (0.02, 0.1),
               (0.038, 0.13), (0.04, 0.16), (0.034, 0.19), (0.033, 0.19)], "glass_clear", segs=12, tile=0.3)
    return mb


def parasol_mesh():
    mb = MeshBuilder()
    tube(mb, [(0, 0, 0.0), (0, 0, 2.5)], [0.025, 0.025], "wood_dark", sides=6)
    lathe(mb, [(0.0, 0.0), (0.25, 0.0), (0.25, 0.06), (0.0, 0.06)], "stone_grey", segs=12)
    n = 8
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p0 = Vector((1.3 * math.cos(a0), 1.3 * math.sin(a0), 2.05))
        p1 = Vector((1.3 * math.cos(a1), 1.3 * math.sin(a1), 2.05))
        top = Vector((0, 0, 2.48))
        mb.poly([p0, p1, top], "cloth_cream", uvs=[(0, 0), (0.6, 0), (0.3, 0.7)], facing=(0, 0, 1))
        pm = (p0 + p1) / 2 + Vector((0, 0, -0.15))
        mb.poly([p0, p1, pm], "cloth_cream", uvs=[(0, 0), (0.6, 0), (0.3, 0.1)], facing=(p0 + p1) / 2)
    return mb


def stall_mesh(rng):
    """Market stall, front (customer side) facing -X. Table along Y."""
    mb = MeshBuilder()
    Lx, Ly = 1.3, 3.4
    for x in (-Lx / 2, Lx / 2):
        for y in (-Ly / 2, Ly / 2):
            top = 2.35 if x < 0 else 2.75
            box(mb, (x - 0.05, y - 0.05, 0), (x + 0.05, y + 0.05, top), "wood_dark")
    box(mb, (-Lx / 2 - 0.1, -Ly / 2 - 0.05, 0.86), (Lx / 2 + 0.05, Ly / 2 + 0.05, 0.9), "wood")
    box(mb, (-Lx / 2 - 0.08, -Ly / 2, 0.1), (-Lx / 2 - 0.05, Ly / 2, 0.86), "wood")       # front skirt
    box(mb, (-Lx / 2, -Ly / 2, 0.15), (Lx / 2, Ly / 2, 0.18), "wood")                       # lower shelf
    # awning: from back top to front, overhanging the customer side
    segs = 5
    z_back, z_front, x_back, x_front = 2.8, 2.25, Lx / 2 + 0.1, -Lx / 2 - 0.7
    rows = []
    for i in range(segs + 1):
        f = i / segs
        sag = 0.06 * math.sin(math.pi * f)
        x = x_back + (x_front - x_back) * f
        z = z_back + (z_front - z_back) * f - sag
        rows.append([Vector((x, -Ly / 2 - 0.15, z)), Vector((x, Ly / 2 + 0.15, z))])
    L = math.hypot(x_front - x_back, z_front - z_back)
    for i in range(segs):
        a0, a1 = rows[i]
        b0, b1 = rows[i + 1]
        mb.poly([a0, a1, b1, b0], "cloth_stripes", uvs=[(-1.0, L * i / segs / 2), (1.0, L * i / segs / 2),
                                                       (1.0, L * (i + 1) / segs / 2), (-1.0, L * (i + 1) / segs / 2)],
                facing=(-0.4, 0, 1))
    e0, e1 = rows[-1]
    n = 12
    for k in range(n):
        ya = e0.y + (e1.y - e0.y) * k / n
        yb = e0.y + (e1.y - e0.y) * (k + 1) / n
        mb.poly([(e0.x, ya, e0.z), (e0.x, yb, e0.z), (e0.x, yb, e0.z - 0.2), (e0.x, (ya + yb) / 2, e0.z - 0.28),
                 (e0.x, ya, e0.z - 0.2)], "cloth_stripes",
                uvs=[(ya / 2, 0), (yb / 2, 0), (yb / 2, -0.1), ((ya + yb) / 4, -0.14), (ya / 2, -0.1)], facing=(-1, 0, 0))
    # produce crates on the table (tilted towards the customer)
    fruits = ["fruit_red", "fruit_orange", "fruit_green", "fruit_yellow", "fruit_red"]
    for k, fr in enumerate(fruits):
        y = -Ly / 2 + 0.35 + k * (Ly - 0.7) / (len(fruits) - 1)
        m = Matrix.Translation((-0.15, y, 0.9)) @ Matrix.Rotation(math.radians(90), 4, "Z") @ Matrix.Rotation(
            math.radians(-12), 4, "X")
        mb.merge(produce_crate_mesh(fr, rng), m)
    # price boards
    for k in range(3):
        y = -Ly / 2 + 0.6 + k * 1.1
        box(mb, (-Lx / 2 - 0.12, y - 0.12, 0.92), (-Lx / 2 - 0.1, y + 0.12, 1.1), "paintedwood_brown")
    return mb


def cart_mesh(rng):
    """Wooden handcart loaded with barrels and crates; origin on the ground at its centre.
    Long axis along X (direction of travel)."""
    mb = MeshBuilder()
    L, W, zp = 2.2, 1.1, 0.62
    box(mb, (-L / 2, -W / 2, zp), (L / 2, W / 2, zp + 0.06), "wood")
    for y in (-W / 2, W / 2 - 0.04):
        box(mb, (-L / 2, y, zp + 0.06), (L / 2, y + 0.04, zp + 0.36), "wood")
    for x in (-L / 2, L / 2 - 0.04):
        box(mb, (x, -W / 2 + 0.04, zp + 0.06), (x + 0.04, W / 2 - 0.04, zp + 0.3), "wood")
    for y in (-W / 2 + 0.1, W / 2 - 0.1):      # shafts / handles
        box(mb, (L / 2 - 0.3, y - 0.035, zp - 0.07), (L / 2 + 1.1, y + 0.035, zp), "wood_dark")
    box(mb, (L / 2 + 0.95, -W / 2 + 0.06, zp - 0.06), (L / 2 + 1.0, W / 2 - 0.06, zp - 0.01), "wood_dark")
    box(mb, (L / 2 - 0.2, -0.03, 0.0), (L / 2 - 0.14, 0.03, zp), "wood_dark")              # prop leg
    box(mb, (-0.05, -W / 2 - 0.2, 0.42), (0.05, W / 2 + 0.2, 0.5), "iron")                 # axle
    for sy in (-1, 1):
        y = sy * (W / 2 + 0.12)
        ring(mb, (0, y, 0.46), 0.43, 0.035, "wood_dark", axis="y", segs=20, sides=5)
        ring(mb, (0, y, 0.46), 0.455, 0.015, "iron", axis="y", segs=20, sides=4)
        lathe_m = Matrix.Translation((0, y, 0.46)) @ Matrix.Rotation(math.pi / 2, 4, "X")
        mb.push(lathe_m)
        lathe(mb, [(0.0, -0.08), (0.08, -0.08), (0.08, 0.08), (0.0, 0.08)], "wood_dark", segs=10)
        mb.pop()
        for k in range(10):
            a = 2 * math.pi * k / 10
            tube(mb, [(0.06 * math.cos(a), y, 0.46 + 0.06 * math.sin(a)), (0.41 * math.cos(a), y, 0.46 + 0.41 * math.sin(a))],
                 [0.018, 0.014], "wood_dark", sides=4)
    # load
    for (x, y) in ((-0.55, -0.25), (-0.55, 0.28)):
        mb.merge(barrel_mesh(), Matrix.Translation((x, y, zp + 0.06)) @ Matrix.Scale(0.85, 4))
    mb.merge(crate_mesh(0.6, 0.6, 0.45, open_top=False), Matrix.Translation((0.35, -0.2, zp + 0.06)))
    mb.merge(produce_crate_mesh("fruit_green", rng), Matrix.Translation((0.35, 0.3, zp + 0.06)) @ Matrix.Rotation(0.1, 4, "Z"))
    return mb


def windmill_tower_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.75, 0.0), (0.72, 0.15), (0.55, 2.0), (0.62, 2.05), (0.0, 2.05)], "paintedwood_white",
          segs=8, tile=1.2)
    lathe(mb, [(0.66, 2.05), (0.7, 2.1), (0.0, 3.0)], "roof_dark", segs=8, tile=1.5)
    box(mb, (-0.2, -0.05, 0.0), (0.2, 0.05, 0.9), "paintedwood_green")
    box(mb, (-0.78, -0.25, 2.2), (-0.5, 0.25, 2.5), "wood_dark")      # nacelle towards -X
    for k in range(4):     # little windows
        a = math.pi / 4 + k * math.pi / 2
        box(mb, (0.66 * math.cos(a) - 0.1, 0.66 * math.sin(a) - 0.1, 1.2), (0.66 * math.cos(a) + 0.1, 0.66 * math.sin(a) + 0.1, 1.45),
            "window_dark")
    return mb


def windmill_sails_mesh():
    """Sails rotating about the local X axis (origin = hub)."""
    mb = MeshBuilder()
    lathe(mb, [(0.0, -0.05), (0.12, -0.05), (0.12, 0.12), (0.06, 0.2), (0.0, 0.2)], "wood_dark", segs=10)
    # lathe is around Z: rotate so the hub axis is X
    mb2 = MeshBuilder()
    mb2.merge(mb, Matrix.Rotation(-math.pi / 2, 4, "Y"))
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        m = Matrix.Rotation(a, 4, "X")
        mb2.push(m)
        box(mb2, (-0.12, -0.04, 0.0), (-0.06, 0.04, 1.45), "wood_dark")
        for j in range(6):
            z = 0.35 + j * 0.2
            box(mb2, (-0.11, -0.02, z), (-0.08, 0.32, z + 0.025), "wood")
        box(mb2, (-0.11, 0.3, 0.3), (-0.08, 0.33, 1.42), "wood")
        mb2.poly([(-0.095, 0.04, 0.32), (-0.095, 0.3, 0.32), (-0.095, 0.3, 1.4), (-0.095, 0.04, 1.4)], "cloth_cream",
                 uvs=[(0, 0), (0.3, 0), (0.3, 1), (0, 1)], facing=(-1, 0, 0))
        mb2.pop()
    return mb2


def sign_bracket_mesh():
    """Wall bracket for the hanging sign; arm along -X from the wall (x=0)."""
    mb = MeshBuilder()
    box(mb, (-0.03, -0.12, -0.25), (0.0, 0.12, 0.25), "iron")
    tube(mb, [(0.0, 0, 0.0), (-1.15, 0, 0.0)], [0.022, 0.018], "iron", sides=6)
    tube(mb, [(0.0, 0, -0.22), (-0.35, 0, -0.1), (-0.7, 0, -0.02)], [0.015] * 3, "iron", sides=5)
    ring(mb, (-0.45, 0, -0.12), 0.09, 0.008, "iron", axis="y", segs=12, sides=4)
    sphere(mb, (-1.17, 0, 0.0), 0.035, "gold", segs=8, rings=5)
    return mb


def sign_mesh():
    """Hanging board; origin = hinge (top centre under the arm). Swings about X."""
    mb = MeshBuilder()
    for x in (-0.3, 0.3):
        tube(mb, [(x, 0, 0.0), (x, 0, -0.12)], [0.006, 0.006], "iron", sides=4)
    box(mb, (-0.42, -0.025, -0.72), (0.42, 0.025, -0.12), "paintedwood_red", tile=1.2)
    box(mb, (-0.44, -0.035, -0.74), (0.44, 0.035, -0.7), "gold")
    box(mb, (-0.44, -0.035, -0.14), (0.44, 0.035, -0.1), "gold")
    for x in (-0.44, 0.4):
        box(mb, (x, -0.035, -0.7), (x + 0.04, 0.035, -0.14), "gold")
    # emblem: a golden tankard silhouette in relief on both faces
    for sy in (-1, 1):
        y = sy * 0.03
        mb.poly([(-0.12, y, -0.6), (0.1, y, -0.6), (0.12, y, -0.26), (-0.14, y, -0.26)], "gold", facing=(0, sy, 0))
        mb.poly([(0.12, y, -0.32), (0.22, y, -0.34), (0.22, y, -0.5), (0.11, y, -0.52)], "gold", facing=(0, sy, 0))
    # board lies in the X-Z plane under the arm (arm and swing axis along X)
    return mb


def fire_barrel_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.29, 0.0), (0.3, 0.02), (0.3, 0.85), (0.28, 0.85), (0.28, 0.06)], "paint_iron_red", segs=18)
    for z in (0.28, 0.56):
        lathe(mb, [(0.302, z - 0.02), (0.31, z), (0.302, z + 0.02)], "paint_iron_red", segs=18)
    rng = random.Random(4)
    for k in range(6):
        a = rng.uniform(0, math.pi)
        tube(mb, [(0.22 * math.cos(a), 0.22 * math.sin(a), 0.62 + rng.uniform(0, 0.1)),
                  (-0.22 * math.cos(a), -0.22 * math.sin(a), 0.72 + rng.uniform(0, 0.15))], [0.04, 0.035], "wood_dark", sides=5)
    lathe(mb, [(0.0, 0.66), (0.27, 0.66)], "embers", segs=14)
    return mb


def bollard_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.16, 0.0), (0.16, 0.1), (0.13, 0.15), (0.12, 0.75), (0.14, 0.8), (0.1, 0.9), (0.0, 0.93)],
          "stone", segs=12)
    return mb


def bin_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.06, 0.0), (0.04, 0.35), (0.04, 0.4)], "paint_iron_green", segs=10)
    lathe(mb, [(0.2, 0.4), (0.22, 0.42), (0.24, 0.95), (0.25, 1.0), (0.22, 1.0), (0.2, 0.45), (0.0, 0.45)],
          "paint_iron_green", segs=16)
    return mb


def wall_spot_mesh():
    """Spot housing pointing down -Z (rotated at placement to match the light)."""
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.12), (0.09, 0.12), (0.11, -0.05), (0.1, -0.06), (0.0, -0.06)], "iron", segs=12)
    lathe(mb, [(0.0, -0.055), (0.095, -0.055)], "lamp_bulb", segs=12)
    return mb


def pot_mesh():
    mb = MeshBuilder()
    lathe(mb, [(0.0, 0.0), (0.18, 0.0), (0.26, 0.45), (0.3, 0.47), (0.3, 0.52), (0.27, 0.52), (0.25, 0.48),
               (0.0, 0.48)], "terracotta", segs=16, tile=1.0)
    return mb


def neon_mesh():
    """'CAFE' in neon tubes built from Blender's text, plus a backing board."""
    curve = bpy.data.curves.new("NeonText", "FONT")
    curve.body = "CAFE"
    curve.size = 0.62
    curve.extrude = 0.0
    curve.bevel_depth = 0.022
    curve.bevel_resolution = 1
    curve.fill_mode = "NONE"
    curve.align_x = "CENTER"
    curve.offset_y = 0.0
    tmp = bpy.data.objects.new("NeonTmp", curve)
    bpy.context.scene.collection.objects.link(tmp)
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg))
    bpy.data.objects.remove(tmp)
    bpy.data.curves.remove(curve)
    mb = MeshBuilder()
    # text lies in XY; stand it up in XZ facing -Y (box-projected UVs keep tangents valid)
    m = Matrix.Rotation(math.pi / 2, 4, "X")
    for p in me.polygons:
        mb.poly([m @ me.vertices[i].co + Vector((0, -0.06, 0.0)) for i in p.vertices], "neon_pink", tile=0.5)
    bpy.data.meshes.remove(me)
    box(mb, (-1.05, -0.03, -0.12), (1.05, 0.0, 0.6), "iron")
    for x in (-0.95, 0.95):
        tube(mb, [(x, -0.045, -0.06), (x, -0.045, 0.54)], [0.012, 0.012], "neon_cyan", sides=6)
    tube(mb, [(-0.95, -0.045, -0.06), (0.95, -0.045, -0.06)], [0.012, 0.012], "neon_cyan", sides=6)
    tube(mb, [(-0.95, -0.045, 0.54), (0.95, -0.045, 0.54)], [0.012, 0.012], "neon_cyan", sides=6)
    return mb


# ------------------------------------------------------------------- layout

LAMPS = [((-14.0, 18.2), False), ((0.0, 18.2), True), ((12.0, 18.2), False), ((20.4, 6.0), False),
         ((20.4, -9.0), True), ((6.0, -18.4), False), ((-9.0, -18.4), True), ((-20.4, 2.0), True)]
FOUNTAIN = (2.0, -1.0)
STALL = (12.0, 7.5)
CART = (-3.0, -13.6)


def build(seed=21):
    rng = random.Random(seed)
    cp = get_coll("Props")
    ca = get_coll("Animated")
    cfx = get_coll("VFX")
    out = {}
    z_side = 0.12   # sidewalk top

    lamp_me = lamp_post_mesh().build_mesh("Lamp_Post")
    out["lamps"] = []
    for i, ((x, y), shadow) in enumerate(LAMPS):
        o = place(lamp_me, f"Lamp_Post_{i + 1:02d}", Matrix.Translation((x, y, z_side)), cp)
        out["lamps"].append((o, Vector((x, y, z_side + LAMP_HEIGHT)), shadow))

    bench_me = bench_mesh().build_mesh("Bench")
    fx, fy = FOUNTAIN
    benches = []
    for k, a in enumerate((math.radians(25), math.radians(115), math.radians(205), math.radians(295))):
        d = 6.0
        benches.append(((fx + d * math.cos(a), fy + d * math.sin(a), 0.0), a - math.pi / 2))
    benches += [((-11.0, 2.2, 0.0), 0.0), ((-14.5, 2.2, 0.0), 0.0), ((-9.5, 18.7, 0.12), 0.0),
                ((12.3, -4.5, 0.0), -math.pi / 2)]
    for i, (p, rz) in enumerate(benches):
        place(bench_me, f"Bench_{i + 1:02d}", Matrix.Translation(p) @ Matrix.Rotation(rz, 4, "Z"), cp)

    f = fountain_mesh().build("Fountain", cp, smooth=True, sharp_angle=40)
    f.location = (fx, fy, 0.0)
    empty("VFX_FountainSpray", (fx, fy, 3.0), cfx, vfx="spray")
    empty("VFX_FountainSpray_Lower", (fx, fy, 2.15), cfx, vfx="spray")

    stall = stall_mesh(rng).build("Market_Stall", cp, smooth=True, sharp_angle=40)
    stall.location = (*STALL, 0.0)

    crate_me = crate_mesh(0.6, 0.6, 0.6, open_top=False).build_mesh("Crate")
    barrel_me = barrel_mesh().build_mesh("Barrel", smooth=True, sharp_angle=50)
    pcrates = {fr: produce_crate_mesh(fr, rng).build_mesh(f"Crate_{fr}", smooth=True, sharp_angle=50)
               for fr in ("fruit_red", "fruit_orange", "fruit_green")}
    crate_spots = [(13.6, 4.6, 0, 0.1), (13.6, 4.6, 0.6, 0.5), (14.3, 5.2, 0, -0.2), (17.0, 22.0, 0, 0.3),
                   (17.6, 22.3, 0, 0.1), (17.3, 22.1, 0.6, 0.8), (-18.4, -16.6, 0, 0.2), (-18.9, -15.9, 0, 0.6),
                   (-9.0, -16.4, 0.0, 0.0), (21.0, 12.0, 0.12, 0.2)]
    for i, (x, y, z, r) in enumerate(crate_spots):
        place(crate_me, f"Crate_{i + 1:02d}", Matrix.Translation((x, y, z)) @ Matrix.Rotation(r, 4, "Z"), cp)
    barrel_spots = [(13.4, 10.2), (14.0, 10.6), (18.3, 22.9), (16.5, 21.0), (-17.6, -17.2), (-9.6, -16.2),
                    (21.2, 13.0), (21.1, 13.7)]
    for i, (x, y) in enumerate(barrel_spots):
        z = 0.12 if (abs(y) > 17.5 and abs(x) < 19.5) or x > 19.5 else 0.0
        place(barrel_me, f"Barrel_{i + 1:02d}", Matrix.Translation((x, y, z)) @ Matrix.Rotation(rng.uniform(0, 6), 4, "Z"), cp)
    for i, (fr, (x, y, r)) in enumerate(zip(("fruit_red", "fruit_orange", "fruit_green", "fruit_red"),
                                            ((11.0, 9.9, 0.3), (11.0, 5.1, -0.2), (14.6, 7.4, 1.4), (13.9, 9.4, 0.7)))):
        place(pcrates[fr], f"ProduceCrate_{i + 1:02d}", Matrix.Translation((x, y, 0.0)) @ Matrix.Rotation(r, 4, "Z"), cp)

    # cafe terrace in front of Bldg_S1 (world x 12..22, facade at y = -20)
    table_me = bistro_table_mesh().build_mesh("Bistro_Table", smooth=True, sharp_angle=50)
    chair_me = chair_mesh().build_mesh("Bistro_Chair", smooth=True, sharp_angle=50)
    bottle_g = bottle_mesh("glass_green").build_mesh("Bottle_Green", smooth=True, sharp_angle=60)
    bottle_a = bottle_mesh("glass_amber").build_mesh("Bottle_Amber", smooth=True, sharp_angle=60)
    glass_me = wine_glass_mesh().build_mesh("Wine_Glass", smooth=True, sharp_angle=60)
    tables = [(13.2, -14.6), (15.6, -15.8), (18.0, -14.4), (15.2, -13.0), (17.8, -16.6)]
    n_ch = 0
    for i, (x, y) in enumerate(tables):
        place(table_me, f"Cafe_Table_{i + 1:02d}", Matrix.Translation((x, y, 0.0)), cp)
        for k in range(2 if i % 2 else 3):
            a = rng.uniform(0, 2 * math.pi) if k == 0 else a + 2 * math.pi / (2 if i % 2 else 3)
            cx, cy = x + 0.62 * math.cos(a), y + 0.62 * math.sin(a)
            n_ch += 1
            place(chair_me, f"Cafe_Chair_{n_ch:02d}",
                  Matrix.Translation((cx, cy, 0.0)) @ Matrix.Rotation(a + math.pi / 2, 4, "Z"), cp)
        place(bottle_g if i % 2 == 0 else bottle_a, f"Bottle_{i + 1:02d}", Matrix.Translation((x + 0.08, y - 0.05, 0.76)), cp)
        for k in range(2):
            place(glass_me, f"Wine_Glass_{i + 1:02d}_{k}", Matrix.Translation((x - 0.15 + 0.1 * k, y + 0.12 - 0.2 * k, 0.76)), cp)
    # a bottle crate & bottles on a side table by the cafe window
    for k in range(6):
        place(bottle_g if k % 2 else bottle_a, f"Bottle_Window_{k + 1:02d}",
              Matrix.Translation((14.3 + 0.12 * k, -19.35, 0.12 + 0.9)), cp)
    side = MeshBuilder()
    box(side, (-0.5, -0.2, 0.0), (0.5, 0.2, 0.9), "wood_dark")
    side.build("Cafe_SideBoard", cp).location = (14.6, -19.35, 0.12)
    par_me = parasol_mesh().build_mesh("Parasol")
    place(par_me, "Parasol_01", Matrix.Translation((15.6, -15.8, 0.0)), cp)
    place(par_me, "Parasol_02", Matrix.Translation((13.2, -14.6, 0.0)), cp)

    # handcart (animated: slides along X)
    cart = cart_mesh(rng).build("Cart", ca, smooth=True, sharp_angle=45)
    cart.location = (*CART, 0.0)
    set_props(cart, anim="slide", anim_axis=[1.0, 0.0, 0.0], anim_amp=9.0, anim_speed=0.25)

    # garden windmill in the park (sails rotate about X)
    wt = windmill_tower_mesh().build("Windmill_Tower", cp)
    wt.location = (-7.2, 9.0, 0.32)
    sails = windmill_sails_mesh().build("Windmill_Sails", ca)
    sails.location = (-7.2 - 0.82, 9.0, 0.32 + 2.35)
    set_props(sails, anim="rotate", anim_axis=[1.0, 0.0, 0.0], anim_speed=1.3)

    # hanging inn sign on Bldg_E2 (east row, facade x = 22)
    br = sign_bracket_mesh().build("Inn_Sign_Bracket", cp)
    br.location = (22.0, 16.2, 3.75)
    sign = sign_mesh().build("Inn_Sign", ca)
    sign.location = (22.0 - 0.75, 16.2, 3.75 - 0.03)
    set_props(sign, anim="swing", anim_axis=[1.0, 0.0, 0.0], anim_speed=1.7, anim_amp=0.14)

    # fire barrel in the SW corner + its emitters
    fb = fire_barrel_mesh().build("Fire_Barrel", cp, smooth=True, sharp_angle=50)
    fb.location = (-18.2, -15.4, 0.0)
    empty("VFX_Fire_Barrel", (-18.2, -15.4, 0.9), cfx, vfx="fire")
    empty("VFX_Smoke_Barrel", (-18.2, -15.4, 1.6), cfx, vfx="smoke")

    bol_me = bollard_mesh().build_mesh("Bollard", smooth=True, sharp_angle=50)
    for i, (x, y) in enumerate(((-19.2, -18.5), (-21.0, -17.8), (-23.0, -17.6), (-17.6, -20.5), (-17.4, -22.6),
                                (15.6, 18.0), (17.2, 18.0), (18.8, 18.0))):
        place(bol_me, f"Bollard_{i + 1:02d}", Matrix.Translation((x, y, 0.0)), cp)
    bin_me = bin_mesh().build_mesh("Bin", smooth=True, sharp_angle=50)
    for i, (x, y) in enumerate(((7.4, 4.2), (-4.0, -6.0), (-12.8, 2.0), (19.0, -11.0))):
        place(bin_me, f"Bin_{i + 1:02d}", Matrix.Translation((x, y, 0.0)), cp)

    pot_me = pot_mesh().build_mesh("Pot", smooth=True, sharp_angle=50)
    pots = [(12.2, -19.2), (20.4, -19.2), (-4.7, 19.3), (-2.2, 19.3), (20.9, 2.6), (20.9, 9.6), (-20.9, 10.5)]
    out["pots"] = []
    for i, (x, y) in enumerate(pots):
        place(pot_me, f"Pot_{i + 1:02d}", Matrix.Translation((x, y, 0.12)), cp)
        out["pots"].append((x, y, 0.12 + 0.47))

    neon = neon_mesh().build("Neon_Sign_Cafe", cp)
    neon.location = (17.0, -20.0 + 0.0, 3.85)
    neon.rotation_euler = (0, 0, math.pi)       # south row faces +Y
    neon.data.transform(neon.matrix_basis.to_3x3().to_4x4())
    neon.rotation_euler = (0, 0, 0)

    out["spot_housing"] = wall_spot_mesh().build_mesh("Wall_Spot", smooth=True, sharp_angle=50)
    out["coll"] = cp
    return out
