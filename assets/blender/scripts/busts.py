"""Two 'character' busts on stone pedestals (screen-space SSS test subjects).

The head is a lofted surface: horizontal slices from the neck to the crown, each a
superellipse r(phi) around a slice centre, with analytic facial features added on top
(brow, nose, lips, chin, cheekbones, eye sockets) and eyelids that wrap the separate eyeball
spheres, leaving an almond-shaped opening. UVs are the cylindrical projection that
tools/textures/procedural.py:skin() paints for:
    u = 0.5 + atan2(x, -y) / 2pi      (face looks down -Y, face centre at u = 0.5)
    v = (z - Z0) / (Z1 - Z0),  Z0 = -0.25, Z1 = 0.135  (origin at eye level)
Eyes are UV spheres (material 'eye', shading model 'eye') whose +V pole looks forward.
"""
import math

import bpy
from mathutils import Matrix, Vector

from common import MeshBuilder, box, get_coll, lathe

Z0, Z1 = -0.25, 0.135
EYE_R = 0.0122

# slice keyframes: z, half-width a, front extent f, back extent b, centre y offset
HEAD_KEYS = [
    (-0.250, 0.065, 0.060, 0.068, 0.022),
    (-0.200, 0.059, 0.053, 0.063, 0.022),
    (-0.150, 0.058, 0.051, 0.061, 0.020),
    (-0.125, 0.056, 0.053, 0.059, 0.018),
    (-0.114, 0.054, 0.066, 0.056, 0.014),
    (-0.104, 0.052, 0.078, 0.054, 0.011),
    (-0.090, 0.049, 0.083, 0.056, 0.008),
    (-0.075, 0.053, 0.084, 0.064, 0.006),
    (-0.060, 0.059, 0.083, 0.076, 0.005),
    (-0.040, 0.067, 0.081, 0.089, 0.004),
    (-0.020, 0.071, 0.080, 0.097, 0.005),
    (0.000, 0.073, 0.080, 0.100, 0.006),
    (0.020, 0.074, 0.084, 0.100, 0.008),
    (0.050, 0.072, 0.083, 0.097, 0.010),
    (0.080, 0.065, 0.075, 0.088, 0.012),
    (0.100, 0.053, 0.061, 0.075, 0.012),
    (0.113, 0.035, 0.040, 0.050, 0.012),
    (0.120, 0.016, 0.018, 0.024, 0.012),
    (0.1225, 0.0, 0.0, 0.0, 0.012),
]

DRAPE_KEYS = [
    (-0.365, 0.205, 0.098, 0.098, 0.020),
    (-0.330, 0.228, 0.104, 0.102, 0.020),
    (-0.280, 0.236, 0.104, 0.102, 0.020),
    (-0.245, 0.228, 0.100, 0.098, 0.020),
    (-0.222, 0.205, 0.093, 0.092, 0.020),
    (-0.205, 0.160, 0.082, 0.084, 0.020),
    (-0.192, 0.110, 0.072, 0.077, 0.020),
    (-0.183, 0.078, 0.065, 0.071, 0.021),
    (-0.174, 0.068, 0.062, 0.069, 0.022),
    (-0.168, 0.066, 0.060, 0.067, 0.022),
]


def catmull(keys, z):
    """Smooth interpolation of slice keyframes at height z (Catmull-Rom on the tuple values)."""
    zs = [k[0] for k in keys]
    if z <= zs[0]:
        return keys[0][1:]
    if z >= zs[-1]:
        return keys[-1][1:]
    i = max(j for j in range(len(zs) - 1) if zs[j] <= z)
    t = (z - zs[i]) / (zs[i + 1] - zs[i])
    p0 = keys[max(0, i - 1)][1:]
    p1, p2 = keys[i][1:], keys[i + 1][1:]
    p3 = keys[min(len(keys) - 1, i + 2)][1:]
    out = []
    for a, b, c, d in zip(p0, p1, p2, p3):
        v = 0.5 * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t + (-a + 3 * b - 3 * c + d) * t ** 3)
        out.append(max(0.0, v) if len(out) < 3 else v)
    return out


def superellipse(phi, a, f, b, n):
    s, c = math.sin(phi), math.cos(phi)
    e = f if c >= 0 else b
    if a <= 1e-6 or e <= 1e-6:
        return 0.0
    return 1.0 / ((abs(s) / a) ** n + (abs(c) / e) ** n) ** (1.0 / n)


def g2(dx, sx, dz, sz):
    return math.exp(-(dx * dx) / (2 * sx * sx) - (dz * dz) / (2 * sz * sz))


def smoothstep(e0, e1, x):
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


class Face:
    def __init__(self, variant):
        self.v = variant
        self.w = 1.03 + 0.03 * variant
        self.eye_x = 0.0318 * self.w
        self.eye_y = -0.0602
        self.eye_z = 0.0035

    def features(self, phi, z):
        """Radial additions (meters) for the face, phi = 0 at the front."""
        v = self.v
        d = 0.0
        # brow ridge
        d += 0.0062 * g2(phi, 0.5, z - 0.021, 0.008)
        d -= 0.0016 * g2(phi, 0.12, z - 0.017, 0.006)            # glabella dip
        # nose: bridge -> tip -> base. Profile height and half-width (radians) vary with z;
        # the lateral shape (1 - x^2)^1.3 gives defined nasal sidewalls instead of a soft mound.
        if -0.05 < z < 0.03:
            prof = [(0.025, 0.0, 0.08), (0.012, 0.0055, 0.085), (-0.005, 0.0115, 0.1), (-0.022, 0.0185, 0.13),
                    (-0.033, 0.0255 + 0.002 * v, 0.17 + 0.02 * v), (-0.039, 0.0225, 0.2 + 0.02 * v),
                    (-0.0445, 0.01, 0.24 + 0.03 * v), (-0.049, 0.0, 0.25)]
            amp, wid = 0.0, 0.1
            for (za, aa, wa), (zb, ab, wb) in zip(prof, prof[1:]):
                if zb <= z <= za:
                    t = (z - za) / (zb - za)
                    t = t * t * (3 - 2 * t)
                    amp, wid = aa + (ab - aa) * t, wa + (wb - wa) * t
            x = phi / wid
            if abs(x) < 1.0:
                d += amp * (1.0 - x * x) ** 1.3
            # nostril wings
            for s in (-1, 1):
                d += 0.004 * g2(phi - s * 0.19, 0.05, z + 0.0395, 0.004)
        # cheekbones and cheeks
        for s in (-1, 1):
            d += 0.0062 * g2(phi - s * 0.8, 0.24, z + 0.013, 0.012)
            d -= 0.0018 * g2(phi - s * 0.62, 0.12, z + 0.05, 0.016)   # nasolabial hollow
            d += 0.0025 * g2(phi - s * 0.5, 0.25, z + 0.045, 0.015)
        # lips (fuller for variant 1) and the mouth line
        lip = 1.0 + 0.35 * v
        d += 0.0062 * lip * g2(phi, 0.26, z + 0.0565, 0.0042)
        d += 0.0068 * lip * g2(phi, 0.23, z + 0.0705, 0.0052)
        d -= 0.0028 * g2(phi, 0.3, z + 0.0632, 0.0011)
        for s in (-1, 1):
            d -= 0.0018 * g2(phi - s * 0.31, 0.04, z + 0.063, 0.003)
        d -= 0.0012 * g2(phi, 0.045, z + 0.049, 0.004)          # philtrum
        # chin and the groove above it
        d += 0.0055 * g2(phi, 0.28, z + 0.097, 0.009)
        d -= 0.0018 * g2(phi, 0.3, z + 0.083, 0.0035)
        # temples
        for s in (-1, 1):
            d -= 0.0025 * g2(phi - s * 1.15, 0.22, z - 0.03, 0.02)
        return d

    def eye(self, P, centre):
        """Eye socket / eyelid shaping. P = current surface point; returns new radial distance
        along the horizontal ray from 'centre' (or None if unaffected)."""
        best = None
        for s in (-1, 1):
            E = Vector((s * self.eye_x, self.eye_y, self.eye_z))
            dirv = Vector((P.x - centre.x, P.y - centre.y, 0.0))
            r0 = dirv.length
            if r0 < 1e-6:
                continue
            dirv /= r0
            O = Vector((centre.x, centre.y, P.z))
            # socket depression around the eye (beneath the brow)
            dx, dz = P.x - E.x, P.z - E.z
            sock = 0.0118 * g2(dx, 0.0165, dz + 0.001, 0.0125)
            r_dent = r0 - sock
            # ray / eyeball intersection (front surface)
            oc = O - E
            b = oc.dot(dirv)
            c = oc.dot(oc) - EYE_R * EYE_R
            disc = b * b - c
            if disc <= 0:
                cand = r_dent
                w = 0.0
            else:
                t_far = -b + math.sqrt(disc)
                h = math.sqrt(disc) / EYE_R                 # 0 at the silhouette, 1 at the centre
                # almond opening (outer corner slightly higher)
                ux = (P.x - E.x) * s / 0.0122
                cz = E.z - 0.0003 + 0.0010 * ux
                half_h = 0.0037 * max(0.0, 1 - ux * ux) ** 0.75 if abs(ux) < 1 else 0.0
                inside = smoothstep(-0.0009, 0.0009, half_h - abs(P.z - cz))
                lid = t_far + 0.0011
                hole = t_far - 0.004
                target = lid * (1 - inside) + hole * inside
                w = smoothstep(0.02, 0.35, h)
                cand = r_dent * (1 - w) + target * w
                # upper lid crease
                cand -= 0.0011 * g2(dx, 0.009, dz - 0.0105, 0.0012) * (1 - inside)
            if best is None or abs(cand - r0) > abs(best - r0):
                best = cand
        return best


def phi_samples(segs, power):
    """Angles from -pi (back) through 0 (front) to +pi, denser at the front when power > 1."""
    out = []
    for j in range(segs):
        t = -1.0 + 2.0 * j / segs
        out.append(math.copysign(math.pi * abs(t) ** power, t))
    return out


def loft(mb, keys, mat, segs, zs, face=None, n_front=2.6, n_back=2.2, uv="head", close_top=True, cap_bottom=False,
         power=1.0):
    rows = []
    phis = phi_samples(segs, power)
    for z in zs:
        a, f, b, cy = catmull(keys, z)
        centre = Vector((0.0, cy, z))
        row, uvr = [], []
        for j in range(segs):
            phi = phis[j]
            n = n_front if (math.cos(phi) > 0 and -0.11 < z < 0.04 and face) else n_back
            r = superellipse(phi, a, f, b, n)
            if face is not None and r > 0:
                r += face.features(phi, z) * min(1.0, r / 0.05)
            P = Vector((centre.x + r * math.sin(phi), centre.y - r * math.cos(phi), z))
            if face is not None and r > 0 and math.cos(phi) > 0.15 and -0.055 < z < 0.06:
                rr = face.eye(P, centre)
                if rr is not None:
                    P = Vector((centre.x + rr * math.sin(phi), centre.y - rr * math.cos(phi), z))
            row.append(P)
        rows.append(row)
    # UVs
    us = [0.5 + ph / (2 * math.pi) for ph in phis] + [1.0]
    if uv == "head":
        vs = [(z - Z0) / (Z1 - Z0) for z in zs]
    else:
        vs = [z * 2.0 for z in zs]
        us = [u * 2.0 for u in us]
    # rotate the seam to the back: phi=-pi is at j=0 -> u = 0.5 + phi/2pi = 0 at the back
    mb.quad_strip(rows, mat, us, vs, close=True)
    if close_top:
        top = rows[-1]
        mb.poly(top, mat, uvs=[(0.5 + (math.atan2(p.x, -p.y)) / (2 * math.pi), vs[-1]) for p in top], facing=(0, 0, 1))
    if cap_bottom:
        mb.poly(rows[0], mat, facing=(0, 0, -1))
    return rows


def ear(mb, s, mat, w):
    """Simple ear: a thin curved shell with a rim, attached to the side of the head."""
    c = Vector((s * 0.0715 * w, 0.012, -0.004))
    m = Matrix.Translation(c) @ Matrix.Rotation(s * math.radians(-18), 4, "Z") @ Matrix.Rotation(math.radians(-10), 4, "X")
    segs, rings = 16, 7
    rows = []
    for i in range(rings + 1):
        t = i / rings                       # 0 = attached base, 1 = outer rim
        row = []
        for j in range(segs):
            a = 2 * math.pi * j / segs
            ry, rz = 0.017 * (0.35 + 0.65 * t), 0.03 * (0.35 + 0.65 * t)
            y = ry * math.cos(a)
            z = rz * math.sin(a)
            x = s * (0.004 + 0.010 * t ** 1.5 + 0.004 * (1 - abs(math.sin(a))) * t)
            if t > 0.8:      # rim folds back
                x -= s * 0.004 * (t - 0.8) / 0.2
            row.append(m @ Vector((x - s * 0.004 * (1 - t), y, z)))
        rows.append(row)
    us = [0.5 + s * 0.24 + 0.02 * j / segs for j in range(segs + 1)]
    vs = [(c.z - Z0) / (Z1 - Z0) + 0.02 * i / rings for i in range(rings + 1)]
    mb.quad_strip(rows, mat, us, vs, close=True)
    mb.poly(rows[-1], mat, uvs=[(us[0], vs[-1])] * len(rows[-1]), facing=(s, 0, 0))


def head_zs():
    zs, z = [], -0.25
    while z < 0.1225:
        zs.append(z)
        if -0.08 < z < 0.03:
            step = 0.0011
        elif z > 0.11:
            step = 0.0012
        else:
            step = 0.0024
        z += step
    zs.append(0.1218)
    return zs


def head_builder(variant, mat):
    face = Face(variant)
    keys = [(z, a * face.w, f, b, cy) for (z, a, f, b, cy) in HEAD_KEYS]
    mb = MeshBuilder()
    loft(mb, keys, mat, 176, head_zs(), face=face, power=1.5)
    for s in (-1, 1):
        ear(mb, s, mat, face.w)
    return mb, face


def eye_mesh():
    """UV sphere with the +V pole looking down -Y (front)."""
    mb = MeshBuilder()
    rings, segs = 12, 20
    rows = []
    for i in range(rings + 1):
        th = math.pi * i / rings          # 0 at the back pole, pi at the front pole
        rows.append([Vector((math.sin(th) * math.cos(2 * math.pi * j / segs) * EYE_R,
                             math.cos(th) * EYE_R,
                             math.sin(th) * math.sin(2 * math.pi * j / segs) * EYE_R)) for j in range(segs)])
    us = [j / segs for j in range(segs + 1)]
    vs = [i / rings for i in range(rings + 1)]
    mb.quad_strip(rows, "eye", us, vs, close=True)
    return mb


def drape_builder(variant, mat):
    mb = MeshBuilder()
    zs = [-0.365 + i * (0.197 / 56) for i in range(57)]
    keys = [(z, a * (1 + 0.03 * variant), f, b, cy) for (z, a, f, b, cy) in DRAPE_KEYS]
    rows = loft(mb, keys, mat, 64, zs, face=None, n_back=2.4, uv="cloth", close_top=False, cap_bottom=True)
    # rolled collar lip turning inwards towards the neck
    top = rows[-1]
    c = Vector((0.0, 0.022, top[0].z))
    inner = [c + (p - c) * 0.86 - Vector((0, 0, 0.006)) for p in top]
    n = len(top)
    for j in range(n):
        k = (j + 1) % n
        mb.poly([top[j], top[k], inner[k], inner[j]], mat, facing=(0, 0, 1))
    return mb


def pedestal_builder():
    mb = MeshBuilder()
    prof = [(0.0, 0.0), (0.42, 0.0), (0.42, 0.14), (0.36, 0.2), (0.32, 0.24), (0.3, 1.0), (0.34, 1.04), (0.38, 1.1),
            (0.38, 1.18), (0.0, 1.18)]
    lathe(mb, prof, "stone", segs=4, angle0=math.pi / 4, tile=1.0)
    box(mb, (-0.12, -0.32, 0.6), (0.12, -0.3, 0.72), "gold")
    return mb


BUSTS = [("Bust_A", 0, (12.9, -1.6), "skin_light", "cloth_blue"), ("Bust_B", 1, (12.9, -7.4), "skin_dark", "cloth_cream")]
PEDESTAL_H = 1.18
LOWER_FACE_SCALE = 0.9
DRAPE_BOTTOM = -0.365


def build():
    coll = get_coll("Busts")
    ped = pedestal_builder().build_mesh("Pedestal")
    eye = eye_mesh().build_mesh("Eye", smooth=True, sharp_angle=None)
    rot = Matrix.Rotation(-math.pi / 2, 4, "Z")      # face west, towards the low sun
    out = []
    for name, variant, (x, y), skin, cloth in BUSTS:
        p = bpy.data.objects.new(f"{name}_Pedestal", ped)
        coll.objects.link(p)
        p.matrix_world = Matrix.Translation((x, y, 0.0)) @ rot
        origin = Vector((x, y, PEDESTAL_H - DRAPE_BOTTOM))
        hb, face = head_builder(variant, skin)
        for i, v in enumerate(hb.verts):          # compress below the eye line: shorter face + neck
            if v.z < 0.0:
                hb.verts[i] = v.__class__((v.x, v.y, v.z * LOWER_FACE_SCALE))
        hm = hb.build(f"{name}_Head", coll, smooth=True, sharp_angle=None)
        hm.data.transform(rot)          # bake the rotation into the (unique) mesh
        hm.location = origin
        dm = drape_builder(variant, cloth).build(f"{name}_Drape", coll, smooth=True, sharp_angle=None)
        dm.data.transform(rot)
        dm.location = origin
        for s in (-1, 1):
            e = bpy.data.objects.new(f"{name}_Eye_{'R' if s < 0 else 'L'}", eye)
            coll.objects.link(e)
            e.matrix_world = Matrix.Translation(origin) @ rot @ Matrix.Translation((s * face.eye_x, face.eye_y, face.eye_z))
        out.append(hm)
    return out
