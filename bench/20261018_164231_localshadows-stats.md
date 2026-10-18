# Benchmark `localshadows` (20261018_164231)

GPU: NVIDIA GeForce RTX 5060. Resolution 1920x1080. Scene: assets\scene\courtyard.gltf. 90 warm-up + 300 measured frames per row, vsync off.

## View: square_overview

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.926 | 4.885 | 5.130 | 0.724 | Base pass 1.247, Shadows 0.660, Local lights 0.635, Sun + ambient 0.581, Anti-aliasing 0.449 |
| local shadows=fixed 1024 | 6.057 | 6.042 | 6.192 | 1.112 | Shadows 1.713, Base pass 1.299, Local lights 0.641, Sun + ambient 0.581, Anti-aliasing 0.460 |
| local shadows=fitted, 1080p+8% (TI) | 6.610 | 6.591 | 6.768 | 2.213 | Shadows 2.149, Base pass 1.337, Local lights 0.652, Sun + ambient 0.604, Anti-aliasing 0.473 |
| local shadows=fitted, half density | 5.840 | 5.844 | 5.966 | 2.215 | Shadows 1.357, Base pass 1.344, Local lights 0.650, Sun + ambient 0.612, Anti-aliasing 0.474 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.160 | 6.158 | 6.331 | 2.436 | Shadows 1.866, Base pass 1.274, Local lights 0.642, Sun + ambient 0.570, Anti-aliasing 0.456 |

## View: foliage_dense

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.650 | 5.620 | 5.825 | 0.510 | Base pass 1.898, Shadows 1.136, Sun + ambient 0.509, GTAO 0.410, Anti-aliasing 0.407 |
| local shadows=fixed 1024 | 6.607 | 6.596 | 6.789 | 0.696 | Shadows 1.982, Base pass 1.956, Sun + ambient 0.515, GTAO 0.421, Anti-aliasing 0.421 |
| local shadows=fitted, 1080p+8% (TI) | 7.578 | 7.595 | 7.801 | 1.316 | Shadows 2.818, Base pass 2.017, Sun + ambient 0.537, GTAO 0.441, Anti-aliasing 0.434 |
| local shadows=fitted, half density | 7.528 | 7.504 | 7.862 | 1.313 | Shadows 2.736, Base pass 2.030, Sun + ambient 0.546, GTAO 0.444, Anti-aliasing 0.436 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.759 | 6.783 | 6.893 | 1.323 | Shadows 2.260, Base pass 1.902, Sun + ambient 0.499, Anti-aliasing 0.417, GTAO 0.414 |

## View: busts_closeup

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.932 | 3.912 | 4.106 | 0.670 | Shadows 0.798, Sun + ambient 0.525, Local lights 0.489, GTAO 0.434, Base pass 0.412 |
| local shadows=fixed 1024 | 4.219 | 4.210 | 4.356 | 0.796 | Shadows 1.005, Sun + ambient 0.546, Local lights 0.506, GTAO 0.452, Base pass 0.418 |
| local shadows=fitted, 1080p+8% (TI) | 4.250 | 4.228 | 4.365 | 0.975 | Shadows 0.938, Sun + ambient 0.554, Local lights 0.511, GTAO 0.455, Base pass 0.420 |
| local shadows=fitted, half density | 4.189 | 4.187 | 4.282 | 0.973 | Shadows 0.921, Sun + ambient 0.565, Local lights 0.517, GTAO 0.461, Base pass 0.421 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.147 | 4.134 | 4.309 | 0.987 | Shadows 0.950, Sun + ambient 0.545, Local lights 0.503, GTAO 0.447, Base pass 0.417 |

## View: lamps

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.158 | 3.145 | 3.288 | 0.418 | Base pass 0.561, Sun + ambient 0.440, GTAO 0.374, Anti-aliasing 0.356, Shadows 0.316 |
| local shadows=fixed 1024 | 3.818 | 3.809 | 3.924 | 0.758 | Shadows 0.898, Base pass 0.572, Sun + ambient 0.457, GTAO 0.387, Anti-aliasing 0.369 |
| local shadows=fitted, 1080p+8% (TI) | 3.779 | 3.767 | 3.865 | 1.153 | Shadows 0.798, Base pass 0.578, Sun + ambient 0.470, GTAO 0.398, Anti-aliasing 0.379 |
| local shadows=fitted, half density | 3.684 | 3.694 | 3.759 | 1.146 | Shadows 0.688, Base pass 0.579, Sun + ambient 0.475, GTAO 0.401, Anti-aliasing 0.383 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.786 | 3.768 | 3.872 | 1.150 | Shadows 0.877, Base pass 0.569, Sun + ambient 0.452, GTAO 0.384, Anti-aliasing 0.365 |

## View: arcade

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 2.707 | 2.689 | 2.837 | 0.337 | Sun + ambient 0.466, GTAO 0.449, Shadows 0.290, Anti-aliasing 0.287, Local lights 0.226 |
| local shadows=fixed 1024 | 2.802 | 2.792 | 2.923 | 0.345 | Sun + ambient 0.483, GTAO 0.464, Shadows 0.330, Anti-aliasing 0.293, Local lights 0.234 |
| local shadows=fitted, 1080p+8% (TI) | 2.847 | 2.835 | 3.028 | 0.368 | Sun + ambient 0.493, GTAO 0.468, Shadows 0.342, Anti-aliasing 0.297, Local lights 0.238 |
| local shadows=fitted, half density | 2.867 | 2.864 | 2.982 | 0.381 | Sun + ambient 0.499, GTAO 0.472, Shadows 0.341, Anti-aliasing 0.299, Local lights 0.241 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 2.859 | 2.865 | 2.937 | 0.346 | Sun + ambient 0.496, GTAO 0.471, Shadows 0.312, Anti-aliasing 0.298, Local lights 0.239 |

## View: skyline

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.886 | 3.873 | 3.936 | 0.501 | Base pass 0.837, Local lights 0.662, Shadows 0.552, Anti-aliasing 0.354, Sun + ambient 0.351 |
| local shadows=fixed 1024 | 4.937 | 4.937 | 5.078 | 0.857 | Shadows 1.495, Base pass 0.875, Local lights 0.674, Sun + ambient 0.374, Anti-aliasing 0.365 |
| local shadows=fitted, 1080p+8% (TI) | 5.341 | 5.356 | 5.471 | 1.674 | Shadows 1.843, Base pass 0.894, Local lights 0.690, Sun + ambient 0.382, Anti-aliasing 0.370 |
| local shadows=fitted, half density | 4.610 | 4.589 | 4.748 | 1.673 | Shadows 1.120, Base pass 0.888, Local lights 0.689, Sun + ambient 0.380, Anti-aliasing 0.369 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.854 | 4.844 | 4.972 | 1.685 | Shadows 1.498, Base pass 0.845, Local lights 0.664, Sun + ambient 0.354, Anti-aliasing 0.353 |

## View: cafe_neon

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.167 | 3.150 | 3.310 | 0.361 | Sun + ambient 0.535, GTAO 0.449, Anti-aliasing 0.361, Local lights 0.353, Shadows 0.317 |
| local shadows=fixed 1024 | 3.425 | 3.417 | 3.553 | 0.466 | Sun + ambient 0.556, Shadows 0.505, GTAO 0.459, Local lights 0.373, Anti-aliasing 0.370 |
| local shadows=fitted, 1080p+8% (TI) | 3.678 | 3.719 | 3.847 | 0.723 | Shadows 0.708, Sun + ambient 0.571, GTAO 0.472, Local lights 0.379, Anti-aliasing 0.378 |
| local shadows=fitted, half density | 3.702 | 3.746 | 3.858 | 0.718 | Shadows 0.708, Sun + ambient 0.575, GTAO 0.474, Local lights 0.385, Anti-aliasing 0.380 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.495 | 3.491 | 3.642 | 0.714 | Shadows 0.585, Sun + ambient 0.552, GTAO 0.457, Anti-aliasing 0.372, Local lights 0.369 |

## View: market

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.191 | 4.176 | 4.337 | 0.559 | Shadows 0.862, Sun + ambient 0.529, Local lights 0.526, Base pass 0.476, Anti-aliasing 0.442 |
| local shadows=fixed 1024 | 4.495 | 4.480 | 4.628 | 0.741 | Shadows 1.102, Sun + ambient 0.555, Local lights 0.546, Base pass 0.486, Anti-aliasing 0.453 |
| local shadows=fitted, 1080p+8% (TI) | 4.622 | 4.634 | 4.719 | 1.155 | Shadows 1.204, Sun + ambient 0.562, Local lights 0.551, Base pass 0.489, Anti-aliasing 0.455 |
| local shadows=fitted, half density | 4.566 | 4.551 | 4.681 | 1.151 | Shadows 1.120, Sun + ambient 0.569, Local lights 0.555, Base pass 0.493, Anti-aliasing 0.460 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.394 | 4.381 | 4.489 | 1.155 | Shadows 1.064, Sun + ambient 0.542, Local lights 0.537, Base pass 0.481, Anti-aliasing 0.438 |

## View: camera path

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.197 | 3.806 | 5.957 | 0.548 | Base pass 0.807, Shadows 0.752, Sun + ambient 0.505, Local lights 0.420, GTAO 0.418 |
| local shadows=fixed 1024 | 4.750 | 4.142 | 7.044 | 0.734 | Shadows 1.218, Base pass 0.830, Sun + ambient 0.525, GTAO 0.433, Local lights 0.429 |
| local shadows=fitted, 1080p+8% (TI) | 5.177 | 4.296 | 8.565 | 1.352 | Shadows 1.605, Base pass 0.837, Sun + ambient 0.535, GTAO 0.437, Local lights 0.436 |
| local shadows=fitted, half density | 5.118 | 4.273 | 8.286 | 1.244 | Shadows 1.516, Base pass 0.843, Sun + ambient 0.541, GTAO 0.446, Local lights 0.438 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.880 | 4.274 | 7.331 | 1.245 | Shadows 1.450, Base pass 0.799, Sun + ambient 0.502, GTAO 0.419, Local lights 0.418 |

## Passes vs TI budget (local shadows=fitted, 1080p+8% (TI) @ square_overview)

| pass | ms | TI budget | |
|---|---|---|---|
| Sky LUTs | 0.003 | | |
| Shadows | 2.149 | 1.60 | OVER |
| Clears | 0.018 | 0.15 | within |
| Depth pre-pass | 0.054 | 0.30 | within |
| Base pass | 1.337 | 2.80 | within |
| Decals | 0.015 | 0.30 | within |
| MSAA edge stencil | 0.061 | 0.50 | within |
| GTAO | 0.402 | 1.00 | within |
| Sun + ambient | 0.604 | 0.54 | OVER |
| Local lights | 0.652 | 1.50 | within |
| Subsurface scattering | 0.026 | 0.30 | within |
| Compose | 0.143 | 0.12 | OVER |
| Sky | 0.019 | 0.18 | within |
| Transparents + VFX | 0.116 | 1.15 | within |
| Exposure | 0.035 | | |
| Bloom | 0.134 | 0.38 | within |
| Tone LUT bake | 0.003 | | |
| Tone map | 0.110 | 0.25 | within |
| Anti-aliasing | 0.473 | 0.63 | within |
| Motion blur | 0.217 | 0.30 | within |
| Final output + UI composite | 0.034 | 0.25 | within |

