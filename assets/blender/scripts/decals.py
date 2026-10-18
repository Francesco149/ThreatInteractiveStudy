"""Decal markers: Empties (cube display) whose box is the decal projection volume.

Engine convention: the decal box is [-0.5, 0.5]^3 in the node's local space, scaled by the
node scale, and projected along the node's local glTF +Y axis. The exporter maps Blender local
+Z to glTF local +Y (and scale (sx, sy, sz) -> (sx, sz, sy)), so in Blender:
  * scale X / Y = footprint in meters, scale Z = projection depth (0.3-0.6 m)
  * ground decals keep identity rotation (projection straight down the up axis)
  * wall decals are rotated so local +Z points out of the wall
Custom properties (node extras): "decal": "stain" | "moss" | "cracks", "opacity": 0.6-1.0.
"""
import math

import bpy

from common import get_coll

SIDEWALK = 0.12

# name, kind, location, rotation (euler XYZ, radians), scale (footprint x, footprint y, depth), opacity
DECALS = [
    # water / oil stains around the market stall, the cart track and the fire barrel
    ("Decal_Stain_Market_01", "stain", (11.0, 7.2, 0.0), (0, 0, 0.3), (2.2, 1.6, 0.4), 0.8),
    ("Decal_Stain_Market_02", "stain", (13.8, 9.9, 0.0), (0, 0, -0.5), (1.3, 1.0, 0.4), 0.7),
    ("Decal_Stain_Cart_01", "stain", (-3.0, -13.6, 0.0), (0, 0, 0.05), (3.2, 1.3, 0.4), 0.6),
    ("Decal_Stain_Cart_02", "stain", (-10.6, -13.4, 0.0), (0, 0, 0.6), (1.5, 1.1, 0.4), 0.75),
    ("Decal_Stain_FireBarrel", "stain", (-18.2, -15.4, 0.0), (0, 0, 0.0), (1.9, 1.9, 0.5), 0.95),
    # moss at the fountain foot, along the park border and at the base of a damp brick wall
    ("Decal_Moss_Fountain_01", "moss", (-1.7, -2.7, 0.0), (0, 0, 0.4), (2.1, 1.2, 0.5), 0.85),
    ("Decal_Moss_Fountain_02", "moss", (5.2, 1.6, 0.0), (0, 0, -0.7), (1.8, 1.0, 0.5), 0.8),
    ("Decal_Moss_Park_South", "moss", (-11.0, 2.75, 0.0), (0, 0, 0.0), (4.0, 0.8, 0.5), 0.9),
    ("Decal_Moss_Park_West", "moss", (-17.25, 9.0, 0.0), (0, 0, 0.0), (0.8, 3.6, 0.5), 0.85),
    ("Decal_Moss_Wall_W3", "moss", (-22.0, 7.0, 0.55), (0, math.pi / 2, 0), (1.1, 3.0, 0.5), 0.8),
    # cracks in the paving and in a plaster facade
    ("Decal_Cracks_North_Sidewalk", "cracks", (-8.0, 18.8, SIDEWALK), (0, 0, 0.2), (1.6, 1.1, 0.3), 1.0),
    ("Decal_Cracks_East_Sidewalk", "cracks", (20.8, -4.0, SIDEWALK), (0, 0, 1.3), (1.7, 1.0, 0.3), 0.9),
    ("Decal_Cracks_Arcade_Floor", "cracks", (3.0, -21.8, SIDEWALK), (0, 0, -0.4), (1.4, 1.2, 0.3), 0.8),
    ("Decal_Cracks_Wall_S4", "cracks", (-10.66, -20.0, 1.6), (-math.pi / 2, 0, 0), (1.0, 1.2, 0.4), 0.85),
]


def build():
    coll = get_coll("Decals")
    out = []
    for name, kind, loc, rot, scale, opacity in DECALS:
        o = bpy.data.objects.get(name)
        if o is None:
            o = bpy.data.objects.new(name, None)
            coll.objects.link(o)
        o.empty_display_type = "CUBE"
        o.empty_display_size = 0.5          # displayed cube == the [-0.5, 0.5]^3 projection box
        o.location = loc
        o.rotation_euler = rot
        o.scale = scale
        o["decal"] = kind
        o["opacity"] = opacity
        out.append(o)
    return out
