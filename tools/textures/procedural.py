"""Procedurally generated textures for the courtyard scene (no source images needed).

  leaf_atlas_albedo.png / leaf_atlas_normal.png   2x2 atlas of leaf-cluster cards (RGBA, MASK)
  grass_atlas_albedo.png                          2x2 atlas of grass / flower clump cards (RGBA)
  skin_albedo.png / skin_normal.png / skin_orm.png  head texture for the cylindrical head UVs
  eye_albedo.png                                  eyeball (iris at the +V pole of a UV sphere)
  brass_albedo.png / brass_orm.png                tarnished brass / gilded metal

Alpha-tested atlases are drawn 4x supersampled, then colour is dilated into transparent texels
(push-pull) so mipmaps don't pick up dark fringes.

    uv run -q --with numpy --with pillow python -I tools/textures/procedural.py [names...]
"""
import math
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import texlib as T  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
DST = ROOT / "assets" / "scene" / "textures"

SS = 4  # supersampling for vector drawing


class Canvas:
    """RGB + alpha + height layers drawn with Pillow at SS x resolution."""

    def __init__(self, size):
        self.size = size
        n = size * SS
        self.rgb = Image.new("RGB", (n, n), (0, 0, 0))
        self.a = Image.new("L", (n, n), 0)
        self.h = Image.new("L", (n, n), 0)
        self.drgb, self.da, self.dh = (ImageDraw.Draw(i) for i in (self.rgb, self.a, self.h))

    def poly(self, pts, color, height=None, alpha=255):
        pts = [(x * SS, y * SS) for x, y in pts]
        self.drgb.polygon(pts, fill=tuple(int(c * 255) for c in color))
        self.da.polygon(pts, fill=alpha)
        if height is not None:
            self.dh.polygon(pts, fill=int(height * 255))

    def line(self, pts, color, width, height=None):
        pts = [(x * SS, y * SS) for x, y in pts]
        w = max(1, int(width * SS))
        self.drgb.line(pts, fill=tuple(int(c * 255) for c in color), width=w, joint="curve")
        self.da.line(pts, fill=255, width=w, joint="curve")
        if height is not None:
            self.dh.line(pts, fill=int(height * 255), width=w, joint="curve")

    def ellipse(self, cx, cy, rx, ry, color, height=None):
        box = [(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS]
        self.drgb.ellipse(box, fill=tuple(int(c * 255) for c in color))
        self.da.ellipse(box, fill=255)
        if height is not None:
            self.dh.ellipse(box, fill=int(height * 255))

    def arrays(self):
        rgb = T.supersample_down(np.asarray(self.rgb).astype(np.float32) / 255.0, SS)
        a = T.supersample_down(np.asarray(self.a).astype(np.float32) / 255.0, SS)
        h = T.supersample_down(np.asarray(self.h).astype(np.float32) / 255.0, SS)
        # un-premultiply the box-filtered colour at the antialiased edges
        rgb = rgb / np.maximum(a[..., None], 1e-3)
        return np.clip(rgb, 0, 1), a, h


def jitter_color(rng, base, amount):
    c = np.array(base, np.float32) * (1 + rng.normal(0, amount, 3)) * rng.uniform(0.85, 1.12)
    return tuple(np.clip(c, 0, 1))


def finish_alpha_atlas(cv, name, rng, normal_strength=None, mottle=0.12):
    rgb, a, h = cv.arrays()
    size = rgb.shape[0]
    # mottling + a hint of shading towards the card base
    m = T.fbm(size, rng, beta=1.8, lo_freq=24)
    rgb = rgb * (1 - mottle / 2 + mottle * m)[..., None]
    rgb = T.dilate_color(np.clip(rgb, 0, 1), a)
    # crisp-but-antialiased alpha: the cutoff at 0.5 lands on the true edge
    T.save(DST / f"{name}_albedo.png", np.concatenate([rgb, a[..., None]], -1))
    if normal_strength:
        hh = T.blur(h, 1.0) * 0.8 + T.blur(a, 3.0) * 0.2
        T.save(DST / f"{name}_normal.png", T.normal_from_height(hh, normal_strength))
    print("ok", name, flush=True)


# ------------------------------------------------------------------- leaves


def leaf_outline(L, W, a=0.8, b=1.1, serr=0.0, n=28):
    """Leaf polygon in local coords: petiole base at origin, tip at (L, 0)."""
    ts = np.linspace(0, 1, n)
    hw = W * np.sin(np.pi * ts ** a) ** b
    if serr:
        hw = hw * (1 + serr * np.abs(np.sin(ts * 46)))
    top = [(t * L, w) for t, w in zip(ts, hw)]
    bot = [(t * L, -w) for t, w in zip(ts, hw)][::-1]
    return top + bot[1:-1]


def xform(pts, ang, ox, oy):
    c, s = math.cos(ang), math.sin(ang)
    return [(ox + x * c - y * s, oy + x * s + y * c) for x, y in pts]


def draw_leaf(cv, rng, x, y, ang, L, W, color, a=0.8, b=1.1, serr=0.0):
    # concentric insets give a domed cross-section in the height layer and a soft
    # colour gradient (slightly lighter towards the midrib)
    steps = 5
    for k in range(steps):
        f = 1.0 - k / steps
        shape = leaf_outline(L * (1 - 0.04 * k), W * f, a, b, serr)
        shade = 0.92 + 0.1 * (k / steps)
        col = tuple(min(1, c * shade) for c in color)
        cv.poly(xform(shape, ang, x, y), col, height=0.35 + 0.5 * (k / steps) ** 0.6)
    # midrib + side veins: lighter colour, carved into the height
    vein_col = tuple(min(1, c * 1.25 + 0.04) for c in color)
    mid = [xform([(t * L * 0.95, 0)], ang, x, y)[0] for t in np.linspace(0.02, 1, 8)]
    cv.line(mid, vein_col, max(0.6, W * 0.06), height=0.25)
    for t in np.linspace(0.18, 0.8, 5):
        for side in (-1, 1):
            hw = W * math.sin(math.pi * t ** a) ** b * 0.85
            p0 = (t * L, 0)
            p1 = (t * L + hw * 0.9, side * hw)
            cv.line(xform([p0, p1], ang, x, y), vein_col, max(0.4, W * 0.025), height=0.3)


def twig(cv, rng, x0, y0, x1, y1, bend, width, color, n=12):
    pts = []
    mx, my = (x0 + x1) / 2 + bend[0], (y0 + y1) / 2 + bend[1]
    for t in np.linspace(0, 1, n):
        x = (1 - t) ** 2 * x0 + 2 * (1 - t) * t * mx + t * t * x1
        y = (1 - t) ** 2 * y0 + 2 * (1 - t) * t * my + t * t * y1
        pts.append((x, y))
    for i in range(len(pts) - 1):
        w = width * (1 - 0.6 * i / n)
        cv.line([pts[i], pts[i + 1]], color, w, height=0.55)
    return pts


def leaf_cluster(cv, rng, cell, kind):
    cx0, cy0, cs = cell
    bark = (0.30, 0.22, 0.15)
    if kind in ("broad", "broad_autumn"):
        greens = [(0.25, 0.40, 0.10), (0.30, 0.45, 0.12), (0.20, 0.34, 0.09), (0.36, 0.46, 0.14)]
        if kind == "broad_autumn":
            greens += [(0.55, 0.50, 0.14), (0.62, 0.42, 0.10), (0.40, 0.46, 0.12)]
        n_twigs, leaves_per, L, W, a, b, serr = 3, 6, 0.25, 0.11, 0.75, 1.0, 0.04
    elif kind == "small":
        greens = [(0.14, 0.27, 0.07), (0.18, 0.32, 0.08), (0.12, 0.23, 0.06)]
        n_twigs, leaves_per, L, W, a, b, serr = 5, 11, 0.12, 0.05, 0.7, 1.0, 0.0
    else:  # narrow (willow / olive-like)
        greens = [(0.33, 0.42, 0.18), (0.38, 0.46, 0.22), (0.28, 0.38, 0.16)]
        n_twigs, leaves_per, L, W, a, b, serr = 4, 9, 0.30, 0.045, 0.9, 1.0, 0.0
    # twigs radiate from the bottom centre of the cell upwards
    base = (cx0 + cs * 0.5, cy0 + cs * 0.97)
    for i in range(n_twigs):
        ang = -math.pi / 2 + (i - (n_twigs - 1) / 2) * (1.0 / max(1, n_twigs - 1)) + rng.normal(0, 0.08)
        ln = cs * rng.uniform(0.56, 0.72)
        end = (base[0] + math.cos(ang) * ln, base[1] + math.sin(ang) * ln)
        bend = (rng.normal(0, cs * 0.06), rng.normal(0, cs * 0.04))
        pts = twig(cv, rng, base[0], base[1], end[0], end[1], bend, cs * 0.012, bark)
        for j in range(leaves_per):
            t = 0.25 + 0.75 * (j + 0.5) / leaves_per
            k = min(len(pts) - 2, int(t * (len(pts) - 1)))
            px, py = pts[k]
            dx, dy = pts[k + 1][0] - px, pts[k + 1][1] - py
            dir_ang = math.atan2(dy, dx)
            side = 1 if j % 2 else -1
            la = dir_ang + side * rng.uniform(0.6, 1.0)
            sc = rng.uniform(0.75, 1.1) * (1.05 - 0.3 * t)
            draw_leaf(cv, rng, px, py, la, L * cs * sc, W * cs * sc,
                      jitter_color(rng, greens[rng.integers(len(greens))], 0.06), a, b, serr)
        # terminal leaf
        ex, ey = pts[-1]
        dir_ang = math.atan2(pts[-1][1] - pts[-2][1], pts[-1][0] - pts[-2][0])
        draw_leaf(cv, rng, ex, ey, dir_ang, L * cs * 0.9, W * cs * 0.9,
                  jitter_color(rng, greens[0], 0.06), a, b, serr)


class Atlas:
    """2x2 atlas assembled from independently drawn cells (no bleeding across cells)."""

    def __init__(self, size):
        self.size = size
        self.cells = []

    def cell(self, ix, iy):
        cv = Canvas(self.size // 2)
        self.cells.append((ix, iy, cv))
        return cv

    def arrays(self):
        h = self.size // 2
        rgb = np.zeros((self.size, self.size, 3), np.float32)
        a = np.zeros((self.size, self.size), np.float32)
        hh = np.zeros_like(a)
        for ix, iy, cv in self.cells:
            c, al, he = cv.arrays()
            rgb[iy * h:(iy + 1) * h, ix * h:(ix + 1) * h] = c
            a[iy * h:(iy + 1) * h, ix * h:(ix + 1) * h] = al
            hh[iy * h:(iy + 1) * h, ix * h:(ix + 1) * h] = he
        return rgb, a, hh


def leaf_atlas(size=1024):
    rng = np.random.default_rng(7)
    at = Atlas(size)
    h = size // 2
    for (ix, iy), kind in zip(((0, 0), (1, 0), (0, 1), (1, 1)),
                              ("broad", "broad_autumn", "small", "narrow")):
        leaf_cluster(at.cell(ix, iy), rng, (6, 6, h - 12), kind)
    finish_alpha_atlas(at, "leaf_atlas", rng, normal_strength=6.0)


# -------------------------------------------------------------------- grass


def blade(cv, rng, x0, y0, height, lean, width, cols, segs=10):
    """Curved tapered blade from (x0,y0) upwards; cols = (base, mid, tip)."""
    tipx = x0 + lean * height
    ctrl = (x0 + lean * height * 0.2, y0 - height * 0.6)
    tip = (tipx, y0 - height)
    pts = []
    for t in np.linspace(0, 1, segs + 1):
        x = (1 - t) ** 2 * x0 + 2 * (1 - t) * t * ctrl[0] + t * t * tip[0]
        y = (1 - t) ** 2 * y0 + 2 * (1 - t) * t * ctrl[1] + t * t * tip[1]
        pts.append((x, y))
    for i in range(segs):
        t0, t1 = i / segs, (i + 1) / segs
        w0, w1 = width * (1 - t0) ** 0.8, width * (1 - t1) ** 0.8
        (xa, ya), (xb, yb) = pts[i], pts[i + 1]
        dx, dy = xb - xa, yb - ya
        ln = math.hypot(dx, dy) + 1e-6
        nx, ny = -dy / ln, dx / ln
        quad = [(xa + nx * w0 / 2, ya + ny * w0 / 2), (xb + nx * w1 / 2, yb + ny * w1 / 2),
                (xb - nx * w1 / 2, yb - ny * w1 / 2), (xa - nx * w0 / 2, ya - ny * w0 / 2)]
        tm = (t0 + t1) / 2
        c = np.array(cols[0]) * (1 - tm) ** 2 + np.array(cols[1]) * 2 * tm * (1 - tm) + np.array(cols[2]) * tm ** 2
        cv.poly(quad, tuple(c), height=0.4 + 0.4 * tm)
    return pts[-1]


GRASS_COLS = [((0.06, 0.12, 0.03), (0.22, 0.38, 0.08), (0.42, 0.52, 0.16)),
              ((0.07, 0.13, 0.04), (0.26, 0.42, 0.10), (0.50, 0.56, 0.20)),
              ((0.06, 0.11, 0.03), (0.18, 0.33, 0.07), (0.33, 0.45, 0.12))]
DRY_COLS = ((0.20, 0.17, 0.07), (0.52, 0.45, 0.22), (0.72, 0.64, 0.38))


def grass_cell(cv, rng, cell, n, hmin, hmax, dry=0.0, seeds=0):
    cx0, cy0, cs = cell
    for i in range(n):
        x0 = cx0 + cs * (0.5 + rng.normal(0, 0.17))
        x0 = float(np.clip(x0, cx0 + cs * 0.08, cx0 + cs * 0.92))
        y0 = cy0 + cs * 0.995
        hgt = cs * rng.uniform(hmin, hmax)
        lean = rng.normal(0, 0.25) + (x0 - (cx0 + cs / 2)) / cs * 0.8
        cols = DRY_COLS if rng.random() < dry else GRASS_COLS[rng.integers(3)]
        cols = tuple(jitter_color(rng, c, 0.05) for c in cols)
        tip = blade(cv, rng, x0, y0, hgt, lean, cs * rng.uniform(0.012, 0.022), cols)
        if seeds and rng.random() < seeds:
            for k in range(6):  # seed head: small ellipses stacked along the tip
                cv.ellipse(tip[0] + rng.normal(0, 1.2), tip[1] + k * cs * 0.012,
                           cs * 0.006, cs * 0.011, jitter_color(rng, (0.62, 0.55, 0.30), 0.08), 0.8)


def flower(cv, rng, x, y, kind, cs):
    if kind == "daisy":
        r = cs * rng.uniform(0.05, 0.07)
        for k in range(14):
            ang = k / 14 * 2 * math.pi
            px, py = x + math.cos(ang) * r * 0.6, y + math.sin(ang) * r * 0.35
            pet = leaf_outline(r * 0.95, r * 0.22, 0.6, 1.0, 0.0, 10)
            pet = [(px2, py2 * 1.0) for px2, py2 in pet]
            pts = xform(pet, ang, x, y)
            pts = [(px2, y + (py2 - y) * 0.55) for px2, py2 in pts]  # tilted towards viewer
            cv.poly(pts, jitter_color(rng, (0.93, 0.92, 0.86), 0.03), height=0.7)
        cv.ellipse(x, y, r * 0.3, r * 0.22, jitter_color(rng, (0.92, 0.70, 0.12), 0.05), 0.9)
    elif kind == "poppy":
        r = cs * rng.uniform(0.055, 0.075)
        red = jitter_color(rng, (0.80, 0.10, 0.06), 0.06)
        for k, ang in enumerate((-2.4, -1.6, -0.8, -1.2)):
            pet = leaf_outline(r * 1.1, r * 0.75, 0.5, 0.8, 0.0, 14)
            cv.poly(xform(pet, ang, x, y + r * 0.3), tuple(c * (0.85 + 0.05 * k) for c in red), height=0.75)
        cv.ellipse(x, y - r * 0.05, r * 0.22, r * 0.15, (0.08, 0.06, 0.05), 0.85)
    else:  # cornflower
        r = cs * rng.uniform(0.04, 0.055)
        blue = jitter_color(rng, (0.20, 0.32, 0.80), 0.06)
        for k in range(11):
            ang = -math.pi + k / 10 * math.pi
            pet = leaf_outline(r, r * 0.25, 0.4, 0.7, 0.3, 10)
            cv.poly(xform(pet, ang, x, y), blue, height=0.75)
        cv.ellipse(x, y, r * 0.25, r * 0.2, (0.16, 0.12, 0.35), 0.8)


def flower_cell(cv, rng, cell, kinds, nflowers, ngrass):
    cx0, cy0, cs = cell
    grass_cell(cv, rng, cell, ngrass, 0.25, 0.6)
    for i in range(nflowers):
        x0 = cx0 + cs * rng.uniform(0.15, 0.85)
        y0 = cy0 + cs * 0.995
        top = (x0 + rng.normal(0, cs * 0.05), cy0 + cs * rng.uniform(0.18, 0.45))
        stem = [(x0, y0), ((x0 + top[0]) / 2 + rng.normal(0, 3), (y0 + top[1]) / 2), top]
        cv.line(stem, jitter_color(rng, (0.20, 0.36, 0.10), 0.05), cs * 0.008, height=0.6)
        flower(cv, rng, top[0], top[1], kinds[i % len(kinds)], cs)


def grass_atlas(size=1024):
    rng = np.random.default_rng(11)
    at = Atlas(size)
    h = size // 2
    grass_cell(at.cell(0, 0), rng, (0, 0, h), 150, 0.35, 0.92)                       # lawn clump
    grass_cell(at.cell(1, 0), rng, (0, 0, h), 90, 0.45, 0.97, dry=0.3, seeds=0.25)   # wild grass
    flower_cell(at.cell(0, 1), rng, (0, 0, h), ["daisy"], 9, 60)
    flower_cell(at.cell(1, 1), rng, (0, 0, h), ["poppy", "cornflower"], 10, 60)
    finish_alpha_atlas(at, "grass_atlas", rng, normal_strength=None, mottle=0.18)


# --------------------------------------------------------------------- skin
# Head UVs (see assets/blender/scripts/busts.py): u = 0.5 + atan2(x, -y) / 2pi (face looks
# down -Y, so the face centre is u = 0.5), v = (z - Z0) / (Z1 - Z0) with the head origin at
# the eye line. Feature positions below are in those coordinates.
SKIN_Z0, SKIN_Z1 = -0.25, 0.135


def head_uv(x, y_front, z):
    """UV of a point on the face given lateral x, forward distance y_front (>0) and height z."""
    u = 0.5 + math.atan2(x, y_front) / (2 * math.pi)
    v = (z - SKIN_Z0) / (SKIN_Z1 - SKIN_Z0)
    return u, v


def gauss2(uu, vv, u0, v0, su, sv):
    du = (uu - u0 + 0.5) % 1.0 - 0.5     # periodic in u
    return np.exp(-(du ** 2 / (2 * su ** 2) + (vv - v0) ** 2 / (2 * sv ** 2)))


def skin(size=1024):
    rng = np.random.default_rng(5)
    v_img, u_img = np.mgrid[0:size, 0:size].astype(np.float32) / size
    uu, vv = u_img, 1.0 - v_img            # Blender/glTF-agnostic: v up = image top
    base = T.srgb_to_linear(np.array([0.80, 0.60, 0.50], np.float32))
    red = T.srgb_to_linear(np.array([0.78, 0.42, 0.38], np.float32))
    lipc = T.srgb_to_linear(np.array([0.66, 0.34, 0.34], np.float32))
    hair = T.srgb_to_linear(np.array([0.20, 0.14, 0.10], np.float32))
    lin = np.ones((size, size, 3), np.float32) * base

    # low-frequency mottling (redness / yellowness), periodic in u because fbm wraps
    m1 = T.fbm(size, rng, beta=2.2, lo_freq=3)
    m2 = T.fbm(size, rng, beta=1.5, lo_freq=30)
    lin *= (1 + 0.10 * (m1 - 0.5))[..., None] * np.array([1.0, 0.97, 0.95])
    lin *= (1 + 0.05 * (m2 - 0.5))[..., None]

    eu, ev = head_uv(0.033, 0.075, 0.0)
    cheek = sum(gauss2(uu, vv, 0.5 + s * 0.10, head_uv(0, 1, -0.03)[1], 0.035, 0.05) for s in (-1, 1))
    nose = gauss2(uu, vv, 0.5, head_uv(0, 1, -0.03)[1], 0.012, 0.03)
    ears = sum(gauss2(uu, vv, 0.5 + s * 0.25, head_uv(0, 1, 0.0)[1], 0.03, 0.06) for s in (-1, 1))
    flush = np.clip(0.45 * cheek + 0.5 * nose + 0.4 * ears, 0, 1)[..., None]
    lin = lin * (1 - flush) + red * flush

    # lips: upper + lower lobes
    lv = head_uv(0, 1, -0.062)[1]
    lips = np.clip(gauss2(uu, vv, 0.5, lv + 0.010, 0.030, 0.008) +
                   gauss2(uu, vv, 0.5, lv - 0.012, 0.027, 0.010), 0, 1)
    lips = T.smoothstep(0.25, 0.7, lips)[..., None]
    lin = lin * (1 - lips) + lipc * lips

    # eye sockets slightly darker / cooler, eyebrows as streaky darker hair
    for s in (-1, 1):
        sock = gauss2(uu, vv, 0.5 + s * (eu - 0.5), ev, 0.03, 0.022)[..., None]
        lin *= 1 - 0.18 * sock * np.array([1.0, 1.05, 0.9])
    brow_v = head_uv(0, 1, 0.024)[1]
    streak = T.fbm(size, rng, beta=1.4, lo_freq=40, aniso=(0.25, 1.0))
    brows = 0
    for s in (-1, 1):
        bu = 0.5 + s * 0.06
        du = (uu - bu)
        arch = brow_v + 0.006 - 2.0 * du ** 2
        brows = brows + np.exp(-(du ** 2) / (2 * 0.03 ** 2)) * np.exp(-((vv - arch) ** 2) / (2 * 0.0075 ** 2))
    brows = np.clip(brows * (0.6 + 0.6 * streak), 0, 1)[..., None]
    lin = lin * (1 - 0.9 * brows) + hair * 0.9 * brows

    # short cropped hair on the scalp: above a hairline that dips at the temples/back
    du_face = np.abs((uu - 0.5 + 0.5) % 1.0 - 0.5)            # 0 at the face centre, 0.5 at the back
    hairline = np.where(du_face < 0.13, head_uv(0, 1, 0.072)[1],
                        head_uv(0, 1, 0.072)[1] - (du_face - 0.13) * 0.55)
    hairline = np.maximum(hairline, head_uv(0, 1, -0.045)[1])
    hair_streak = T.fbm(size, rng, beta=1.2, lo_freq=60, aniso=(1.0, 0.2))
    hm = T.smoothstep(hairline - 0.006, hairline + 0.012, vv + 0.01 * (hair_streak - 0.5))
    hm = hm[..., None]
    hair_col = hair * (0.75 + 0.5 * hair_streak)[..., None]
    lin = lin * (1 - hm) + hair_col * hm

    T.save(DST / "skin_albedo.png", T.linear_to_srgb(lin))

    # normal: pores + fine wrinkles, hair strands on the scalp
    pores = T.fbm(size, rng, beta=0.6, lo_freq=180)
    fine = T.fbm(size, rng, beta=1.4, lo_freq=60, aniso=(1.0, 0.35))
    hgt = 0.6 * pores + 0.4 * fine
    hgt = hgt * (1 - hm[..., 0]) + hair_streak * hm[..., 0]
    hgt = hgt - 0.3 * lips[..., 0] * T.fbm(size, rng, beta=1.0, lo_freq=80, aniso=(0.1, 1.0))
    T.save(DST / "skin_normal.png", T.normal_from_height(T.normalize(hgt), 1.2))

    rough = 0.52 - 0.08 * np.clip(nose * 1.5 + gauss2(uu, vv, 0.5, head_uv(0, 1, 0.05)[1], 0.06, 0.05), 0, 1)
    rough = rough - 0.14 * lips[..., 0] + 0.03 * (pores - 0.5)
    rough = rough * (1 - hm[..., 0]) + 0.72 * hm[..., 0]
    ao = 1 - 0.25 * sum(gauss2(uu, vv, 0.5 + s * (eu - 0.5), ev, 0.022, 0.016) for s in (-1, 1))
    T.save(DST / "skin_orm.png", T.pack_orm(ao, np.clip(rough, 0, 1), np.zeros_like(ao)))
    print("ok skin", flush=True)


def eye(w=512, h=256):
    """Equirectangular eyeball: image top row = +V pole = front of the eye (cornea)."""
    rng = np.random.default_rng(3)
    v_img, u_img = np.mgrid[0:h, 0:w].astype(np.float32)
    theta = (v_img + 0.5) / h * math.pi            # angle from the front pole
    phi = (u_img + 0.5) / w * 2 * math.pi
    sclera = T.srgb_to_linear(np.array([0.86, 0.82, 0.78], np.float32))
    iris_in = T.srgb_to_linear(np.array([0.42, 0.33, 0.14], np.float32))   # hazel centre
    iris_out = T.srgb_to_linear(np.array([0.22, 0.30, 0.22], np.float32))  # green-grey rim
    lin = np.ones((h, w, 3), np.float32) * sclera
    # faint veins towards the back
    veins = T.resize(T.fbm(256, rng, beta=1.2, lo_freq=20, aniso=(1, 0.3)), w)[:h]
    lin *= (1 - 0.15 * T.smoothstep(0.7, 0.9, veins) * T.smoothstep(0.6, 1.4, theta))[..., None] * np.array([1, 0.9, 0.9])
    iris_r, pupil_r = math.radians(31), math.radians(10)
    t = np.clip((theta - pupil_r) / (iris_r - pupil_r), 0, 1)[..., None]
    fib = 0.5 + 0.5 * np.sin(phi * 60 + 3 * np.sin(phi * 7)) * 0.5 + 0.25 * (
        T.resize(T.fbm(256, rng, beta=1.0, lo_freq=30), w)[:h] - 0.5)
    iris = (iris_in * (1 - t) + iris_out * t) * (0.75 + 0.5 * fib[..., None])
    iris *= 1 - 0.6 * T.smoothstep(0.82, 1.0, t)    # dark limbal ring
    in_iris = (theta < iris_r)[..., None]
    lin = np.where(in_iris, iris, lin)
    lin = np.where((theta < pupil_r)[..., None], np.array([0.01, 0.01, 0.012], np.float32), lin)
    # soften the iris/sclera boundary a touch
    out = T.linear_to_srgb(lin)
    T.save(DST / "eye_albedo.png", out)
    print("ok eye", flush=True)


def brass(size=512):
    rng = np.random.default_rng(9)
    base = T.srgb_to_linear(np.array([0.93, 0.76, 0.42], np.float32))
    tarnish = T.srgb_to_linear(np.array([0.55, 0.42, 0.22], np.float32))
    n = T.fbm(size, rng, beta=2.0, lo_freq=3)
    fine = T.fbm(size, rng, beta=1.0, lo_freq=40)
    tm = T.smoothstep(0.6, 0.85, n)[..., None] * 0.45
    lin = base * (1 - tm) + tarnish * tm
    lin *= (0.95 + 0.1 * fine)[..., None]
    T.save(DST / "brass_albedo.png", T.linear_to_srgb(lin))
    rough = 0.22 + 0.25 * tm[..., 0] + 0.06 * (fine - 0.5)
    T.save(DST / "brass_orm.png", T.pack_orm(np.ones_like(n), np.clip(rough, 0, 1), np.ones_like(n)))
    print("ok brass", flush=True)


JOBS = {"leaf": leaf_atlas, "grass": grass_atlas, "skin": skin, "eye": eye, "brass": brass}

if __name__ == "__main__":
    for name in sys.argv[1:] or list(JOBS):
        JOBS[name]()
