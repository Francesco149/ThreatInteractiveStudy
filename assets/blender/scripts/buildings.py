"""Row houses enclosing the square: facades with recessed openings, instanced window / door
modules, shopfronts (one with a see-through cafe interior), an arcade (portico), an archway
passage, roofs with ridge tiles, chimneys, cornices, string courses, quoins and a church tower.

Every facade is generated in a row-local frame: facade plane y = 0 facing -y (towards the
square), x along the row, z up; the building body occupies y in [0, depth].
"""
import math
import random

import bpy
from mathutils import Matrix, Vector

from common import (MeshBuilder, box, empty, get_coll, lathe, place, set_props, sweep_x, tube)

REVEAL = 0.22          # depth of window / door reveals
SHOP_REVEAL = 0.30


def row_matrix(x, y, rot_z):
    return Matrix.Translation((x, y, 0.0)) @ Matrix.Rotation(rot_z, 4, "Z")


ROWS = {
    "N": row_matrix(-22.0, 20.0, 0.0),            # faces south (-Y); local x -> +X
    "E": row_matrix(22.0, 26.0, -math.pi / 2),    # faces west (-X); local x -> -Y
    "S": row_matrix(22.0, -20.0, math.pi),        # faces north (+Y); local x -> -X
    "W": row_matrix(-22.0, -14.0, math.pi / 2),   # faces east (+X); local x -> +Y
    # background rows closing the street views
    "NB": row_matrix(15.0, 29.0, 0.0),            # end of the NE street, faces south
    "SWS": row_matrix(-16.0, -30.0, math.pi),     # south side of the SW street, faces north
    "SWW": row_matrix(-38.0, -30.0, math.pi / 2), # end of the SW street, faces east
}


# =============================================================== facade walls


def arch_points(cx, spring, r, n=12):
    return [Vector((cx + r * math.cos(math.pi - math.pi * k / n), 0.0,
                    spring + r * math.sin(math.pi - math.pi * k / n))) for k in range(n + 1)]


def wall_with_openings(mb, x0, x1, z0, z1, openings, mat, y=0.0, facing=-1, reveal_mat=None,
                       reveals=True):
    """Planar wall [x0,x1]x[z0,z1] at depth y with rectangular / arched holes.
    openings: dicts with x0,x1,z0,z1 (bounding box), kind 'rect'|'arch', depth (reveal)."""
    rmat = reveal_mat or mat
    fn = Vector((0, facing, 0))
    ops = [o for o in openings if o["x1"] > x0 and o["x0"] < x1 and o["z1"] > z0 and o["z0"] < z1]
    xs = sorted({x0, x1, *[min(max(o["x0"], x0), x1) for o in ops], *[min(max(o["x1"], x0), x1) for o in ops]})
    zs = sorted({z0, z1, *[min(max(o["z0"], z0), z1) for o in ops], *[min(max(o["z1"], z0), z1) for o in ops]})
    for i in range(len(xs) - 1):
        for j in range(len(zs) - 1):
            cx, cz = (xs[i] + xs[i + 1]) / 2, (zs[j] + zs[j + 1]) / 2
            if any(o["x0"] < cx < o["x1"] and o["z0"] < cz < o["z1"] for o in ops):
                continue
            mb.poly([(xs[i], y, zs[j]), (xs[i + 1], y, zs[j]), (xs[i + 1], y, zs[j + 1]), (xs[i], y, zs[j + 1])],
                    mat, facing=fn)
    for o in ops:
        d = o.get("depth", REVEAL) * (-facing)       # reveals go into the wall (away from viewer)
        ox0, ox1, oz0, oz1 = o["x0"], o["x1"], o["z0"], o["z1"]
        if o.get("kind") == "arch":
            r = (ox1 - ox0) / 2
            cx = (ox0 + ox1) / 2
            spring = oz1 - r
            P = arch_points(cx, spring, r)
            # spandrels between the arch curve and the bounding box top
            for k in range(len(P) - 1):
                q = []
                for p in (P[k], P[k + 1]):
                    dx, dz = p.x - cx, p.z - spring
                    ln = math.hypot(dx, dz)
                    ux, uz = dx / ln, dz / ln
                    t = min(r / abs(ux) if abs(ux) > 1e-6 else 1e9, r / uz if uz > 1e-6 else 1e9)
                    q.append(Vector((cx + ux * t, y, spring + uz * t)))
                pk, pk1 = Vector((P[k].x, y, P[k].z)), Vector((P[k + 1].x, y, P[k + 1].z))
                if (q[0] - pk).length < 1e-6 and (q[1] - pk1).length < 1e-6:
                    continue
                pts = [pk, pk1, q[1], q[0]]
                if (q[1] - q[0]).length < 1e-6:
                    pts = [pk, pk1, q[0]]
                mb.poly(pts, mat, facing=fn)
            if reveals:
                for k in range(len(P) - 1):
                    a, b = P[k], P[k + 1]
                    mid = (a + b) / 2
                    inward = Vector((cx - mid.x, 0, spring - mid.z))
                    mb.poly([(a.x, y, a.z), (b.x, y, b.z), (b.x, y + d, b.z), (a.x, y + d, a.z)], rmat, facing=inward)
                if spring > oz0:
                    mb.poly([(ox0, y, oz0), (ox0, y + d, oz0), (ox0, y + d, spring), (ox0, y, spring)], rmat, facing=(1, 0, 0))
                    mb.poly([(ox1, y, oz0), (ox1, y + d, oz0), (ox1, y + d, spring), (ox1, y, spring)], rmat, facing=(-1, 0, 0))
        elif reveals:
            mb.poly([(ox0, y, oz0), (ox0, y + d, oz0), (ox0, y + d, oz1), (ox0, y, oz1)], rmat, facing=(1, 0, 0))
            mb.poly([(ox1, y, oz0), (ox1, y + d, oz0), (ox1, y + d, oz1), (ox1, y, oz1)], rmat, facing=(-1, 0, 0))
            mb.poly([(ox0, y, oz1), (ox1, y, oz1), (ox1, y + d, oz1), (ox0, y + d, oz1)], rmat, facing=(0, 0, -1))
            if oz0 > z0 + 1e-4:
                mb.poly([(ox0, y, oz0), (ox1, y, oz0), (ox1, y + d, oz0), (ox0, y + d, oz0)], rmat, facing=(0, 0, 1))


# ============================================================ module meshes


class ModuleLib:
    """Builds each distinct window/door module once; placements share the mesh."""

    def __init__(self):
        self.cache = {}

    def get(self, key, fn):
        if key not in self.cache:
            self.cache[key] = fn().build_mesh("MOD_" + key)
        return self.cache[key]


def _shutter(mb, x0, x1, h, mat):
    """Louvred shutter leaf, open flat against the wall."""
    t = 0.05
    box(mb, (x0, -0.075, 0.02), (x0 + t, -0.035, h - 0.02), mat)
    box(mb, (x1 - t, -0.075, 0.02), (x1, -0.035, h - 0.02), mat)
    box(mb, (x0 + t, -0.075, 0.02), (x1 - t, -0.035, 0.02 + t), mat)
    box(mb, (x0 + t, -0.075, h - 0.02 - t), (x1 - t, -0.035, h - 0.02), mat)
    box(mb, (x0 + t, -0.07, h * 0.5 - 0.03), (x1 - t, -0.04, h * 0.5 + 0.03), mat)
    box(mb, (x0 + t, -0.045, 0.02), (x1 - t, -0.04, h - 0.02), mat, faces={"-y"})   # back board
    n = int((h - 0.2) / 0.07)
    for k in range(n):
        z = 0.02 + t + (h - 0.04 - 2 * t) * (k + 0.5) / n
        if abs(z - h * 0.5) < 0.05:
            continue
        # slat: a thin board tilted 40 degrees (outer edge lower)
        mb.poly([(x0 + t, -0.07, z - 0.025), (x1 - t, -0.07, z - 0.025), (x1 - t, -0.045, z + 0.012),
                 (x0 + t, -0.045, z + 0.012)], mat, facing=(0, -1, 1))
        mb.poly([(x0 + t, -0.07, z - 0.025), (x1 - t, -0.07, z - 0.025), (x1 - t, -0.045, z - 0.012 - 0.025),
                 (x0 + t, -0.045, z - 0.012 - 0.025)], mat, facing=(0, -1, -1))


def window_mesh(w, h, frame="paintedwood_white", glass="window_dark", trim="stone", shutter=None,
                hood="flat", balcony=False, rail="paint_iron_green", panes=True):
    mb = MeshBuilder()
    d = REVEAL
    fy0, fy1 = d - 0.10, d - 0.03
    t = 0.07
    box(mb, (-w / 2, fy0, 0), (-w / 2 + t, fy1, h), frame)
    box(mb, (w / 2 - t, fy0, 0), (w / 2, fy1, h), frame)
    box(mb, (-w / 2 + t, fy0, h - t), (w / 2 - t, fy1, h), frame)
    box(mb, (-w / 2 + t, fy0, 0), (w / 2 - t, fy1, t), frame)
    zt = h * 0.74 if h > 1.3 else None
    if zt:
        box(mb, (-w / 2 + t, fy0 + 0.01, zt - 0.035), (w / 2 - t, fy1 - 0.01, zt + 0.035), frame)
    box(mb, (-0.035, fy0 + 0.01, t), (0.035, fy1 - 0.01, (zt or h) - 0.03), frame)
    if panes:   # glazing bars on each casement
        top = (zt or h) - 0.035
        for k in (1, 2):
            z = t + (top - t) * k / 3
            box(mb, (-w / 2 + t, fy0 + 0.02, z - 0.015), (w / 2 - t, fy1 - 0.03, z + 0.015), frame)
    gy = (fy0 + fy1) / 2
    mb.poly([(-w / 2 + t, gy, t), (w / 2 - t, gy, t), (w / 2 - t, gy, h - t), (-w / 2 + t, gy, h - t)], glass,
            uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], facing=(0, -1, 0))
    # stone surround + sill + hood
    s = 0.14
    box(mb, (-w / 2 - s, -0.04, 0), (-w / 2, 0.0, h + s), trim)
    box(mb, (w / 2, -0.04, 0), (w / 2 + s, 0.0, h + s), trim)
    box(mb, (-w / 2, -0.04, h), (w / 2, 0.0, h + s), trim)
    if not balcony:
        box(mb, (-w / 2 - 0.1, -0.1, -0.09), (w / 2 + 0.1, d * 0.6, 0.0), trim)
        box(mb, (-w / 2 + 0.05, -0.06, -0.2), (-w / 2 + 0.15, 0.0, -0.09), trim)   # corbels
        box(mb, (w / 2 - 0.15, -0.06, -0.2), (w / 2 - 0.05, 0.0, -0.09), trim)
    if hood == "flat":
        box(mb, (-w / 2 - 0.22, -0.12, h + s), (w / 2 + 0.22, 0.0, h + s + 0.07), trim)
        box(mb, (-w / 2 - 0.26, -0.16, h + s + 0.07), (w / 2 + 0.26, 0.0, h + s + 0.12), trim)
    elif hood == "pediment":
        z0 = h + s
        hw = w / 2 + 0.3
        pk = 0.45
        box(mb, (-hw, -0.14, z0), (hw, 0.0, z0 + 0.08), trim)
        for yy, face in ((-0.14, (0, -1, 0)), (0.0, (0, 1, 0))):
            mb.poly([(-hw, yy, z0 + 0.08), (hw, yy, z0 + 0.08), (0, yy, z0 + 0.08 + pk)], trim, facing=face)
        mb.poly([(-hw, -0.14, z0 + 0.08), (0, -0.14, z0 + 0.08 + pk), (0, 0, z0 + 0.08 + pk), (-hw, 0, z0 + 0.08)], trim,
                facing=(-pk, 0, hw))
        mb.poly([(hw, -0.14, z0 + 0.08), (0, -0.14, z0 + 0.08 + pk), (0, 0, z0 + 0.08 + pk), (hw, 0, z0 + 0.08)], trim,
                facing=(pk, 0, hw))
    elif hood == "keystone":
        box(mb, (-0.09, -0.07, h - 0.06), (0.09, 0.0, h + s + 0.1), trim)
    if shutter:
        sw = w / 2 + 0.02
        _shutter(mb, -w / 2 - s - sw, -w / 2 - s - 0.01, h, shutter)
        _shutter(mb, w / 2 + s + 0.01, w / 2 + s + sw, h, shutter)
    if balcony:
        bw = w / 2 + 0.5
        box(mb, (-bw, -0.75, -0.14), (bw, d * 0.6, 0.0), trim)
        box(mb, (-bw - 0.03, -0.78, -0.18), (bw + 0.03, -0.0, -0.14), trim)
        for sx in (-1, 1):
            box(mb, (sx * (w / 2 + 0.25) - 0.07, -0.6, -0.5), (sx * (w / 2 + 0.25) + 0.07, 0.0, -0.18), trim)
        rz = 0.95
        fy = -0.70
        box(mb, (-bw + 0.04, fy - 0.03, rz), (bw - 0.04, fy + 0.03, rz + 0.05), rail)
        box(mb, (-bw + 0.04, fy - 0.02, 0.08), (bw - 0.04, fy + 0.02, 0.11), rail)
        for sx in (-1, 1):
            x = sx * (bw - 0.07)
            box(mb, (x - 0.03, fy, rz), (x + 0.03, 0.0, rz + 0.05), rail)
            box(mb, (x - 0.02, fy, 0.08), (x + 0.02, 0.0, 0.11), rail)
        n = int((2 * bw - 0.1) / 0.12)
        for k in range(n + 1):
            x = -bw + 0.07 + k * (2 * bw - 0.14) / n
            box(mb, (x - 0.012, fy - 0.012, 0.0), (x + 0.012, fy + 0.012, rz), rail)
        for sx in (-1, 1):
            x = sx * (bw - 0.07)
            for k in range(1, 6):
                yy = fy + k * (-fy) / 6
                box(mb, (x - 0.012, yy - 0.012, 0.0), (x + 0.012, yy + 0.012, rz), rail)
    return mb


def door_mesh(w=1.3, h=2.8, leaf="paintedwood_green", trim="stone", frame="paintedwood_white",
              glass="window_dark", hood="cornice"):
    mb = MeshBuilder()
    d = REVEAL
    step_h = 0.16
    # step
    box(mb, (-w / 2 - 0.25, -0.42, 0), (w / 2 + 0.25, d, step_h), "stone_grey")
    tr_h = 0.55
    zl = h - tr_h
    ly0, ly1 = d - 0.09, d - 0.03
    # leaves with raised panels
    for sx in (-1, 1):
        xa, xb = (-w / 2, 0.0) if sx < 0 else (0.0, w / 2)
        box(mb, (xa + 0.01, ly0, step_h), (xb - 0.01, ly1, zl - 0.06), leaf)
        for (za, zb) in ((step_h + 0.15, step_h + 0.85), (step_h + 1.0, zl - 0.2)):
            box(mb, (xa + 0.1, ly0 - 0.025, za), (xb - 0.1, ly0, zb), leaf)
        hx = -0.08 if sx < 0 else 0.08
        box(mb, (hx - 0.015, ly0 - 0.07, 1.05), (hx + 0.015, ly0, 1.2), "gold")
    # transom frame + glass
    box(mb, (-w / 2, ly0, zl - 0.06), (w / 2, ly1, zl), frame)
    box(mb, (-w / 2, ly0, h - 0.05), (w / 2, ly1, h), frame)
    for x in (-w / 2, -0.02, w / 2 - 0.04):
        box(mb, (x, ly0, zl), (x + 0.04, ly1, h - 0.05), frame)
    mb.poly([(-w / 2, (ly0 + ly1) / 2, zl), (w / 2, (ly0 + ly1) / 2, zl), (w / 2, (ly0 + ly1) / 2, h),
             (-w / 2, (ly0 + ly1) / 2, h)], glass, uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], facing=(0, -1, 0))
    # surround
    s = 0.2
    box(mb, (-w / 2 - s, -0.06, 0), (-w / 2, 0.0, h + s), trim)
    box(mb, (w / 2, -0.06, 0), (w / 2 + s, 0.0, h + s), trim)
    box(mb, (-w / 2, -0.06, h), (w / 2, 0.0, h + s), trim)
    box(mb, (-0.12, -0.1, h - 0.08), (0.12, 0.0, h + s + 0.06), trim)        # keystone
    if hood == "cornice":
        box(mb, (-w / 2 - 0.35, -0.18, h + s), (w / 2 + 0.35, 0.0, h + s + 0.1), trim)
        box(mb, (-w / 2 - 0.4, -0.24, h + s + 0.1), (w / 2 + 0.4, 0.0, h + s + 0.17), trim)
    return mb


def shopfront_mesh(W, H, frame="paintedwood_green", glass="window_dark", fascia=None, awning=False,
                   reveal=SHOP_REVEAL, interior=False):
    """Shop opening W x H (z from 0): glazed door in the middle, two display windows on
    stall risers, pilasters, fascia board and an optional striped awning."""
    mb = MeshBuilder()
    fascia = fascia or frame
    d = reveal
    fy0, fy1 = d - 0.1, d - 0.02
    gy = (fy0 + fy1) / 2
    dw = 1.0
    riser = 0.55
    t = 0.08
    # outer frame
    box(mb, (-W / 2, fy0, 0), (-W / 2 + t, fy1, H), frame)
    box(mb, (W / 2 - t, fy0, 0), (W / 2, fy1, H), frame)
    box(mb, (-W / 2, fy0, H - t), (W / 2, fy1, H), frame)
    # door posts
    for x in (-dw / 2 - t, dw / 2):
        box(mb, (x, fy0, 0), (x + t, fy1, H - t), frame)
    # risers + transom bar
    for sx in (-1, 1):
        xa, xb = (-W / 2 + t, -dw / 2 - t) if sx < 0 else (dw / 2 + t, W / 2 - t)
        box(mb, (xa, fy0 - 0.02, 0), (xb, fy1, riser), frame)
        box(mb, (xa + 0.08, fy0 - 0.04, 0.1), (xb - 0.08, fy0 - 0.02, riser - 0.1), frame)
        box(mb, (xa, fy0, H - 0.6), (xb, fy1, H - 0.54), frame)
        mb.poly([(xa, gy, riser), (xb, gy, riser), (xb, gy, H - t), (xa, gy, H - t)], glass,
                uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], facing=(0, -1, 0))
    # glazed door
    box(mb, (-dw / 2, fy0, 0), (dw / 2, fy1, 0.25), frame)
    box(mb, (-dw / 2, fy0, H - 0.65), (dw / 2, fy1, H - t), frame)
    box(mb, (-dw / 2, fy0, 0.25), (-dw / 2 + 0.1, fy1, H - 0.65), frame)
    box(mb, (dw / 2 - 0.1, fy0, 0.25), (dw / 2, fy1, H - 0.65), frame)
    mb.poly([(-dw / 2 + 0.1, gy, 0.25), (dw / 2 - 0.1, gy, 0.25), (dw / 2 - 0.1, gy, H - 0.65), (-dw / 2 + 0.1, gy, H - 0.65)],
            glass, uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], facing=(0, -1, 0))
    box(mb, (dw / 2 - 0.2, fy0 - 0.06, 1.0), (dw / 2 - 0.17, fy0, 1.3), "gold")
    # pilasters + fascia + cornice on the facade
    box(mb, (-W / 2 - 0.32, -0.1, 0), (-W / 2, 0.0, H + 0.7), frame)
    box(mb, (W / 2, -0.1, 0), (W / 2 + 0.32, 0.0, H + 0.7), frame)
    box(mb, (-W / 2, -0.08, H), (W / 2, 0.0, H + 0.6), fascia)
    box(mb, (-W / 2 - 0.4, -0.2, H + 0.6), (W / 2 + 0.4, 0.0, H + 0.72), frame)
    box(mb, (-W / 2 - 0.36, -0.14, -0.0), (-W / 2 + 0.04, 0.0, 0.25), "stone_grey")
    box(mb, (W / 2 - 0.04, -0.14, -0.0), (W / 2 + 0.36, 0.0, 0.25), "stone_grey")
    if awning:
        aw = W / 2 + 0.25
        z_top, z_bot, depth = H + 0.55, H - 0.45, 1.7
        segs = 6
        rows = []
        for i in range(segs + 1):
            f = i / segs
            sag = 0.08 * math.sin(math.pi * f)
            rows.append([Vector((-aw, -depth * f, z_top + (z_bot - z_top) * f - sag)),
                         Vector((aw, -depth * f, z_top + (z_bot - z_top) * f - sag))])
        L = math.hypot(depth, z_top - z_bot)
        us = [-aw / 2.0, aw / 2.0]
        vs = [L * i / segs / 2.0 for i in range(segs + 1)]
        for i in range(segs):
            a0, a1 = rows[i]
            b0, b1 = rows[i + 1]
            mb.poly([a0, a1, b1, b0], "cloth_stripes",
                    uvs=[(us[0], vs[i]), (us[1], vs[i]), (us[1], vs[i + 1]), (us[0], vs[i + 1])], facing=(0, -0.5, 1))
        e0, e1 = rows[-1]
        n = 10   # scalloped valance
        for k in range(n):
            xa = -aw + 2 * aw * k / n
            xb = -aw + 2 * aw * (k + 1) / n
            mb.poly([(xa, e0.y, e0.z), (xb, e0.y, e0.z), (xb, e0.y, e0.z - 0.22), ((xa + xb) / 2, e0.y, e0.z - 0.3),
                     (xa, e0.y, e0.z - 0.22)], "cloth_stripes",
                    uvs=[(xa / 2, 0), (xb / 2, 0), (xb / 2, -0.11), ((xa + xb) / 4, -0.15), (xa / 2, -0.11)], facing=(0, -1, 0))
        for sx in (-1, 1):   # iron arms
            tube(mb, [(sx * (aw - 0.05), 0.0, z_top - 0.9), (sx * (aw - 0.05), -depth * 0.98, z_bot - 0.02)],
                 [0.015, 0.015], "iron", sides=5)
    if interior:
        # see-through cafe: a lit room behind the glass
        iw, idp, ih = W / 2 + 0.6, 4.2, H + 0.5
        y0 = d
        mb.poly([(-iw, y0, 0.01), (iw, y0, 0.01), (iw, idp, 0.01), (-iw, idp, 0.01)], "wood", facing=(0, 0, 1))
        mb.poly([(-iw, idp, 0), (iw, idp, 0), (iw, idp, ih), (-iw, idp, ih)], "plaster_cream", facing=(0, -1, 0))
        mb.poly([(-iw, y0, ih), (iw, y0, ih), (iw, idp, ih), (-iw, idp, ih)], "plaster_white", facing=(0, 0, -1))
        for sx in (-1, 1):
            mb.poly([(sx * iw, y0, 0), (sx * iw, idp, 0), (sx * iw, idp, ih), (sx * iw, y0, ih)], "plaster_cream",
                    facing=(-sx, 0, 0))
        # inner face of the front wall around the opening
        for (xa, xb) in ((-iw, -W / 2), (W / 2, iw)):
            mb.poly([(xa, y0, 0), (xb, y0, 0), (xb, y0, ih), (xa, y0, ih)], "plaster_cream", facing=(0, 1, 0))
        mb.poly([(-W / 2, y0, H), (W / 2, y0, H), (W / 2, y0, ih), (-W / 2, y0, ih)], "plaster_cream", facing=(0, 1, 0))
        # counter, shelves, bottles
        box(mb, (-W / 2 + 0.2, idp - 1.3, 0), (W / 2 - 0.6, idp - 0.7, 1.05), "wood_dark")
        box(mb, (-W / 2 + 0.15, idp - 1.35, 1.05), (W / 2 - 0.55, idp - 0.65, 1.1), "marble")
        for z in (1.3, 1.75, 2.2):
            box(mb, (-W / 2, idp - 0.3, z), (W / 2, idp, z + 0.04), "wood_dark")
            for k in range(14):
                x = -W / 2 + 0.2 + k * (W - 0.4) / 13
                gm = ("glass_green", "glass_amber", "glass_clear")[k % 3]
                lathe(mb, [(0.0, 0.0), (0.035, 0.0), (0.035, 0.2), (0.012, 0.27), (0.012, 0.32), (0.0, 0.32)],
                      gm, segs=8, center=(x, idp - 0.15, z + 0.04))
        # interior lamp shades
        for x in (-W / 4, W / 4):
            lathe(mb, [(0.0, -0.3), (0.25, -0.3), (0.08, -0.12), (0.02, -0.1), (0.02, 0.0)], "gold",
                  segs=12, center=(x, 1.8, ih))
            lathe(mb, [(0.0, -0.32), (0.08, -0.3), (0.0, -0.22)], "lamp_bulb", segs=8, center=(x, 1.8, ih))
    return mb


# ============================================================== houses


def roof_and_gables(mb, x0, x1, depth, H, pitch, wall_mat, ov=0.45, side_ov=0.12, thick=0.2):
    tp = math.tan(math.radians(pitch))
    zR = H + depth / 2 * tp
    ym = depth / 2
    xa, xb = x0 - side_ov, x1 + side_ov
    for front in (True, False):
        if front:
            ye, ze = -ov, H - ov * tp
        else:
            ye, ze = depth + ov, H - ov * tp
        slope = math.hypot(ym - ye, zR - ze)
        face = Vector((0, -1 if front else 1, 1 / max(tp, 1e-3)))
        top = [(xa, ye, ze), (xb, ye, ze), (xb, ym, zR), (xa, ym, zR)]
        uv = [(xa / 2.4, 0), (xb / 2.4, 0), (xb / 2.4, slope / 2.4), (xa / 2.4, slope / 2.4)]
        mb.poly(top, "roof", uvs=uv, facing=face)
        bot = [(x, y, z - thick) for (x, y, z) in top]
        mb.poly(bot, "wood_dark", facing=-face)
        mb.poly([(xa, ye, ze), (xb, ye, ze), (xb, ye, ze - thick), (xa, ye, ze - thick)], "wood_dark",
                facing=(0, -1 if front else 1, 0))
        for x, sx in ((xa, -1), (xb, 1)):
            mb.poly([(x, ye, ze), (x, ym, zR), (x, ym, zR - thick), (x, ye, ze - thick)], "wood_dark", facing=(sx, 0, 0))
    # gable walls
    for x, sx in ((x0, -1), (x1, 1)):
        mb.poly([(x, 0, H), (x, depth, H), (x, ym, zR - thick)], wall_mat, facing=(sx, 0, 0))
    # ridge tiles
    tube(mb, [(xa - 0.02, ym, zR + 0.02), (xb + 0.02, ym, zR + 0.02)], [0.13, 0.13], "roof", sides=8)
    return zR


def dormer(mb, xc, yd, H, tp, wall, roof_mat="roof", w=1.5, h=1.45):
    """Gabled dormer on the front roof slope (roof z = H + y * tp). Returns the window
    opening (x, y, z) for a window module."""
    z0 = H + yd * tp
    hw = w / 2
    ze, zr = z0 + h, z0 + h + hw * 1.0 + 0.15
    win = dict(x0=xc - 0.38, x1=xc + 0.38, z0=z0 + 0.3, z1=z0 + 1.2, depth=REVEAL)
    mb.push(Matrix.Translation((0, yd, 0)))
    wall_with_openings(mb, xc - hw, xc + hw, z0, ze, [win], wall)
    mb.pop()
    mb.poly([(xc - hw, yd, ze), (xc + hw, yd, ze), (xc, yd, zr - 0.12)], wall, facing=(0, -1, 0))
    y_back = (ze - H) / tp
    for sx in (-1, 1):
        x = xc + sx * hw
        mb.poly([(x, yd, z0), (x, yd, ze), (x, y_back, ze)], wall, facing=(sx, 0, 0))
    # dormer roof: two planes at 45 degrees running back until they meet the main roof
    for sx in (-1, 1):
        xe = xc + sx * (hw + 0.15)
        ze_o = zr - (hw + 0.15)
        ya = yd - 0.18
        yb_e = (ze_o - H) / tp
        yb_r = (zr - H) / tp
        pts = [(xe, ya, ze_o), (xc, ya, zr), (xc, yb_r, zr), (xe, yb_e, ze_o)]
        L = math.hypot(hw + 0.15, hw + 0.15)
        uvs = [(ya / 2.4, 0), (ya / 2.4, L / 2.4), (yb_r / 2.4, L / 2.4), (yb_e / 2.4, 0)]
        mb.poly(pts, roof_mat, uvs=uvs, facing=(sx, 0, 1))
        mb.poly([(xe, ya, ze_o), (xc, ya, zr), (xc, ya, zr - 0.12), (xe, ya, ze_o - 0.12)], "wood_dark", facing=(0, -1, 0))
        mb.poly([(xe, ya, ze_o - 0.12), (xc, ya, zr - 0.12), (xc, yb_r, zr - 0.12), (xe, yb_e, ze_o - 0.12)], "wood_dark",
                facing=(-sx, 0, -1))
    return (xc, yd, z0 + 0.3)


def chimney(mb, x, y, z_base, z_top, mat, w=0.7, d=0.55):
    box(mb, (x - w / 2, y - d / 2, z_base), (x + w / 2, y + d / 2, z_top), mat, faces={"+x", "-x", "+y", "-y"})
    box(mb, (x - w / 2 - 0.06, y - d / 2 - 0.06, z_top), (x + w / 2 + 0.06, y + d / 2 + 0.06, z_top + 0.1), "stone")
    for dx in (-0.15, 0.15):
        lathe(mb, [(0.09, 0), (0.1, 0.05), (0.08, 0.3), (0.095, 0.34), (0.06, 0.34), (0.06, 0.1)], "terracotta",
              segs=10, center=(x + dx, y, z_top + 0.1))
    return Vector((x, y, z_top + 0.5))


STRING = [(0, 0), (0.06, 0.0), (0.06, 0.1), (0.035, 0.14), (0, 0.16)]
CORNICE = [(0, 0), (0.05, 0.0), (0.05, 0.06), (0.1, 0.1), (0.12, 0.18), (0.2, 0.24), (0.3, 0.3), (0.34, 0.36),
           (0.34, 0.42), (0, 0.42)]


def house(spec, mods, coll_b, coll_m, rng):
    """Build one row house. spec: see HOUSES."""
    F = ROWS[spec["row"]]
    x0, w = spec["x"], spec["w"]
    x1 = x0 + w
    depth = spec.get("depth", 9.0)
    floors = spec["floors"]
    gf_h, fl_h = spec.get("gf_h", 4.0), spec.get("fl_h", 3.3)
    H = gf_h + (floors - 1) * fl_h + 0.7
    wall = spec["wall"]
    trim = spec.get("trim", "stone")
    frame = spec.get("frame", "paintedwood_white")
    shutter = spec.get("shutter")
    cols = spec.get("cols", max(2, int(w / 2.2)))
    xc = [x0 + w * (i + 0.5) / cols for i in range(cols)]
    lit_p = spec.get("lit", 0.18)

    origin = F @ Vector(((x0 + x1) / 2, 0, 0))
    flower_boxes = []
    mb = MeshBuilder()
    mb.push(Matrix.Translation(-origin) @ F)
    openings, placements = [], []

    def add_window(x, z, ww, hh, key, fn):
        openings.append(dict(x0=x - ww / 2, x1=x + ww / 2, z0=z, z1=z + hh, depth=REVEAL))
        placements.append((key, fn, x, z))

    def win(x, z, ww=1.05, hh=1.65, hood="flat", balcony=False, sh=shutter, small=False):
        lit = rng.random() < lit_p
        glass = "window_lit" if lit else "window_dark"
        key = f"win_{ww:.2f}x{hh:.2f}_{frame}_{glass}_{trim}_{sh}_{hood}_{int(balcony)}"
        add_window(x, z, ww, hh, key, lambda: window_mesh(ww, hh, frame, glass, trim, sh, hood, balcony))

    # ---- ground floor
    gf = spec.get("gf", "door")
    door_col = spec.get("door_col", cols // 2)
    shop_w = spec.get("shop_w", min(w - 2.0, 4.2))
    if gf in ("shop", "cafe"):
        sx = spec.get("shop_x", (x0 + x1) / 2)
        sh_h = spec.get("shop_h", 3.0)
        openings.append(dict(x0=sx - shop_w / 2, x1=sx + shop_w / 2, z0=0, z1=sh_h, depth=SHOP_REVEAL))
        cafe = gf == "cafe"
        key = f"shop_{spec['name']}"
        fr = spec.get("shop_frame", "paintedwood_green")
        placements.append((key, lambda: shopfront_mesh(shop_w, sh_h, fr, "glass_clear" if cafe else "window_dark",
                                                       awning=spec.get("awning", False), interior=cafe), sx, 0))
        for x in xc:
            if abs(x - sx) > shop_w / 2 + 0.9:
                win(x, 1.1, 1.0, 1.75, sh=None)
    elif gf in ("door", "windows"):
        for i, x in enumerate(xc):
            if gf == "door" and i == door_col:
                dm = spec.get("door", "paintedwood_green")
                openings.append(dict(x0=x - 0.68, x1=x + 0.68, z0=0, z1=2.85, depth=REVEAL))
                placements.append((f"door_{dm}_{trim}_{frame}", lambda dm=dm: door_mesh(1.36, 2.85, dm, trim, frame), x, 0))
            else:
                win(x, 1.1, 1.05, 1.8, hood="keystone", sh=None)
    elif gf == "arcade":
        n_arch = spec.get("arches", 3)
        pier_end, pier = 0.85, 0.7
        span = (w - 2 * pier_end - (n_arch - 1) * pier) / n_arch
        ax = x0 + pier_end
        for k in range(n_arch):
            openings.append(dict(x0=ax, x1=ax + span, z0=0, z1=2.9 + span / 2, depth=0.6, kind="arch"))
            ax += span + pier
    elif gf == "passage":
        pw = spec.get("passage_w", 3.4)
        pc = spec.get("passage_x", (x0 + x1) / 2)
        openings.append(dict(x0=pc - pw / 2, x1=pc + pw / 2, z0=0, z1=2.6 + pw / 2, depth=depth - 0.6, kind="arch"))
        for x in xc:
            if abs(x - pc) > pw / 2 + 1.0:
                win(x, 1.1, 1.0, 1.7, hood="keystone", sh=None)

    # ---- upper floors
    balc = spec.get("balcony", {})
    for f in range(1, floors):
        zf = gf_h + (f - 1) * fl_h
        top = f == floors - 1
        for i, x in enumerate(xc):
            if f in balc and i in balc[f]:
                win(x, zf + 0.08, 1.1, 2.45, hood="pediment" if spec.get("grand") else "flat", balcony=True)
            elif top and spec.get("small_top"):
                win(x, zf + 0.9, 0.9, 1.25, hood=None)
            else:
                hood = "pediment" if (f == 1 and spec.get("grand")) else ("flat" if f == 1 else "keystone")
                win(x, zf + 0.85, 1.05, 1.7, hood=hood)
                if f == 1 and wall != "brick" and rng.random() < spec.get("flowers", 0.45):
                    flower_boxes.append(F @ Matrix.Translation((x, 0.0, zf + 0.85)))

    # ---- walls
    reveal_mat = trim if wall == "brick" else wall
    wall_with_openings(mb, x0, x1, 0, H, openings, wall, reveal_mat=reveal_mat)
    for x, sx in ((x0, -1), (x1, 1)):
        mb.poly([(x, 0, 0), (x, depth, 0), (x, depth, H), (x, 0, H)], wall, facing=(sx, 0, 0))
    mb.poly([(x0, depth, 0), (x1, depth, 0), (x1, depth, H), (x0, depth, H)], wall, facing=(0, 1, 0))

    # arcade / passage interiors
    if gf == "arcade":
        ld = spec.get("loggia_depth", 3.4)
        ceil = gf_h - 0.35
        ow = [o for o in openings if o.get("kind") == "arch"]
        wall_with_openings(mb, x0 + 0.45, x1 - 0.45, 0, ceil, ow, wall, y=0.6, facing=1, reveals=False)
        mb.poly([(x0 + 0.45, 0.6, ceil), (x1 - 0.45, 0.6, ceil), (x1 - 0.45, ld, ceil), (x0 + 0.45, ld, ceil)],
                "plaster_white", facing=(0, 0, -1))
        for k in range(int((w - 0.9) / 1.4) + 1):     # ceiling beams
            bx = x0 + 0.6 + k * 1.4
            if bx < x1 - 0.5:
                box(mb, (bx, 0.6, ceil - 0.22), (bx + 0.18, ld, ceil), "wood_dark", faces={"-z", "+x", "-x"})
        for x, sx in ((x0 + 0.45, 1), (x1 - 0.45, -1)):
            mb.poly([(x, 0.6, 0), (x, ld, 0), (x, ld, ceil), (x, 0.6, ceil)], wall, facing=(sx, 0, 0))
        back = []
        for x in xc:
            back.append(dict(x0=x - 0.6, x1=x + 0.6, z0=0, z1=2.7, depth=REVEAL))
        wall_with_openings(mb, x0 + 0.45, x1 - 0.45, 0, ceil, back, "plaster_cream", y=ld, facing=-1)
        for i, x in enumerate(xc):
            if i % 2 == 0:
                placements.append((f"door_paintedwood_brown_{trim}_{frame}_a",
                                   lambda: door_mesh(1.2, 2.7, "paintedwood_brown", trim, frame), x, 0, ld))
            else:
                placements.append(("shop_arcade_small",
                                   lambda: shopfront_mesh(1.2, 2.7, "paintedwood_brown", "window_lit", reveal=REVEAL), x, 0, ld))
        # arch archivolts + keystones on the facade
        for o in ow:
            r = (o["x1"] - o["x0"]) / 2
            cx = (o["x0"] + o["x1"]) / 2
            spring = o["z1"] - r
            P = arch_points(cx, spring, r, 16)
            Q = arch_points(cx, spring, r + 0.22, 16)
            for k in range(16):
                mb.poly([(P[k].x, -0.05, P[k].z), (P[k + 1].x, -0.05, P[k + 1].z), (Q[k + 1].x, -0.05, Q[k + 1].z),
                         (Q[k].x, -0.05, Q[k].z)], trim, facing=(0, -1, 0))
                mb.poly([(Q[k].x, -0.05, Q[k].z), (Q[k + 1].x, -0.05, Q[k + 1].z), (Q[k + 1].x, 0, Q[k + 1].z),
                         (Q[k].x, 0, Q[k].z)], trim, facing=(Q[k].x - cx, 0, Q[k].z - spring))
                mb.poly([(P[k].x, -0.05, P[k].z), (P[k + 1].x, -0.05, P[k + 1].z), (P[k + 1].x, 0, P[k + 1].z),
                         (P[k].x, 0, P[k].z)], trim, facing=(cx - P[k].x, 0, spring - P[k].z))
            box(mb, (cx - 0.13, -0.1, spring + r - 0.08), (cx + 0.13, 0.0, spring + r + 0.3), trim)
            for sx in (o["x0"], o["x1"]):    # impost blocks
                box(mb, (sx - 0.1, -0.08, spring - 0.18), (sx + 0.1, 0.0, spring), trim)
    if gf == "passage":
        o = [o for o in openings if o.get("kind") == "arch"][0]
        r = (o["x1"] - o["x0"]) / 2
        cx = (o["x0"] + o["x1"]) / 2
        spring = o["z1"] - r
        gy = o["depth"]
        P = arch_points(cx, spring, r, 12)
        gate = [Vector((o["x0"], gy, 0))] + [Vector((p.x, gy, p.z)) for p in P] + [Vector((o["x1"], gy, 0))]
        mb.poly(gate, "paintedwood_brown", facing=(0, -1, 0), tile=1.2)
        box(mb, (cx - 0.03, gy - 0.05, 0), (cx + 0.03, gy, spring + r - 0.05), "iron")
        for z in (0.5, 1.6, 2.6):
            box(mb, (o["x0"] + 0.05, gy - 0.04, z), (o["x1"] - 0.05, gy, z + 0.08), "iron")
        Q = arch_points(cx, spring, r + 0.25, 12)
        for k in range(12):
            mb.poly([(P[k].x, -0.06, P[k].z), (P[k + 1].x, -0.06, P[k + 1].z), (Q[k + 1].x, -0.06, Q[k + 1].z),
                     (Q[k].x, -0.06, Q[k].z)], trim, facing=(0, -1, 0))
            mb.poly([(Q[k].x, -0.06, Q[k].z), (Q[k + 1].x, -0.06, Q[k + 1].z), (Q[k + 1].x, 0, Q[k + 1].z),
                     (Q[k].x, 0, Q[k].z)], trim, facing=(Q[k].x - cx, 0, Q[k].z - spring))
        box(mb, (cx - 0.15, -0.12, spring + r - 0.1), (cx + 0.15, 0.0, spring + r + 0.35), trim)

    # ---- trims
    plinth_h = 0.6 if gf not in ("arcade",) else 0.0
    if plinth_h:
        xs = [x0] + sorted([v for o in openings if o["z0"] < 0.01 for v in (o["x0"], o["x1"])]) + [x1]
        for a, b in zip(xs[0::2], xs[1::2]):
            if b - a > 0.05:
                box(mb, (a, -0.06, 0), (b, 0.0, plinth_h), "stone_grey", faces={"-y", "+z", "-x", "+x"})
    for f in range(1, floors):
        zf = gf_h + (f - 1) * fl_h - 0.08
        sweep_x(mb, STRING, x0, x1, trim, z=zf)
    sweep_x(mb, CORNICE, x0 - 0.02, x1 + 0.02, spec.get("cornice", trim), z=H - 0.42)
    if spec.get("quoins"):
        z = plinth_h
        k = 0
        while z < H - 0.6:
            for xe, sgn in ((x0, 1), (x1, -1)):
                ln = 0.55 if k % 2 == 0 else 0.32
                xa, xb = (xe, xe + ln) if sgn > 0 else (xe - ln, xe)
                box(mb, (xa, -0.035, z + 0.01), (xb, 0.0, z + 0.31), trim, faces={"-y", "+z", "-z", "-x", "+x"})
            z += 0.32
            k += 1

    # ---- roof (+ dormers on the front slope)
    pitch = spec.get("pitch", 38)
    zR = roof_and_gables(mb, x0, x1, depth, H, pitch, wall)
    for i in spec.get("dormers", []):
        dx, dy, dz = dormer(mb, xc[i], 1.1, H, math.tan(math.radians(pitch)), wall)
        key = f"win_0.76x0.90_{frame}_window_dark_{trim}_dormer"
        placements.append((key, lambda: window_mesh(0.76, 0.9, frame, "window_dark", trim, None, None, panes=True),
                           dx, dz, dy))
    vfx = []
    for (cxr, cyr, smoke) in spec.get("chimneys", []):
        cx_ = x0 + cxr * w
        cy_ = depth * cyr
        top = chimney(mb, cx_, cy_, H, zR + 1.0, spec.get("chimney_mat", wall if wall != "brick" else "brick"))
        if smoke:
            vfx.append(top)
    mb.pop()
    obj = mb.build(spec["name"], coll_b)
    obj.location = origin
    set_props(obj, kind="building")

    # ---- place modules
    for p in placements:
        key, fn, x, z = p[:4]
        yoff = p[4] if len(p) > 4 else 0.0
        me = mods.get(key, fn)
        M = F @ Matrix.Translation((x, yoff, z))
        place(me, f"{spec['name']}_{key.split('_')[0]}", M, coll_m)
    return obj, [F @ v for v in vfx], dict(H=H, zR=zR, flower_boxes=flower_boxes)


HOUSES = [
    # north row (faces south), x from -22 to 15
    dict(name="Bldg_N1", row="N", x=0.0, w=8.0, floors=3, wall="plaster_yellow", shutter="paintedwood_green",
         cols=3, gf="door", door_col=1, balcony={1: [1]}, chimneys=[(0.25, 0.6, True)], door="paintedwood_green",
         dormers=[0, 2]),
    dict(name="Bldg_N2", row="N", x=8.0, w=7.0, floors=3, wall="brick", quoins=True, cols=3, gf="shop",
         awning=True, shop_w=4.0, shop_frame="paintedwood_red", fl_h=3.2, chimneys=[(0.7, 0.4, False)]),
    dict(name="Bldg_N3", row="N", x=15.0, w=9.5, floors=3, wall="plaster_cream", shutter="paintedwood_blue",
         gf_h=4.6, fl_h=3.7, cols=4, gf="door", door_col=1, grand=True, balcony={1: [1, 2]},
         chimneys=[(0.2, 0.5, False), (0.8, 0.5, True)], pitch=34, door="paintedwood_brown"),
    dict(name="Bldg_N4", row="N", x=24.5, w=6.5, floors=3, wall="plaster_pink", shutter="paintedwood_green",
         cols=2, gf="door", door_col=0, small_top=True, chimneys=[(0.5, 0.65, False)], door="paintedwood_blue"),
    dict(name="Bldg_N5", row="N", x=31.0, w=6.0, floors=2, wall="plaster_sage", cols=2, gf="shop",
         shop_w=3.6, shop_frame="paintedwood_brown", gf_h=4.2, chimneys=[(0.5, 0.4, False)], pitch=42),
    # east row (faces west), local x from y=26 down to y=-20
    dict(name="Bldg_E1", row="E", x=0.0, w=8.0, floors=3, wall="plaster_ochre", shutter="paintedwood_brown",
         cols=3, gf="door", door_col=1, chimneys=[(0.5, 0.5, True)], door="paintedwood_red"),
    dict(name="Bldg_E2", row="E", x=8.0, w=7.0, floors=2, wall="brick", quoins=True, cols=3, gf="door",
         door_col=1, gf_h=4.2, chimneys=[(0.3, 0.6, False)], door="paintedwood_green", pitch=45, dormers=[0, 2]),
    dict(name="Bldg_E3", row="E", x=15.0, w=10.0, floors=3, wall="plaster_blue", shutter="paintedwood_white",
         frame="paintedwood_white", cols=4, gf="shop", shop_w=4.6, awning=True, shop_frame="paintedwood_blue",
         balcony={1: [0, 3]}, chimneys=[(0.5, 0.45, False)]),
    dict(name="Bldg_E4", row="E", x=25.0, w=7.0, floors=3, wall="plaster_yellow", shutter="paintedwood_blue",
         cols=3, gf="door", door_col=2, chimneys=[(0.6, 0.6, False)], door="paintedwood_brown"),
    dict(name="Bldg_E5", row="E", x=32.0, w=8.0, floors=2, wall="plaster_pink", shutter="paintedwood_green",
         cols=3, gf="door", door_col=0, gf_h=4.2, chimneys=[(0.4, 0.5, True)], door="paintedwood_blue", pitch=42,
         dormers=[1]),
    dict(name="Bldg_E6", row="E", x=40.0, w=6.0, floors=3, wall="brick", quoins=True, cols=2, gf="door",
         door_col=0, door="paintedwood_red", chimneys=[(0.5, 0.5, False)]),
    # south row (faces north), local x from x=22 towards -X
    dict(name="Bldg_S1", row="S", x=0.0, w=10.0, floors=3, wall="plaster_cream", shutter="paintedwood_green",
         cols=4, gf="cafe", shop_w=5.0, shop_h=3.0, awning=True, shop_frame="paintedwood_green",
         chimneys=[(0.3, 0.6, False)], lit=0.35),
    dict(name="Bldg_S2", row="S", x=10.0, w=12.0, floors=3, wall="plaster_white", trim="stone", cols=4,
         gf="arcade", gf_h=5.2, fl_h=3.6, arches=3, grand=True, balcony={1: [1, 2]}, shutter="paintedwood_brown",
         chimneys=[(0.25, 0.55, False), (0.75, 0.55, False)], pitch=32, cornice="stone"),
    dict(name="Bldg_S3", row="S", x=22.0, w=8.0, floors=2, wall="brick", quoins=True, cols=3, gf="passage",
         gf_h=4.8, passage_w=3.4, chimneys=[(0.7, 0.6, True)], pitch=44),
    dict(name="Bldg_S4", row="S", x=30.0, w=8.0, floors=2, wall="plaster_ochre", shutter="paintedwood_green",
         cols=3, gf="door", door_col=2, chimneys=[(0.4, 0.5, False)], door="paintedwood_green", dormers=[0, 1]),
    # west row (faces east), local x from y=-14 to y=20
    dict(name="Bldg_W1", row="W", x=0.0, w=8.0, floors=2, wall="plaster_pink", shutter="paintedwood_brown",
         cols=3, gf="door", door_col=1, chimneys=[(0.5, 0.5, False)], door="paintedwood_brown", pitch=40),
    dict(name="Bldg_W2", row="W", x=8.0, w=9.0, floors=2, wall="plaster_sage", shutter="paintedwood_white",
         cols=3, gf="shop", shop_w=4.0, awning=True, shop_frame="paintedwood_brown", chimneys=[(0.3, 0.5, True)],
         dormers=[0, 2]),
    dict(name="Bldg_W3", row="W", x=17.0, w=8.0, floors=2, wall="brick", quoins=True, cols=3, gf="door",
         door_col=0, door="paintedwood_green", chimneys=[(0.6, 0.5, False)], pitch=44),
    dict(name="Bldg_W4", row="W", x=25.0, w=9.0, floors=2, wall="plaster_yellow", shutter="paintedwood_blue",
         cols=3, gf="door", door_col=2, chimneys=[(0.5, 0.5, False)], door="paintedwood_red", pitch=40, dormers=[1]),
]


BG_HOUSES = [
    dict(name="Bldg_NE_End", row="NB", x=0.0, w=11.0, depth=6.5, floors=3, wall="plaster_ochre",
         shutter="paintedwood_green", cols=4, gf="door", door_col=1, chimneys=[(0.3, 0.5, False)], lit=0.25),
    dict(name="Bldg_SW_1", row="SWS", x=0.0, w=9.0, floors=2, wall="plaster_cream", shutter="paintedwood_blue",
         cols=3, gf="door", door_col=1, chimneys=[(0.5, 0.5, True)], lit=0.25),
    dict(name="Bldg_SW_2", row="SWS", x=9.0, w=8.0, floors=3, wall="brick", quoins=True, cols=3, gf="shop",
         shop_w=3.6, shop_frame="paintedwood_brown", chimneys=[(0.4, 0.6, False)], lit=0.25),
    dict(name="Bldg_SW_3", row="SWS", x=17.0, w=10.0, floors=2, wall="plaster_pink", shutter="paintedwood_green",
         cols=4, gf="door", door_col=2, chimneys=[(0.6, 0.5, False)], lit=0.25, pitch=42),
    dict(name="Bldg_SW_5", row="SWW", x=0.0, w=8.0, floors=3, wall="plaster_sage", shutter="paintedwood_brown",
         cols=3, gf="door", door_col=1, chimneys=[(0.5, 0.5, False)], lit=0.25),
    dict(name="Bldg_SW_6", row="SWW", x=8.0, w=8.0, floors=2, wall="plaster_blue", cols=3, gf="door",
         door_col=2, chimneys=[(0.4, 0.5, False)], lit=0.25, pitch=44),
]


def filler(mb, lo, hi, H, mat):
    """Corner block behind two rows: plain walls + flat roof with a parapet."""
    (x0, y0), (x1, y1) = lo, hi
    box(mb, (x0, y0, 0), (x1, y1, H), mat, faces={"+x", "-x", "+y", "-y"})
    mb.poly([(x0, y0, H - 0.6), (x1, y0, H - 0.6), (x1, y1, H - 0.6), (x0, y1, H - 0.6)], "stone_grey", facing=(0, 0, 1))
    box(mb, (x0 - 0.05, y0 - 0.05, H), (x1 + 0.05, y1 + 0.05, H + 0.1), "stone")


def church_tower(coll, coll_m):
    """Bell tower behind the north row: the skyline landmark."""
    mb = MeshBuilder()
    cx, cy, s = 0.0, 0.0, 6.0
    H = 24.0
    box(mb, (-s / 2, -s / 2, 0), (s / 2, s / 2, H), "stone", faces={"+x", "-x", "+y", "-y"})
    for z in (6.0, 12.0, 18.0):
        box(mb, (-s / 2 - 0.12, -s / 2 - 0.12, z), (s / 2 + 0.12, s / 2 + 0.12, z + 0.25), "stone_grey")
    # belfry with arched openings on every side
    bz0, bz1 = H, H + 5.0
    for rot in range(4):
        mb.push(Matrix.Rotation(rot * math.pi / 2, 4, "Z") @ Matrix.Translation((0, -s / 2, 0)))
        ops = [dict(x0=-1.9, x1=-0.3, z0=bz0 + 0.8, z1=bz0 + 4.0, depth=0.6, kind="arch"),
               dict(x0=0.3, x1=1.9, z0=bz0 + 0.8, z1=bz0 + 4.0, depth=0.6, kind="arch")]
        wall_with_openings(mb, -s / 2, s / 2, bz0, bz1, ops, "stone")
        wall_with_openings(mb, -s / 2 + 0.6, s / 2 - 0.6, bz0, bz1, ops, "stone", y=0.6, facing=1, reveals=False)
        mb.pop()
    mb.poly([(-s / 2 + 0.6, -s / 2 + 0.6, bz0 + 0.8), (s / 2 - 0.6, -s / 2 + 0.6, bz0 + 0.8),
             (s / 2 - 0.6, s / 2 - 0.6, bz0 + 0.8), (-s / 2 + 0.6, s / 2 - 0.6, bz0 + 0.8)], "stone_grey", facing=(0, 0, 1))
    box(mb, (-s / 2 - 0.2, -s / 2 - 0.2, bz1), (s / 2 + 0.2, s / 2 + 0.2, bz1 + 0.35), "stone_grey")
    # pyramid spire
    tz = bz1 + 0.35
    peak = Vector((0, 0, tz + 9.0))
    c = [Vector((-s / 2 - 0.3, -s / 2 - 0.3, tz)), Vector((s / 2 + 0.3, -s / 2 - 0.3, tz)),
         Vector((s / 2 + 0.3, s / 2 + 0.3, tz)), Vector((-s / 2 - 0.3, s / 2 + 0.3, tz))]
    for i in range(4):
        a, b = c[i], c[(i + 1) % 4]
        sl = (peak - (a + b) / 2).length
        mb.poly([a, b, peak], "roof", uvs=[(0, 0), ((b - a).length / 2.4, 0), ((b - a).length / 4.8, sl / 2.4)],
                facing=((a + b) / 2 - Vector((0, 0, tz))) + Vector((0, 0, 3)))
    # golden orb + cross
    lathe(mb, [(0.0, 0.0), (0.08, 0.0), (0.08, 0.3)], "gold", segs=8, center=(0, 0, peak.z - 0.1))
    prof = [(math.sin(math.pi * k / 10) * 0.28, -math.cos(math.pi * k / 10) * 0.28 + 0.28) for k in range(11)]
    lathe(mb, prof, "gold", segs=16, center=(0, 0, peak.z + 0.2))
    box(mb, (-0.05, -0.05, peak.z + 0.7), (0.05, 0.05, peak.z + 2.0), "gold")
    box(mb, (-0.4, -0.05, peak.z + 1.4), (0.4, 0.05, peak.z + 1.5), "gold")
    # clock faces (flat discs) on the shaft
    for rot in range(4):
        m = Matrix.Rotation(rot * math.pi / 2, 4, "Z") @ Matrix.Translation((0, -s / 2 - 0.02, 20.5)) @ Matrix.Rotation(math.pi / 2, 4, "X")
        mb.push(m)
        lathe(mb, [(0.0, 0.0), (1.2, 0.0), (1.2, 0.08), (1.05, 0.1)], "stone", segs=24, cap_top=True,
              cap_mat="plaster_white")
        mb.pop()
        mb.push(Matrix.Rotation(rot * math.pi / 2, 4, "Z") @ Matrix.Translation((0, -s / 2 - 0.15, 20.5)))
        box(mb, (-0.04, -0.02, -0.05), (0.04, 0.0, 0.75), "gold")
        box(mb, (-0.04, -0.03, -0.04), (0.55, -0.01, 0.04), "gold")
        mb.pop()
    o = mb.build("Church_Tower", coll)
    o.location = (-6.0, 36.0, 0.0)
    set_props(o, kind="building")
    return o


def build(rng_seed=3):
    rng = random.Random(rng_seed)
    coll_b = get_coll("Buildings")
    coll_m = get_coll("Building_Modules", coll_b)
    coll_fx = get_coll("VFX")
    mods = ModuleLib()
    info = {}
    smoke_n = 0
    for spec in HOUSES + BG_HOUSES:
        o, vfx, inf = house(spec, mods, coll_b, coll_m, rng)
        info[spec["name"]] = inf
        for v in vfx:
            smoke_n += 1
            empty(f"VFX_ChimneySmoke_{smoke_n:02d}", v, coll_fx, vfx="smoke")
    for name, lo, hi, H in (("Bldg_Corner_NW", (-31, 20), (-22, 29), 9.5), ("Bldg_Corner_SE", (22, -29), (31, -20), 11.0)):
        mb = MeshBuilder()
        filler(mb, lo, hi, H, "plaster_cream")
        o = mb.build(name, coll_b)
        set_props(o, kind="building")
    church_tower(coll_b, coll_m)
    return info
