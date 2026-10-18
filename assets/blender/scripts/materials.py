"""glTF-compatible Principled BSDF materials for the courtyard.

Node conventions (what the Blender glTF exporter understands):
  * Base Color: Image (sRGB) [-> Mix MULTIPLY with a constant colour = baseColorFactor]
  * ORM image (Non-Color) -> Separate Color: G -> Roughness, B -> Metallic, R -> Occlusion input
    of a "glTF Material Output" group node. All three come from the same image, so the exporter
    writes one texture for metallicRoughnessTexture + occlusionTexture.
  * Normal: Image (Non-Color) -> Normal Map (tangent space) -> Normal
  * MASK: image alpha -> Math ROUND -> Alpha (alphaCutoff 0.5)
  * BLEND: constant Alpha < 1, surface_render_method BLENDED
  * Emission Color / Strength -> emissiveFactor (+ KHR_materials_emissive_strength)
Each material carries custom properties "shading_model" and "profile" (exported as extras).
"""
import os

import bpy

from common import TEX_DIR

# name: spec. Keys: albedo, normal, orm, tint, color, rough, metal, sm (shading_model),
# profile, alpha ('MASK'|'BLEND'), alpha_value, emission (rgb), strength, double, nstr
SPECS = {
    # walls -----------------------------------------------------------------
    "plaster_ochre": dict(albedo="plaster", normal="plaster", tint=(0.86, 0.60, 0.32), rough=0.88, profile="plaster"),
    "plaster_yellow": dict(albedo="plaster", normal="plaster", tint=(0.93, 0.79, 0.46), rough=0.88, profile="plaster"),
    "plaster_pink": dict(albedo="plaster", normal="plaster", tint=(0.86, 0.52, 0.44), rough=0.88, profile="plaster"),
    "plaster_cream": dict(albedo="plaster", normal="plaster", tint=(0.88, 0.82, 0.68), rough=0.88, profile="plaster"),
    "plaster_sage": dict(albedo="plaster", normal="plaster", tint=(0.58, 0.67, 0.50), rough=0.88, profile="plaster"),
    "plaster_blue": dict(albedo="plaster", normal="plaster", tint=(0.52, 0.64, 0.74), rough=0.88, profile="plaster"),
    "plaster_white": dict(albedo="plaster", normal="plaster", tint=(0.82, 0.80, 0.76), rough=0.9, profile="plaster"),
    "brick": dict(albedo="brick", normal="brick", orm="brick", profile="brick"),
    "stone": dict(albedo="stone", normal="stone", orm="stone", tint=(0.92, 0.90, 0.86), profile="stone"),
    "stone_grey": dict(albedo="stone", normal="stone", orm="stone", tint=(0.66, 0.66, 0.66), profile="stone"),
    "roof": dict(albedo="roof", normal="roof", orm="roof", tint=(0.78, 0.58, 0.50), profile="ceramic"),
    "roof_dark": dict(albedo="roof", normal="roof", orm="roof", tint=(0.55, 0.40, 0.36), profile="ceramic"),
    # ground ----------------------------------------------------------------
    "cobble": dict(albedo="cobble", normal="cobble", orm="cobble", profile="stone"),
    "paving": dict(albedo="paving", normal="paving", rough=0.8, profile="stone"),
    "grass": dict(albedo="grass", normal="grass", rough=0.92, profile="ground"),
    # wood ------------------------------------------------------------------
    "wood": dict(albedo="wood", normal="wood", orm="wood", profile="wood"),
    "wood_dark": dict(albedo="wood", normal="wood", orm="wood", tint=(0.55, 0.42, 0.33), profile="wood"),
    "paintedwood_green": dict(albedo="paintedwood", normal="paintedwood", tint=(0.22, 0.38, 0.28), rough=0.62, profile="wood"),
    "paintedwood_blue": dict(albedo="paintedwood", normal="paintedwood", tint=(0.25, 0.38, 0.52), rough=0.62, profile="wood"),
    "paintedwood_red": dict(albedo="paintedwood", normal="paintedwood", tint=(0.55, 0.16, 0.12), rough=0.62, profile="wood"),
    "paintedwood_brown": dict(albedo="paintedwood", normal="paintedwood", tint=(0.36, 0.22, 0.14), rough=0.62, profile="wood"),
    "paintedwood_white": dict(albedo="paintedwood", normal="paintedwood", tint=(0.88, 0.86, 0.80), rough=0.6, profile="wood"),
    "bark": dict(albedo="bark", normal="bark", rough=0.92, profile="bark"),
    # cloth -----------------------------------------------------------------
    "cloth_stripes": dict(albedo="cloth", normal="cloth", rough=0.9, profile="cloth", double=True),
    "cloth_blue": dict(color=(0.10, 0.16, 0.30), normal="cloth", rough=0.85, profile="cloth"),
    "cloth_cream": dict(color=(0.78, 0.72, 0.60), normal="cloth", rough=0.9, profile="cloth", double=True),
    # metals ----------------------------------------------------------------
    "paint_iron_green": dict(albedo="paint_iron_green", normal="paint_iron", orm="paint_iron", profile="paint_iron"),
    "paint_iron_red": dict(albedo="paint_iron_red", normal="paint_iron", orm="paint_iron", profile="paint_iron"),
    "iron": dict(color=(0.32, 0.31, 0.30), normal="paint_iron", rough=0.55, metal=1.0, profile="iron"),
    "gold": dict(albedo="brass", orm="brass", profile="gold"),
    # glass / water / emissive -------------------------------------------------
    "window_dark": dict(color=(0.035, 0.045, 0.055), rough=0.06, profile="glass"),
    "window_lit": dict(color=(0.0, 0.0, 0.0), rough=0.3, emission=(1.0, 0.62, 0.32), strength=2.0,
                       sm="emissive", profile="glass"),
    "glass_clear": dict(color=(0.85, 0.92, 0.92), rough=0.04, alpha="BLEND", alpha_value=0.15, sm="glass",
                        profile="glass", double=True),
    "glass_green": dict(color=(0.12, 0.42, 0.16), rough=0.05, alpha="BLEND", alpha_value=0.55, sm="glass",
                        profile="glass"),
    "glass_amber": dict(color=(0.55, 0.28, 0.06), rough=0.05, alpha="BLEND", alpha_value=0.55, sm="glass",
                        profile="glass"),
    "water": dict(color=(0.10, 0.22, 0.24), rough=0.03, alpha="BLEND", alpha_value=0.65, sm="glass", profile="glass"),
    "embers": dict(color=(0.05, 0.03, 0.02), rough=0.9, emission=(1.0, 0.32, 0.06), strength=12.0,
                   sm="emissive", profile="wood"),
    "lamp_bulb": dict(color=(1.0, 0.85, 0.65), rough=0.3, emission=(1.0, 0.78, 0.52), strength=40.0,
                      sm="emissive", profile="glass"),
    "neon_pink": dict(color=(0.9, 0.3, 0.6), rough=0.3, emission=(1.0, 0.12, 0.45), strength=30.0,
                      sm="emissive", profile="glass"),
    "neon_cyan": dict(color=(0.3, 0.8, 0.9), rough=0.3, emission=(0.1, 0.85, 1.0), strength=30.0,
                      sm="emissive", profile="glass"),
    # characters --------------------------------------------------------------
    "skin_light": dict(albedo="skin", normal="skin", orm="skin", sm="skin", profile="skin", nstr=0.6),
    "skin_dark": dict(albedo="skin", normal="skin", orm="skin", tint=(0.52, 0.38, 0.30), sm="skin", profile="skin",
                      nstr=0.6),
    "eye": dict(albedo="eye", rough=0.05, sm="eye", profile="eye"),
    # foliage -----------------------------------------------------------------
    "leaves": dict(albedo="leaf_atlas", normal="leaf_atlas", rough=0.6, alpha="MASK", sm="foliage",
                   profile="foliage", double=True),
    "grass_cards": dict(albedo="grass_atlas", rough=0.7, alpha="MASK", sm="foliage", profile="foliage",
                        double=True),
    # misc ----------------------------------------------------------------------
    "terracotta": dict(color=(0.62, 0.30, 0.18), normal="stone", rough=0.8, profile="ceramic"),
    "marble": dict(albedo="stone", normal="stone", tint=(1.0, 1.0, 1.0), rough=0.25, profile="stone"),
    "fruit_red": dict(color=(0.55, 0.06, 0.04), rough=0.35, profile="foliage"),
    "fruit_orange": dict(color=(0.85, 0.38, 0.04), rough=0.5, profile="foliage"),
    "fruit_green": dict(color=(0.28, 0.48, 0.10), rough=0.55, profile="foliage"),
    "fruit_yellow": dict(color=(0.85, 0.70, 0.12), rough=0.45, profile="foliage"),
    "black_rubber": dict(color=(0.03, 0.03, 0.03), rough=0.8, profile="default"),
}


def _image(name, srgb=True):
    path = os.path.join(TEX_DIR, name + ".png")
    key = os.path.basename(path)
    im = bpy.data.images.get(key)
    if im is None:
        im = bpy.data.images.load(path, check_existing=True)
        im.name = key
    im.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    im.alpha_mode = "STRAIGHT"
    return im


def gltf_output_group():
    """The custom node group the glTF exporter looks for to find the occlusion texture."""
    g = bpy.data.node_groups.get("glTF Material Output")
    if g is None:
        g = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        g.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
        g.interface.new_socket("Thickness", in_out="INPUT", socket_type="NodeSocketFloat")
        g.nodes.new("NodeGroupInput")
    return g


def make_material(name, spec):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    N, L = nt.nodes, nt.links
    out = N.new("ShaderNodeOutputMaterial"); out.location = (600, 0)
    bsdf = N.new("ShaderNodeBsdfPrincipled"); bsdf.location = (250, 0)
    L.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    x = -500

    if spec.get("albedo"):
        img = N.new("ShaderNodeTexImage"); img.location = (x, 300)
        img.image = _image(spec["albedo"] + "_albedo", True)
        col = img.outputs["Color"]
        if spec.get("tint"):
            mix = N.new("ShaderNodeMix"); mix.location = (-150, 300)
            mix.data_type = "RGBA"; mix.blend_type = "MULTIPLY"
            mix.inputs["Factor"].default_value = 1.0
            mix.inputs["B"].default_value = (*spec["tint"], 1.0)
            L.new(col, mix.inputs["A"])
            col = mix.outputs["Result"]
        L.new(col, bsdf.inputs["Base Color"])
        if spec.get("alpha") == "MASK":
            rnd = N.new("ShaderNodeMath"); rnd.operation = "ROUND"; rnd.location = (-150, 100)
            L.new(img.outputs["Alpha"], rnd.inputs[0])
            L.new(rnd.outputs[0], bsdf.inputs["Alpha"])
    else:
        bsdf.inputs["Base Color"].default_value = (*spec.get("color", (0.8, 0.8, 0.8)), 1.0)

    if spec.get("orm"):
        orm = N.new("ShaderNodeTexImage"); orm.location = (x, 0)
        orm.image = _image(spec["orm"] + "_orm", False)
        sep = N.new("ShaderNodeSeparateColor"); sep.location = (-200, 0)
        L.new(orm.outputs["Color"], sep.inputs["Color"])
        L.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
        L.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
        grp = N.new("ShaderNodeGroup"); grp.node_tree = gltf_output_group(); grp.location = (250, -450)
        L.new(sep.outputs["Red"], grp.inputs["Occlusion"])
    else:
        bsdf.inputs["Roughness"].default_value = spec.get("rough", 0.5)
        bsdf.inputs["Metallic"].default_value = spec.get("metal", 0.0)

    if spec.get("normal"):
        nimg = N.new("ShaderNodeTexImage"); nimg.location = (x, -300)
        nimg.image = _image(spec["normal"] + "_normal", False)
        nm = N.new("ShaderNodeNormalMap"); nm.location = (-150, -300)
        nm.inputs["Strength"].default_value = spec.get("nstr", 1.0)
        L.new(nimg.outputs["Color"], nm.inputs["Color"])
        L.new(nm.outputs["Normal"], bsdf.inputs["Normal"])

    if spec.get("alpha") == "BLEND":
        bsdf.inputs["Alpha"].default_value = spec.get("alpha_value", 0.3)
        m.surface_render_method = "BLENDED"
        m.use_transparency_overlap = False
    else:
        m.surface_render_method = "DITHERED"

    if spec.get("emission"):
        bsdf.inputs["Emission Color"].default_value = (*spec["emission"], 1.0)
        bsdf.inputs["Emission Strength"].default_value = spec.get("strength", 1.0)

    if spec.get("sm") == "skin":
        # Blender preview only (not exported): a little random-walk SSS for EEVEE renders
        bsdf.inputs["Subsurface Weight"].default_value = 0.25
        bsdf.inputs["Subsurface Radius"].default_value = (1.0, 0.35, 0.15)
        bsdf.inputs["Subsurface Scale"].default_value = 0.01

    m.use_backface_culling = not spec.get("double", False)
    m["shading_model"] = spec.get("sm", "default")
    m["profile"] = spec.get("profile", "default")
    return m


def build_all():
    return {name: make_material(name, spec) for name, spec in SPECS.items()}
