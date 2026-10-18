"""Shared helpers for the courtyard texture pipeline (numpy + Pillow only).

All images are handled as float32 arrays in [0, 1], shape (H, W) or (H, W, C).
Every filter treats images as periodic (wrap-around), so tileable inputs stay tileable.

Conventions
  * Albedo PNGs are sRGB. Height/roughness/AO/metallic are linear data.
  * Normal maps are tangent space, OpenGL / glTF convention: +X right, +Y up (towards the
    top of the image), +Z out of the surface; encoded as n * 0.5 + 0.5.
  * ORM packing (glTF): R = ambient occlusion, G = roughness, B = metallic.
"""
import pathlib

import numpy as np
from PIL import Image

# --------------------------------------------------------------------------- io


def load(path, size=None):
    im = Image.open(path)
    im = im.convert("RGBA" if im.mode in ("RGBA", "LA", "P") else "RGB")
    if size is not None and im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im).astype(np.float32) / 255.0


def save(path, arr):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    a = np.clip(arr, 0.0, 1.0)
    a = (a * 255.0 + 0.5).astype(np.uint8)
    mode = {2: "L"}.get(a.ndim, {3: "RGB", 4: "RGBA"}.get(a.shape[-1] if a.ndim == 3 else 0))
    if a.ndim == 3 and a.shape[-1] == 1:
        a, mode = a[..., 0], "L"
    Image.fromarray(a, mode).save(path, optimize=True)
    return path


def resize(arr, size):
    """Lanczos resize of a float image to (size, size)."""
    if arr.shape[0] == size and arr.shape[1] == size:
        return arr
    chans = [arr] if arr.ndim == 2 else [arr[..., c] for c in range(arr.shape[-1])]
    out = []
    for ch in chans:
        im = Image.fromarray(ch.astype(np.float32), "F").resize((size, size), Image.LANCZOS)
        out.append(np.asarray(im))
    return out[0] if arr.ndim == 2 else np.stack(out, -1)


# ------------------------------------------------------------------ color space


def srgb_to_linear(c):
    c = np.clip(c, 0.0, None)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, None)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def luminance(rgb):
    lin = srgb_to_linear(rgb[..., :3])
    return lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def saturation(rgb):
    mx = rgb[..., :3].max(-1)
    mn = rgb[..., :3].min(-1)
    return np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)


# ---------------------------------------------------------------- filtering


def _gauss_kernel_fft(shape, sigma):
    h, w = shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    return np.exp(-2.0 * (np.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2))


def blur(img, sigma):
    """Periodic gaussian blur (FFT). sigma in pixels."""
    if sigma <= 0:
        return img
    k = _gauss_kernel_fft(img.shape[:2], sigma)
    if img.ndim == 2:
        return np.real(np.fft.ifft2(np.fft.fft2(img) * k)).astype(np.float32)
    return np.stack([blur(img[..., c], sigma) for c in range(img.shape[-1])], -1)


def normalize(x, lo_pct=0.5, hi_pct=99.5):
    lo, hi = np.percentile(x, [lo_pct, hi_pct])
    return np.clip((x - lo) / max(hi - lo, 1e-6), 0.0, 1.0).astype(np.float32)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# -------------------------------------------------------------------- noise


def fbm(size, rng, beta=2.0, lo_freq=2.0, hi_freq=None, aniso=(1.0, 1.0)):
    """Periodic fractal noise via 1/f^beta spectral synthesis, normalised to [0,1].

    lo_freq: lowest frequency kept (cycles per image); hi_freq: optional cutoff.
    aniso: (sx, sy) frequency stretch, e.g. (1, 6) gives streaks along x... (vertical grain)
    """
    h = w = size
    fy = np.fft.fftfreq(h)[:, None] * h * aniso[1]
    fx = np.fft.fftfreq(w)[None, :] * w * aniso[0]
    f = np.sqrt(fx ** 2 + fy ** 2)
    amp = np.where(f >= lo_freq, 1.0 / np.maximum(f, 1e-6) ** (beta / 2.0), 0.0)
    if hi_freq:
        amp *= np.exp(-(f / hi_freq) ** 2)
    phase = rng.uniform(0, 2 * np.pi, (h, w))
    spec = amp * np.exp(1j * phase)
    n = np.real(np.fft.ifft2(spec))
    return normalize(n, 0.1, 99.9)


def warp(img, dx, dy):
    """Periodic domain warp by per-pixel offsets (in pixels), bilinear."""
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    x = (xx + dx) % w
    y = (yy + dy) % h
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    fx = x - x0; fy = y - y0
    x1 = (x0 + 1) % w; y1 = (y0 + 1) % h
    if img.ndim == 3:
        fx = fx[..., None]; fy = fy[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    b = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return (a * (1 - fy) + b * fy).astype(np.float32)


# ------------------------------------------------------------------ tiling


def flatten_lighting(rgb, sigma_frac=0.12, strength=0.85):
    """Remove low-frequency brightness gradients (uneven lighting) while keeping color."""
    lum = luminance(rgb) + 1e-3
    low = blur(lum, rgb.shape[0] * sigma_frac)
    gain = (lum.mean() / low) ** strength
    lin = srgb_to_linear(rgb[..., :3]) * gain[..., None]
    return linear_to_srgb(lin).astype(np.float32)


def _min_cut(err):
    """Minimum-error vertical path through err (H x Wb). Returns column index per row."""
    h, wb = err.shape
    cost = err.copy()
    back = np.zeros((h, wb), np.int8)
    for i in range(1, h):
        prev = cost[i - 1]
        left = np.concatenate([[np.inf], prev[:-1]])
        right = np.concatenate([prev[1:], [np.inf]])
        stack = np.stack([left, prev, right])
        idx = np.argmin(stack, 0)
        cost[i] += stack[idx, np.arange(wb)]
        back[i] = idx - 1
    path = np.zeros(h, int)
    path[-1] = int(np.argmin(cost[-1]))
    for i in range(h - 1, 0, -1):
        path[i - 1] = np.clip(path[i] + back[i, path[i]], 0, wb - 1)
    return path


def _tile_axis_x(img, band, feather):
    h, w = img.shape[:2]
    a = img
    b = np.roll(img, w // 2, axis=1)            # wraps in x, seam at w/2
    bw = max(8, int(w * band))
    c = w // 2
    gap = max(2, bw // 6)
    err = ((a - b) ** 2).reshape(h, w, -1).sum(-1)
    err = blur(err, 1.5)
    left = _min_cut(err[:, c - bw:c - gap]) + (c - bw)
    right = _min_cut(err[:, c + gap:c + bw]) + (c + gap)
    xx = np.arange(w)[None, :]
    m = ((xx >= left[:, None]) & (xx < right[:, None])).astype(np.float32)
    m = blur(m, feather)
    if img.ndim == 3:
        m = m[..., None]
    return b * (1 - m) + a * m


def make_tileable(img, band=0.2, feather=2.5):
    """Tileable version of img: half-offset + two minimum-error boundary cuts per axis
    (image-quilting style), so structured textures (bricks, tiles) don't ghost."""
    out = _tile_axis_x(img, band, feather)
    out = np.swapaxes(_tile_axis_x(np.swapaxes(out, 0, 1), band, feather), 0, 1)
    return out.astype(np.float32)


# ---------------------------------------------------------- derived maps


def normal_from_height(height, strength):
    """Tangent-space normal map (OpenGL/glTF +Y up) from a periodic height field.

    height in [0,1]; strength scales slopes (height units per pixel)."""
    h = height.astype(np.float32)
    # Sobel with wrap-around
    def sh(dy, dx):
        return np.roll(np.roll(h, dy, 0), dx, 1)
    gx = (sh(-1, -1) + 2 * sh(0, -1) + sh(1, -1)) - (sh(-1, 1) + 2 * sh(0, 1) + sh(1, 1))
    gy = (sh(-1, -1) + 2 * sh(-1, 0) + sh(-1, 1)) - (sh(1, -1) + 2 * sh(1, 0) + sh(1, 1))
    # sh(0,-1)[x] = h[x+1], sh(0,1)[x] = h[x-1]  ->  gx = +8 dh/dx
    # sh(-1,0)[r] = h[r+1], sh(1,0)[r] = h[r-1]  ->  gy = +8 dh/drow (rows grow downwards)
    nx = -gx / 8.0 * strength       # n_x = -dh/dx
    ny = gy / 8.0 * strength        # n_y = -dh/dup = +dh/drow
    nz = np.ones_like(h)
    n = np.stack([nx, ny, nz], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return (n * 0.5 + 0.5).astype(np.float32)


def ao_from_height(height, radius, strength):
    """Cheap cavity AO: how far each texel sits below its blurred neighbourhood."""
    acc = np.zeros_like(height)
    for r, wgt in ((radius * 0.35, 0.5), (radius, 0.35), (radius * 2.5, 0.15)):
        acc += wgt * np.clip(blur(height, r) - height, 0, None)
    return np.clip(1.0 - strength * acc / max(acc.max(), 1e-6), 0.0, 1.0).astype(np.float32)


def multiscale_height(base, scales):
    """Sum of band-passed copies of base: scales = [(sigma_detail, weight), ...]."""
    h = np.zeros_like(base)
    for sigma, wgt in scales:
        h += wgt * blur(base, sigma)
    return normalize(h)


def pack_orm(ao, rough, metal):
    return np.stack([ao, rough, metal], -1).astype(np.float32)


# ----------------------------------------------------- alpha / atlases


def dilate_color(rgb, alpha, thresh=0.02):
    """Push-pull fill: replace color where alpha < thresh with an average of nearby opaque
    color so mip-mapped alpha-tested textures don't get dark fringes."""
    h, w = alpha.shape
    weight = (alpha >= thresh).astype(np.float32)
    levels = [(rgb * weight[..., None], weight)]
    c, wgt = levels[0]
    while c.shape[0] > 1 and c.shape[1] > 1:
        c = c.reshape(c.shape[0] // 2, 2, c.shape[1] // 2, 2, 3).sum((1, 3))
        wgt = wgt.reshape(wgt.shape[0] // 2, 2, wgt.shape[1] // 2, 2).sum((1, 3))
        levels.append((c, wgt))
    # pull: coarse to fine
    c, wgt = levels[-1]
    filled = c / np.maximum(wgt[..., None], 1e-6)
    for c, wgt in reversed(levels[:-1]):
        up = np.repeat(np.repeat(filled, 2, 0), 2, 1)
        own = c / np.maximum(wgt[..., None], 1e-6)
        filled = np.where(wgt[..., None] > 0, own, up)
    out = np.where(weight[..., None] > 0, rgb, filled)
    return out.astype(np.float32)


def supersample_down(arr, factor):
    """Box downsample by an integer factor (used for antialiased procedural drawing)."""
    h, w = arr.shape[:2]
    if arr.ndim == 2:
        return arr.reshape(h // factor, factor, w // factor, factor).mean((1, 3))
    return arr.reshape(h // factor, factor, w // factor, factor, arr.shape[-1]).mean((1, 3))
