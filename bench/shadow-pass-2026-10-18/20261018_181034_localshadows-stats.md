# Benchmark `localshadows` (20261018_181034)

GPU: NVIDIA GeForce RTX 5060. Resolution 1920x1080. Scene: .claude\worktrees\jolly-robinson-27ec02\assets\scene\courtyard.gltf. 90 warm-up + 300 measured frames per row, vsync off.

## View: square_overview

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 8.370 | 8.395 | 8.803 | 0.666 | Base pass 2.130, Shadows 1.197, Local lights 1.158, Sun + ambient 0.892, Anti-aliasing 0.872 |
| local shadows=fixed 1024 | 9.499 | 9.604 | 9.885 | 1.057 | Shadows 2.438, Base pass 2.140, Local lights 1.142, Sun + ambient 0.899, Anti-aliasing 0.767 |
| local shadows=fitted, 1080p+8% (TI) | 9.955 | 10.048 | 10.315 | 2.254 | Shadows 2.898, Base pass 2.123, Local lights 1.141, Sun + ambient 0.899, Anti-aliasing 0.761 |
| local shadows=fitted, half density | 9.143 | 9.247 | 9.524 | 2.242 | Base pass 2.123, Shadows 2.097, Local lights 1.140, Sun + ambient 0.902, Anti-aliasing 0.765 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 9.272 | 9.338 | 9.659 | 2.250 | Shadows 2.188, Base pass 2.117, Local lights 1.147, Sun + ambient 0.941, Anti-aliasing 0.765 |
| local shadows=fitted, no far clip | 10.754 | 10.815 | 11.140 | 2.250 | Shadows 3.706, Base pass 2.113, Local lights 1.146, Sun + ambient 0.901, Anti-aliasing 0.763 |
| local shadows=fitted, cache | 9.954 | 10.051 | 10.312 | 2.250 | Shadows 2.904, Base pass 2.126, Local lights 1.154, Sun + ambient 0.893, Anti-aliasing 0.758 |
| local shadows=fitted, cache, moving casters every 2nd frame | 9.731 | 9.665 | 10.445 | 2.262 | Shadows 2.670, Base pass 2.125, Local lights 1.155, Sun + ambient 0.908, Anti-aliasing 0.774 |
| local shadows=fitted, wind off | 9.855 | 9.951 | 10.213 | 2.248 | Shadows 2.883, Base pass 2.107, Local lights 1.147, Sun + ambient 0.900, Anti-aliasing 0.765 |
| local shadows=fitted, wind off, cache | 9.533 | 9.634 | 9.910 | 2.211 | Shadows 2.562, Base pass 2.105, Local lights 1.165, Sun + ambient 0.898, Anti-aliasing 0.770 |

## View: foliage_dense

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 9.423 | 9.466 | 9.851 | 0.475 | Base pass 3.187, Shadows 2.061, Sun + ambient 0.796, Anti-aliasing 0.705, GTAO 0.671 |
| local shadows=fixed 1024 | 10.349 | 10.403 | 10.765 | 0.691 | Base pass 3.200, Shadows 2.954, Sun + ambient 0.786, Anti-aliasing 0.705, GTAO 0.664 |
| local shadows=fitted, 1080p+8% (TI) | 11.275 | 11.316 | 11.697 | 1.349 | Shadows 3.890, Base pass 3.212, Sun + ambient 0.792, Anti-aliasing 0.719, GTAO 0.667 |
| local shadows=fitted, half density | 11.127 | 11.167 | 11.542 | 1.342 | Shadows 3.780, Base pass 3.165, Sun + ambient 0.814, Anti-aliasing 0.696, GTAO 0.691 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 10.028 | 10.088 | 10.447 | 1.358 | Base pass 3.193, Shadows 2.696, Sun + ambient 0.788, Anti-aliasing 0.704, GTAO 0.669 |
| local shadows=fitted, no far clip | 12.193 | 12.194 | 12.691 | 1.342 | Shadows 4.838, Base pass 3.190, Sun + ambient 0.791, Anti-aliasing 0.704, GTAO 0.675 |
| local shadows=fitted, cache | 11.253 | 11.301 | 11.675 | 1.340 | Shadows 3.882, Base pass 3.201, Sun + ambient 0.784, Anti-aliasing 0.719, GTAO 0.667 |
| local shadows=fitted, cache, moving casters every 2nd frame | 10.866 | 10.904 | 11.711 | 1.348 | Shadows 3.513, Base pass 3.184, Sun + ambient 0.789, Anti-aliasing 0.712, GTAO 0.676 |
| local shadows=fitted, wind off | 11.145 | 11.224 | 11.487 | 1.343 | Shadows 3.897, Base pass 3.188, Sun + ambient 0.779, Anti-aliasing 0.705, GTAO 0.684 |
| local shadows=fitted, wind off, cache | 10.933 | 11.008 | 11.262 | 1.331 | Shadows 3.687, Base pass 3.186, Sun + ambient 0.793, Anti-aliasing 0.700, GTAO 0.676 |

## View: busts_closeup

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.598 | 6.479 | 7.025 | 0.638 | Shadows 1.439, Local lights 0.866, Sun + ambient 0.842, GTAO 0.719, Base pass 0.693 |
| local shadows=fixed 1024 | 6.905 | 6.785 | 7.329 | 0.765 | Shadows 1.744, Local lights 0.871, Sun + ambient 0.849, GTAO 0.711, Base pass 0.702 |
| local shadows=fitted, 1080p+8% (TI) | 6.761 | 6.643 | 7.208 | 0.960 | Shadows 1.583, Local lights 0.870, Sun + ambient 0.842, GTAO 0.714, Base pass 0.704 |
| local shadows=fitted, half density | 6.822 | 6.741 | 7.307 | 0.961 | Shadows 1.572, Sun + ambient 0.924, Local lights 0.867, GTAO 0.714, Base pass 0.706 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 6.558 | 6.424 | 6.999 | 0.963 | Shadows 1.406, Local lights 0.877, Sun + ambient 0.844, GTAO 0.712, Base pass 0.693 |
| local shadows=fitted, no far clip | 6.848 | 6.739 | 7.282 | 0.963 | Shadows 1.672, Local lights 0.878, Sun + ambient 0.837, GTAO 0.717, Base pass 0.696 |
| local shadows=fitted, cache | 6.772 | 6.654 | 7.178 | 0.960 | Shadows 1.597, Local lights 0.874, Sun + ambient 0.867, GTAO 0.711, Base pass 0.700 |
| local shadows=fitted, cache, moving casters every 2nd frame | 6.761 | 6.657 | 7.186 | 0.962 | Shadows 1.603, Local lights 0.882, Sun + ambient 0.839, GTAO 0.718, Base pass 0.695 |
| local shadows=fitted, wind off | 6.774 | 6.578 | 7.209 | 0.967 | Shadows 1.593, Local lights 0.874, Sun + ambient 0.864, GTAO 0.712, Base pass 0.701 |
| local shadows=fitted, wind off, cache | 6.900 | 6.760 | 7.328 | 0.958 | Shadows 1.589, Local lights 0.879, Sun + ambient 0.842, Anti-aliasing 0.823, GTAO 0.719 |

## View: lamps

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.264 | 5.131 | 5.735 | 0.412 | Base pass 0.937, Sun + ambient 0.714, Anti-aliasing 0.635, GTAO 0.619, Shadows 0.585 |
| local shadows=fixed 1024 | 5.916 | 5.788 | 6.380 | 0.568 | Shadows 1.233, Base pass 0.933, Sun + ambient 0.704, Anti-aliasing 0.632, GTAO 0.616 |
| local shadows=fitted, 1080p+8% (TI) | 5.768 | 5.633 | 6.193 | 1.151 | Shadows 1.076, Base pass 0.938, Sun + ambient 0.705, Anti-aliasing 0.646, GTAO 0.623 |
| local shadows=fitted, half density | 5.616 | 5.480 | 6.060 | 1.143 | Base pass 0.926, Shadows 0.920, Sun + ambient 0.702, Anti-aliasing 0.656, GTAO 0.612 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 5.891 | 5.745 | 6.325 | 1.145 | Shadows 1.063, Base pass 0.938, Anti-aliasing 0.777, Sun + ambient 0.705, GTAO 0.626 |
| local shadows=fitted, no far clip | 6.121 | 5.987 | 6.536 | 1.150 | Shadows 1.433, Base pass 0.927, Sun + ambient 0.701, Anti-aliasing 0.651, GTAO 0.627 |
| local shadows=fitted, cache | 5.795 | 5.656 | 6.235 | 1.130 | Shadows 1.063, Base pass 0.936, Sun + ambient 0.709, Anti-aliasing 0.645, GTAO 0.619 |
| local shadows=fitted, cache, moving casters every 2nd frame | 5.685 | 5.688 | 6.159 | 1.146 | Shadows 0.990, Base pass 0.932, Sun + ambient 0.707, Anti-aliasing 0.642, GTAO 0.625 |
| local shadows=fitted, wind off | 5.737 | 5.581 | 6.177 | 1.152 | Shadows 1.077, Base pass 0.940, Sun + ambient 0.704, Anti-aliasing 0.643, GTAO 0.615 |
| local shadows=fitted, wind off, cache | 5.714 | 5.573 | 6.155 | 1.142 | Shadows 1.047, Base pass 0.932, Sun + ambient 0.704, Anti-aliasing 0.635, GTAO 0.626 |

## View: arcade

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.464 | 4.309 | 4.936 | 0.277 | Sun + ambient 0.755, GTAO 0.738, Shadows 0.521, Anti-aliasing 0.503, Local lights 0.368 |
| local shadows=fixed 1024 | 4.651 | 4.503 | 5.096 | 0.305 | Sun + ambient 0.898, GTAO 0.730, Shadows 0.594, Anti-aliasing 0.489, Local lights 0.367 |
| local shadows=fitted, 1080p+8% (TI) | 4.535 | 4.397 | 5.009 | 0.340 | Sun + ambient 0.757, GTAO 0.734, Shadows 0.614, Anti-aliasing 0.510, Local lights 0.370 |
| local shadows=fitted, half density | 4.530 | 4.397 | 4.999 | 0.337 | Sun + ambient 0.751, GTAO 0.734, Shadows 0.609, Anti-aliasing 0.497, Local lights 0.372 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.490 | 4.354 | 4.973 | 0.350 | Sun + ambient 0.760, GTAO 0.730, Shadows 0.573, Anti-aliasing 0.502, Local lights 0.371 |
| local shadows=fitted, no far clip | 4.535 | 4.399 | 4.993 | 0.338 | Sun + ambient 0.757, GTAO 0.733, Shadows 0.616, Anti-aliasing 0.504, Local lights 0.368 |
| local shadows=fitted, cache | 4.605 | 4.519 | 5.023 | 0.332 | Sun + ambient 0.835, GTAO 0.731, Shadows 0.604, Anti-aliasing 0.502, Local lights 0.369 |
| local shadows=fitted, cache, moving casters every 2nd frame | 4.522 | 4.388 | 4.984 | 0.340 | Sun + ambient 0.752, GTAO 0.738, Shadows 0.606, Anti-aliasing 0.501, Local lights 0.370 |
| local shadows=fitted, wind off | 4.521 | 4.388 | 5.001 | 0.333 | Sun + ambient 0.752, GTAO 0.729, Shadows 0.610, Anti-aliasing 0.499, Local lights 0.371 |
| local shadows=fitted, wind off, cache | 4.517 | 4.381 | 4.973 | 0.331 | Sun + ambient 0.753, GTAO 0.736, Shadows 0.594, Anti-aliasing 0.498, Local lights 0.369 |

## View: skyline

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 6.765 | 6.669 | 7.174 | 0.484 | Base pass 1.424, Local lights 1.224, Shadows 1.018, Anti-aliasing 0.612, Sun + ambient 0.589 |
| local shadows=fixed 1024 | 7.809 | 7.788 | 8.229 | 0.818 | Shadows 2.098, Base pass 1.438, Local lights 1.219, Anti-aliasing 0.616, Sun + ambient 0.609 |
| local shadows=fitted, 1080p+8% (TI) | 8.280 | 8.340 | 8.686 | 1.712 | Shadows 2.579, Base pass 1.420, Local lights 1.231, Anti-aliasing 0.619, Sun + ambient 0.572 |
| local shadows=fitted, half density | 7.497 | 7.334 | 7.904 | 1.697 | Shadows 1.801, Base pass 1.425, Local lights 1.231, Anti-aliasing 0.618, Sun + ambient 0.586 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 7.572 | 7.573 | 7.989 | 1.702 | Shadows 1.869, Base pass 1.428, Local lights 1.213, Anti-aliasing 0.622, Sun + ambient 0.587 |
| local shadows=fitted, no far clip | 8.868 | 8.921 | 9.282 | 1.695 | Shadows 3.163, Base pass 1.428, Local lights 1.237, Anti-aliasing 0.612, Sun + ambient 0.583 |
| local shadows=fitted, cache | 8.288 | 8.353 | 8.708 | 1.707 | Shadows 2.539, Base pass 1.404, Local lights 1.242, Anti-aliasing 0.616, Sun + ambient 0.613 |
| local shadows=fitted, cache, moving casters every 2nd frame | 7.834 | 7.718 | 8.696 | 1.710 | Shadows 2.122, Base pass 1.430, Local lights 1.218, Anti-aliasing 0.610, Sun + ambient 0.589 |
| local shadows=fitted, wind off | 8.374 | 8.373 | 8.805 | 1.692 | Shadows 2.576, Base pass 1.425, Local lights 1.230, Anti-aliasing 0.703, Sun + ambient 0.585 |
| local shadows=fitted, wind off, cache | 8.172 | 8.201 | 8.580 | 1.686 | Shadows 2.510, Base pass 1.416, Local lights 1.233, Anti-aliasing 0.618, Sun + ambient 0.591 |

## View: cafe_neon

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.291 | 5.132 | 5.758 | 0.320 | Sun + ambient 0.882, GTAO 0.734, Anti-aliasing 0.633, Local lights 0.630, Shadows 0.582 |
| local shadows=fixed 1024 | 5.508 | 5.361 | 5.970 | 0.438 | Shadows 0.860, Sun + ambient 0.855, GTAO 0.734, Anti-aliasing 0.631, Local lights 0.611 |
| local shadows=fitted, 1080p+8% (TI) | 5.852 | 5.823 | 6.401 | 0.708 | Shadows 1.190, Sun + ambient 0.854, GTAO 0.727, Anti-aliasing 0.636, Local lights 0.622 |
| local shadows=fitted, half density | 5.852 | 5.824 | 6.415 | 0.709 | Shadows 1.184, Sun + ambient 0.865, GTAO 0.738, Anti-aliasing 0.631, Local lights 0.612 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 5.655 | 5.618 | 6.053 | 0.703 | GTAO 0.883, Sun + ambient 0.860, Shadows 0.829, Anti-aliasing 0.642, Local lights 0.619 |
| local shadows=fitted, no far clip | 5.877 | 5.866 | 6.406 | 0.707 | Shadows 1.225, Sun + ambient 0.854, GTAO 0.729, Anti-aliasing 0.630, Local lights 0.616 |
| local shadows=fitted, cache | 5.853 | 5.828 | 6.378 | 0.696 | Shadows 1.191, Sun + ambient 0.862, GTAO 0.726, Anti-aliasing 0.638, Local lights 0.610 |
| local shadows=fitted, cache, moving casters every 2nd frame | 5.733 | 5.752 | 6.348 | 0.708 | Shadows 1.070, Sun + ambient 0.859, GTAO 0.734, Anti-aliasing 0.629, Local lights 0.617 |
| local shadows=fitted, wind off | 5.809 | 5.643 | 6.248 | 0.706 | Shadows 1.160, Sun + ambient 0.858, GTAO 0.729, Anti-aliasing 0.635, Local lights 0.616 |
| local shadows=fitted, wind off, cache | 5.507 | 5.368 | 5.978 | 0.698 | Sun + ambient 0.852, GTAO 0.725, Shadows 0.716, Local lights 0.631, Anti-aliasing 0.630 |

## View: market

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 7.093 | 7.012 | 7.599 | 0.543 | Shadows 1.555, Local lights 0.939, Sun + ambient 0.904, Base pass 0.806, Anti-aliasing 0.743 |
| local shadows=fixed 1024 | 7.262 | 7.131 | 7.677 | 0.679 | Shadows 1.886, Local lights 0.936, Sun + ambient 0.845, Base pass 0.797, Anti-aliasing 0.736 |
| local shadows=fitted, 1080p+8% (TI) | 7.411 | 7.298 | 7.852 | 1.147 | Shadows 2.024, Local lights 0.940, Sun + ambient 0.842, Base pass 0.805, Anti-aliasing 0.739 |
| local shadows=fitted, half density | 7.268 | 7.157 | 7.682 | 1.149 | Shadows 1.890, Local lights 0.936, Sun + ambient 0.846, Base pass 0.805, Anti-aliasing 0.739 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 7.007 | 6.833 | 7.440 | 1.155 | Shadows 1.632, Local lights 0.943, Sun + ambient 0.839, Base pass 0.806, Anti-aliasing 0.723 |
| local shadows=fitted, no far clip | 7.469 | 7.348 | 7.912 | 1.154 | Shadows 2.079, Local lights 0.939, Sun + ambient 0.848, Base pass 0.801, Anti-aliasing 0.736 |
| local shadows=fitted, cache | 7.418 | 7.306 | 7.860 | 1.149 | Shadows 2.023, Local lights 0.938, Sun + ambient 0.847, Base pass 0.798, Anti-aliasing 0.736 |
| local shadows=fitted, cache, moving casters every 2nd frame | 7.362 | 7.320 | 7.901 | 1.158 | Shadows 1.934, Local lights 0.945, Sun + ambient 0.845, Base pass 0.805, Anti-aliasing 0.767 |
| local shadows=fitted, wind off | 7.370 | 7.231 | 7.790 | 1.155 | Shadows 2.041, Local lights 0.946, Sun + ambient 0.834, Base pass 0.796, Anti-aliasing 0.731 |
| local shadows=fitted, wind off, cache | 7.366 | 7.178 | 7.794 | 1.143 | Shadows 2.029, Local lights 0.938, Sun + ambient 0.845, Base pass 0.793, Anti-aliasing 0.736 |

## View: camera path

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 7.041 | 6.550 | 10.087 | 0.528 | Shadows 1.341, Base pass 1.333, Sun + ambient 0.798, Anti-aliasing 0.745, Local lights 0.715 |
| local shadows=fixed 1024 | 7.511 | 6.734 | 10.971 | 0.703 | Shadows 1.917, Base pass 1.345, Sun + ambient 0.790, Local lights 0.723, GTAO 0.687 |
| local shadows=fitted, 1080p+8% (TI) | 7.849 | 6.892 | 12.058 | 1.244 | Shadows 2.254, Base pass 1.339, Sun + ambient 0.797, Local lights 0.742, GTAO 0.670 |
| local shadows=fitted, half density | 7.749 | 6.862 | 11.913 | 1.250 | Shadows 2.128, Base pass 1.342, Sun + ambient 0.821, Local lights 0.730, GTAO 0.673 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 7.498 | 6.953 | 10.778 | 1.249 | Shadows 1.732, Base pass 1.333, Sun + ambient 0.957, Local lights 0.743, GTAO 0.678 |
| local shadows=fitted, no far clip | 8.384 | 7.088 | 13.596 | 1.255 | Shadows 2.769, Base pass 1.339, Sun + ambient 0.815, Local lights 0.728, GTAO 0.676 |
| local shadows=fitted, cache | 7.860 | 6.945 | 12.056 | 1.230 | Shadows 2.251, Base pass 1.338, Sun + ambient 0.798, Local lights 0.729, GTAO 0.679 |
| local shadows=fitted, cache, moving casters every 2nd frame | 7.852 | 6.877 | 12.359 | 1.262 | Shadows 2.180, Base pass 1.342, Sun + ambient 0.797, Local lights 0.735, Anti-aliasing 0.705 |
| local shadows=fitted, wind off | 7.832 | 6.866 | 12.139 | 1.252 | Shadows 2.254, Base pass 1.326, Sun + ambient 0.791, Local lights 0.745, GTAO 0.672 |
| local shadows=fitted, wind off, cache | 7.745 | 6.847 | 11.911 | 1.240 | Shadows 2.166, Base pass 1.336, Sun + ambient 0.805, Local lights 0.726, GTAO 0.680 |

## Passes vs TI budget (local shadows=fitted, 1080p+8% (TI) @ square_overview)

| pass | ms | TI budget | |
|---|---|---|---|
| Sky LUTs | 0.004 | | |
| Shadows | 2.898 | 1.60 | OVER |
| Clears | 0.030 | 0.15 | within |
| Depth pre-pass | 0.090 | 0.30 | within |
| Base pass | 2.123 | 2.80 | within |
| Decals | 0.020 | 0.30 | within |
| MSAA edge stencil | 0.087 | 0.50 | within |
| GTAO | 0.637 | 1.00 | within |
| Sun + ambient | 0.899 | 0.54 | OVER |
| Local lights | 1.141 | 1.50 | within |
| Subsurface scattering | 0.045 | 0.30 | within |
| Compose | 0.201 | 0.12 | OVER |
| Sky | 0.027 | 0.18 | within |
| Transparents + VFX | 0.188 | 1.15 | within |
| Exposure | 0.051 | | |
| Bloom | 0.223 | 0.38 | within |
| Tone LUT bake | 0.005 | | |
| Tone map | 0.146 | 0.25 | within |
| Anti-aliasing | 0.761 | 0.63 | OVER |
| Motion blur | 0.314 | 0.30 | OVER |
| Final output + UI composite | 0.055 | 0.25 | within |

