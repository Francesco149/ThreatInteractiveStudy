"""Shared helpers for building the courtyard scene inside Blender (bpy + bmesh only).

Blender frame: X east, Y north, Z up, meters. The glTF exporter converts to +Y up
(glTF x = X, y = Z, z = -Y).

Geometry is accumulated in a MeshBuilder (vertices, faces with a material name and per-corner
UVs) and turned into a bpy mesh at the end. UVs are authored in meters / tile size so every
material tiles at a consistent real-world scale (see TILE).
"""
import math
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))   # the repository
TEX_DIR = os.path.join(ROOT, "assets", "scene", "textures")

# meters covered by one repeat of each material's texture
TILE = {
    "brick": 2.5, "plaster": 3.0, "stone": 2.0, "cobble": 2.2, "paving": 3.0, "grass": 3.0,
    "wood": 1.2, "paintedwood": 1.2, "bark": 1.4, "roof": 2.4, "cloth": 2.0,
    "paint_iron": 1.0, "brass": 0.5, "default": 1.0,
}


# ----------------------------------------------------------------- scene utils


def get_coll(name, parent=None):
    c = bpy.data.collections.get(name)
    if c is None:
        c = bpy.data.collections.new(name)
        (parent or bpy.context.scene.collection).children.link(c)
    return c


def reset_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras,
                 bpy.data.curves, bpy.data.metaballs, bpy.data.node_groups):
        for d in list(coll):
            coll.remove(d)
    for c in list(bpy.data.collections):
        bpy.data.collections.remove(c)
    for im in list(bpy.data.images):
        bpy.data.images.remove(im)


def set_props(idblock, **props):
    for k, v in props.items():
        idblock[k] = v
    return idblock


def instance(src, name, loc=(0, 0, 0), rot_z=0.0, scale=1.0, coll=None, rot=None):
    """Linked duplicate: a new object sharing src's mesh data (exported as a reused glTF mesh)."""
    o = bpy.data.objects.new(name, src.data)
    o.location = loc
    o.rotation_euler = rot if rot is not None else (0.0, 0.0, rot_z)
    o.scale = (scale, scale, scale)
    for k in src.keys():
        o[k] = src[k]
    (coll or src.users_collection[0]).objects.link(o)
    return o


def place(mesh, name, matrix, coll, **props):
    """Object using an existing mesh datablock (instancing) with a full world matrix."""
    o = bpy.data.objects.new(name, mesh)
    o.matrix_world = matrix
    coll.objects.link(o)
    set_props(o, **props)
    return o


def empty(name, loc, coll, size=0.3, **props):
    o = bpy.data.objects.new(name, None)
    o.empty_display_type = "SPHERE"
    o.empty_display_size = size
    o.location = loc
    coll.objects.link(o)
    set_props(o, **props)
    return o


# ---------------------------------------------------------------- MeshBuilder


def _v(p):
    return Vector(p) if not isinstance(p, Vector) else p


class MeshBuilder:
    """Accumulates vertices/faces. A transform stack places local geometry in the mesh frame;
    box-projected UVs are computed in the *local* frame so textures follow facades."""

    def __init__(self):
        self.verts = []
        self.faces = []      # (vert idx tuple, material, uv tuple or None)
        self.mats = []
        self.xf = [Matrix.Identity(4)]

    # -- transforms
    def push(self, m):
        self.xf.append(self.xf[-1] @ m)

    def pop(self):
        self.xf.pop()

    def at(self, loc=(0, 0, 0), rot_z=0.0, scale=1.0):
        return Matrix.Translation(loc) @ Matrix.Rotation(rot_z, 4, "Z") @ Matrix.Scale(scale, 4)

    # -- raw
    def vert(self, p):
        self.verts.append(self.xf[-1] @ _v(p))
        return len(self.verts) - 1

    def face(self, idx, mat, uvs=None):
        if mat not in self.mats:
            self.mats.append(mat)
        self.faces.append((tuple(idx), mat, tuple(uvs) if uvs else None))

    def poly(self, pts, mat, uvs=None, tile=None, facing=None):
        """Add a polygon from local points; UVs explicit or box-projected (local frame).
        facing: optional direction the polygon normal must point to (winding is fixed up)."""
        pts = [_v(p) for p in pts]
        if facing is not None and newell(pts).dot(_v(facing)) < 0:
            pts = pts[::-1]
            if uvs is not None:
                uvs = list(uvs)[::-1]
        if uvs is None:
            uvs = box_uv(pts, tile if tile is not None else TILE.get(mat_base(mat), 1.0))
        idx = [self.vert(p) for p in pts]
        self.face(idx, mat, uvs)

    def quad_strip(self, rows, mat, us, vs, close=False):
        """Grid of shared vertices: rows = list of point lists (same length).
        us[j], vs[i] are UV coords for column j and row i (len(us) = n+1 if close)."""
        idx = [[self.vert(p) for p in row] for row in rows]
        n = len(rows[0])
        cols = n if close else n - 1
        for i in range(len(rows) - 1):
            for j in range(cols):
                j1 = (j + 1) % n
                f = (idx[i][j], idx[i][j1], idx[i + 1][j1], idx[i + 1][j])
                uv = ((us[j], vs[i]), (us[j + 1], vs[i]), (us[j + 1], vs[i + 1]), (us[j], vs[i + 1]))
                self.face(f, mat, uv)
        return idx

    def merge(self, other, m=None):
        base = len(self.verts)
        m = m or Matrix.Identity(4)
        for v in other.verts:
            self.verts.append(self.xf[-1] @ m @ v)
        for idx, mat, uvs in other.faces:
            self.face([i + base for i in idx], mat, uvs)

    # -- output
    def build(self, name, coll, smooth=False, sharp_angle=35.0, weld=True, materials=None):
        me = self.build_mesh(name, smooth, sharp_angle, weld, materials)
        o = bpy.data.objects.new(name, me)
        coll.objects.link(o)
        return o

    def build_mesh(self, name, smooth=False, sharp_angle=35.0, weld=True, materials=None):
        me = bpy.data.meshes.new(name)
        bm = bmesh.new()
        bverts = [bm.verts.new(v) for v in self.verts]
        uvl = bm.loops.layers.uv.new("UVMap")
        mat_index = {m: i for i, m in enumerate(self.mats)}
        for idx, mat, uvs in self.faces:
            try:
                f = bm.faces.new([bverts[i] for i in idx])
            except ValueError:      # duplicate face; skip
                continue
            f.material_index = mat_index[mat]
            if uvs:
                for loop, uv in zip(f.loops, uvs):
                    loop[uvl].uv = uv
        if weld:
            bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
        # n-gons -> triangles: Blender (and so the glTF exporter) only computes tangents for tris/quads
        ngons = [f for f in bm.faces if len(f.verts) > 4]
        if ngons:
            bmesh.ops.triangulate(bm, faces=ngons, quad_method="BEAUTY", ngon_method="BEAUTY")
        bm.normal_update()
        bm.to_mesh(me)
        bm.free()
        mats = materials or bpy.data.materials
        for m in self.mats:
            me.materials.append(mats[m])
        if smooth:
            me.shade_smooth()
            if sharp_angle:
                me.set_sharp_from_angle(angle=math.radians(sharp_angle))
        else:
            me.shade_flat()
        return me


def mat_base(mat):
    """Material name -> texture family (e.g. 'plaster_ochre' -> 'plaster')."""
    for k in TILE:
        if mat == k or mat.startswith(k + "_"):
            return k
    return "default"


def newell(pts):
    n = Vector((0, 0, 0))
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        n.x += (a.y - b.y) * (a.z + b.z)
        n.y += (a.z - b.z) * (a.x + b.x)
        n.z += (a.x - b.x) * (a.y + b.y)
    return n


def box_uv(pts, tile):
    """Planar UV by dominant normal axis, in meters / tile."""
    n = newell(pts)
    ax = max(range(3), key=lambda i: abs(n[i]))
    out = []
    for p in pts:
        if ax == 0:
            u, v = (p.y if n.x > 0 else -p.y), p.z
        elif ax == 1:
            u, v = (-p.x if n.y > 0 else p.x), p.z
        else:
            u, v = p.x, (p.y if n.z > 0 else -p.y)
        out.append((u / tile, v / tile))
    return out


# --------------------------------------------------------------- primitives


def box(mb, lo, hi, mat, tile=None, faces="all", uv_offset=(0.0, 0.0)):
    """Axis aligned box in the builder's local frame. faces: 'all' or a set of
    {'+x','-x','+y','-y','+z','-z'}."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    P = [Vector(p) for p in ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    F = {"-z": (0, 3, 2, 1), "+z": (4, 5, 6, 7), "-y": (0, 1, 5, 4),
         "+y": (2, 3, 7, 6), "-x": (3, 0, 4, 7), "+x": (1, 2, 6, 5)}
    t = tile if tile is not None else TILE.get(mat_base(mat), 1.0)
    for k, f in F.items():
        if faces != "all" and k not in faces:
            continue
        pts = [P[i] for i in f]
        uvs = [(u + uv_offset[0], v + uv_offset[1]) for u, v in box_uv(pts, t)]
        mb.poly(pts, mat, uvs)


def lathe(mb, profile, mat, segs=24, center=(0, 0, 0), tile=None, cap_top=False, cap_bottom=False,
          cap_mat=None, angle0=0.0, sweep=2 * math.pi, uv_u=None):
    """Surface of revolution around Z. profile: [(radius, z), ...] bottom to top."""
    cx, cy, cz = center
    t = tile if tile is not None else TILE.get(mat_base(mat), 1.0)
    full = abs(sweep - 2 * math.pi) < 1e-6
    n = segs if full else segs + 1
    rows = []
    for r, z in profile:
        row = []
        for j in range(n):
            a = angle0 + sweep * j / segs
            row.append(Vector((cx + r * math.cos(a), cy + r * math.sin(a), cz + z)))
        rows.append(row)
    rmax = max(r for r, _ in profile)
    circ = (uv_u if uv_u is not None else rmax * sweep / t)
    us = [circ * j / segs for j in range(segs + 1)]
    vs, acc = [0.0], 0.0
    for i in range(1, len(profile)):
        (r0, z0), (r1, z1) = profile[i - 1], profile[i]
        acc += math.hypot(r1 - r0, z1 - z0)
        vs.append(acc / t)
    idx = mb.quad_strip(rows, mat, us, vs, close=full)
    cm = cap_mat or mat
    if cap_top and profile[-1][0] > 1e-5 and full:
        r, z = profile[-1]
        pts = [Vector((cx + r * math.cos(angle0 + 2 * math.pi * j / segs),
                       cy + r * math.sin(angle0 + 2 * math.pi * j / segs), cz + z)) for j in range(segs)]
        mb.poly(pts, cm)
    if cap_bottom and profile[0][0] > 1e-5 and full:
        r, z = profile[0]
        pts = [Vector((cx + r * math.cos(angle0 + 2 * math.pi * j / segs),
                       cy + r * math.sin(angle0 + 2 * math.pi * j / segs), cz + z)) for j in range(segs)]
        mb.poly(list(reversed(pts)), cm)
    return idx


def sweep_x(mb, profile, x0, x1, mat, tile=None, cap=True, y=0.0, z=0.0):
    """Extrude a 2D profile [(out, up), ...] (out = -Y towards the viewer) along X.
    Used for cornices, string courses and moldings on a facade in its local frame."""
    t = tile if tile is not None else TILE.get(mat_base(mat), 1.0)
    rows = []
    for (o, u) in profile:
        rows.append([Vector((x0, y - o, z + u)), Vector((x1, y - o, z + u))])
    vs, acc = [0.0], 0.0
    for i in range(1, len(profile)):
        acc += math.hypot(profile[i][0] - profile[i - 1][0], profile[i][1] - profile[i - 1][1])
        vs.append(acc / t)
    # rows are profile points; columns are the two ends -> build quads manually for uv
    for i in range(len(profile) - 1):
        a0, a1 = rows[i]
        b0, b1 = rows[i + 1]
        uv = ((x0 / t, vs[i]), (x1 / t, vs[i]), (x1 / t, vs[i + 1]), (x0 / t, vs[i + 1]))
        mb.poly([a0, a1, b1, b0], mat, uv)
    if cap:
        pts0 = [Vector((x0, y - o, z + u)) for o, u in profile]
        pts1 = [Vector((x1, y - o, z + u)) for o, u in profile]
        if len(pts0) >= 3:
            mb.poly(list(reversed(pts0)) if True else pts0, mat)
            mb.poly(pts1, mat)


def tube(mb, path, radii, mat, sides=6, tile=None, cap_end=False, twist=0.0):
    """Tube along a 3D polyline with per-point radius (branches, rods). Parallel transport frames."""
    t = tile if tile is not None else TILE.get(mat_base(mat), 1.0)
    path = [_v(p) for p in path]
    tangents = []
    for i in range(len(path)):
        a = path[max(0, i - 1)]
        b = path[min(len(path) - 1, i + 1)]
        tangents.append((b - a).normalized())
    up = Vector((0, 0, 1)) if abs(tangents[0].z) < 0.9 else Vector((1, 0, 0))
    nrm = tangents[0].cross(up).normalized()
    rows, frames = [], []
    for i, p in enumerate(path):
        tg = tangents[i]
        nrm = (nrm - tg * nrm.dot(tg)).normalized()
        bnm = tg.cross(nrm)
        row = []
        for j in range(sides):
            a = 2 * math.pi * j / sides + twist * i
            row.append(p + (nrm * math.cos(a) + bnm * math.sin(a)) * radii[i])
        rows.append(row)
    rmax = max(radii)
    circ = max(1, round(2 * math.pi * rmax / t * 2)) / 2 if rmax * 2 * math.pi > t * 0.5 else 0.5
    us = [circ * j / sides for j in range(sides + 1)]
    vs, acc = [0.0], 0.0
    for i in range(1, len(path)):
        acc += (path[i] - path[i - 1]).length
        vs.append(acc / t)
    mb.quad_strip(rows, mat, us, vs, close=True)
    if cap_end and radii[-1] > 1e-4:
        mb.poly(rows[-1], mat)


def card(mb, center, right, up, w, h, mat, uv_rect, base_at_bottom=True):
    """Single quad (2 tris). right/up are unit vectors; uv_rect = (u0, v0, u1, v1)."""
    c = _v(center)
    r = _v(right) * (w / 2)
    u = _v(up) * h
    b = c if base_at_bottom else c - u / 2
    pts = [b - r, b + r, b + r + u, b - r + u]
    u0, v0, u1, v1 = uv_rect
    mb.poly(pts, mat, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])


def apply_transforms(obj):
    """Bake rotation/scale into the mesh (only for single-user meshes)."""
    mw = obj.matrix_basis.copy()
    loc, rot, sca = mw.decompose()
    m = Matrix.LocRotScale(None, rot, sca)
    obj.data.transform(m)
    obj.rotation_euler = (0, 0, 0)
    obj.scale = (1, 1, 1)
