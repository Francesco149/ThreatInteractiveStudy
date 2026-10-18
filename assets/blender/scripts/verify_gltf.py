"""Sanity-check the exported courtyard.gltf (plain Python, no Blender needed).

    python -I assets/blender/scripts/verify_gltf.py [path/to/courtyard.gltf] [--json stats.json]

Reports: node / mesh / primitive / material / texture counts, rendered triangle count
(instancing expanded), draw count (primitives x mesh instances), alpha modes, shading models,
extras usage (no_shadow, wind, anim, vfx, shadow), lights, and missing texture files.
Exits non-zero when a convention is violated.
"""
import argparse
import json
import os
import sys
from collections import Counter

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scene", "courtyard.gltf")
SHADING_MODELS = {"default", "skin", "foliage", "eye", "glass", "emissive"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default=DEFAULT)
    ap.add_argument("--json", default=None, help="also write the stats to this file")
    args = ap.parse_args()
    path = os.path.abspath(args.path)
    g = json.load(open(path, encoding="utf-8"))
    base = os.path.dirname(path)
    errors = []

    meshes, nodes, mats = g.get("meshes", []), g.get("nodes", []), g.get("materials", [])
    acc = g.get("accessors", [])

    def prim_tris(p):
        if "indices" in p:
            return acc[p["indices"]]["count"] // 3
        return acc[p["attributes"]["POSITION"]]["count"] // 3

    mesh_tris = [sum(prim_tris(p) for p in m["primitives"]) for m in meshes]
    mesh_nodes = [n for n in nodes if "mesh" in n]
    inst = Counter(n["mesh"] for n in mesh_nodes)
    tris = sum(mesh_tris[m] * c for m, c in inst.items())
    unique_tris = sum(mesh_tris)
    draws = sum(len(meshes[m]["primitives"]) * c for m, c in inst.items())

    for m in meshes:
        for p in m["primitives"]:
            a = p["attributes"]
            for need in ("POSITION", "NORMAL", "TEXCOORD_0", "TANGENT"):
                if need not in a:
                    errors.append(f"mesh {m.get('name')} primitive lacks {need}")

    alpha = Counter(m.get("alphaMode", "OPAQUE") for m in mats)
    sm = Counter((m.get("extras") or {}).get("shading_model", "<missing>") for m in mats)
    prof = Counter((m.get("extras") or {}).get("profile", "<missing>") for m in mats)
    for m in mats:
        ex = m.get("extras") or {}
        if ex.get("shading_model") not in SHADING_MODELS:
            errors.append(f"material {m['name']}: bad shading_model {ex.get('shading_model')}")
        if "profile" not in ex:
            errors.append(f"material {m['name']}: missing profile")
        if ex.get("shading_model") == "foliage" and m.get("alphaMode") != "MASK":
            errors.append(f"material {m['name']}: foliage should be MASK")
        if ex.get("shading_model") == "glass" and m.get("alphaMode") != "BLEND":
            errors.append(f"material {m['name']}: glass should be BLEND")
        if ex.get("profile") == "paint_iron" and "metallicRoughnessTexture" not in m.get("pbrMetallicRoughness", {}):
            errors.append(f"material {m['name']}: paint_iron needs a metallicRoughness mask texture")

    tags = Counter()
    for n in nodes:
        for k in (n.get("extras") or {}):
            tags[k] += 1
    anim = [(n["name"], n["extras"]) for n in nodes if "anim" in (n.get("extras") or {})]
    vfx = Counter(n["extras"]["vfx"] for n in nodes if "vfx" in (n.get("extras") or {}))
    decals = Counter(n["extras"]["decal"] for n in nodes if "decal" in (n.get("extras") or {}))
    for n in nodes:
        ex = n.get("extras") or {}
        if "decal" in ex:
            if ex["decal"] not in ("stain", "moss", "cracks"):
                errors.append(f"decal {n['name']}: unknown kind {ex['decal']}")
            if "mesh" in n or len(n.get("scale", [])) != 3:
                errors.append(f"decal {n['name']}: must be an empty with a scale (box size)")

    lights = g.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
    light_nodes = [n for n in nodes if "KHR_lights_punctual" in n.get("extensions", {})]
    lt = Counter(l["type"] for l in lights)
    shadow_lights = sum(1 for n in light_nodes if (n.get("extras") or {}).get("shadow") == 1)

    missing = [im["uri"] for im in g.get("images", []) if "uri" in im and not os.path.exists(os.path.join(base, im["uri"]))]
    errors += [f"missing image {u}" for u in missing]
    if any("draco" in e.lower() or "meshopt" in e.lower() for e in g.get("extensionsUsed", [])):
        errors.append("compression extension used")
    if any("camera" in n for n in nodes):
        errors.append("camera node exported")

    stats = dict(
        file=path, nodes=len(nodes), mesh_nodes=len(mesh_nodes), meshes=len(meshes),
        primitives=sum(len(m["primitives"]) for m in meshes), draws=draws, triangles=tris,
        unique_triangles=unique_tris, materials=len(mats), textures=len(g.get("textures", [])),
        images=len(g.get("images", [])), alpha_modes=dict(alpha), shading_models=dict(sm), profiles=dict(prof),
        node_extras=dict(tags), animated=anim, vfx=dict(vfx), decals=dict(decals), lights=dict(lt), light_nodes=len(light_nodes),
        shadow_lights=shadow_lights, extensions=g.get("extensionsUsed", []), errors=errors,
    )
    print(json.dumps(stats, indent=1))
    if args.json:
        json.dump(stats, open(args.json, "w"), indent=1)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
