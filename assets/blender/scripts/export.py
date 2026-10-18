"""glTF 2.0 export of the courtyard (run inside Blender).

Settings follow the engine conventions: separate .gltf + .bin + PNG textures, +Y up, modifiers
applied, normals + tangents + UVs, materials, custom properties as extras, punctual lights,
no cameras, no animation, no Draco / meshopt. Textures are referenced in place from
assets/scene/textures (export_keep_originals), so the processed PNGs are not re-encoded.
"""
import os

import bpy

from common import ROOT

OUT = os.path.join(ROOT, "assets", "scene", "courtyard.gltf")


def export(path=OUT):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # preview cameras must not end up in the file
    cams = bpy.data.collections.get("Preview_Cameras")
    vl = bpy.context.view_layer
    excluded = []
    if cams is not None:
        for lc in vl.layer_collection.children:
            if lc.collection == cams:
                excluded.append((lc, lc.exclude))
                lc.exclude = True
    try:
        bpy.ops.export_scene.gltf(
            filepath=path,
            export_format="GLTF_SEPARATE",
            export_texture_dir="textures",
            export_keep_originals=True,
            export_image_format="AUTO",
            export_yup=True,
            export_apply=True,
            export_texcoords=True,
            export_normals=True,
            export_tangents=True,
            export_materials="EXPORT",
            export_vertex_color="NONE",
            export_extras=True,
            export_lights=True,
            export_cameras=False,
            export_animations=False,
            export_draco_mesh_compression_enable=False,
            export_use_gltfpack=False,
            export_gpu_instances=False,
            export_import_convert_lighting_mode="SPEC",
            use_visible=False,
            use_renderable=False,
            use_selection=False,
        )
    finally:
        for lc, ex in excluded:
            lc.exclude = ex
    return path
