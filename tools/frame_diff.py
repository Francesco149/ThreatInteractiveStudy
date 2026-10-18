"""Temporal stability test: mean absolute difference between two consecutive frames of a static
scene (opposite camera-jitter phases). With a perfectly stable resolve the difference is zero;
texture crawl and edge flicker show up as non-zero differences.

    uv run --with pillow --with numpy python -I tools/frame_diff.py a.png b.png [diff.png]

Prints the mean absolute difference (8-bit units) over the whole image and the fraction of pixels
that changed by more than 2/255; optionally writes an amplified difference image.
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.float32)
    b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(np.float32)
    d = np.abs(a - b).max(axis=2)
    print(f"mean |diff| = {d.mean():.3f} (8-bit units), pixels changed > 2: {(d > 2).mean() * 100:.2f}%")
    if len(sys.argv) > 3:
        Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).save(sys.argv[3])


if __name__ == "__main__":
    main()
