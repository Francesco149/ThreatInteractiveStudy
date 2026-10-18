"""Punctual lights (exported through KHR_lights_punctual): golden-hour sun, street-lamp point
lights, wall-mounted spots, neon / interior fill lights.

Custom properties: "shadow": 1 for shadow casters (sun, 4 street lamps, all spots), 0 otherwise.
Point / spot "range" comes from Blender's custom cutoff distance (exported as light.range).
Intensities are Blender watts (sun: W/m^2); the exporter converts them to cd / lux."""
import math

import bpy
from mathutils import Matrix, Vector

from common import get_coll, place, set_props
import preview

LAMP_COLOR = (1.0, 0.70, 0.42)


def light(name, kind, loc, coll, energy, color, shadow, direction=None, rng=None, radius=0.05, spot_deg=45.0,
          blend=0.25):
    ld = bpy.data.lights.new(name, kind)
    ld.energy = energy
    ld.color = color
    # Blender 5.x lights always carry a node tree; the glTF exporter reads colour (and, for the
    # sun, the intensity) from its Emission node, so mirror the light settings there.
    if getattr(ld, "node_tree", None) is not None:
        for n in ld.node_tree.nodes:
            if n.bl_idname == "ShaderNodeEmission":
                n.inputs["Color"].default_value = (*color, 1.0)
                n.inputs["Strength"].default_value = energy if kind == "SUN" else 1.0
    ld.use_shadow = bool(shadow)
    if kind != "SUN":
        ld.shadow_soft_size = radius
    if rng:
        ld.use_custom_distance = True
        ld.cutoff_distance = rng
    if kind == "SPOT":
        ld.spot_size = math.radians(spot_deg)
        ld.spot_blend = blend
    o = bpy.data.objects.new(name, ld)
    o.location = loc
    if direction is not None:
        o.rotation_euler = Vector(direction).normalized().to_track_quat("-Z", "Y").to_euler()
    coll.objects.link(o)
    set_props(o, shadow=int(bool(shadow)))
    set_props(ld, shadow=int(bool(shadow)))
    return o


SPOTS = [
    # name, position, target, energy (W), cone (deg)
    ("Light_Spot_Arcade", (6.0, -22.0, 4.7), (6.0, -21.2, 0.0), 350.0, 70.0),
    ("Light_Spot_Market", (21.85, 7.5, 4.6), (12.0, 7.5, 0.8), 600.0, 38.0),
    ("Light_Spot_Busts", (21.85, -4.5, 4.4), (12.9, -4.5, 1.5), 500.0, 34.0),
    ("Light_Spot_Entrance", (-3.44, 19.75, 4.0), (-3.44, 17.0, 0.0), 300.0, 60.0),
]


def build(props=None):
    coll = get_coll("Lights")
    d = preview.sun_direction()
    sun = light("Light_Sun", "SUN", (0.0, 0.0, 30.0), coll, 5.5, (1.0, 0.68, 0.42), True, direction=-d)
    sun.data.angle = math.radians(0.6)
    if props:
        for i, (o, pos, shadow) in enumerate(props["lamps"]):
            light(f"Light_Lamp_{i + 1:02d}", "POINT", pos, coll, 120.0, LAMP_COLOR, shadow, rng=10.0, radius=0.06)
        housing = props["spot_housing"]
    else:
        housing = None
    for name, pos, tgt, energy, cone in SPOTS:
        dv = Vector(tgt) - Vector(pos)
        light(name, "SPOT", pos, coll, energy, (1.0, 0.82, 0.62), True, direction=dv, rng=16.0, radius=0.08,
              spot_deg=cone)
        if housing is not None:
            m = Matrix.Translation(pos) @ dv.normalized().to_track_quat("-Z", "Y").to_matrix().to_4x4()
            place(housing, name.replace("Light_", "Housing_"), m @ Matrix.Translation((0, 0, 0.08)), props["coll"])
    # cafe interior + neon glow (no shadows)
    light("Light_Cafe_Interior", "POINT", (17.0, -22.0, 2.6), coll, 80.0, (1.0, 0.72, 0.45), False, rng=6.0, radius=0.3)
    light("Light_Neon_Glow", "POINT", (17.0, -19.6, 4.1), coll, 25.0, (1.0, 0.25, 0.6), False, rng=4.0, radius=0.4)
    light("Light_Fire_Barrel", "POINT", (-18.2, -15.4, 1.1), coll, 60.0, (1.0, 0.45, 0.15), False, rng=6.0, radius=0.2)
    return sun
