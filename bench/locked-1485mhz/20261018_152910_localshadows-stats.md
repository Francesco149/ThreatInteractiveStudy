# Benchmark `localshadows` (20261018_152910)

GPU: NVIDIA GeForce RTX 5060. Resolution 1920x1080. Scene: assets\scene\courtyard.gltf. 90 warm-up + 300 measured frames per row, vsync off.

## View: square_overview

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 8.227 | 8.196 | 8.491 | 0.681 | Base pass 2.117, Shadows 1.192, Local lights 1.152, Sun + ambient 0.899, Anti-aliasing 0.757 |
| local shadows=fixed 1024 | 10.137 | 10.108 | 10.445 | 1.079 | Shadows 3.085, Base pass 2.137, Local lights 1.150, Sun + ambient 0.901, Anti-aliasing 0.761 |
| local shadows=fitted, 1080p+8% (TI) | 10.753 | 10.714 | 11.090 | 2.216 | Shadows 3.700, Base pass 2.124, Local lights 1.146, Sun + ambient 0.901, Anti-aliasing 0.766 |
| local shadows=fitted, half density | 9.385 | 9.357 | 9.669 | 2.208 | Shadows 2.343, Base pass 2.121, Local lights 1.145, Sun + ambient 0.901, Anti-aliasing 0.758 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 10.489 | 10.447 | 10.804 | 2.188 | Shadows 3.451, Base pass 2.118, Local lights 1.152, Sun + ambient 0.901, Anti-aliasing 0.762 |

## View: foliage_dense

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 9.388 | 9.354 | 9.740 | 0.486 | Base pass 3.185, Shadows 2.037, Sun + ambient 0.796, Anti-aliasing 0.697, GTAO 0.668 |
| local shadows=fixed 1024 | 10.877 | 10.828 | 11.210 | 0.691 | Shadows 3.533, Base pass 3.186, Sun + ambient 0.793, Anti-aliasing 0.700, GTAO 0.668 |
| local shadows=fitted, 1080p+8% (TI) | 12.165 | 12.102 | 12.424 | 1.293 | Shadows 4.815, Base pass 3.187, Sun + ambient 0.796, Anti-aliasing 0.706, GTAO 0.668 |
| local shadows=fitted, half density | 12.002 | 11.926 | 12.346 | 1.297 | Shadows 4.653, Base pass 3.184, Sun + ambient 0.790, Anti-aliasing 0.704, GTAO 0.680 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 11.501 | 11.428 | 11.896 | 1.306 | Shadows 4.179, Base pass 3.193, Sun + ambient 0.791, Anti-aliasing 0.703, GTAO 0.669 |

## View: busts_closeup

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.563 | 6.538 | 6.815 | 0.664 | Shadows 1.437, Local lights 0.864, Sun + ambient 0.845, GTAO 0.708, Base pass 0.689 |
| local shadows=fixed 1024 | 6.938 | 6.916 | 7.196 | 0.787 | Shadows 1.785, Local lights 0.879, Sun + ambient 0.844, GTAO 0.710, Base pass 0.690 |
| local shadows=fitted, 1080p+8% (TI) | 6.804 | 6.775 | 7.076 | 0.977 | Shadows 1.656, Local lights 0.876, Sun + ambient 0.845, GTAO 0.709, Base pass 0.691 |
| local shadows=fitted, half density | 6.764 | 6.729 | 7.073 | 0.974 | Shadows 1.613, Local lights 0.876, Sun + ambient 0.843, GTAO 0.713, Base pass 0.691 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.894 | 6.874 | 7.192 | 0.966 | Shadows 1.752, Local lights 0.877, Sun + ambient 0.845, GTAO 0.710, Base pass 0.693 |

## View: lamps

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.230 | 5.222 | 5.478 | 0.421 | Base pass 0.930, Sun + ambient 0.706, Anti-aliasing 0.627, GTAO 0.619, Shadows 0.576 |
| local shadows=fixed 1024 | 6.318 | 6.311 | 6.591 | 0.586 | Shadows 1.619, Base pass 0.936, Sun + ambient 0.707, Anti-aliasing 0.639, GTAO 0.620 |
| local shadows=fitted, 1080p+8% (TI) | 6.106 | 6.081 | 6.367 | 1.126 | Shadows 1.426, Base pass 0.933, Sun + ambient 0.708, Anti-aliasing 0.646, GTAO 0.619 |
| local shadows=fitted, half density | 5.914 | 5.874 | 6.230 | 1.130 | Shadows 1.228, Base pass 0.935, Sun + ambient 0.710, Anti-aliasing 0.647, GTAO 0.620 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.292 | 6.264 | 6.569 | 1.131 | Shadows 1.611, Base pass 0.935, Sun + ambient 0.707, Anti-aliasing 0.637, GTAO 0.623 |

## View: arcade

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.425 | 4.400 | 4.673 | 0.283 | Sun + ambient 0.756, GTAO 0.729, Shadows 0.518, Anti-aliasing 0.497, Local lights 0.367 |
| local shadows=fixed 1024 | 4.502 | 4.475 | 4.763 | 0.296 | Sun + ambient 0.757, GTAO 0.729, Shadows 0.592, Anti-aliasing 0.496, Local lights 0.366 |
| local shadows=fitted, 1080p+8% (TI) | 4.523 | 4.500 | 4.691 | 0.345 | Sun + ambient 0.755, GTAO 0.727, Shadows 0.611, Anti-aliasing 0.498, Local lights 0.367 |
| local shadows=fitted, half density | 4.583 | 4.503 | 4.838 | 0.343 | Sun + ambient 0.760, GTAO 0.733, Shadows 0.608, Anti-aliasing 0.555, Local lights 0.365 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.476 | 4.453 | 4.705 | 0.342 | Sun + ambient 0.755, GTAO 0.727, Shadows 0.564, Anti-aliasing 0.499, Local lights 0.367 |

## View: skyline

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.719 | 6.692 | 7.015 | 0.507 | Base pass 1.430, Local lights 1.220, Shadows 1.010, Anti-aliasing 0.613, Sun + ambient 0.586 |
| local shadows=fixed 1024 | 8.353 | 8.332 | 8.608 | 0.840 | Shadows 2.666, Base pass 1.426, Local lights 1.222, Anti-aliasing 0.612, Sun + ambient 0.585 |
| local shadows=fitted, 1080p+8% (TI) | 8.855 | 8.835 | 9.135 | 1.656 | Shadows 3.156, Base pass 1.432, Local lights 1.235, Anti-aliasing 0.615, Sun + ambient 0.587 |
| local shadows=fitted, half density | 7.660 | 7.626 | 7.855 | 1.653 | Shadows 1.943, Base pass 1.431, Local lights 1.234, Anti-aliasing 0.636, Sun + ambient 0.582 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 8.453 | 8.430 | 8.702 | 1.645 | Shadows 2.770, Base pass 1.430, Local lights 1.215, Anti-aliasing 0.609, Sun + ambient 0.584 |

## View: cafe_neon

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.224 | 5.186 | 5.570 | 0.321 | Sun + ambient 0.864, GTAO 0.734, Anti-aliasing 0.631, Local lights 0.610, Shadows 0.571 |
| local shadows=fixed 1024 | 5.547 | 5.520 | 5.781 | 0.446 | Shadows 0.903, Sun + ambient 0.864, GTAO 0.724, Anti-aliasing 0.627, Local lights 0.616 |
| local shadows=fitted, 1080p+8% (TI) | 5.874 | 5.901 | 6.202 | 0.728 | Shadows 1.222, Sun + ambient 0.862, GTAO 0.725, Anti-aliasing 0.630, Local lights 0.615 |
| local shadows=fitted, half density | 5.889 | 5.927 | 6.318 | 0.712 | Shadows 1.225, Sun + ambient 0.868, GTAO 0.727, Anti-aliasing 0.630, Local lights 0.618 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.010 | 6.041 | 6.325 | 0.708 | Shadows 1.074, GTAO 1.010, Sun + ambient 0.862, Anti-aliasing 0.631, Local lights 0.613 |

## View: market

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.919 | 6.883 | 7.243 | 0.569 | Shadows 1.547, Local lights 0.935, Sun + ambient 0.843, Base pass 0.795, Anti-aliasing 0.737 |
| local shadows=fixed 1024 | 7.300 | 7.273 | 7.565 | 0.704 | Shadows 1.927, Local lights 0.938, Sun + ambient 0.842, Base pass 0.799, Anti-aliasing 0.733 |
| local shadows=fitted, 1080p+8% (TI) | 7.447 | 7.434 | 7.738 | 1.145 | Shadows 2.077, Local lights 0.940, Sun + ambient 0.842, Base pass 0.795, Anti-aliasing 0.732 |
| local shadows=fitted, half density | 7.292 | 7.265 | 7.530 | 1.146 | Shadows 1.921, Local lights 0.938, Sun + ambient 0.843, Base pass 0.796, Anti-aliasing 0.732 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 7.313 | 7.292 | 7.562 | 1.140 | Shadows 1.959, Local lights 0.941, Sun + ambient 0.842, Base pass 0.796, Anti-aliasing 0.721 |

## View: camera path

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.916 | 6.279 | 9.935 | 0.555 | Shadows 1.338, Base pass 1.336, Sun + ambient 0.798, Local lights 0.715, GTAO 0.676 |
| local shadows=fixed 1024 | 7.935 | 7.013 | 11.629 | 0.717 | Shadows 2.144, Base pass 1.333, Sun + ambient 0.996, Local lights 0.722, GTAO 0.685 |
| local shadows=fitted, 1080p+8% (TI) | 8.366 | 6.901 | 13.526 | 1.221 | Shadows 2.772, Base pass 1.330, Sun + ambient 0.797, Local lights 0.733, GTAO 0.675 |
| local shadows=fitted, half density | 8.190 | 6.818 | 13.314 | 1.235 | Shadows 2.600, Base pass 1.331, Sun + ambient 0.796, Local lights 0.734, GTAO 0.676 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 8.299 | 7.159 | 12.575 | 1.221 | Shadows 2.675, Base pass 1.333, Sun + ambient 0.845, Local lights 0.721, GTAO 0.677 |

## Passes vs TI budget (local shadows=fitted, 1080p+8% (TI) @ square_overview)

| pass | ms | TI budget | |
|---|---|---|---|
| Sky LUTs | 0.004 | | |
| Shadows | 3.700 | 1.60 | OVER |
| Clears | 0.028 | 0.15 | within |
| Depth pre-pass | 0.091 | 0.30 | within |
| Base pass | 2.124 | 2.80 | within |
| Decals | 0.017 | 0.30 | within |
| MSAA edge stencil | 0.087 | 0.50 | within |
| GTAO | 0.634 | 1.00 | within |
| Sun + ambient | 0.901 | 0.54 | OVER |
| Local lights | 1.146 | 1.50 | within |
| Subsurface scattering | 0.042 | 0.30 | within |
| Compose | 0.200 | 0.12 | OVER |
| Sky | 0.027 | 0.18 | within |
| Transparents + VFX | 0.182 | 1.15 | within |
| Exposure | 0.055 | | |
| Bloom | 0.228 | 0.38 | within |
| Tone LUT bake | 0.005 | | |
| Tone map | 0.144 | 0.25 | within |
| Anti-aliasing | 0.766 | 0.63 | OVER |
| Motion blur | 0.304 | 0.30 | OVER |
| Final output + UI composite | 0.056 | 0.25 | within |

