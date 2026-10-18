"""Turn the Gemini source albedos into tileable scene textures.

For every material in MATS:
  assets/source_textures/<src>_src.png
    -> resize, flatten uneven lighting, make tileable (half offset + min-error cuts)
    -> textures/<name>_albedo.png  (sRGB)
    -> textures/<name>_normal.png  (tangent space, OpenGL/glTF +Y up)
    -> textures/<name>_orm.png     (R=AO, G=roughness, B=metallic)  [only if orm=True]
Height (for normal/AO) and roughness are derived from the albedo with per-material recipes.

The painted-iron material is special: two paint albedos and one rust albedo are composited
with a procedural blend mask that is also written to the METALLIC (B) channel of the ORM
texture (0 = paint, 1 = bare/rusted iron), as required by the engine's "paint_iron" profile.

    uv run -q --with numpy --with pillow python -I tools/textures/process_photo.py [names...]
"""
import pathlib
import sys
import zlib

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import texlib as T  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "assets" / "source_textures"
DST = ROOT / "assets" / "scene" / "textures"


def h_lum(rgb):
    return T.normalize(T.luminance(rgb))


def h_sat(rgb):
    return T.normalize(T.saturation(rgb))


# Per-material recipe. height(rgb) -> [0,1]; rough(h, rgb, rng) -> [0,1]
MATS = {
    "brick": dict(
        size=1024, flatten=0.8, orm=True, nstr=5.0, ao=(5, 0.75),
        # bricks are saturated, mortar is grey -> saturation separates them well
        height=lambda rgb: np.clip(0.75 * T.smoothstep(0.18, 0.45, T.blur(h_sat(rgb), 1.2))
                                   + 0.25 * h_lum(rgb), 0, 1),
        rough=lambda h, rgb, rng: np.clip(0.93 - 0.12 * h + 0.04 * (T.fbm(h.shape[0], rng, lo_freq=4) - 0.5), 0, 1),
    ),
    "plaster": dict(
        size=1024, flatten=0.9, orm=False, nstr=2.0,
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(0.6, 0.6), (3, 0.4)]),
    ),
    "stone": dict(
        size=1024, flatten=0.9, orm=True, nstr=2.5, ao=(4, 0.35),
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(0.6, 0.5), (2.5, 0.5)]),
        rough=lambda h, rgb, rng: np.clip(0.72 - 0.1 * (h - 0.5) + 0.08 * (T.fbm(h.shape[0], rng, lo_freq=3) - 0.5), 0, 1),
    ),
    "cobble": dict(
        size=2048, flatten=0.8, orm=True, nstr=10.0, ao=(9, 0.85),
        # dark gaps between setts; dome the stones by blurring a soft stone mask
        height=lambda rgb: np.clip(0.75 * T.blur(T.smoothstep(0.12, 0.42, T.blur(h_lum(rgb), 2.0)), 5.0)
                                   + 0.25 * h_lum(rgb), 0, 1),
        rough=lambda h, rgb, rng: np.clip(0.97 - 0.42 * T.smoothstep(0.35, 0.85, h), 0, 1),
    ),
    "paving": dict(
        size=1024, flatten=0.85, orm=False, nstr=3.0,
        height=lambda rgb: np.clip(0.6 * T.smoothstep(0.1, 0.35, T.blur(h_lum(rgb), 1.0)) + 0.4 * h_lum(rgb), 0, 1),
    ),
    "grass": dict(
        size=1024, flatten=0.85, orm=False, nstr=3.0,
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(0.7, 0.6), (3, 0.4)]),
    ),
    "wood": dict(
        size=1024, flatten=0.85, orm=True, nstr=3.5, ao=(4, 0.6),
        height=lambda rgb: np.clip(0.55 * T.smoothstep(0.08, 0.3, T.blur(h_lum(rgb), 1.0)) + 0.45 * h_lum(rgb), 0, 1),
        rough=lambda h, rgb, rng: np.clip(0.82 - 0.12 * h + 0.06 * (T.fbm(h.shape[0], rng, lo_freq=3, aniso=(1, 6)) - 0.5), 0, 1),
    ),
    "paintedwood": dict(
        size=1024, flatten=0.85, orm=False, nstr=3.0,
        height=lambda rgb: np.clip(0.55 * T.smoothstep(0.1, 0.35, T.blur(h_lum(rgb), 1.0)) + 0.45 * h_lum(rgb), 0, 1),
    ),
    "bark": dict(
        size=1024, flatten=0.8, orm=False, nstr=7.0,
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(1.0, 0.4), (4, 0.6)]),
    ),
    "roof": dict(
        size=1024, flatten=0.8, orm=True, nstr=6.0, ao=(6, 0.8),
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(0.8, 0.5), (3, 0.5)]),
        rough=lambda h, rgb, rng: np.clip(0.8 - 0.1 * h + 0.06 * (T.fbm(h.shape[0], rng, lo_freq=3) - 0.5), 0, 1),
    ),
    "cloth": dict(
        size=1024, flatten=0.9, orm=False, nstr=1.5,
        height=lambda rgb: T.multiscale_height(h_lum(rgb), [(0.5, 0.8), (2, 0.2)]),
    ),
}


def prepare(src_name, size, flatten):
    rgb = T.load(SRC / f"{src_name}_src.png")[..., :3]
    rgb = T.resize(rgb, size)
    if flatten:
        rgb = T.flatten_lighting(rgb, strength=flatten)
    return np.clip(T.make_tileable(rgb), 0, 1)


def process(name, cfg):
    rng = np.random.default_rng(zlib.crc32(name.encode()))
    rgb = prepare(name, cfg["size"], cfg["flatten"])
    T.save(DST / f"{name}_albedo.png", rgb)
    h = cfg["height"](rgb)
    T.save(DST / f"{name}_normal.png", T.normal_from_height(h, cfg["nstr"]))
    if cfg.get("orm"):
        r, s = cfg["ao"]
        ao = T.ao_from_height(h, r, s)
        rough = cfg["rough"](h, rgb, rng)
        T.save(DST / f"{name}_orm.png", T.pack_orm(ao, rough, np.zeros_like(h)))
    print("ok", name, flush=True)


def scratches(size, rng, count):
    """Thin random scratch lines (periodic), drawn supersampled."""
    from PIL import Image, ImageDraw
    ss = 2
    im = Image.new("L", (size * ss, size * ss), 0)
    d = ImageDraw.Draw(im)
    for _ in range(count):
        x, y = rng.uniform(0, size * ss, 2)
        ang = rng.normal(0.3, 0.6)
        ln = rng.uniform(0.02, 0.12) * size * ss
        pts = []
        for t in np.linspace(0, 1, 8):
            pts.append((x + np.cos(ang + 0.3 * t) * ln * t, y + np.sin(ang + 0.3 * t) * ln * t))
        fill, width = int(rng.uniform(120, 255)), max(1, int(rng.uniform(1, 3)))
        S = size * ss
        for ox in (-S, 0, S):           # draw wrapped copies so the result tiles
            for oy in (-S, 0, S):
                d.line([(px + ox, py + oy) for px, py in pts], fill=fill, width=width)
    a = np.asarray(im).astype(np.float32) / 255.0
    return T.supersample_down(a, ss)


def paint_iron(size=1024):
    """Painted iron with rust patches. Writes albedo for each paint color + shared normal/ORM."""
    rng = np.random.default_rng(1234)
    rust = prepare("rust", size, 0.8)
    paints = {c: prepare(f"paint_{c}", size, 0.9) for c in ("green", "red")}

    # --- blend mask: warped fbm blotches + pitting speckles + scratches
    n = T.fbm(size, rng, beta=2.4, lo_freq=3)
    wx = (T.fbm(size, rng, beta=2.0, lo_freq=4) - 0.5) * 60
    wy = (T.fbm(size, rng, beta=2.0, lo_freq=4) - 0.5) * 60
    n = T.warp(n, wx, wy)
    detail = T.fbm(size, rng, beta=1.6, lo_freq=16)
    blot = T.smoothstep(0.60, 0.68, 0.8 * n + 0.2 * detail)
    pits = T.smoothstep(0.82, 0.9, T.fbm(size, rng, beta=1.2, lo_freq=40)) * T.smoothstep(0.45, 0.6, n)
    scr = scratches(size, rng, 140) * T.smoothstep(0.3, 0.55, n)
    mask = np.clip(np.maximum(np.maximum(blot, pits), scr * 0.9), 0, 1)
    mask = T.blur(mask, 0.6)

    # rust stains bleed a little into the surrounding paint
    stain = np.clip(T.blur(mask, 10) * 1.3, 0, 1)[..., None] * 0.35
    rust_lin = T.srgb_to_linear(rust)
    for c, paint in paints.items():
        p_lin = T.srgb_to_linear(paint)
        p_lin = p_lin * (1 - stain) + p_lin * rust_lin * 2.2 * stain   # brownish discoloration
        out = p_lin * (1 - mask[..., None]) + rust_lin * mask[..., None]
        T.save(DST / f"paint_iron_{c}_albedo.png", T.linear_to_srgb(out))

    # --- height: paint film sits above the corroded iron, rust has its own pitted relief
    rust_h = T.multiscale_height(h_lum(rust), [(0.8, 0.5), (3, 0.5)])
    paint_h = T.multiscale_height(h_lum(paints["green"]), [(0.6, 1.0)])
    h = (1 - mask) * (0.65 + 0.05 * paint_h) + mask * (0.45 * rust_h)
    h = np.clip(h, 0, 1)
    T.save(DST / "paint_iron_normal.png", T.normal_from_height(h, 4.0))
    ao = T.ao_from_height(h, 4, 0.6)
    rough = 0.38 + 0.06 * (T.fbm(size, rng, lo_freq=4) - 0.5)
    rough = rough * (1 - mask) + (0.82 + 0.1 * (rust_h - 0.5)) * mask
    T.save(DST / "paint_iron_orm.png", T.pack_orm(ao, np.clip(rough, 0, 1), mask))
    T.save(SRC / "paint_iron_mask_preview.png", mask)
    print("ok paint_iron", flush=True)


def main():
    want = sys.argv[1:] or list(MATS) + ["paint_iron"]
    for name in want:
        if name == "paint_iron":
            paint_iron()
        else:
            if not (SRC / f"{name}_src.png").exists():
                print("missing source for", name)
                continue
            process(name, MATS[name])


if __name__ == "__main__":
    main()
