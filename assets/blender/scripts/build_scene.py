"""Build the whole courtyard scene from scratch, save the .blend and export the glTF.

Run inside Blender (e.g. Scripting tab, or `blender -b -P assets/blender/scripts/build_scene.py`
from the repository root), with the scripts directory on sys.path:
    import sys; sys.path.insert(0, r"<repository>/assets/blender/scripts")
    import build_scene; build_scene.main()
"""
import importlib
import os
import sys

import bpy

# This file's folder; when exec()'d without __file__, assume Blender was started in the repository.
SCRIPTS = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else \
    os.path.join(os.getcwd(), "assets", "blender", "scripts")
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

MODULES = ["common", "materials", "buildings", "ground", "props", "foliage", "busts", "lights", "decals",
           "preview", "bench_path", "export"]
BLEND = os.path.abspath(os.path.join(SCRIPTS, "..", "courtyard.blend"))


def load():
    mods = {}
    for name in MODULES:
        if not os.path.exists(os.path.join(SCRIPTS, name + ".py")):
            continue
        mods[name] = importlib.reload(sys.modules[name]) if name in sys.modules else importlib.import_module(name)
    return mods


def main(save=True, export=True, stages=None):
    m = load()
    m["common"].reset_scene()
    m["materials"].build_all()
    stages = stages or ["buildings", "ground", "props", "foliage", "busts", "lights", "decals"]
    res = {}
    if "buildings" in stages:
        res["buildings"] = m["buildings"].build()
    if "ground" in stages:
        m["ground"].build()
    props = None
    if "props" in stages:
        props = m["props"].build()
    if "foliage" in stages:
        boxes = [mtx for inf in res.get("buildings", {}).values() for mtx in inf.get("flower_boxes", [])]
        m["foliage"].build(pots=props["pots"] if props else (), flower_boxes=boxes)
    if "busts" in stages:
        m["busts"].build()
    if "lights" in stages and "lights" in m:
        m["lights"].build(props)
    if "decals" in stages and "decals" in m:
        m["decals"].build()
    if "preview" in m:
        m["preview"].setup_world()
        m["preview"].setup_eevee()
    if "bench_path" in m:
        m["bench_path"].write()
        m["bench_path"].make_cameras()
    if save:
        os.makedirs(os.path.dirname(BLEND), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=BLEND, compress=True)
    if export and "export" in m:
        res["export"] = m["export"].export()
    return res


if __name__ == "__main__":
    main()
