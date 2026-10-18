"""Procedural foliage: trees (tapered trunk/limbs/branches + alpha-tested leaf-cluster cards),
bushes, potted shrubs and instanced grass / flower tiles made of crossed alpha cards.
Everything that sways is tagged wind = 1."""
import math
import random

from mathutils import Matrix, Vector

from common import MeshBuilder, get_coll, place, set_props, tube

# atlas cells in Blender UV space (v up): (u0, v0, u1, v1)
LEAF_CELLS = {"broad": (0.0, 0.5, 0.5, 1.0), "autumn": (0.5, 0.5, 1.0, 1.0),
              "small": (0.0, 0.0, 0.5, 0.5), "narrow": (0.5, 0.0, 1.0, 0.5)}
GRASS_CELLS = {"lawn": (0.0, 0.5, 0.5, 1.0), "wild": (0.5, 0.5, 1.0, 1.0),
               "daisy": (0.0, 0.0, 0.5, 0.5), "poppy": (0.5, 0.0, 1.0, 0.5)}

TREE_KINDS = {
    "linden": dict(height=10.5, trunk_h=2.9, r0=0.24, crown_r=3.4, limbs=6, subs=8, cards=13000, card=0.57,
                   cells=(("broad", 0.85), ("autumn", 0.15)), elev=(35, 60)),
    "narrow": dict(height=8.0, trunk_h=2.2, r0=0.16, crown_r=2.3, limbs=5, subs=7, cards=7500, card=0.54,
                   cells=(("narrow", 1.0),), elev=(55, 75)),
    "small": dict(height=5.8, trunk_h=1.7, r0=0.13, crown_r=2.2, limbs=5, subs=6, cards=6000, card=0.5,
                  cells=(("broad", 0.55), ("autumn", 0.45)), elev=(30, 55)),
}


def pick(rng, weighted):
    t = rng.random() * sum(w for _, w in weighted)
    for k, w in weighted:
        t -= w
        if t <= 0:
            return k
    return weighted[-1][0]


def leaf_card(mb, rng, pos, up, size, cell, outward):
    """Leaf-cluster card with its base at pos, growing along 'up'."""
    up = up.normalized()
    side = up.cross(Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))).normalized()
    if side.length < 1e-3:
        side = up.orthogonal().normalized()
    w = size
    h = size
    p0 = pos - side * (w / 2)
    p1 = pos + side * (w / 2)
    # slight fold along the card makes it read less flat (still 2 triangles)
    u0, v0, u1, v1 = cell
    pts = [p0, p1, p1 + up * h, p0 + up * h]
    mb.poly(pts, "leaves", uvs=[(u0, v0), (u1, v0), (u1, v1), (u0, v1)], facing=outward)


def branch_path(start, direction, length, n, rng, bend_up=0.12, jitter=0.15):
    pts = [start.copy()]
    d = direction.normalized()
    step = length / (n - 1)
    for i in range(1, n):
        d = (d + Vector((rng.uniform(-jitter, jitter), rng.uniform(-jitter, jitter), bend_up))).normalized()
        pts.append(pts[-1] + d * step)
    return pts


def tree_mesh(kind, seed):
    P = TREE_KINDS[kind]
    rng = random.Random(seed)
    mb = MeshBuilder()
    th, r0, cr = P["trunk_h"], P["r0"], P["crown_r"]
    crown_c = Vector((0, 0, th + (P["height"] - th) * 0.5))
    crown_h = (P["height"] - th) * 0.55

    # trunk with root flare
    trunk = [Vector((0, 0, -0.1))]
    lean = Vector((rng.uniform(-0.08, 0.08), rng.uniform(-0.08, 0.08), 1.0)).normalized()
    for i in range(1, 8):
        z = th * 1.08 * i / 7
        trunk.append(Vector((lean.x * z + rng.uniform(-0.03, 0.03), lean.y * z + rng.uniform(-0.03, 0.03), z)))
    radii = [r0 * 1.7, r0 * 1.25] + [r0 * (1.0 - 0.3 * i / 6) for i in range(1, 7)]
    tube(mb, trunk, radii, "bark", sides=10)
    top = trunk[-1]

    twigs = []      # (position, direction)
    n_limbs = P["limbs"]
    base_az = rng.uniform(0, 2 * math.pi)
    for li in range(n_limbs):
        az = base_az + 2 * math.pi * li / n_limbs + rng.uniform(-0.3, 0.3)
        el = math.radians(rng.uniform(*P["elev"]))
        d = Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el)))
        start = trunk[5 + (li % 3) if 5 + (li % 3) < len(trunk) else -1].copy()
        L = cr * rng.uniform(1.0, 1.3)
        path = branch_path(start, d, L, 6, rng, bend_up=0.1)
        rl = [r0 * 0.62 * (1 - 0.8 * i / 5) + 0.015 for i in range(6)]
        tube(mb, path, rl, "bark", sides=6)
        twigs.append((path[-1], (path[-1] - path[-2])))
        for si in range(P["subs"]):
            t = rng.uniform(0.3, 0.95)
            k = min(4, int(t * 5))
            f = t * 5 - k
            sp = path[k].lerp(path[k + 1], f)
            pd = (path[k + 1] - path[k]).normalized()
            rnd = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-0.2, 1))).normalized()
            out = Vector((sp.x - crown_c.x, sp.y - crown_c.y, 0)).normalized() if (sp - crown_c).length > 0.1 else rnd
            sd = (pd * 0.4 + rnd * 0.6 + out * 0.5).normalized()
            sl = L * rng.uniform(0.35, 0.6) * (1.1 - t * 0.5)
            spath = branch_path(sp, sd, sl, 4, rng, bend_up=0.15, jitter=0.25)
            srad = [max(0.012, rl[k] * 0.45), 0.03, 0.02, 0.012]
            tube(mb, spath, srad, "bark", sides=4)
            for j in range(1, 4):
                twigs.append((spath[j], spath[j] - spath[j - 1]))
            # small sub-twigs to give the card cloud more anchor points
            for tj in range(2):
                tp = spath[1].lerp(spath[3], rng.random())
                twigs.append((tp, (sd + Vector((rng.uniform(-0.6, 0.6), rng.uniform(-0.6, 0.6), 0.3))).normalized()))

    # leaf cards: 65% around twigs, 35% filling the crown shell
    n = P["cards"]
    for i in range(n):
        if i < n * 0.65:
            p, d = twigs[rng.randrange(len(twigs))]
            pos = p + Vector((rng.gauss(0, 0.35), rng.gauss(0, 0.35), rng.gauss(0, 0.3)))
            up = (d.normalized() + Vector((rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7), rng.uniform(-0.2, 0.8)))).normalized()
        else:
            while True:
                v = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
                if 0.35 < v.length <= 1.0:
                    break
            pos = crown_c + Vector((v.x * cr, v.y * cr, v.z * crown_h))
            up = (Vector((v.x, v.y, 0.6 + abs(v.z))) + Vector((rng.uniform(-0.4, 0.4), rng.uniform(-0.4, 0.4), 0))).normalized()
        outward = pos - crown_c
        size = P["card"] * rng.uniform(0.75, 1.2)
        leaf_card(mb, rng, pos, up, size, LEAF_CELLS[pick(rng, P["cells"])], outward)
    return mb


def bush_mesh(seed, rx=0.85, rz=0.95, cards=320, card=0.5, cell="small"):
    rng = random.Random(seed)
    mb = MeshBuilder()
    c = Vector((0, 0, rz * 0.55))
    for k in range(6):
        a = 2 * math.pi * k / 6 + rng.uniform(-0.3, 0.3)
        d = Vector((math.cos(a) * 0.5, math.sin(a) * 0.5, 1.0)).normalized()
        path = branch_path(Vector((0, 0, -0.05)), d, rz * 0.9, 4, rng, bend_up=0.05, jitter=0.2)
        tube(mb, path, [0.035, 0.025, 0.015, 0.008], "bark", sides=4)
    for i in range(cards):
        while True:
            v = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
            if 0.25 < v.length <= 1.0:
                break
        pos = c + Vector((v.x * rx, v.y * rx, v.z * rz * 0.6))
        pos.z = max(pos.z, 0.05)
        up = (Vector((v.x, v.y, 0.5 + abs(v.z))) + Vector((rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), 0))).normalized()
        cl = cell if not isinstance(cell, tuple) else pick(rng, cell)
        leaf_card(mb, rng, pos, up, card * rng.uniform(0.8, 1.2), LEAF_CELLS[cl], pos - c)
    return mb


def grass_clump(mb, rng, x, y, z, w, h, cell, quads=2):
    a0 = rng.uniform(0, math.pi)
    for q in range(quads):
        a = a0 + q * math.pi / quads
        right = Vector((math.cos(a), math.sin(a), 0))
        tilt = Vector((rng.uniform(-0.12, 0.12), rng.uniform(-0.12, 0.12), 1.0)).normalized()
        base = Vector((x, y, z - 0.02))
        u0, v0, u1, v1 = cell
        pts = [base - right * (w / 2), base + right * (w / 2), base + right * (w / 2) + tilt * h, base - right * (w / 2) + tilt * h]
        mb.poly(pts, "grass_cards", uvs=[(u0, v0), (u1, v0), (u1, v1), (u0, v1)],
                facing=right.cross(Vector((0, 0, 1))))


def grass_tile_mesh(seed, size=3.8, clumps=1000, mix=(("lawn", 0.6), ("wild", 0.3), ("daisy", 0.06), ("poppy", 0.04))):
    rng = random.Random(seed)
    mb = MeshBuilder()
    for i in range(clumps):
        x = rng.uniform(-size / 2 + 0.15, size / 2 - 0.15)
        y = rng.uniform(-size / 2 + 0.15, size / 2 - 0.15)
        cell = pick(rng, mix)
        s = rng.uniform(0.75, 1.25)
        h = (0.45 if cell in ("wild", "daisy", "poppy") else 0.34) * s
        grass_clump(mb, rng, x, y, 0.0, 0.55 * s, h, GRASS_CELLS[cell], quads=3)
    return mb


def round_patch_mesh(seed, r=3.0, clumps=900):
    rng = random.Random(seed)
    mb = MeshBuilder()
    mix = (("lawn", 0.3), ("wild", 0.15), ("daisy", 0.3), ("poppy", 0.25))
    for i in range(clumps):
        while True:
            x, y = rng.uniform(-r, r), rng.uniform(-r, r)
            if x * x + y * y < (r - 0.2) ** 2 and x * x + y * y > 0.35 ** 2:
                break
        cell = pick(rng, mix)
        s = rng.uniform(0.8, 1.2)
        grass_clump(mb, rng, x, y, 0.0, 0.55 * s, 0.48 * s, GRASS_CELLS[cell], quads=3)
    return mb


# name, kind, seed, position, ground height. The first ten stand in / next to the square; the
# rest grow in the back yards and show above the roofs (skyline depth, sun dappling).
def ivy_mesh(seed, width, height, cards, card=0.3):
    """Ivy on a wall. Local frame: wall plane y = 0, leaves grow towards -y, x along the wall
    from 0 to width, z up. Denser near the ground with a ragged top edge."""
    rng = random.Random(seed)
    mb = MeshBuilder()
    # ragged top edge (random walk), a few stems
    tops, h = [], height * rng.uniform(0.6, 1.0)
    for i in range(41):
        h = min(height, max(height * 0.3, h + rng.uniform(-0.45, 0.45)))
        tops.append(h)

    def top_at(x):
        f = x / width * 40
        i = min(39, int(f))
        return tops[i] + (tops[i + 1] - tops[i]) * (f - i)

    for k in range(int(width / 0.9)):
        x = (k + rng.uniform(0.2, 0.8)) * width / int(width / 0.9)
        pts, z = [], 0.0
        while z < top_at(x) * 0.9:
            pts.append(Vector((x, -0.04, z)))
            z += 0.5
            x = min(width, max(0.0, x + rng.uniform(-0.25, 0.25)))
        if len(pts) > 1:
            tube(mb, pts, [0.025 * (1 - i / len(pts)) + 0.006 for i in range(len(pts))], "bark", sides=4)
    for i in range(cards):
        x = rng.uniform(0.0, width)
        z = top_at(x) * (rng.random() ** 0.7)
        off = rng.uniform(0.03, 0.2)
        a = rng.uniform(-0.6, 0.6)
        right = Vector((math.cos(a), 0.0, math.sin(a)))
        up = Vector((-math.sin(a), 0.0, math.cos(a)))
        tilt = rng.uniform(0.2, 0.9)         # lean away from the wall
        up = (up * math.cos(tilt) + Vector((0, -1, 0)) * math.sin(tilt)).normalized()
        size = card * rng.uniform(0.7, 1.3)
        cell = LEAF_CELLS["small" if rng.random() < 0.8 else "broad"]
        base = Vector((x, -off, z - size * 0.3))
        u0, v0, u1, v1 = cell
        pts = [base - right * size / 2, base + right * size / 2, base + right * size / 2 + up * size,
               base - right * size / 2 + up * size]
        mb.poly(pts, "leaves", uvs=[(u0, v0), (u1, v0), (u1, v1), (u0, v1)], facing=(0, -1, 0.3))
    return mb


def flower_box_mesh(seed):
    """Terracotta window box with flowers; origin = window opening bottom centre on the facade
    (row-local frame: facade y = 0, outside towards -y)."""
    from common import box
    rng = random.Random(seed)
    mb = MeshBuilder()
    box(mb, (-0.46, -0.25, 0.0), (0.46, -0.05, 0.17), "terracotta")
    mb.poly([(-0.43, -0.23, 0.15), (0.43, -0.23, 0.15), (0.43, -0.07, 0.15), (-0.43, -0.07, 0.15)], "grass", facing=(0, 0, 1))
    for k in range(9):
        x = -0.38 + 0.76 * k / 8 + rng.uniform(-0.03, 0.03)
        cell = GRASS_CELLS[("daisy", "poppy")[rng.random() < 0.55]]
        grass_clump(mb, rng, x, -0.15, 0.16, 0.32, 0.34 * rng.uniform(0.85, 1.15), cell, quads=2)
    return mb


IVY = [  # name, origin, rotation about Z, width, height, cards
    ("Ivy_S4_SideWall", (-16.0, -20.3, 0.0), -math.pi / 2, 8.5, 7.0, 3600),
    ("Ivy_W1_SideWall", (-30.8, -14.0, 0.0), 0.0, 8.5, 6.5, 3400),
    ("Ivy_E1_SideWall", (30.8, 26.0, 0.0), math.pi, 8.5, 7.5, 3600),
]


TREES = [("Tree_Linden_01", "linden", 1, (-14.0, 12.0), 0.32), ("Tree_Narrow_01", "narrow", 2, (-8.6, 13.2), 0.32),
         ("Tree_Linden_02", "linden", 3, (-12.4, 6.2), 0.32), ("Tree_Small_01", "small", 4, (-8.0, -8.0), 0.32),
         ("Tree_Linden_03", "linden", 3, (15.8, 13.6), 0.0), ("Tree_Small_02", "small", 4, (16.4, -9.4), 0.0),
         ("Tree_Narrow_02", "narrow", 2, (-17.2, -4.0), 0.0), ("Tree_Linden_04", "linden", 1, (6.0, 14.6), 0.0),
         ("Tree_Linden_05", "linden", 1, (-25.0, -22.5), 0.0), ("Tree_Narrow_03", "narrow", 2, (17.6, 25.0), 0.0),
         ("Tree_Yard_01", "linden", 3, (-15.0, 33.0), 0.0), ("Tree_Yard_02", "linden", 1, (9.0, 33.5), 0.0),
         ("Tree_Yard_03", "narrow", 2, (35.0, 12.0), 0.0), ("Tree_Yard_04", "linden", 3, (35.5, -6.0), 0.0),
         ("Tree_Yard_05", "linden", 1, (9.0, -33.5), 0.0), ("Tree_Yard_06", "small", 4, (17.0, -33.0), 0.0),
         ("Tree_Yard_07", "linden", 3, (-35.0, 4.0), 0.0), ("Tree_Yard_08", "narrow", 2, (-35.5, 14.0), 0.0),
         ("Tree_Yard_09", "linden", 1, (-26.0, 33.0), 0.0), ("Tree_Yard_10", "small", 4, (30.0, 31.0), 0.0),
         ("Tree_Yard_11", "linden", 3, (35.0, -24.0), 0.0), ("Tree_Yard_12", "narrow", 2, (-12.0, -35.0), 0.0),
         ("Tree_Yard_13", "linden", 1, (24.0, -34.0), 0.0), ("Tree_Yard_14", "small", 4, (-1.0, 34.0), 0.0),
         ("Tree_Yard_15", "linden", 3, (36.0, 22.0), 0.0), ("Tree_Yard_16", "linden", 1, (-36.0, -6.0), 0.0),
         ("Tree_Street_SW", "small", 4, (-30.0, -18.5), 0.0)]
SQUARE_TREES = 8      # trees standing on the paving of the square get a tree pit


def build(pots=(), flower_boxes=()):
    ct = get_coll("Foliage")
    meshes = {}
    for seed, kind in ((1, "linden"), (3, "linden"), (2, "narrow"), (4, "small")):
        meshes[(kind, seed)] = tree_mesh(kind, seed).build_mesh(f"Tree_{kind}_{seed}", smooth=True, sharp_angle=89)
    rng = random.Random(9)
    trees = []
    for name, kind, seed, (x, y), z in TREES:
        o = place(meshes[(kind, seed)], name, Matrix.Translation((x, y, z)) @ Matrix.Rotation(rng.uniform(0, 6.28), 4, "Z")
                  @ Matrix.Scale(rng.uniform(0.92, 1.08), 4), ct, wind=1)
        trees.append(o)
    # tree pits for the trees standing in the paving (stone frame)
    from common import box
    pits = MeshBuilder()
    for name, kind, seed, (x, y), z in TREES[:SQUARE_TREES + 2]:
        if z == 0.0:
            for (a, b) in (((-0.8, -0.8), (0.8, -0.65)), ((-0.8, 0.65), (0.8, 0.8)), ((-0.8, -0.65), (-0.65, 0.65)),
                           ((0.65, -0.65), (0.8, 0.65))):
                box(pits, (x + a[0], y + a[1], 0.0), (x + b[0], y + b[1], 0.06), "stone_grey")
            pits.poly([(x - 0.65, y - 0.65, 0.02), (x + 0.65, y - 0.65, 0.02), (x + 0.65, y + 0.65, 0.02),
                       (x - 0.65, y + 0.65, 0.02)], "grass", facing=(0, 0, 1))
    set_props(pits.build("Tree_Pits", get_coll("Ground")), no_shadow=1)

    # bushes
    bush_a = bush_mesh(11).build_mesh("Bush_A", smooth=True, sharp_angle=89)
    bush_b = bush_mesh(12, rx=1.1, rz=1.2, cards=420, card=0.6, cell=(("small", 0.7), ("broad", 0.3))).build_mesh(
        "Bush_B", smooth=True, sharp_angle=89)
    bush_spots = [(-16.2, 4.0, 0.32, bush_a), (-16.0, 14.0, 0.32, bush_b), (-5.9, 4.1, 0.32, bush_b), (-6.0, 14.0, 0.32, bush_a),
                  (-11.0, 14.2, 0.32, bush_a), (-16.2, 9.2, 0.32, bush_b), (-9.8, -5.8, 0.32, bush_a), (-6.2, -10.2, 0.32, bush_a),
                  (20.8, 18.6, 0.12, bush_b), (-20.8, -12.8, 0.12, bush_b), (-20.9, 16.4, 0.12, bush_a), (8.4, 18.9, 0.12, bush_a)]
    for i, (x, y, z, me) in enumerate(bush_spots):
        place(me, f"Bush_{i + 1:02d}", Matrix.Translation((x, y, z)) @ Matrix.Rotation(rng.uniform(0, 6.28), 4, "Z"), ct, wind=1)
    pot_bush = bush_mesh(13, rx=0.35, rz=0.55, cards=90, card=0.32).build_mesh("Pot_Shrub", smooth=True, sharp_angle=89)
    for i, (x, y, z) in enumerate(pots):
        place(pot_bush, f"Pot_Shrub_{i + 1:02d}", Matrix.Translation((x, y, z)) @ Matrix.Rotation(rng.uniform(0, 6.28), 4, "Z"),
              ct, wind=1)

    # ivy on blank side walls
    for i, (name, org, rz, w, h, n) in enumerate(IVY):
        ivy = ivy_mesh(50 + i, w, h, n).build(name, ct, smooth=True, sharp_angle=89)
        ivy.data.transform(Matrix.Rotation(rz, 4, "Z"))
        ivy.location = org
        set_props(ivy, wind=1)
    # flower boxes on first-floor window sills (two variants)
    fb = [flower_box_mesh(61).build_mesh("Flower_Box_A"), flower_box_mesh(62).build_mesh("Flower_Box_B")]
    for i, m in enumerate(flower_boxes):
        place(fb[i % 2], f"Flower_Box_{i + 1:02d}", m, ct, wind=1)

    # grass: 3x3 instanced tiles over the park lawn (two tile variants) + round flower bed
    gt = [grass_tile_mesh(21).build_mesh("Grass_Tile_A", smooth=False),
          grass_tile_mesh(22, mix=(("lawn", 0.4), ("wild", 0.25), ("daisy", 0.2), ("poppy", 0.15))).build_mesh("Grass_Tile_B")]
    x0, y0 = -17.0 + 0.3 + 1.9, 3.0 + 0.3 + 1.9
    k = 0
    for i in range(3):
        for j in range(3):
            k += 1
            place(gt[(i + j) % 2], f"Grass_Park_{k:02d}",
                  Matrix.Translation((x0 + i * 3.8, y0 + j * 3.8, 0.32)) @ Matrix.Rotation(math.pi / 2 * ((i * 3 + j) % 4), 4, "Z"),
                  ct, wind=1)
    rp = round_patch_mesh(31).build("Flower_Bed_Round", ct)
    rp.location = (-8.0, -8.0, 0.32)
    set_props(rp, wind=1)
    # thin grass strips in the tree pits and along the west sidewalk
    pit_me = grass_tile_mesh(41, size=1.3, clumps=26).build_mesh("Grass_Pit")
    n = 0
    for name, kind, seed, (x, y), z in TREES[:SQUARE_TREES + 2]:
        if z == 0.0:
            n += 1
            place(pit_me, f"Grass_Pit_{n:02d}", Matrix.Translation((x, y, 0.02)), ct, wind=1)
    return trees
