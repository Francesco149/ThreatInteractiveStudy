# Benchmark `localshadows` (20261018_175353)

GPU: NVIDIA GeForce RTX 5060. Resolution 1920x1080. Scene: .claude\worktrees\jolly-robinson-27ec02\assets\scene\courtyard.gltf. 90 warm-up + 300 measured frames per row, vsync off.

## View: square_overview

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 4.634 | 4.558 | 4.876 | 0.647 | Base pass 1.187, Shadows 0.641, Local lights 0.606, Sun + ambient 0.588, Anti-aliasing 0.411 |
| local shadows=fixed 1024 | 5.376 | 5.292 | 5.638 | 1.041 | Shadows 1.338, Base pass 1.233, Local lights 0.615, Sun + ambient 0.562, Anti-aliasing 0.422 |
| local shadows=fitted, 1080p+8% (TI) | 5.749 | 5.659 | 6.030 | 2.245 | Shadows 1.645, Base pass 1.246, Local lights 0.623, Sun + ambient 0.576, Anti-aliasing 0.426 |
| local shadows=fitted, half density | 5.281 | 5.191 | 5.549 | 2.275 | Base pass 1.245, Shadows 1.172, Local lights 0.620, Sun + ambient 0.582, Anti-aliasing 0.426 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 5.188 | 5.105 | 5.460 | 2.271 | Base pass 1.215, Shadows 1.171, Local lights 0.624, Sun + ambient 0.557, Anti-aliasing 0.420 |
| local shadows=fitted, no far clip | 6.292 | 6.251 | 6.571 | 2.227 | Shadows 2.107, Base pass 1.285, Local lights 0.627, Sun + ambient 0.591, Anti-aliasing 0.435 |
| local shadows=fitted, cache | 5.919 | 5.832 | 6.180 | 2.245 | Shadows 1.692, Base pass 1.300, Local lights 0.631, Sun + ambient 0.600, Anti-aliasing 0.439 |
| local shadows=fitted, cache, moving casters every 2nd frame | 5.791 | 5.766 | 6.271 | 2.251 | Shadows 1.538, Base pass 1.303, Local lights 0.642, Sun + ambient 0.601, Anti-aliasing 0.447 |
| local shadows=fitted, wind off | 5.881 | 5.780 | 6.146 | 2.240 | Shadows 1.694, Base pass 1.301, Local lights 0.632, Sun + ambient 0.606, Anti-aliasing 0.441 |
| local shadows=fitted, wind off, cache | 5.672 | 5.647 | 5.946 | 2.200 | Shadows 1.479, Base pass 1.303, Local lights 0.648, Sun + ambient 0.605, Anti-aliasing 0.443 |

## View: foliage_dense

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 5.337 | 5.289 | 5.602 | 0.459 | Base pass 1.825, Shadows 1.112, Sun + ambient 0.485, GTAO 0.392, Anti-aliasing 0.369 |
| local shadows=fixed 1024 | 5.920 | 5.869 | 6.167 | 0.664 | Base pass 1.849, Shadows 1.636, Sun + ambient 0.498, GTAO 0.396, Anti-aliasing 0.377 |
| local shadows=fitted, 1080p+8% (TI) | 6.561 | 6.532 | 6.820 | 1.333 | Shadows 2.222, Base pass 1.879, Sun + ambient 0.500, GTAO 0.403, Anti-aliasing 0.385 |
| local shadows=fitted, half density | 6.513 | 6.479 | 6.779 | 1.324 | Shadows 2.151, Base pass 1.886, Sun + ambient 0.508, GTAO 0.406, Anti-aliasing 0.386 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 5.658 | 5.613 | 5.952 | 1.320 | Base pass 1.820, Shadows 1.444, Sun + ambient 0.484, GTAO 0.391, Anti-aliasing 0.382 |
| local shadows=fitted, no far clip | 7.259 | 7.268 | 7.568 | 1.324 | Shadows 2.784, Base pass 1.941, Sun + ambient 0.522, GTAO 0.421, Anti-aliasing 0.399 |
| local shadows=fitted, cache | 6.761 | 6.738 | 7.051 | 1.333 | Shadows 2.282, Base pass 1.949, Sun + ambient 0.519, GTAO 0.424, Anti-aliasing 0.396 |
| local shadows=fitted, cache, moving casters every 2nd frame | 6.558 | 6.564 | 7.142 | 1.341 | Shadows 2.045, Base pass 1.956, Sun + ambient 0.530, GTAO 0.425, Anti-aliasing 0.394 |
| local shadows=fitted, wind off | 6.726 | 6.658 | 7.000 | 1.328 | Shadows 2.300, Base pass 1.960, Sun + ambient 0.529, GTAO 0.423, Anti-aliasing 0.393 |
| local shadows=fitted, wind off, cache | 6.609 | 6.601 | 6.900 | 1.316 | Shadows 2.162, Base pass 1.967, Sun + ambient 0.537, GTAO 0.428, Anti-aliasing 0.394 |

## View: busts_closeup

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.662 | 3.602 | 3.907 | 0.627 | Shadows 0.781, Sun + ambient 0.509, Local lights 0.473, GTAO 0.410, Anti-aliasing 0.372 |
| local shadows=fixed 1024 | 3.876 | 3.814 | 4.122 | 0.750 | Shadows 0.951, Sun + ambient 0.525, Local lights 0.483, GTAO 0.413, Base pass 0.374 |
| local shadows=fitted, 1080p+8% (TI) | 3.871 | 3.847 | 4.146 | 0.948 | Shadows 0.866, Sun + ambient 0.527, Local lights 0.486, GTAO 0.419, Anti-aliasing 0.370 |
| local shadows=fitted, half density | 3.824 | 3.764 | 4.097 | 0.957 | Shadows 0.865, Sun + ambient 0.535, Local lights 0.490, GTAO 0.426, Base pass 0.373 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.658 | 3.607 | 3.904 | 0.952 | Shadows 0.750, Sun + ambient 0.522, Local lights 0.477, GTAO 0.416, Base pass 0.367 |
| local shadows=fitted, no far clip | 3.904 | 3.848 | 4.117 | 0.962 | Shadows 0.919, Sun + ambient 0.541, Local lights 0.489, GTAO 0.428, Base pass 0.377 |
| local shadows=fitted, cache | 3.894 | 3.849 | 4.144 | 0.950 | Shadows 0.884, Sun + ambient 0.549, Local lights 0.497, GTAO 0.439, Anti-aliasing 0.382 |
| local shadows=fitted, cache, moving casters every 2nd frame | 3.923 | 3.875 | 4.190 | 0.955 | Shadows 0.896, Sun + ambient 0.553, Local lights 0.499, GTAO 0.445, Anti-aliasing 0.383 |
| local shadows=fitted, wind off | 3.933 | 3.879 | 4.196 | 0.956 | Shadows 0.892, Sun + ambient 0.560, Local lights 0.495, GTAO 0.442, Anti-aliasing 0.389 |
| local shadows=fitted, wind off, cache | 3.938 | 3.894 | 4.163 | 0.954 | Shadows 0.898, Sun + ambient 0.560, Local lights 0.499, GTAO 0.446, Anti-aliasing 0.389 |

## View: lamps

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 2.892 | 2.839 | 3.112 | 0.388 | Base pass 0.514, Sun + ambient 0.437, GTAO 0.352, Anti-aliasing 0.320, Shadows 0.299 |
| local shadows=fixed 1024 | 3.298 | 3.247 | 3.543 | 0.555 | Shadows 0.666, Base pass 0.516, Sun + ambient 0.443, GTAO 0.361, Anti-aliasing 0.325 |
| local shadows=fitted, 1080p+8% (TI) | 3.253 | 3.199 | 3.480 | 1.131 | Shadows 0.584, Base pass 0.524, Sun + ambient 0.448, GTAO 0.364, Anti-aliasing 0.328 |
| local shadows=fitted, half density | 3.173 | 3.117 | 3.397 | 1.121 | Base pass 0.527, Shadows 0.495, Sun + ambient 0.453, GTAO 0.363, Anti-aliasing 0.329 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.272 | 3.214 | 3.501 | 1.139 | Shadows 0.551, Base pass 0.516, Sun + ambient 0.441, Anti-aliasing 0.413, GTAO 0.358 |
| local shadows=fitted, no far clip | 3.510 | 3.474 | 3.786 | 1.147 | Shadows 0.778, Base pass 0.536, Sun + ambient 0.461, GTAO 0.371, Anti-aliasing 0.341 |
| local shadows=fitted, cache | 3.347 | 3.295 | 3.568 | 1.129 | Shadows 0.584, Base pass 0.534, Sun + ambient 0.470, GTAO 0.380, Anti-aliasing 0.344 |
| local shadows=fitted, cache, moving casters every 2nd frame | 3.301 | 3.275 | 3.595 | 1.137 | Base pass 0.547, Shadows 0.538, Sun + ambient 0.468, GTAO 0.378, Anti-aliasing 0.337 |
| local shadows=fitted, wind off | 3.376 | 3.329 | 3.593 | 1.134 | Shadows 0.595, Base pass 0.537, Sun + ambient 0.471, Anti-aliasing 0.379, GTAO 0.379 |
| local shadows=fitted, wind off, cache | 3.327 | 3.299 | 3.577 | 1.118 | Shadows 0.577, Base pass 0.544, Sun + ambient 0.475, GTAO 0.380, Anti-aliasing 0.341 |

## View: arcade

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 2.460 | 2.404 | 2.696 | 0.259 | Sun + ambient 0.461, GTAO 0.429, Anti-aliasing 0.269, Shadows 0.268, Local lights 0.212 |
| local shadows=fixed 1024 | 2.512 | 2.464 | 2.730 | 0.276 | Sun + ambient 0.468, GTAO 0.435, Shadows 0.303, Anti-aliasing 0.258, Local lights 0.217 |
| local shadows=fitted, 1080p+8% (TI) | 2.542 | 2.492 | 2.748 | 0.322 | Sun + ambient 0.473, GTAO 0.435, Shadows 0.315, Anti-aliasing 0.259, Local lights 0.220 |
| local shadows=fitted, half density | 2.552 | 2.500 | 2.768 | 0.321 | Sun + ambient 0.475, GTAO 0.440, Shadows 0.311, Anti-aliasing 0.262, Local lights 0.221 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 2.509 | 2.462 | 2.723 | 0.320 | Sun + ambient 0.472, GTAO 0.434, Shadows 0.282, Anti-aliasing 0.262, Local lights 0.220 |
| local shadows=fitted, no far clip | 2.583 | 2.533 | 2.806 | 0.321 | Sun + ambient 0.485, GTAO 0.442, Shadows 0.318, Anti-aliasing 0.265, Local lights 0.225 |
| local shadows=fitted, cache | 2.595 | 2.556 | 2.810 | 0.321 | Sun + ambient 0.492, GTAO 0.447, Shadows 0.313, Anti-aliasing 0.267, Local lights 0.226 |
| local shadows=fitted, cache, moving casters every 2nd frame | 2.611 | 2.575 | 2.831 | 0.316 | Sun + ambient 0.493, GTAO 0.450, Shadows 0.311, Anti-aliasing 0.271, Local lights 0.232 |
| local shadows=fitted, wind off | 2.615 | 2.588 | 2.845 | 0.324 | Sun + ambient 0.494, GTAO 0.450, Shadows 0.314, Anti-aliasing 0.272, Local lights 0.230 |
| local shadows=fitted, wind off, cache | 2.632 | 2.578 | 2.847 | 0.316 | Sun + ambient 0.500, GTAO 0.455, Shadows 0.312, Anti-aliasing 0.274, Local lights 0.230 |

## View: skyline

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.651 | 3.608 | 3.898 | 0.472 | Base pass 0.782, Local lights 0.652, Shadows 0.532, Anti-aliasing 0.352, Sun + ambient 0.344 |
| local shadows=fixed 1024 | 4.279 | 4.205 | 4.478 | 0.814 | Shadows 1.149, Base pass 0.797, Local lights 0.672, Sun + ambient 0.351, Anti-aliasing 0.325 |
| local shadows=fitted, 1080p+8% (TI) | 4.612 | 4.548 | 4.842 | 1.715 | Shadows 1.457, Base pass 0.811, Local lights 0.651, Sun + ambient 0.361, Anti-aliasing 0.327 |
| local shadows=fitted, half density | 4.151 | 4.079 | 4.422 | 1.718 | Shadows 0.988, Base pass 0.809, Local lights 0.657, Sun + ambient 0.364, Anti-aliasing 0.324 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.111 | 4.043 | 4.323 | 1.704 | Shadows 0.992, Base pass 0.794, Local lights 0.654, Sun + ambient 0.354, Anti-aliasing 0.321 |
| local shadows=fitted, no far clip | 5.031 | 4.964 | 5.308 | 1.710 | Shadows 1.799, Base pass 0.835, Local lights 0.657, Sun + ambient 0.375, Anti-aliasing 0.337 |
| local shadows=fitted, cache | 4.768 | 4.707 | 4.992 | 1.721 | Shadows 1.503, Base pass 0.854, Local lights 0.658, Sun + ambient 0.377, Anti-aliasing 0.340 |
| local shadows=fitted, cache, moving casters every 2nd frame | 4.467 | 4.378 | 4.992 | 1.699 | Shadows 1.210, Base pass 0.849, Local lights 0.663, Sun + ambient 0.373, Anti-aliasing 0.342 |
| local shadows=fitted, wind off | 4.744 | 4.716 | 4.989 | 1.704 | Shadows 1.508, Base pass 0.846, Local lights 0.663, Sun + ambient 0.377, Anti-aliasing 0.337 |
| local shadows=fitted, wind off, cache | 4.684 | 4.607 | 4.866 | 1.698 | Shadows 1.466, Base pass 0.843, Local lights 0.657, Sun + ambient 0.383, Anti-aliasing 0.338 |

## View: cafe_neon

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 2.909 | 2.850 | 3.159 | 0.303 | Sun + ambient 0.531, GTAO 0.426, Local lights 0.342, Anti-aliasing 0.329, Shadows 0.300 |
| local shadows=fixed 1024 | 3.090 | 3.034 | 3.310 | 0.421 | Sun + ambient 0.538, Shadows 0.456, GTAO 0.433, Local lights 0.346, Anti-aliasing 0.333 |
| local shadows=fitted, 1080p+8% (TI) | 3.324 | 3.331 | 3.648 | 0.688 | Shadows 0.657, Sun + ambient 0.550, GTAO 0.437, Local lights 0.350, Anti-aliasing 0.335 |
| local shadows=fitted, half density | 3.337 | 3.345 | 3.663 | 0.690 | Shadows 0.666, Sun + ambient 0.549, GTAO 0.441, Local lights 0.353, Anti-aliasing 0.336 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.234 | 3.244 | 3.553 | 0.691 | GTAO 0.596, Sun + ambient 0.532, Shadows 0.431, Local lights 0.350, Anti-aliasing 0.336 |
| local shadows=fitted, no far clip | 3.393 | 3.415 | 3.739 | 0.693 | Shadows 0.688, Sun + ambient 0.554, GTAO 0.446, Local lights 0.358, Anti-aliasing 0.346 |
| local shadows=fitted, cache | 3.402 | 3.430 | 3.732 | 0.693 | Shadows 0.670, Sun + ambient 0.562, GTAO 0.449, Local lights 0.361, Anti-aliasing 0.345 |
| local shadows=fitted, cache, moving casters every 2nd frame | 3.333 | 3.350 | 3.714 | 0.685 | Shadows 0.597, Sun + ambient 0.559, GTAO 0.451, Local lights 0.367, Anti-aliasing 0.346 |
| local shadows=fitted, wind off | 3.493 | 3.439 | 3.727 | 0.690 | Shadows 0.652, Sun + ambient 0.575, GTAO 0.474, Local lights 0.365, Anti-aliasing 0.340 |
| local shadows=fitted, wind off, cache | 3.105 | 3.054 | 3.325 | 0.676 | Sun + ambient 0.567, GTAO 0.449, Shadows 0.370, Local lights 0.368, Anti-aliasing 0.345 |

## View: market

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.886 | 3.814 | 4.120 | 0.531 | Shadows 0.838, Sun + ambient 0.517, Local lights 0.514, Base pass 0.430, Anti-aliasing 0.407 |
| local shadows=fixed 1024 | 4.111 | 4.045 | 4.324 | 0.673 | Shadows 1.039, Sun + ambient 0.526, Local lights 0.515, Base pass 0.431, Anti-aliasing 0.416 |
| local shadows=fitted, 1080p+8% (TI) | 4.238 | 4.190 | 4.497 | 1.147 | Shadows 1.130, Sun + ambient 0.540, Local lights 0.518, Base pass 0.438, Anti-aliasing 0.418 |
| local shadows=fitted, half density | 4.159 | 4.101 | 4.376 | 1.139 | Shadows 1.062, Sun + ambient 0.532, Local lights 0.513, Base pass 0.431, Anti-aliasing 0.421 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 3.952 | 3.897 | 4.197 | 1.143 | Shadows 0.874, Local lights 0.518, Sun + ambient 0.509, GTAO 0.444, Base pass 0.431 |
| local shadows=fitted, no far clip | 4.351 | 4.314 | 4.622 | 1.150 | Shadows 1.181, Sun + ambient 0.550, Local lights 0.532, Base pass 0.443, Anti-aliasing 0.429 |
| local shadows=fitted, cache | 4.360 | 4.306 | 4.604 | 1.151 | Shadows 1.154, Sun + ambient 0.556, Local lights 0.534, Base pass 0.450, Anti-aliasing 0.436 |
| local shadows=fitted, cache, moving casters every 2nd frame | 4.296 | 4.291 | 4.592 | 1.148 | Shadows 1.096, Sun + ambient 0.562, Local lights 0.531, Base pass 0.448, Anti-aliasing 0.431 |
| local shadows=fitted, wind off | 4.325 | 4.276 | 4.587 | 1.151 | Shadows 1.158, Sun + ambient 0.560, Local lights 0.537, Base pass 0.445, Anti-aliasing 0.434 |
| local shadows=fitted, wind off, cache | 4.344 | 4.289 | 4.565 | 1.140 | Shadows 1.158, Sun + ambient 0.561, Local lights 0.538, Base pass 0.448, Anti-aliasing 0.436 |

## View: camera path

| config | GPU mean | median | p95 | CPU mean | top passes (ms) |
|---|---|---|---|---|---|
| local shadows=off | 3.883 | 3.559 | 5.678 | 0.513 | Base pass 0.748, Shadows 0.731, Sun + ambient 0.487, GTAO 0.393, Local lights 0.392 |
| local shadows=fixed 1024 | 4.250 | 3.800 | 6.328 | 0.692 | Shadows 1.052, Base pass 0.752, Sun + ambient 0.500, Local lights 0.404, GTAO 0.401 |
| local shadows=fitted, 1080p+8% (TI) | 4.503 | 3.871 | 7.115 | 1.239 | Shadows 1.273, Base pass 0.768, Sun + ambient 0.502, Local lights 0.408, GTAO 0.403 |
| local shadows=fitted, half density | 4.444 | 3.876 | 7.012 | 1.249 | Shadows 1.202, Base pass 0.770, Sun + ambient 0.519, Local lights 0.407, GTAO 0.400 |
| local shadows=fitted, all foliage as solid cards (cascades too) | 4.141 | 3.752 | 6.084 | 1.229 | Shadows 0.921, Base pass 0.755, Sun + ambient 0.502, Local lights 0.395, GTAO 0.392 |
| local shadows=fitted, no far clip | 4.890 | 4.047 | 8.102 | 1.231 | Shadows 1.571, Base pass 0.791, Sun + ambient 0.527, Local lights 0.416, GTAO 0.416 |
| local shadows=fitted, cache | 4.646 | 3.992 | 7.367 | 1.237 | Shadows 1.301, Base pass 0.795, Sun + ambient 0.522, Local lights 0.421, GTAO 0.420 |
| local shadows=fitted, cache, moving casters every 2nd frame | 4.598 | 4.015 | 7.519 | 1.235 | Shadows 1.253, Base pass 0.802, Sun + ambient 0.525, GTAO 0.421, Local lights 0.421 |
| local shadows=fitted, wind off | 4.656 | 4.031 | 7.347 | 1.249 | Shadows 1.309, Base pass 0.798, Sun + ambient 0.529, Local lights 0.424, GTAO 0.423 |
| local shadows=fitted, wind off, cache | 4.590 | 3.961 | 7.250 | 1.239 | Shadows 1.255, Base pass 0.805, Sun + ambient 0.532, Local lights 0.418, GTAO 0.418 |

## Passes vs TI budget (local shadows=fitted, 1080p+8% (TI) @ square_overview)

| pass | ms | TI budget | |
|---|---|---|---|
| Sky LUTs | 0.000 | | |
| Shadows | 1.645 | 1.60 | OVER |
| Clears | 0.013 | 0.15 | within |
| Depth pre-pass | 0.046 | 0.30 | within |
| Base pass | 1.246 | 2.80 | within |
| Decals | 0.004 | 0.30 | within |
| MSAA edge stencil | 0.055 | 0.50 | within |
| GTAO | 0.370 | 1.00 | within |
| Sun + ambient | 0.576 | 0.54 | OVER |
| Local lights | 0.623 | 1.50 | within |
| Subsurface scattering | 0.022 | 0.30 | within |
| Compose | 0.141 | 0.12 | OVER |
| Sky | 0.007 | 0.18 | within |
| Transparents + VFX | 0.081 | 1.15 | within |
| Exposure | 0.032 | | |
| Bloom | 0.124 | 0.38 | within |
| Tone LUT bake | 0.000 | | |
| Tone map | 0.107 | 0.25 | within |
| Anti-aliasing | 0.426 | 0.63 | within |
| Motion blur | 0.202 | 0.30 | within |
| Final output + UI composite | 0.029 | 0.25 | within |

