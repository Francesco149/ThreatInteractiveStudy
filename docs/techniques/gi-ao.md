# Global illumination and ambient occlusion: baked probe grid, sun relighting, GTAO

Video chapter: 49:45 "Global Illumination: Cascaded Probe Lighting" (the chart's GI block, marked
WIP; the narration runs to ~53:30). Research:
[../research/05-sss-gi-reflections-sky.md](../research/05-sss-gi-reflections-sky.md) (section 6 for
the probes, section 7 for GTAO; "R5" below) and
[../research/00-video-pipeline-chart.md](../research/00-video-pipeline-chart.md) (section 2.12, the
GI block and its budget). Code: `src/r_gi.cpp`, `shaders/gi.hlsli`, `shaders/gi_capture.hlsl`,
`shaders/gi_relight.hlsl`, `src/r_gtao.cpp`, `shaders/gtao.hlsl`, and `ps_sun` in
`shaders/lighting.hlsl`. The draw that consumes the result: [lighting.md](lighting.md). The sky that
lights the probes: [tonemap-sky-bloom.md](tonemap-sky-bloom.md).

TI's requirements: world-space GI that is **noiseless** and is fed by the **shadow maps that
already exist**. A world-space trace is only a fallback, and there are **no reflective shadow maps**
(0.3-0.6 ms in Crysis 3 and Tesseract, for a poor result). Baked grids or probes (The Division,
Days Gone, Fox Engine) replace lightmaps and light static and dynamic objects with the same data.
Each probe owns **irradiance sensors**. Their baked data is the most unoccluded direction and the
average albedo of the static geometry facing it; their dynamic data is direct-light occlusion from
the shadow maps, sampled at several positions to avoid popping. The sensors are **fed into the
probes**, and light is **propagated across probes** for 3+ bounces. Days Gone updates its baked
probes from shadow maps in 0.3 ms. Probes and sensors are placed automatically. On top come
screen-space layers: SSGI driven by the lit scene (not the albedo buffer), and AO that is
noiseless, has no halos (unlike CIAO), never touches the sky or direct light, and uses an ALU Bayer
pattern because it is not temporal. Budget (chart 2.12): **1.0 ms world-space GI + 1.0 ms for the
screen-space techniques**.

## What runs, and when

| Work | When | Profiler scope | Where in the frame |
|---|---|---|---|
| **Capture**: rasterize what every probe sees into "sensors" | scene load, "Rebake GI" | GI capture | `gi_update()`, after `pass_sky_luts()`, before the draw lists |
| **Relight**: whole-scene sun shadow map, then one compute pass per bounce | after a capture; when the sun changes | GI relight (Sun shadow map, Bounces) | same |
| **GTAO** | every frame | GTAO (Linear depth, Horizon search, Denoise 4x4, Upsample) | after the MSAA edge stencil |
| **Probe lookup + AO application** | every frame | inside Sun + ambient | `ps_sun`, every shading-model draw |

The capture/relight split is the core of the design. Geometry is rasterized once per bake. When
the sun moves, the stored sensors are re-lit in milliseconds instead of re-baked: TI's "shadow maps
first" relighting, run on demand instead of every frame. The sky LUTs are updated first, so a bake
or relight sees this frame's sky. Nothing is stochastic: no rays, no noise, no temporal
accumulation.

## The grid

### Placement: the playable volume (`place_grid`)

- **Box:** the bounds of the static instances (`anim == ANIM_NONE`; animated instances are dynamic:
  lit by the grid, not part of it), **cropped to the scene's camera positions** (viewpoints and
  benchmark path) plus a margin of `VOLUME_MARGIN` = (12, 6, 12) m. Probes only pay off where
  shading is seen. Without the crop, a 120 m ground plane or a 40 m tower would spread the budget
  thin. A scene without camera positions uses the static bounds.
- **Spacing:** `gi_probe_spacing` (1.5 m, at least 0.25 m). It is multiplied by 1.05 until
  `nx * ny * nz <= gi_max_probes` (16384), with at least 2 probes per axis because a lookup
  interpolates a 2x2x2 cell. The grid is centred on the box and overhangs it by up to half a cell.
- Courtyard: 35x13x35 = 15925 probes at 1.57 m (the budget grew the 1.5 m spacing by one step).
  Test scene: 46x6x25 = 6900 probes at 1.5 m (with the "leak room" viewpoint).
- Lookups outside the grid clamp to its outer probes.

### Storage

- **Grid:** one `Texture3D` RGBA16F of nx x ny x 6nz. The six axis directions of an **ambient
  cube** (Valve, McTaggart 2004; used as an irradiance volume by Hooker 2016) are six stacks along
  z: +X, -X, +Y, -Y, +Z, -Z. A texel holds `(E_axis * validity, validity)`, where `E_axis` is the
  irradiance received by a surface facing that axis. Premultiplying means an invalid probe drops
  out of any weighted sum on its own. Two grids ping-pong between bounces, and a third holds the
  static emissive contribution. Ambient cubes need three of the six colours per normal and, unlike
  L1 SH, cannot ring negative under a strong sun.
- **Sensors:** an `R32G32_UINT` atlas, 128 probes per row (`PROBES_PER_ROW`). Each probe is a strip
  of six 8x8 face tiles (`FACE_RES` = `GI_FACE_RES` = 8), so the atlas is 6144 px wide and
  `8 * ceil(N / 128)` px tall. Probe `i = x + nx * (y + ny * z)` owns tile column
  `(i % 128) * 6 + face` and tile row `i / 128`. The sensors stay resident: the relight needs them,
  and so does the runtime visibility test.
- **Memory:** 3 KB of sensors per probe (6 x 8 x 8 texels x 8 bytes), 144 bytes of grids per probe
  (3 grids x 6 texels x 8 bytes), plus the relight's 16 MB shadow map. Courtyard: 65.1 MB, 46.9 MB
  of which are sensors. The capture also allocates an R11G11B10F emissive atlas and a D32 depth
  atlas of the same size: 8 bytes per texel, 46.9 MB on the courtyard, freed afterwards.

## Capture (`capture()`, `gi_capture.hlsl`)

### What a sensor stores

Every cube texel keeps the nearest surface in its direction, or "sky":

| bits | content |
|---|---|
| x 0-23 | albedo rgb, sqrt-encoded, 8 bits each (base colour x albedo texture, after the alpha test) |
| x 24-31 | flags: 1 = hit, 2 = back face (a cleared texel, 0, is sky) |
| y 0-15 | octahedral normal, 8 + 8 bits (flipped towards the probe on back faces) |
| y 16-31 | hit distance from the probe, f16, metres |

This is TI's baked sensor data, without clustering: one sensor per 11° texel, instead of a dominant
direction with an average albedo. Emissive radiance goes to a second render target (front faces
only).

### One instanced draw per batch, per block and face

Probes are captured in blocks of 8x8x8 (`BLOCK`), one cube face at a time. For each (block, face)
the CPU keeps the objects (one primitive of one static instance; alpha-blended materials excluded)
that can matter:

- **Inside the face's 90° frustum for at least one probe of the block.** A probe's frustum is the 4
  planes through it with normals `F - R`, `F + R`, `F - U`, `F + U`. For each plane, the offset is
  taken from the block's most permissive probe (the box's support point), so a bounding sphere is
  rejected if it lies behind one plane for every probe of the block. The normals have length √2,
  hence `radius * 1.4142136` in the test.
- **Not sub-texel:** an object whose radius is below 0.06 x its distance from the block
  (`MIN_ANGULAR_RADIUS`, less than about half a texel across) is skipped. Below 0.5
  (`FAR_ANGULAR_RADIUS`) it uses its coarsest LOD; otherwise LOD 2 (`CAPTURE_LOD`, 4x fewer
  triangles, plenty for 8x8-texel faces).

The kept objects are sorted by (index of the primitive's first object, LOD, object). The order
does not depend on pointers, so coplanar surfaces z-fight the same way in every bake. Each run of
the same primitive and LOD becomes **one `DrawIndexedInstanced`** with `objects x probes in the
block` instances. Runs are split into draws of at most 25 M triangles (`MAX_DRAW_TRIANGLES`), but
never below one object per draw.

The vertex shader turns the instance into (object = `instance % count`, probe =
`instance / count`) and projects with that face's camera in the D3D cube convention:
`x = rel.right`, `y = rel.up`, `w = rel.forward`. It then remaps the face's [-1,1]² NDC square onto
the probe's tile, in clip space (`x * R/W + w * offset_x`, likewise for y) so the mapping stays
linear before the perspective divide. Four `SV_ClipDistance` values `(w - x, w + x, w - y, w + y)`
cut triangles at the tile's frustum, so nothing spills into neighbouring tiles. D3D11 needs no
viewport arrays or geometry shader for this. Depth is reversed Z with `z = near` (2 cm,
`CAPTURE_NEAR`), so device depth is `near / w`, tested GREATER into one D32 atlas.

The rasterizer culls nothing: back faces are detected, not culled. It treats clockwise triangles
as front faces (`FrontCounterClockwise = FALSE`), because the cube cameras are left-handed while
the main camera is right-handed, which mirrors the image. A texel is a **back face** when
`!SV_IsFrontFace` and the material is single-sided: the probe is looking at the inside of a closed
mesh.

### Pacing

After every 100 M submitted triangles (`CAPTURE_TRIANGLES_PER_WAIT`, about 10-20 ms of GPU work)
the CPU waits for the GPU with an event query. The queue then never holds seconds of work, because
one long submission on a busy GPU can hit the 2 s TDR timeout or worse. The courtyard bake is
44907 draws, 11.4 G triangles and 110 waits.

### Emissive

After the capture, one dispatch of the relight shader compiled with `EMISSIVE_ONLY` projects the
captured emissive radiance (front-face hits only) into a static grid, with the same projection as
the relight below. Emission does not depend on the sun, so every relight just adds that grid.

## Relight (`relight()`, `gi_relight.hlsl`)

### 1. Whole-scene sun shadow map

The camera cascades only cover the view, but the sensors cover the whole grid, so the relight
renders its own shadow map:

- 2048² D32F (`SHADOW_SIZE`), orthographic, reversed Z. In light space, x and y are fitted to the
  grid's box grown by one spacing; the depth range spans the whole scene's bounds ± 1 m, so casters
  outside the grid still shadow it.
- Casters: static, shadow-casting, non-blended instances at the capture LOD, alpha test included,
  drawn with the regular shadow rasterizer states. The list goes through the frame's draw buffer,
  which is why `gi_update()` runs before the frame builds its own lists.
- Sensor lookup: offset 1.5 texels along the sensor's normal and 1 texel towards the sun (normal
  offset, Holbert 2011), then one bilinear comparison tap (2x2 PCF). A sensor outside the map is
  lit.

### 2. One compute pass per bounce

One thread group of 64 threads per probe (the dispatch layout is the atlas layout,
128 x rows). Thread `(x, y)` handles texel `(x, y)` of all six faces:

```
L = sky_radiance(dir)                                   texel saw the sky (atmosphere, no sun disk)
  = 0, and d_omega counts as back face                  texel saw a back face
  = albedo/pi * (E_sun * max(0, n.l) * shadow(P)        texel saw a surface at P = probe + dir * distance
                 + E_prev(P, n))
E_axis += L * max(0, dir.axis) * d_omega                with d_omega = (2/8)^2 / (1 + u^2 + v^2)^1.5
```

- `E_prev` is the previous bounce's grid, sampled by `gi_sample_grid()` **exactly as the lighting
  pass samples it** (same biases and visibility test, with V pointing back at the probe). It is zero
  on bounce 0. N passes give N bounces (`gi_bounces`, default 3), ping-ponging the two grids: each
  pass carries light one surface further. That is "propagate light across probes", and the radiance
  transfer idea of Far Cry 3's Deferred Radiance Transfer Volumes (GDC 2012) and The Division
  (Stefanov, GDC 2016), done with rasterized sensors instead of SH transfer matrices or rays.
- The sky texels exclude the sun's disk, and the sun reaches sensors only as shadowed direct light.
  A probe therefore never contains the direct sun at its own position: that is `ps_sun`'s job, so
  nothing is counted twice.
- Groupshared tree reduction over the 64 threads (6 float3 plus the back-face weight), then threads
  0-5 write the six axes.
- **Projection normalization:** the discrete sum of `max(0, dir.x) * d_omega` over the 384 texels is
  not exactly π. `gi_init()` computes it on the CPU, exactly as the shader sums it, and the result is
  scaled by `π / sum`. A uniform sky of radiance 1 then gives exactly E = π.
- The emissive grid is added.

### Probe validity (inside-geometry classification)

A probe buried in a wall or under the ground sees mostly back faces. If the back-face solid angle
is at least 25% of the total (`BACKFACE_LIMIT`; the total, the sum of `d_omega`, is about 4π), its
validity is 0. This is DDGI's classification, and Hooker 2016 likewise ignores voxels that see back
faces. Double-sided materials (leaf cards, cloth) never count as back faces. The validity is stored
premultiplied, so an invalid probe drops out of the lighting lookups *and* of the next bounce's
`E_prev`. A read-back counts the invalid probes once after each bake: 4255 of 15925 (27%) on the
courtyard, 6 on the test scene.

### Triggers

- After every capture. That relight is timed with GPU waits; the log reports its two halves.
- When the sun's direction changes (sum of absolute component differences > 1e-5) or its
  top-of-atmosphere illuminance (`sun_color.w`) does, with `gi_relight_on_sun_change`. Not
  `sun_color.rgb`: that is the illuminance after the atmosphere's transmittance at the camera's
  altitude, so it changes whenever the camera moves up or down (the first merged version compared it
  and re-lit the probes on most frames of the climbing and descending benchmark path).
- When `gi_bounces`, the lookup biases or the visibility test change: every bounce samples the
  previous grid with them.
- Every frame with the cost test (`gi_relight_every_frame`).

Geometry is only re-captured on scene load or Rebake; spacing and budget changes need a Rebake.

## Sampling in the lighting pass (`gi_sample_grid`, `indirect_light`)

The lookup is DDGI's robust interpolant (Majercik et al. 2019; RTXGI's `Irradiance.hlsl`), with
ambient cubes instead of octahedral irradiance and without its depth maps:

1. **Biased position:** `Pb = P + (N * 0.25 + V * 0.15) * spacing` (`gi_normal_bias`,
   `gi_view_bias`; normal offset as in Silvennoinen & Timonen 2015). The point is pushed off the
   surface and towards the viewer, so it interpolates probes on the side the surface faces.
2. **Cell:** the 8 probes around `Pb` (clamped to the grid) and their trilinear weights `t`.
3. **Smooth backface weight:** `w = t * (wrap² + 0.2)` with
   `wrap = (dot(normalize(probe - P), N) + 1) / 2`: probes behind the surface count less.
4. **Visibility** (`gi_visibility`, on by default): the probe looks up its own sensor in the
   direction of `Pb` and gets `seen`, the distance its capture reached in that direction (sky =
   unbounded). With `dist = |Pb - probe|`, the weight is multiplied by
   `max(saturate(1 + (seen - dist) / (0.25 * spacing)), 0.02)`. A probe on the surface's side sees
   past `Pb` (that is what the biases buy), so it keeps full weight. A probe behind a wall hits the
   wall first and drops to 2% over a soft quarter-cell ramp. This stops light leaking through walls
   thinner than the probe spacing, using the capture the bake already has instead of DDGI's
   per-probe mean/mean² depth maps and Chebyshev test. The 2% floor keeps the normalization defined
   when all 8 probes are occluded.
5. **Ambient cube:** `E(N) = sum(N_i² * E_axis(sign N_i))`, three loads per probe (three more for
   the specular direction).
6. Normalize by `sum(w * validity)`. The returned validity is `sum(t * validity)`.

`indirect_light()` starts from the sky ambient cube (atmosphere): irradiance `π * sky_ambient(N)`,
specular radiance `sky_ambient(dir)`. That is all there is with GI off, before the first bake, or
where no valid probe is near. With a baked grid it blends towards the probes by
`k = saturate(20 * validity)`: fully probe-lit once 5% of the trilinear weight sits on valid probes.

**Specular fallback:** the probe irradiance around the reflection direction, divided by π, is the
cosine-weighted average radiance of that hemisphere: what a roughness-1 lobe reflects. It has no
high-frequency detail (that is a reflection pass's job, not implemented yet), but the scene
occludes and tints it like the diffuse GI. The direction is the mirror direction bent towards the
normal with roughness, Frostbite's dominant direction (Lagarde & de Rousiers 2014):
`lerp(N, R, (1 - a) * (sqrt(1 - a) + a))` with `a = roughness²`.

**Cost:** 8 x (3 + 3 + 1) = **56 point loads** per shaded sample, inside every shading model's sun
draw (pixel frequency, plus sample frequency on MSAA edges). The manual 8-probe weights rule out
hardware trilinear filtering.

## Ambient occlusion: GTAO (`r_gtao.cpp`, `gtao.hlsl`)

GTAO (Jimenez, Wu, Pesce, Jarabo 2016, "Practical Real-Time Strategies for Accurate Indirect
Occlusion"), horizon-based and cosine-weighted, at full (default) or half resolution, **without
temporal history**. Three pixel-shader passes (four at half resolution) on full-screen triangles,
1x targets, reading MSAA sample 0 of the depth and the G-buffer normal:

1. **Linear depth** (R32F at AO resolution): view distance, sky = 1e6. At half resolution, the
   top-left pixel of each 2x2.
2. **Horizon search** (R8): the paper's Algorithm 1 in a left-handed view space (x right, y up,
   z forward), with the jittered projection undone exactly.
   - Radius 0.75 m (`ao_radius`), projected at the pixel's depth and capped at 64 full-res pixels.
     Pixels where the radius covers less than 1 px return 1.
   - 2 slices (`ao_slices`). Slice angle `(s + noise) * π / slices`, with `noise` from a **4x4 Bayer
     matrix** (/16): a per-pixel rotation computed in ALU, no noise texture.
   - 4 steps per side (`ao_steps`) at `t = ((k + o) / steps)²` of the radius (denser near the
     centre), at least `k + 1` pixels out. `o = frac(7 * noise + 0.5)` is a permutation of the same
     16 values, decorrelated from the rotation.
   - Occluders fade to "no occlusion" (cos = -1) over the last `0.33 * radius` metres.
   - Horizons are clamped to the hemisphere around the projected normal. Each side contributes the
     analytic cosine-weighted visible arc `¼(-cos(2h - n) + cos n + 2h sin n)`, weighted by the
     projected normal's length.
3. **Denoise 4x4** (R8; `ao_denoise`): offsets -2..+1. Any 4x4 window contains each of the 16 Bayer
   rotations exactly once, so the average combines 2 x 16 = 32 slice directions per pixel. This is
   the spatial half of the paper's 4x4 spatial x 6 temporal scheme; XeGTAO's denoiser likewise works
   without TAA. A neighbour's weight is `saturate(1 - |N_c . (P_q - P_c)| / (0.02 * z * scale + 0.01))`,
   its distance to the centre's tangent plane, so AO never bleeds across depth discontinuities. No
   halos, which is TI's complaint about CIAO.
4. **Upsample** (half res only): AO texel `h` stands for full-res pixel `2h`. Even pixels take one
   texel, odd ones the bilinear average of two; each tap is weighted by
   `saturate(1 - |dz| / (0.05 z)) + 1e-3`.

The result is bound at t25 for the lighting pass and the debug views.

**Application** (`ps_sun`), indirect light only:

- `ao = min(material AO, GTAO)`.
- Diffuse: `E / π * gtao_multibounce(ao, albedo) * (1 - metal)`. The multi-bounce fit is the
  paper's cubic in AO and albedo: light albedos are darkened less, because interreflections inside
  the crease would have lit them. A uniform grey darkening would be wrong there.
- Specular: `L_spec * EnvBRDF(f0, roughness, NoV) * SO`, with Karis's analytic split-sum
  approximation and the specular occlusion of Lagarde & de Rousiers 2014 (section 4.10):
  `SO = saturate((NoV + ao)^(2^(-16 r - 1)) - 1 + ao)`.
- The sun and the local lights never see AO (TI; Frostbite's occlusion table agrees).

The radius is small on purpose. Large-scale static occlusion is already in the bake, so the grid is
never multiplied by another large-scale AO term. GTAO adds the near field and the occlusion caused
by dynamic objects.

## Debug views

- `VIEW_GI_IRRADIANCE` (`--view 14`, UI "GI: probe irradiance"): `indirect_light(...).irradiance / π`
  with roughness 1, i.e. the radiance of a white diffuse surface, without AO or albedo, through the
  debug tone curve. It shows the probes when baked and the sky ambient cube otherwise: leaks, dark
  invalid regions and the sky fallback are easy to spot here.
- `VIEW_AO` (`--view 15`, UI "GTAO"): the final GTAO visibility (white when AO is off).
- "Diffuse light" (`--view 9`) shows the diffuse lighting buffer, where the multi-bounce AO is
  applied to its indirect part.

## Normals of non-uniformly scaled instances

Commit `ccf5bf9` fixed a G-buffer bug: normals were transformed with the world matrix M. That is wrong under non-uniform scale; normals transform with
the inverse transpose. For M with columns a, b, c, `M^-T = [b x c, c x a, a x b] / det(M)` (the
cofactor matrix). The result is normalized afterwards, so only the sign of the determinant matters:
`transform_normal()` in `common.hlsli` is three cross products, with no per-vertex inverse. The
G-buffer and capture vertex shaders use it. Tangents lie in the surface and still transform with M.
The same commit fixed the test scene's "head", which reused an index buffer that `add_mesh()` had
already reordered in place; its scrambled triangles showed up as banding in GTAO.

## Measurements

From the final benchmark pass (2026-10-18; RTX 5060 with the GPU clock locked at 1485 MHz, about an RTX 3060's shader throughput; at stock clocks every
pass is 1.4-1.8x faster, [../perf-tables-stock.md](../perf-tables-stock.md); 1920x1080, 2x MSAA; mean
over the 8 courtyard viewpoints and the camera path; `--bench gi`, full table in
[../perf-tables.md](../perf-tables.md), method and noise floor in [../PERF.md](../PERF.md)).

| Bake (one-off) | Courtyard | Test scene |
|---|---|---|
| Grid | 35x13x35 = 15925 probes at 1.57 m | 46x6x25 = 6900 probes at 1.5 m (with the leak-room viewpoint) |
| Inside geometry | 4255 | 39 |
| Memory | 65.1 MB (+46.9 MB during the capture) | 37.2 MB (+20.2 MB) |
| Capture | 5.26-5.28 s: 44907 draws, 11.4 G triangles, 110 GPU waits | 104 ms, 766 draws |
| Relight | 10-11 ms: sun shadow map 3.4-4.0 ms, 3 bounces 6.5-6.8 ms | 6.3 ms |

| `--bench gi` configuration | frame (ms) | vs default | GTAO | Sun + ambient | GI relight |
|---|---|---|---|---|---|
| probes + GTAO full res (default) | 7.46 | | 0.65 | 0.80 | |
| probes + GTAO half res | 7.02 | -0.44 | 0.22 | 0.81 | |
| probes, no AO | 6.86 | -0.60 | | 0.81 | |
| sky ambient only (GI and AO off) | 6.34 | -1.12 | | 0.30 | |
| probes relit every frame + GTAO full res | 14.12 | +6.66 | 0.72 | 0.82 | 6.60 |
| probes without the visibility test, no AO | 6.64 | -0.82 | | 0.63 | |

Against TI's budget (1.0 ms world space + 1.0 ms screen space):

- With a static sun the world-space share is only the lookups: +0.50 ms inside "Sun + ambient"
  (0.80 vs 0.30), of which the visibility test against leaks is 0.18 ms. Within the 1.0 ms. The
  benchmarks' budget table gives "Sun + ambient" 0.54 ms (sun shading + shadow projection) and cannot
  split the scope, so with GI on it reports that pass as over.
- GTAO takes 0.65 ms at full resolution (two thirds of the screen-space share; TI's chart: SSAO
  0.40 ms + SSGI 0.55 ms), 0.22 ms at half resolution.
- GI and AO together cost 1.12 ms of frame time over the sky ambient alone (stock clocks: GTAO
  0.39 ms, probe lookups +0.30 ms, a per-frame relight 4.0 ms).
- The relight fits only because it runs on demand: every frame it is 6.6 ms, 6.6x the world-space
  share (Days Gone updates its probes from shadow maps in 0.3 ms).
- Toggling the visibility test re-lights the grid, and toggling GTAO's resolution re-creates its
  targets; either can shift the cost of the passes reading them by ~0.1-0.15 ms for the rest of a
  process (GPU memory placement), which is why the benchmark runs the default first and the
  visibility toggle last.

## Settings, UI, CLI

| Setting | Default | UI ("Global illumination + AO") | CLI |
|---|---|---|---|
| `gi` | on | Probe GI | `--gi 0\|1` |
| `gi_probe_spacing` | 1.5 m | Probe spacing (0.5-4 m, applied on Rebake) | `--gi-spacing M` |
| `gi_max_probes` | 16384 | Probe budget (1024-65536, applied on Rebake) | `--gi-max-probes N` (clamped to 8-262144) |
| `gi_bounces` | 3 | Bounces (1-4, re-lit at once) | |
| `gi_relight_on_sun_change` | on | Relight when the sun moves | |
| `gi_relight_every_frame` | off | Relight every frame (cost test) | `--gi-relight-every-frame` |
| `gi_normal_bias`, `gi_view_bias` | 0.25, 0.15 | Normal bias, View bias (0-1, x spacing) | |
| `gi_visibility` | on | Probe visibility test | `--gi-visibility 0\|1` |
| `ao` | GTAO full res | Ambient occlusion: Off / GTAO full res / GTAO half res | `--ao off\|full\|half` (anything else = full) |
| `ao_radius` | 0.75 m | AO radius (0.1-3 m) | |
| `ao_slices`, `ao_steps` | 2, 4 | AO slices (1-4), AO steps per side (2-8) | |
| `ao_denoise` | on | AO denoise (off shows the raw 4x4 Bayer pattern) | |

Also:

- The "Rebake GI" button re-captures the probes. The status lines show the last bake's capture and
  relight times, the grid, the inside-geometry count and the memory. The log has the same
  (`GI bake:` and `GI bake time:` lines).
- `--view 14` / `--view 15` select the debug views. `--camera px,py,pz,tx,ty,tz` places the camera
  for reproducible screenshots. `--bench gi` runs the benchmark above. `--test-scene` builds the
  procedural scene, which includes a GI leak-test room (see the known issues).
- Registers: GI and AO own `b4` (`GICB`), `t24` (grid), `t25` (GTAO) and `t26` (sensors), so they
  never collide with the per-pass slots of the shaders that include `gi.hlsli` (`t20-t23` are the
  sky's).

## Deviations from the research, and known issues

- **Sensors captured once, every bounce by relighting.** R5 6.2 (v1) re-renders 16x16 cube captures
  with a forward bake shader for each bounce. Here the scene is rasterized once into 8x8 sensors
  (R5 6.3's v2 resolution), and all bounces, including the first, are relights. A bake is one
  rasterization plus N cheap passes, and a sun change costs a relight instead of a re-bake.
- **On demand, not every frame.** TI and R5 6.3 want per-frame (or time-sliced) sensor updates, so
  time of day and moving lights just work. The biggest part of the relight is the whole-scene
  shadow map (5.6-6.0 ms on the courtyard: every static caster, foliage included; see
  [shadows.md](shadows.md)). Ways down, none done yet: reuse the camera cascades where they cover a
  sensor (R5 6.3), time-slice the probes, cluster sensors (TI's dominant direction plus average
  albedo, or R5's 4x4 faces), and update only probes near the view (TI's "visible probes" step).
- **One shadow tap per sensor, no fallback.** One 2x2 PCF tap, where TI samples several positions
  around the sensor. Sensors outside the GI shadow map's footprint (surfaces a probe sees beyond the
  grid) are treated as lit. TI's world-space ray-traced fallback is not implemented.
- **Sun, sky and static emissive only.** Local lights do not feed the probes, and emissive surfaces
  are captured once.
- **Invalid probes are dropped, not in-painted.** R5 6.2 dilates them from valid neighbours. Here the
  premultiplied validity removes them, and the lookup falls back to the sky ambient cube where less
  than 5% of the trilinear weight is valid. The 27% of courtyard probes inside geometry are still
  stored; a sparse grid would reclaim that memory.
- **Visibility from the capture, not from depth moments.** R5 kept DDGI-style visibility as an
  escalation; it is done here with the hit distances the capture already has. There is one exact
  distance per 11° texel (nearest texel, unfiltered), no Chebyshev test, no weight crush, and a 2%
  floor. The quarter-cell ramp softens only along the distance, so where a wall edge crosses a
  probe's texel the weight can step. Together with the manual interpolation, the lookup is 56 point
  loads rather than R5's 3-6 trilinear fetches.
- **One uniform grid.** There is no streaming, no cascades, no visible-probe selection and no
  propagation paths. Outside the grid, lookups clamp to the outer probes (R5 suggested the sky
  fallback).
- **The volume follows the scene's viewpoints and bench path only.** A `--camera` position elsewhere
  is not covered. That is why the test scene has a "leak room" viewpoint at its leak-test room
  (x 30-36 m): without it the cropped grid ended near x = 17 m and the room was lit by the clamped
  boundary probes.
- **Dynamic objects** are lit by the grid but do not occlude it or bounce light into it. GTAO is
  their only occlusion: there are no capsule or SDF indirect shadows (TI's "indirect SDF shadows").
  Transparents (glass, particles) use the sky ambient cube, not the probes.
- **Screen space: GTAO only.** There is no SSGI and no bent normals. The chart transcription
  (00, 2.12, step 10) reads the partly illegible note as advising against GTAO, while R5 reads it as
  "avoid CIAO (halos)". This implementation follows R5.
- **Mirrored instances** (negative determinant) flip the winding. Nothing in the engine compensates
  for that, and in the capture their outer faces would come out as back faces (black sensors,
  invalid probes).
- **Not cached:** every load bakes again (2.9-5.3 s on the courtyard). The capture time also varies
  between sessions.
