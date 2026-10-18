"""Contact sheet of textures for quick visual review.

    uv run -q --with numpy --with pillow python -I tools/textures/contact_sheet.py out.png file1.png file2.png ...
Options: --cell N (default 256), --tile 2 (repeat each texture NxN to check seams).
RGBA images are composited over a grey checkerboard.
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--cell", type=int, default=256)
    ap.add_argument("--tile", type=int, default=1)
    ap.add_argument("--cols", type=int, default=4)
    a = ap.parse_args()
    c = a.cell
    rows = (len(a.files) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (a.cols * c, rows * (c + 14)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    yy, xx = np.mgrid[0:c, 0:c]
    checker = np.where(((yy // 16) + (xx // 16)) % 2 == 0, 90, 140).astype(np.float32)[..., None] / 255.0
    for i, f in enumerate(a.files):
        im = Image.open(f)
        im = im.convert("RGBA")
        if a.tile > 1:
            w, h = im.size
            t = Image.new("RGBA", (w * a.tile, h * a.tile))
            for ty in range(a.tile):
                for tx in range(a.tile):
                    t.paste(im, (tx * w, ty * h))
            im = t
        im = np.asarray(im.resize((c, c), Image.LANCZOS)).astype(np.float32) / 255.0
        rgb = im[..., :3] * im[..., 3:] + checker * (1 - im[..., 3:])
        x, y = (i % a.cols) * c, (i // a.cols) * (c + 14)
        sheet.paste(Image.fromarray((rgb * 255).astype(np.uint8)), (x, y + 14))
        d.text((x + 3, y + 1), f.replace("\\", "/").split("/")[-1][:40], fill=(230, 230, 230))
    sheet.save(a.out)


if __name__ == "__main__":
    main()
