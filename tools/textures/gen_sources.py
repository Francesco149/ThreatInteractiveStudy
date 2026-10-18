"""Generate the photographic source albedos for the courtyard scene with Gemini.

Calls the project's image tool (tools/imagegen/gen.py) once per material and writes
the raw results to assets/source_textures/<name>_src.png. Existing files are skipped,
so delete a file (or pass --force NAME) to regenerate it.

    python -I tools/textures/gen_sources.py            # generate everything missing
    python -I tools/textures/gen_sources.py --only brick,roof
    python -I tools/textures/gen_sources.py --force cobble
"""
import argparse
import pathlib
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "imagegen" / "gen.py"
OUT = ROOT / "assets" / "source_textures"

SUFFIX = ("Seamless tileable texture, top-down orthographic view, flat even diffuse lighting, "
          "no shadows, no highlights, no perspective, no vignette, albedo color map only, "
          "photographic, high detail, fills the whole frame edge to edge, no text, no watermark.")

# name -> subject description. Kept here so the README can quote them verbatim.
PROMPTS = {
    "brick": "Old European red clay brick wall in running bond, about 8 bricks across and 22 "
             "courses tall, bricks with natural variation from dark red to orange-brown and a few "
             "darker burnt bricks, recessed light grey lime mortar joints, slightly weathered.",
    "plaster": "Off-white lime plaster stucco wall surface, subtle trowel texture and fine sand "
               "grain, very faint patchy discoloration and light weathering stains, uniform "
               "overall brightness.",
    "stone": "Smooth honey-beige limestone surface (dressed building stone), fine grain with tiny "
             "pores, subtle weathering, no block joints, uniform overall brightness.",
    "cobble": "Old European cobblestone street pavement seen from directly above, square granite "
              "setts in straight rows, each stone about 10 cm, grey with warm brown and bluish "
              "tones, rounded worn tops, dark sandy gaps between stones, about 20 stones across.",
    "paving": "Sidewalk made of large rectangular light grey sandstone paving slabs in straight "
              "courses, about 4 slabs across, thin dark joints, subtle stains and wear.",
    "grass": "Short lawn grass seen from directly above, dense green blades with natural color "
             "variation, some dry yellowish blades, a few clover leaves and tiny bits of brown soil "
             "showing through, irregular and natural, evenly distributed.",
    "wood": "Weathered natural pine wooden planks running horizontally, about 6 planks across the "
            "height, visible grain and knots, thin dark gaps between planks, warm light brown.",
    "paintedwood": "Old wooden boards painted in off-white paint, boards running vertically, about "
                   "6 boards across, slightly worn paint showing a little wood grain, thin gaps "
                   "between boards, uniform brightness.",
    "bark": "Bark of a mature linden tree, vertical furrows and ridges running vertically, "
            "grey-brown with a subtle moss-green tint, uniform brightness.",
    "rust": "Heavily rusted bare iron surface, orange-brown and dark brown rust with pitting and "
            "flaking, uniform distribution.",
    "paint_green": "Painted iron surface with dark bottle-green enamel paint, slightly worn with fine "
                   "scratches and subtle brush marks, no rust, uniform color.",
    "paint_red": "Painted steel surface with oxblood red enamel paint, slightly worn and faded with "
                 "fine scratches, no rust, uniform color.",
    "roof": "Terracotta clay roof tiles (flat beaver-tail tiles) in overlapping rows, orange-red "
            "with color variation and slight dark weathering, a few lichen spots, rows horizontal, "
            "about 8 tiles across.",
    "cloth": "Woven canvas awning fabric with wide vertical stripes alternating deep red and cream, "
             "visible weave texture, slightly faded, 4 stripe pairs across.",
}


def prompt_for(name):
    return f"{PROMPTS[name]} {SUFFIX}"


def run(name, size, force, retries=6):
    out = OUT / f"{name}_src.png"
    if out.exists() and not force:
        return f"skip {name}"
    cmd = [sys.executable, "-I", str(GEN), "gen", prompt_for(name), str(out), "--size", size]
    for attempt in range(retries):
        p = subprocess.run(cmd, capture_output=True, text=True)
        log = f"{p.stdout.strip()[-300:]} {p.stderr.strip()[-300:]}"
        if p.returncode == 0:
            return f"{name}: ok {log}"
        if "429" not in log:            # only rate limits are worth retrying
            break
        time.sleep(20 * (attempt + 1))  # quota resets quickly; back off linearly
    return f"{name}: FAILED rc={p.returncode} {log}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma separated subset")
    ap.add_argument("--force", default="", help="comma separated names to regenerate")
    ap.add_argument("--size", default="2K")
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    names = [n for n in a.only.split(",") if n] or list(PROMPTS)
    force = set(n for n in a.force.split(",") if n)
    with ThreadPoolExecutor(a.jobs) as ex:
        for msg in ex.map(lambda n: run(n, a.size, n in force), names):
            print(msg, flush=True)


if __name__ == "__main__":
    main()
