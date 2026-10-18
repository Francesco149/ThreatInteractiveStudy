"""Preview rendering helpers: golden-hour world, EEVEE settings and named cameras.

Cameras live in the 'Preview_Cameras' collection and are NOT exported (export_cameras=False).
"""
import math

import bpy
from mathutils import Vector

from common import get_coll

SUN_AZIMUTH = 240.0     # degrees clockwise from north (sun is in the WSW)
SUN_ELEVATION = 17.0


def sun_direction():
    """Unit vector pointing *towards* the sun (Blender frame)."""
    az, el = math.radians(SUN_AZIMUTH), math.radians(SUN_ELEVATION)
    return Vector((math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)))


def setup_world(strength=0.1):
    sc = bpy.context.scene
    w = sc.world or bpy.data.worlds.new("World")
    sc.world = w
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    sky = nt.nodes.new("ShaderNodeTexSky")
    sky.sky_type = "MULTIPLE_SCATTERING"
    sky.sun_disc = False
    sky.sun_elevation = math.radians(SUN_ELEVATION)
    # verified by rendering: Blender's sky sun_rotation equals the compass azimuth used here
    sky.sun_rotation = math.radians(SUN_AZIMUTH)
    bg = nt.nodes.new("ShaderNodeBackground")
    bg.inputs["Strength"].default_value = strength
    out = nt.nodes.new("ShaderNodeOutputWorld")
    nt.links.new(sky.outputs["Color"], bg.inputs["Color"])
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])
    return w


def setup_eevee(res=(1920, 1080), samples=64):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.resolution_percentage = 100
    ee = sc.eevee
    ee.taa_render_samples = samples
    for attr, val in (("use_raytracing", True), ("use_shadows", True), ("shadow_ray_count", 2),
                      ("shadow_step_count", 8), ("use_fast_gi", True)):
        if hasattr(ee, attr):
            setattr(ee, attr, val)
    if hasattr(ee, "ray_tracing_options"):
        ee.ray_tracing_options.resolution_scale = "2"
    sc.view_settings.view_transform = "AgX"
    sc.view_settings.look = "AgX - Medium High Contrast" if "AgX - Medium High Contrast" in [
        i.identifier for i in sc.view_settings.bl_rna.properties["look"].enum_items] else "None"
    sc.view_settings.exposure = 0.0
    sc.render.image_settings.file_format = "JPEG"   # reference renders, shipped as JPEG
    sc.render.image_settings.quality = 90


def camera(name, pos, target, lens=24.0):
    coll = get_coll("Preview_Cameras")
    cam = bpy.data.objects.get(name)
    if cam is None:
        cam = bpy.data.objects.new(name, bpy.data.cameras.new(name))
        coll.objects.link(cam)
    cam.data.lens = lens
    cam.data.clip_start = 0.05
    cam.data.clip_end = 400
    cam.location = pos
    d = Vector(target) - Vector(pos)
    cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    return cam


def render(cam, path, res=None, samples=None):
    sc = bpy.context.scene
    sc.camera = cam
    if res:
        sc.render.resolution_x, sc.render.resolution_y = res
    if samples:
        sc.eevee.taa_render_samples = samples
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    return path
