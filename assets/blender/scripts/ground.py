"""Ground: cobblestone square + streets, raised paved sidewalks with curbs, a park bed and a
round flower bed. All walkable surfaces are tagged no_shadow = 1 (they never cast shadows)."""
import math

from common import MeshBuilder, get_coll, set_props

SIDEWALK_H = 0.12
BED_H = 0.32

# axis aligned rectangles (x0, y0, x1, y1)
COBBLE = [(-19.5, -17.5, 19.5, 17.5),     # the square
          (-36.0, -36.0, -16.0, -17.5),    # SW street, south part
          (-36.0, -17.5, -19.5, -14.0),    # SW street, west part
          (15.0, 17.5, 19.5, 29.0)]        # NE street
SIDEWALKS = [(-22.0, 17.5, 15.0, 20.0),    # north
             (19.5, -20.0, 22.0, 26.0),    # east
             (-16.0, -20.0, 19.5, -17.5),  # south
             (-22.0, -14.0, -19.5, 17.5)]  # west
PARK = (-17.0, 3.0, -5.0, 15.0)
ROUND_BED = ((-8.0, -8.0), 3.4)


def grid_rect(mb, r, z, mat, cell=4.0, facing=(0, 0, 1)):
    x0, y0, x1, y1 = r
    nx = max(1, int(math.ceil((x1 - x0) / cell)))
    ny = max(1, int(math.ceil((y1 - y0) / cell)))
    for i in range(nx):
        for j in range(ny):
            a = x0 + (x1 - x0) * i / nx
            b = x0 + (x1 - x0) * (i + 1) / nx
            c = y0 + (y1 - y0) * j / ny
            d = y0 + (y1 - y0) * (j + 1) / ny
            mb.poly([(a, c, z), (b, c, z), (b, d, z), (a, d, z)], mat, facing=facing)


def build():
    coll = get_coll("Ground")
    objs = []

    # cobbles: square with holes for the beds handled by drawing beds on top (raised)
    mb = MeshBuilder()
    for r in COBBLE:
        grid_rect(mb, r, 0.0, "cobble")
    objs.append(mb.build("Ground_Cobble", coll))

    # sidewalks: top + curb face towards the square
    mb = MeshBuilder()
    for r in SIDEWALKS:
        grid_rect(mb, r, SIDEWALK_H, "paving")
    curbs = [((-22.0, 17.5), (15.0, 17.5), (0, -1)), ((19.5, -17.5), (19.5, 26.0), (-1, 0)),
             ((-16.0, -17.5), (19.5, -17.5), (0, 1)), ((-19.5, -14.0), (-19.5, 17.5), (1, 0)),
             ((15.0, 17.5), (15.0, 20.0), (1, 0)), ((19.5, 26.0), (22.0, 26.0), (0, 1)),
             ((-16.0, -20.0), (-16.0, -17.5), (-1, 0)), ((-22.0, -14.0), (-19.5, -14.0), (0, -1))]
    for (a, b, n) in curbs:
        mb.poly([(a[0], a[1], 0), (b[0], b[1], 0), (b[0], b[1], SIDEWALK_H), (a[0], a[1], SIDEWALK_H)],
                "stone_grey", facing=(n[0], n[1], 0))
    objs.append(mb.build("Ground_Sidewalks", coll))

    # far ground under / around everything (only seen from high viewpoints)
    mb = MeshBuilder()
    for r in ((-60, -60, 60, -36), (-60, 29, 60, 60), (-60, -36, -36, 29), (22, -36, 60, 29),
              (-36, -36, 22, -20), (-36, 20, -22, 29), (19.5, 26, 22, 29), (-36, -14, -22, 20)):
        grid_rect(mb, r, -0.03, "paving", cell=12.0)
    objs.append(mb.build("Ground_Outer", coll))

    # park bed: curb wall (casts shadows) + lawn surface (no shadow)
    x0, y0, x1, y1 = PARK
    t = 0.3
    mb = MeshBuilder()
    for (a, b, c, d, n) in (((x0, y0), (x1, y0), (x1, y0 + t), (x0, y0 + t), (0, -1)),
                            ((x0, y1 - t), (x1, y1 - t), (x1, y1), (x0, y1), (0, 1)),
                            ((x0, y0 + t), (x0 + t, y0 + t), (x0 + t, y1 - t), (x0, y1 - t), (-1, 0)),
                            ((x1 - t, y0 + t), (x1, y0 + t), (x1, y1 - t), (x1 - t, y1 - t), (1, 0))):
        mb.poly([(*a, BED_H + 0.08), (*b, BED_H + 0.08), (*c, BED_H + 0.08), (*d, BED_H + 0.08)], "stone", facing=(0, 0, 1))
    for (a, b, n) in (((x0, y0), (x1, y0), (0, -1)), ((x1, y0), (x1, y1), (1, 0)), ((x1, y1), (x0, y1), (0, 1)),
                      ((x0, y1), (x0, y0), (-1, 0))):
        mb.poly([(*a, 0), (*b, 0), (*b, BED_H + 0.08), (*a, BED_H + 0.08)], "stone", facing=(*n, 0))
    for (a, b, n) in (((x0 + t, y0 + t), (x1 - t, y0 + t), (0, 1)), ((x1 - t, y0 + t), (x1 - t, y1 - t), (-1, 0)),
                      ((x1 - t, y1 - t), (x0 + t, y1 - t), (0, -1)), ((x0 + t, y1 - t), (x0 + t, y0 + t), (1, 0))):
        mb.poly([(*a, BED_H), (*b, BED_H), (*b, BED_H + 0.08), (*a, BED_H + 0.08)], "stone", facing=(*n, 0))
    # round bed curb
    (cx, cy), r = ROUND_BED
    n = 32
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p = [(cx + (r + dr) * math.cos(a), cy + (r + dr) * math.sin(a)) for dr in (0.0, -0.3) for a in (a0, a1)]
        mb.poly([(*p[0], BED_H + 0.08), (*p[1], BED_H + 0.08), (*p[3], BED_H + 0.08), (*p[2], BED_H + 0.08)], "stone",
                facing=(0, 0, 1))
        mb.poly([(*p[0], 0), (*p[1], 0), (*p[1], BED_H + 0.08), (*p[0], BED_H + 0.08)], "stone",
                facing=(math.cos(a0), math.sin(a0), 0))
        mb.poly([(*p[2], BED_H), (*p[3], BED_H), (*p[3], BED_H + 0.08), (*p[2], BED_H + 0.08)], "stone",
                facing=(-math.cos(a0), -math.sin(a0), 0))
    objs.append(mb.build("Park_Curbs", coll))

    mb = MeshBuilder()
    grid_rect(mb, (x0 + t, y0 + t, x1 - t, y1 - t), BED_H, "grass", cell=3.0)
    ring = [(cx + (r - 0.3) * math.cos(2 * math.pi * k / n), cy + (r - 0.3) * math.sin(2 * math.pi * k / n), BED_H)
            for k in range(n)]
    mb.poly(ring, "grass", facing=(0, 0, 1))
    objs.append(mb.build("Ground_Lawn", coll))

    # floors inside the arcade loggia (Bldg_S2) and the archway passage (Bldg_S3)
    mb = MeshBuilder()
    grid_rect(mb, (0.45, -23.4, 11.55, -20.0), SIDEWALK_H, "paving")
    grid_rect(mb, (-5.7, -28.4, -2.3, -20.0), SIDEWALK_H, "cobble")
    objs.append(mb.build("Ground_Arcade_Floors", coll))

    for o in objs:
        if o.name != "Park_Curbs":
            set_props(o, no_shadow=1)
    return objs
