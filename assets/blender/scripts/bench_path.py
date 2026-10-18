"""Benchmark camera fly-through + static viewpoints -> assets/scene/bench_path.json.

Keys are authored in Blender coordinates (X east, Y north, Z up) and written in glTF
coordinates (+Y up): gltf = (X, Z, -Y). Plain Python; inside Blender it can also create
preview cameras for the viewpoints (make_cameras()).

    python -I assets/blender/scripts/bench_path.py
"""
import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scene", "bench_path.json")

DURATION = 40.0

# (t, pos, target, note) in Blender coordinates
KEYS = [
    (0.0, (-15.0, -15.5, 12.0), (4.0, 4.0, 1.0), "overview of the square from the SW corner"),
    (4.0, (-9.0, -13.0, 6.0), (4.0, 2.0, 1.0), "descending"),
    (7.0, (-12.5, -3.5, 1.8), (-8.0, -8.0, 0.8), "round flower bed (dense alpha cards)"),
    (10.0, (-14.8, 3.4, 2.0), (-12.5, 9.0, 1.6), "stepping into the park"),
    (13.0, (-14.6, 8.0, 2.0), (-10.0, 11.0, 2.0), "walking through grass under the trees"),
    (16.0, (-11.0, 10.4, 2.0), (-6.0, 12.0, 2.2), "dense foliage, windmill"),
    (19.0, (-6.6, 12.2, 2.0), (0.0, 6.0, 1.8), "leaving the park"),
    (22.0, (4.0, 4.0, 1.7), (12.9, -1.6, 1.6), "walking towards the busts"),
    (25.0, (11.5, -2.3, 1.62), (12.9, -1.6, 1.55), "close-up bust A (skin SSS)"),
    (27.5, (11.4, -5.0, 1.62), (12.9, -4.5, 1.45), "pan"),
    (29.5, (11.3, -7.0, 1.62), (12.9, -7.4, 1.57), "close-up bust B"),
    (30.5, (11.0, -7.0, 1.7), (14.0, -7.5, 1.6), "start of the fast turn"),
    (30.9, (11.0, -7.0, 1.7), (11.0, -4.0, 1.7), "fast 180 degree turn (motion blur)"),
    (31.3, (11.0, -7.0, 1.7), (8.0, -7.5, 1.7), "turn finished, facing the low sun"),
    (33.5, (6.0, -12.0, 1.7), (-9.0, -18.4, 3.5), "street lamps along the south sidewalk"),
    (36.5, (-1.0, -11.0, 1.8), (-3.0, -13.6, 1.0), "the moving handcart"),
    (40.0, (3.6, -16.2, 1.7), (6.5, -22.8, 2.6), "into the arcade"),
]

VIEWPOINTS = [
    ("square_overview", (-15.0, -15.5, 12.0), (4.0, 4.0, 1.0)),
    ("foliage_dense", (-13.5, 4.2, 1.15), (-11.0, 11.0, 2.8)),
    ("busts_closeup", (11.95, -2.05, 1.6), (12.9, -1.6, 1.55)),
    ("lamps", (4.5, -12.0, 1.7), (-12.0, -18.4, 3.0)),
    ("arcade", (3.6, -16.2, 1.7), (6.5, -22.8, 2.6)),
    ("skyline", (10.0, -12.0, 1.6), (-6.0, 36.0, 18.0)),
    ("cafe_neon", (14.0, -10.5, 1.7), (17.0, -20.0, 2.8)),
    ("market", (6.0, 9.0, 1.7), (12.0, 7.5, 1.5)),
]


def to_gltf(p):
    x, y, z = p
    return [round(x, 4), round(z, 4), round(-y, 4)]


def write(path=OUT):
    data = {
        "duration": DURATION,
        "coordinate_system": "glTF (+Y up, meters); same frame as courtyard.gltf",
        "keys": [{"t": t, "pos": to_gltf(p), "target": to_gltf(q), "note": n} for t, p, q, n in KEYS],
        "viewpoints": [{"name": n, "pos": to_gltf(p), "target": to_gltf(q)} for n, p, q in VIEWPOINTS],
    }
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
    return os.path.abspath(path)


def make_cameras(lens=24.0):
    """Inside Blender: one preview camera per viewpoint (collection Preview_Cameras)."""
    import preview
    return {n: preview.camera("View_" + n, p, q, lens=lens) for n, p, q in VIEWPOINTS}


if __name__ == "__main__":
    print(write())
