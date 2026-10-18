# Shadows: sun cascades and density-synced local light shadows

What the engine does for shadows, why, the math, and what it costs. The research behind it is
in [`docs/research/03-shadows.md`](../research/03-shadows.md) ("R3"). Section numbers like R3 2.4
point into that note; "chart 2.8" points into
[`docs/research/00-video-pipeline-chart.md`](../research/00-video-pipeline-chart.md).

Code:

| File | What |
|---|---|
| `src/r_shadows.cpp` | Sun cascades: splits, stable fitting, caster culling, amortized updates, the shadow pass (which also runs the local pass) |
| `src/r_local_shadows.cpp` | Local (spot/point) light shadows: fitting, density rule, hysteresis, distance fade, cache, atlas packing, caster lists, raster states |
| `src/r_geometry.cpp` | `select_lod()`, `finalize_shadow_list()`, `draw_shadow_list()` (the shadow draws, incl. the solid-card path) |
| `src/renderer_internal.h` | `ShadowConstants` (C++ side of `ShadowCB`), `shadow_solid_card()` |
| `shaders/shadow.hlsli` | `ShadowCB`, `ShadowProjector`, optimized PCF kernels, `local_light_shadow()` |
| `shaders/lighting.hlsl` | `sun_shadow()` in `ps_sun`, the `local_light_shadow()` call in `ps_light` |
| `shaders/depth.hlsl`, `shaders/geometry.hlsli` | Shadow vertex shaders (`SHADOW=1`; `FAR_CLIP=1` for the local views), alpha-test and LOD-dither pixel shaders; `geometry.hlsli` has the wind sway (`wind_offset()`) and includes `shadow.hlsli` for `ShadowCB` |
| `src/scene.cpp` | `wind_bend()`, `instance_bounds()`: how far each instance sways, and the bounds that include it (section 5) |
| `shaders/debug.hlsl` | `shadow_density_view()`: the `VIEW_SHADOW_DENSITY` heat map |

Conventions: reversed depth everywhere (near = 1, far = 0, depth test `GREATER`, maps cleared to
0, comparison sampler `GREATER_EQUAL` with clamp addressing), so depth bias is negative.

Not covered here: the GI probe relight renders its own whole-scene 2048² D32F sun shadow map
(`src/r_gi.cpp`), only when the sun's direction or intensity changes. It is independent of the cascades, and its cost
shows under the "GI relight" profiler scope, not "Shadows".

---

## 1. Sun: cascaded shadow maps

### 1.1 As implemented

- **4 cascades of 2048² in one 4-slice `Texture2DArray`** (UI: 1-4 cascades, 512²-4096²). D16 by
  default (`Settings::shadow_d16`), D32F as the A/B. Every slice is cleared and rendered in its own
  pass.
- **Splits:** the practical split scheme (Zhang et al., GPU Gems 3 ch. 10) with lambda = 0.75 from
  the camera's near plane (0.1 m) to `shadow_distance` (120 m): 7.96 / 17.6 / 37.8 / 120 m.
  Beyond 120 m the sun is unshadowed.
- **Stable fitting** (Valient, ShaderX6; MJP's Shadows sample):
  - each slice is enclosed by a sphere around the centroid of its 8 corners, with the radius
    rounded up to 1/16 m (rotation-invariant: no shimmer when the camera turns);
  - the sphere's center is snapped to whole texels in light space (no shimmer when it moves);
  - orthographic reversed-Z projection. Its depth range spans the scene's light-space bounds
    (plus 1 m on each side), so every caster within the scene bounds is inside it; depth clipping
    is also off ("pancaking").
- **Casters:** back faces culled (none for double-sided materials), `DepthBias = -2`,
  `SlopeScaledDepthBias = -2.0` (D16 and D32F alike), no pixel shader for opaque casters.
  `INST_NO_SHADOW` instances (glTF extras `"no_shadow": 1`, e.g. floors) and alpha-blended
  materials never cast. The LOD is the main camera's (same screen-size rule), one level coarser in
  cascades 2-3, with no dithered cross-fade (section 5).
- **Lookup:** interval-based cascade selection by view depth, no blend band between cascades. The
  receiver is pushed along its normal by 0.75 texel when it faces the sun, rising to 1.5 texels at
  grazing angles (`1.5 * (1 - NoL/2)`), plus 0.5 texel toward the sun ("normal offset", Holbert
  2011). Then PCF (1.4).

Density with the defaults (1920x1080, vertical FOV 1 rad = 57.3°), from the formula in R3 2.5:

| Cascade | Range (m) | Sphere radius (m) | Texel (mm) | Texels per 1080p pixel, far edge / near edge |
|---|---|---|---|---|
| 0 | 0.1-8.0 | 9.75 | 9.5 | 0.85 / ~0 |
| 1 | 8.0-17.6 | 20.25 | 19.8 | 0.90 / 0.41 |
| 2 | 17.6-37.8 | 43.31 | 42.3 | 0.90 / 0.42 |
| 3 | 37.8-120 | 139.94 | 136.7 | 0.89 / 0.28 |

As R3 2.5 derives, full-frustum cascades cannot meet the "1080p + 8%" rule everywhere: with 2048²
they reach about 0.9 texels per pixel at each slice's far edge, and less nearer the camera.
`VIEW_SHADOW_DENSITY` shows the cascades dimmed and red throughout (every value above is below 1).
This is open question Q1 of the research; the local lights below are where the rule is enforced.

### 1.2 Caster culling: the extruded camera slice (`cascade_slice_culling`, default on)

The plain test keeps every caster inside the cascade's box. That box is the bounding *sphere* of
the slice, extended over the whole scene along the light direction: far bigger than what can
shadow the slice. Selection is by view depth, so a cascade only needs the casters whose shadows can
land inside *its* slice. That volume is the slice swept toward the sun (Dybisz & Witanowski,
"Shadows Optimizations in Cyberpunk 2077", SIGGRAPH 2021). Its planes (`slice_caster_volume()`):

- the slice faces whose inward normal does not point along the light's travel direction (moving
  toward the sun never leaves them);
- one plane through each silhouette edge (between a kept and a dropped face), parallel to the
  light direction.

Casters are tested as bounding spheres against these planes (5 to 10 for a frustum slice). The
test is inflated by 4 texels, the PCF reach plus the normal offset, because a receiver at the slice
border samples a little outside it. The cheap light-space box test stays as a pre-filter, and
casters whose bounding diameter is under 1.5 texels are skipped.

### 1.3 Amortized far cascades (`cascade_amortize`, A/B, default off)

KCD2 (TI's tutorial, 44:40-47:00) renders cascades 1-2 every frame and one of 3/4/5 per frame. Our
version:

- cascades 0-1 render every frame; cascades 2+ one per frame in rotation. With 4 cascades, each
  far cascade updates every 2nd frame (with 3, cascade 2 still updates every frame);
- a far cascade that is re-rendered gets a 15% larger sphere, so it still covers the camera's
  slice a few frames later. Its texels are 15% larger too, even on the frames it renders. Lookups
  use the matrix and texel size stored when the map was rendered;
- **cut handling:** a cached map is reused only while its padded sphere still contains the current
  slice's sphere (`|c - c_cached| + r <= r_cached`) and the sun direction has not changed
  (`dot > 0.99999`, about 0.26°). Otherwise it is re-rendered in the same frame, so a stale map is
  never sampled outside what it covers. This is an exact test, not a "camera moved more than X m"
  heuristic. It replaces TI's proposed reduced-resolution cut frame;
- amortized cascades keep the box test instead of slice culling, because their map must also
  serve the next frames' slices;
- dynamic casters in the far cascades lag by up to one frame;
- a skipped cascade still opens its (empty) profiler scope, so the averages include the savings.

### 1.4 PCF (`shadow_filter`)

Castaño's optimized PCF (The Witness, 2013; MJP's sample): bilinear comparison taps, with offsets
and weights solved so they sum to the target kernel (R3 6.2).

- **4 taps: a 3x3 tent over a 4x4-texel footprint (default, `PCF_TENT_4TAP`).** It replaced the
  old 3x3 grid of 9 uniformly weighted bilinear taps, which covered the same footprint with more
  than twice the taps.
- 9 taps (`PCF_GAUSS_9TAP`): a 5x5 near-Gaussian over 6x6 texels, the A/B option.

TI asks for "barely softened" shadows: the small kernel just anti-aliases the edge at about 1
texel per pixel and needs the least bias. The same kernels serve the cascades and the local-light
atlas.

### 1.5 Foliage casters (`shadow_foliage_cards`, A/B, default off)

TI (around 46:10, paraphrased): most of Crysis 3's ~1.2 ms of cascade time came from its last
cascades, which drew static high-poly or alpha-tested content. Chart 2.8's workflow notes add that
foliage shadows from shadow maps can be expensive and hardly worth it. On the courtyard, the tree
crowns (alpha-tested `leaves` cards) dominate the cascade cost (section 3).

With the toggle on, alpha-tested casters whose bounds center is farther than
`shadow_foliage_distance` (20 m) from the camera are drawn as **solid cards**, in every shadow view
(cascades and local lights alike): depth-only, without the alpha test or a pixel shader (local
views keep the LOD-dither pixel shader during a cross-fade). This reuses the no-discard path of the
pre-passed base-pass draws. In each view the solid cards are drawn first: they are cheap, and they
occlude.

- The expected visual effect: distant crowns cast blob shadows (card-shaped) instead of dappled
  ones, and self-shadow a little more.
- Measurements: section 3.

---

## 2. Local lights: density-synced perspective shadow maps

### 2.1 What and why

TI, tutorial 39:00-41:20 (paraphrased):

- Games render huge amounts of shadow depth where the on-screen light can never evaluate it, and
  raise resolution (4-8x the surface area) to hide aliasing.
- Instead, render each shadowed light's map only over the region the visible light can affect.
  Scale its frustum and field of view from the player's perspective ("perspective shadow maps").
- Pick the resolution from a density rule instead of a fixed size: "1080p + 8%", i.e. shadow
  texels at least 8% denser than 1080p screen pixels on the visible receivers. Cap each map at
  2048², shrinking it as the light gets farther away.
- Budget (chart 2.8): 1.00 ms for all non-cascaded shadow maps. The local lights' shadows are looked
  up inline in the lighting draws, with minimal PCF and no projection mask pass (44:40).

Following R3 3.5, we do **not** warp in the camera's post-perspective space (PSM proper).
Instead, each light keeps its own fixed orientation, and only the *window* of its perspective
projection changes ("focused" shadow maps; LiSPSM's authors note that nearby local lights don't
need warping). That avoids PSM's singularities and keeps the bias standard.

### 2.2 Per frame, per projector (`local_shadows_setup()`)

A *projector* is a spot light, or one of the 6 cube faces (+X -X +Y -Y +Z -Z) of a point light,
for lights with `Light::shadow` (glTF node extras `"shadow": 1`). Its pyramid runs along the axis
from the minimum near plane (2.5) to the light's range, with a half-extent of 1 in tan space for a
cube face (90°) and `tan(outer cone)` for a spot (outer cone clamped to 1.3 rad).

1. **Receivers:** for each shadowed light whose range sphere touches the view frustum, the
   instances inside the camera frustum whose bounds come within the light's range. A light with no
   visible receivers gets no shadow work at all.
2. **Body B:** the parts of those receivers' bounds (padded 10 cm for the normal offset; the
   bounds of wind-swayed instances already include their sway, section 5) that lie inside the
   projector's pyramid. Large receivers (a bounding-box half-extent over a quarter of the light's range, i.e.
   wider than half of it: floors, walls) are also clipped exactly to the camera frustum. That is
   the cut that makes a fitted window small, the video's pond example.
   - The clipping is plain convex hull intersection: every face of each hull is clipped
     (Sutherland-Hodgman) by the planes of the others; R3 4.2.
   - B empty: the projector is skipped. For a point light, faces that see nothing visible (often
     the ones pointing away from the camera) are dropped.
3. **Window:** the bounding rectangle of B on the light's image plane, in tan space (x/z, y/z),
   clamped to the pyramid.
4. **Texel size from the density rule (2.3).** Quantized down to quarter-octave levels
   `delta = 2^(k/4) / 4096`, with hysteresis (2.4).
   A light whose whole view would get fewer than ~96 texels at the density it asks for fades to
   unshadowed and renders nothing (distance fade, 2.9).
5. **Snap** the window outward to the level's texel grid and pad it by 5 texels (the 9-tap
   PCF's reach of 3 + the normal offset). If the tile exceeds `local_shadow_max_res` (2048, and never more than the atlas),
   coarsen a level and repeat.
6. **Near/far plane** from B's depth range along the axis: near = B's minimum rounded down to a
   quarter octave, but at least `max(range/32, 5 cm)`; far = B's maximum rounded up to a quarter
   octave (so it can exceed the range by up to 19%), and at least 1.5x near (2.5).
7. **Pack** all tiles into one atlas: a shelf packer, tallest first. On overflow, every projector
   is coarsened one level (fixed mode: halved, down to 64²) and the packing is retried; the first
   overflow is logged.
8. **Caster list:** instances whose bounds intersect the fitted pyramid's 4 side planes and its
   far plane; the near plane is not tested, because of pancaking. That pyramid is exactly the
   volume that can cast into view (R3 4.6). Further rules:
   - `INST_NO_SHADOW` instances and alpha-blended materials never cast;
   - casters whose bounding diameter is under 1.5 texels at their light depth are skipped;
   - LOD and dithered LOD cross-fades are selected exactly as in the main view (`select_lod()`
     from the main camera, `lod_dither` honoured; TI 34:40: undithered shadow LODs pop).
   - casters lying entirely deeper than B's maximum depth are skipped. On the GPU, the rest are
     clipped there too (2.5), per vertex rather than per bounding box.
9. **Upload:** one 128-byte record per view into a structured buffer (`ShadowProjector`, t15):
   world-to-atlas matrix, light position, texel size `delta`, axis, cos(outer cone) for spots,
   range, a "tile valid" flag and the atlas slice. A point light always reserves 6 consecutive
   records; a skipped face is flagged as not valid. `LightCB.light_shadow` tells `ps_light` where a
   light's records start, how many there are (1 or 6) and the distance fade.

The atlas (`shadow_atlas_size`, default 8192² D16 = 128 MiB; D32F = 256 MiB with
`shadow_d16` off) is a one-slice texture array, cleared once per frame with one
`ClearDepthStencilView`, a metadata fast clear on current GPUs. There are no quad clears and no
copies (TI's Crysis 3 / Callisto Protocol / Dead Space Remake anti-pattern). Each tile is rendered
with its own viewport and scissor rectangle. The optional cache (2.10) adds a second slice for the
tiles it keeps.

### 2.3 The density rule, in numbers

A texel of angular size `delta` (tan units) spans `delta * zL` metres at depth `zL` along the
light's axis. A 1080p-reference pixel spans `p * zc` metres at camera depth `zc`, with
`p = 2 tan(fovy/2) / 1080`. The reference stays 1080 lines whatever the output resolution (TI's
definition). Requiring `D` texels per pixel (D = 1.08, `local_shadow_density`) at every point of
B gives:

```
delta <= (p / D) * min over B of (zc / zL)
```

`zc/zL` is linear-fractional, so its minimum over each convex piece of B lies at a vertex: the
minimum over the clipped vertices is exact, apart from the near-camera clamp below. The level is
then rounded down (finer), so the rule holds with margin.

- Camera depths under 1 m (`FOCUS_MIN`) are clamped to 1 m. Otherwise standing next to a light
  always demands the cap. Large receivers that reach that close are also clipped at 1 m and their
  clipped vertices enter the minimum (slightly conservative); for small receivers the clamp at the
  vertices is an approximation.
- The tile then has `window_width / delta` texels plus the padding, capped at 2048.
- A light farther away within the view has a larger `zc` everywhere, so its tile shrinks, as the
  video asks.

For example, a cube face seen from about 30 m whose receivers sit up to 8 m from the light needs
`delta ~ (1.012e-3 / 1.08) * 30/8 = 3.5e-3`, i.e. about 570 texels across a full 90° face (2 tan
units); rounded down to level k = 15 that is 609, plus the 10 pad texels. Seen from 3 m, the same
face asks for ten times as many (about 5700) and is capped at 2048.

### 2.4 Stability: snapping and hysteresis

- A projector's orientation never changes for a static light, and its texel boundaries lie on the
  fixed grid `i * delta` in tan space.
- As the camera moves, the window only slides by whole texels. Every texel keeps its exact
  position on the receivers, so shadow edges don't swim. This is the perspective analogue of
  cascade texel snapping.
- Only a change of level `k` re-grids the texels. A finer level is taken at once. A coarser one
  waits until it has been requested for 30 consecutive frames (`COARSEN_FRAMES`), so a camera
  hovering at a level boundary doesn't flip back and forth. The state is kept per light and cube
  face, and records the level actually used (after the cap and any overflow coarsening).
- Near/far are quantized to quarter octaves, so depth values also change only in steps.

### 2.5 Depth range, bias and the near plane

- **Pancaking:** depth clip is disabled, so casters between the light and the near plane are
  clamped to depth 1 and still occlude everything behind the near plane.
- **Caster clip** (`local_shadow_clip`, default `LOCAL_CLIP_RECEIVERS`). Disabling depth clip
  clamps *both* ends. Everything beyond the far plane is clamped to depth 0, the cleared value, so
  it can never pass the `GREATER` test. It was still rasterized, though, and the alpha-tested leaf
  cards were still shaded: with depth clamping, the hardware's early depth rejection did not catch
  these fragments. The local views' vertex shaders (`depth.hlsl` with `FAR_CLIP=1`) therefore
  output `SV_ClipDistance0 = clip_z - clip.w`, where `clip.w` is the depth along the view axis and
  `clip_z` comes from the pass constants. The rasterizer cuts off what lies deeper, while the near
  plane stays pancaked. The modes:
  - `LOCAL_CLIP_FAR_PLANE`: `clip_z` = the far plane, i.e. B's maximum depth rounded up to a
    quarter octave (up to 19% deeper). This was the first version (the shadow pass's
    `SV_ClipDistance0 = clip.z`, the same plane).
  - `LOCAL_CLIP_RECEIVERS` (default): `clip_z` = B's exact maximum depth. Every visible receiver
    point lies inside B, so a caster deeper than all of them can't shadow anything the camera
    sees. The projection, and with it the depth quantization, still uses the rounded far plane.
    The CPU caster list skips instances lying entirely deeper too. This needs bounds that hold
    every receiver: B is built from instance bounds plus `RECEIVER_PAD`, which wind-swayed
    receivers used to leave by metres (section 5).
  - With the cache on (2.10), views clip at the far plane: a kept tile is reused while the
    receivers, and with them B's depth, change.
  - The maps are identical: at fixed exposure, screenshots with and without the clip differ by
    no more than two runs of the same build do. For the receiver clip against the far-plane clip
    this was checked at all 8 viewpoints with the sun off (`--shadow sun=0 --exposure 4
    --time 20`), so that only the local lights shade: 0-0.18% of the pixels differ, the same as
    between two runs with the same setting.
  - Cost: clipping at the far plane took the local atlas pass from 1.24 to 0.87 ms (-30%) at the
    locked clock (3.2). Large tree crowns that straddle the far plane of a fitted view are the main
    beneficiaries. Clipping at the receivers instead takes another 8% off (0.84 -> 0.77 ms in one
    process; -11% in the two wide views, 3.2).
  - Clipping at the near plane as well (plain `DepthClipEnable`) would be cheaper still in some
    views. It is wrong, because it drops casters between the light and the near plane. It is
    also *slower* in others: those pancaked casters occlude a lot of what is drawn after them.
  - The sun cascades don't need this: their depth range spans the whole scene, so nothing lies
    beyond their far plane.
- **Near plane:** at least `max(range/32, 5 cm)`. Receivers closer than that project to depth > 1,
  which the `GREATER_EQUAL` comparison always reads as lit. The fit can therefore ignore them,
  which matters for a lamp whose own housing sits a few cm from the light: it would otherwise make
  every cube face full size.
  - D16 depth step at depth z: `(f - n) z² / (65536 f n)`. For a 10 m lamp, ~5 mm at 10 m with
    n = 0.31 m, against ~15 mm with n = 0.1 m (R3 4.4 gives the same 15 mm).
- **Raster bias:** reversed Z, so casters are pushed *away* from the light. D16: `DepthBias = -4`
  (units of 1/65536); D32F: 0. `SlopeScaledDepthBias = -1.5`. Back faces culled (none for
  double-sided materials), scissor on.
- **Receiver bias:** a normal offset of 1.5 texels times sin(theta) (0 when the surface faces the
  light), plus 0.5 texel toward the light. The texel's world size at the receiver is
  `delta * zL` (`local_light_shadow()`).

### 2.6 Lookup in the lighting draw

`ps_light` runs per shading model under the light's stencil volume, at pixel frequency, or at
sample frequency on MSAA edges.

- For a point light it picks the cube face by the major axis of `P - light`. Every visible lit
  pixel lies in a face whose B contained it, so that face was rendered; a face that was not
  rendered this frame reads as lit.
- It then projects the biased position with that projector's `world_to_atlas` matrix and runs the
  selected PCF kernel (1.4) on the atlas.
- The lookup is skipped where the light's attenuation is already 0.
- There is no per-light mask pass and no extra full-screen draw.

### 2.7 The baseline (`local_shadow_mode = LOCAL_SHADOWS_FIXED`)

The brute force the video criticises: the whole cone of each spot light, and **all six** faces of
each point light, at a fixed resolution (`local_shadow_fixed_res`, default 1024; UI 512-4096),
whenever the light's sphere touches the view. Casters are culled against the full pyramid. Same
atlas, bias, PCF and near-plane rule, so only the sizing and fitting differ.

### 2.8 Debug view: shadow texel density (`VIEW_SHADOW_DENSITY`, `--view 16`)

Per pixel: shadow texels per 1080p-reference pixel, from the same formula as the sizing rule:

- local lights: `p * zc / (delta * zL)`, over every rendered projector whose range and cone (spot)
  or face (point) contains the point;
- cascades: `p * zc / texel`, only where no local projector covers the point.

Where several shadowed lights cover a pixel, it shows the worst one. Local lights are drawn at full
brightness, the sun cascades dimmed (grey beyond the last split):

- red: < 1 (undersampled; darker the lower it is)
- yellow: 1-1.08
- green: 1.08-2 (the rule is met)
- blue to purple: > 2 (texels wasted; full purple at 8)

The view is analytic (no screen-space derivatives), so it is clean at depth discontinuities. It
ignores the projection term `cos(beta)/cos(alpha)` (R3 2.3), which no choice of resolution can fix.

What to expect, from the formula:

- Fitted maps are green where their minimum `zc/zL` sits (typically the far edge of each lit
  region) and blue/purple near the light. The density of an unwarped perspective map falls off as
  1/zL: meeting the rule at the edge oversamples next to the light. That is the price of not
  warping.
- Red only where a projector hit the 2048 cap or was coarsened by an atlas overflow (close to the
  camera, inside a point light's range), and within 1 m of the camera. Adjacent cube faces can
  show a step in density at their seam, since each face has its own level.
- The fixed baseline at 1024² gives a cube face about `0.51 * zc/zL`: red wherever a receiver's
  camera depth is less than about twice its depth along the face's axis.

### 2.9 Distance fade (`local_shadow_fade_px`, default 96)

R3 4.9, phase 1. If a light's tile would drop below ~96 texels, the light is too small on screen
for a shadow map. Its shadow fades to unshadowed instead.

- **The size that counts** is the light's *whole* view at the density it asks for:
  `2 tan_limit / delta_req`, with the finest unquantized `delta_req` of its faces. That is all of a
  cube face (2 tan units) or the spot's full cone. The fitted tile itself would be the wrong
  measure: a close light with only a sliver in view gets a small tile, yet its shadow is fully
  visible in that sliver.
- **The fade:** the shadow fades out between 4/3 x 96 = 128 and 96 texels (R3's band) and the
  light renders nothing below.
  - The fade comes from the continuous `delta_req`, not from the quantized level, so it changes
    smoothly as the camera moves.
  - `LightCB.light_shadow.z` carries it to `local_light_shadow()`, which returns
    `lerp(1, pcf, fade)`.
  - All faces of a point light fade together, so no seams appear between faces.
- **In the courtyard it never triggers.** The smallest whole-view size at any benchmark viewpoint
  is ~385 texels (a spot light in `square_overview`), four times the threshold. A 10 m lamp falls
  under 128 texels only when its receivers are more than ~17 times farther from the camera than
  from the light: about 170 m away. The courtyard is ~45 m across.
  - It costs nothing when inactive. It matters for large scenes with many distant lights, where
    it also saves the per-view CPU setup (~75 µs per view).
- Phase 2 (fading to a per-light screen-space shadow instead) is not implemented.

### 2.10 Caching static views (`local_shadow_cache`, A/B, default off)

Lights are static, and so are most casters. A view whose window, level and depth range haven't
changed, and whose casters haven't moved, could reuse last frame's map. TI warns against
*copying* cached maps (R3 4.8), so the cache samples its tiles in place.

- **The atlas gets a second slice.**
  - Slice 0 holds the views rendered for this frame only and is cleared every frame, as without
    the cache.
  - Slice 1 holds the kept tiles. That costs another 128 MiB in D16, allocated only while the
    cache is on.
- **Reuse.** A view samples its slice-1 tile while that tile covers the fitted view and its
  casters are unchanged. Covering means the same light, axis and texel size (texels sit on the
  grid `i * delta`, so a larger window holds the very same texels), a window containing the
  fitted one, and a depth range containing its receivers. With the cache on, views clip their
  casters at the far plane rather than at B's maximum depth (2.5), so that a kept tile still
  holds every caster while the receivers change. Unchanged casters means an identical
  hash of the caster list (instance, primitive, LOD, solid-card flag), with no moving caster.
- **What moves:** wind-swayed instances (with wind on), animated instances (the cart, the
  windmill sails, the inn sign), and casters in a dithered LOD cross-fade (the dither pattern
  changes every frame). Views with any of these render every frame into slice 0.
  - Optionally, `local_shadow_dynamic_interval = N` re-renders them only every Nth frame,
    staggered across views. They then live in slice 1, and their shadows lag the motion by up to
    N - 1 frames.
- **Slack.** A static view renders a window 12.5% larger on each side (within the pyramid and the
  size cap) and a depth range a quarter octave larger at each end. Small camera moves then keep
  its tile.
- **Clearing without quad clears.** A depth view can only be cleared whole. So a re-rendered
  tile goes into slice-1 rows that are still clear since that slice was last cleared.
  - When those rows run out, slice 1 starts over: one clear, and every kept view renders again.
  - The first version had a single slice and used the same scheme. There, the views that render
    every frame used up the clean rows every few frames, forcing a full clear and re-render each
    time. The second slice is what makes the cache work at all.
- **Results** (3.2): no gain in the courtyard, because the cost sits in views with swaying
  foliage.
  - With wind on, practically every view that has casters has a wind-swayed one: trees, bushes,
    flower boxes, ivy. Drawing only the static casters costs 0.09-0.12 ms per viewpoint (stock
    clock), the most a perfect static cache could save. The cost is the swaying foliage: without
    it the local atlas pass drops from 1.38 to 0.11 ms in the overview (stock, before the far clip).
  - Splitting static and moving casters into two layers would not help either. Drawing the
    moving casters without the static ones in front is *slower* (1.67 vs 1.38 ms in the overview
    at stock clocks): the static casters occlude foliage fill.
  - The alternative of copying the cached static depth into this frame's tile before drawing the
    moving casters on top is what TI warns against. D3D11 cannot even copy part of a depth
    texture (`CopySubresourceRegion` copies depth-stencil resources whole), so it would take a
    shader copy of every texel.
  - With wind off, the cache saves 16% (stock). At a still camera it keeps 13 of 23 overview
    views; the rest have LOD cross-fades or the animated props.
  - Those numbers predate the receiver clip (2.5). A kept tile must clip at its far plane, so
    against today's default the cache saves 6% with wind off and loses 8% with wind on (3.2).

---

## 3. Measurements

### 3.1 Setup and method

RTX 5060 8 GB, 1920x1080, 2x MSAA, SMAA 4x, courtyard scene (`assets/scene/courtyard.gltf`: 446
instances, 16 lights: the sun, 11 point and 4 spot lights). 8 local lights cast shadows: 4 point
lamps (range 10 m) and the 4 spots (range 16 m), so at most 4 x 6 + 4 = 28 local views.

Profiler scopes: `Shadows` contains `Cascade 0` to `Cascade 3` and `Local lights`, which is the
atlas pass. Don't confuse it with the top-level `Local lights` lighting pass, which holds the
inline lookups.

Tools:

- `--bench shadows` and `--bench localshadows` (`src/bench.cpp`): every configuration at every
  viewpoint (the 8 courtyard viewpoints plus the camera path), 90 warm-up + 300 measured frames at
  fixed 1/60 s steps, vsync off. Mean per scope, written to `bench/<stamp>_<name>.{md,csv}`; the
  `.md` also compares the first row with TI's budgets.
- `--profile-dump out.csv`: per-scope exponential moving average (5% per frame) at exit; with
  `--stats` it also fills in the last frame's pixel-shader invocations per scope.
- The exit line in `build/engine.log`: `Shadow stats (last frame): cascade draws, local views,
  local texels, local draws, tris (cascades + local), local setup ms CPU`. The UI shows the same
  local numbers live.

### 3.2 Results

From the final benchmark pass (2026-10-18; RTX 5060 with the GPU clock locked at 1485 MHz, about an RTX 3060's shader throughput; at stock clocks every
pass is 1.4-1.8x faster, [../perf-tables-stock.md](../perf-tables-stock.md); mean over the 8 viewpoints
and the camera path; full tables in [../perf-tables.md](../perf-tables.md), method and noise floor
in [../PERF.md](../PERF.md)). Each configuration changes only what its label says.

| Configuration | Shadows total | Cascades | Local atlas pass | Cascade PS invocations | Frame vs default |
|---|---|---|---|---|---|
| 4x2048 D16, slice culling, fitted local maps (default) | 2.40 | 1.13 | 1.27 | 81.6 M (all shadow passes) | |
| shadows off | | | | | -2.38 |
| 3x2048 | 2.16 | | | 74.3 M | -0.25 |
| 4x1024 | 1.85 | | | 63.9 M | -0.54 |
| 4x4096 | 4.01 | | | 164.4 M | +1.56 |
| D32F (cascades and atlas) | 3.34 | 1.64 | 1.70 | 125.9 M | +0.91 |
| 9-tap PCF | 2.40 | | | | +0.00 (lookup +0.02 in Sun + ambient) |
| no slice culling | 3.00 | 1.73 | | 94.5 M | +0.62 |
| amortized far cascades (KCD2) | 2.31 | 1.04 | | 79.8 M | -0.09 |
| distant foliage as cards | 2.47 | 1.02 | 1.45 | 40.3 M | +0.09 |
| local shadows off | | | | | -1.22 |
| local: fixed 1024 per face | | | 0.92 (PS 20.1 M) | | -0.29 |
| local: fitted at half density | | | 0.89 (PS 35.4 M) | | -0.39 |
| local: all foliage as solid cards (cascades too) | | 0.98 | 1.26 (PS 0.3 M) | | -0.16 |

The fitted local maps' PS invocations are 55.1 M by default. The local atlas pass per view: foliage
dense 2.84 ms, square overview 2.56, skyline 2.20, camera path 1.46, lamps 0.86, cafe 0.66, market
0.54, busts 0.22, arcade 0.08.

**After the far-plane clip, the distance fade and the cache** (evening of 2026-10-18, same locked
clock; the GPU read 1520-1540 MHz under load this time). `--bench shadows` and
`--bench localshadows` were rerun with the new build; files in `bench/shadow-pass-2026-10-18/`.

| Configuration | Shadows total | Cascades | Local atlas pass | Frame vs default |
|---|---|---|---|---|
| default (now with far clip) | 1.96 | 1.09 | 0.86 | |
| 4x1024 | 1.42 | 0.56 | | -0.50 |
| D32F (cascades and atlas) | 2.69 | 1.58 | 1.11 | +0.72 |
| no slice culling | 2.54 | 1.68 | | +0.63 |
| amortized far cascades (KCD2) | 1.87 | 1.01 | | -0.03 |
| distant foliage as cards (20 m) | 1.73 | 0.98 | 0.74 | **-0.21** (was +0.09) |
| local: no far clip (the old default) | 2.33 | | 1.23 | +0.28 |
| local: fixed 1024 per face | | | 0.60 | -0.25 |
| local: fitted at half density | | | 0.62 | -0.30 |
| local: all foliage as solid cards (cascades too) | 1.50 | | 0.55 | -0.45 |
| local: cache | | | 0.86 | -0.06 (noise) |
| local: cache, moving casters every 2nd frame | | | 0.70 | -0.21 |
| local: wind off | | | 0.85 | -0.13 |
| local: wind off, cache | | | 0.72 | -0.29 |

Local atlas pass per view, without -> with the far clip (same run): foliage dense 2.75 -> 1.84 ms, square
overview 2.48 -> 1.69, skyline 2.18 -> 1.56, camera path 1.41 -> 0.90, lamps 0.84 -> 0.49, cafe
0.63 -> 0.62, market 0.52 -> 0.46, busts 0.21 -> 0.14, arcade 0.08 -> 0.08. At stock clocks the same
A/B gives 0.72 -> 0.51 ms on average (the stock `localshadows` run in the same folder).

End to end, `--bench smoke` in the order old build (62a8c03), new, old, new, gave shadows
2.34 -> 1.97 ms and a GPU frame (mean of the view medians) of 7.30 -> 7.00 ms. The two runs of each
build agreed to within 0.01-0.04 ms per pass. The cascades (1.10) and the local-light lookups
(0.74) did not change; the lookup now reads a texture array, which costs the same.

**After bounding the wind sway, with the receiver clip** (late evening of 2026-10-18, locked
clock, 1507-1537 MHz under load; files in `bench/wind-pass-2026-10-18/`). `--bench localshadows`,
all rows from one process:

| Configuration | Shadows total | Cascades | Local atlas pass | Frame vs default |
|---|---|---|---|---|
| default (clip at the receivers) | 1.87 | 1.10 | 0.77 | |
| local: clip at the far plane (the previous default) | 1.93 | 1.09 | 0.84 | +0.01 |
| local: no clip | 2.31 | | 1.21 | +0.39 |
| local: fixed 1024 per face | | | 0.58 | -0.14 |
| local: fitted at half density | | | 0.56 | -0.20 |
| local: all foliage as solid cards (cascades too) | 1.41 | 0.94 | 0.48 | -0.40 |
| local: cache (clips at the far plane) | | | 0.83 | +0.01 |
| local: cache, moving casters every 2nd frame | | | 0.67 | -0.14 |
| local: wind off | | | 0.76 | -0.07 |
| local: wind off, cache | | | 0.72 | -0.12 |

Local atlas pass per view, far plane -> receiver clip (same run): foliage dense 1.76 -> 1.56 ms,
square overview 1.64 -> 1.46, skyline 1.64 -> 1.55, camera path 0.88 -> 0.80, lamps 0.48 -> 0.44,
cafe 0.45 -> 0.43, market 0.45 -> 0.44, busts 0.13 -> 0.13, arcade 0.08 -> 0.08. The frame column
moves less than the atlas pass here: the other passes vary by a few hundredths between rows.

End to end, `--bench smoke` in the order old build (471567f: unbounded sway, far-plane clip), new,
old, new: local maps 0.88 / 0.85 -> 0.77 / 0.77 ms (-11%), shadows 2.00 / 1.93 -> 1.88 / 1.87 ms,
cascades and lookups unchanged (1.10, 0.72-0.75), GPU frame 7.07 / 6.81 -> 6.82 / 6.74 ms. Most of
the gain is the receiver clip; the bounded sway on its own is within the noise between processes,
except in the cafe view (0.61 -> 0.45 ms; not investigated, likely the 6.5 m tree 4 m from the
shadowed lamp there, whose crown swung up to 1.5 m).

### 3.3 Against TI's budgets, and what drives the cost

The `Shadows` scope's budget is 1.60 ms: sun cascades 0.60 ms (chart 2.10) plus local shadow maps
1.00 ms (chart 2.8). With the receiver clip, the local maps take 0.77 ms on average (0.86 with
the far-plane clip), within their budget; the three wide views still need 1.46-1.56 ms. The
cascades take 1.10 ms (1.8x), and they are now the larger overrun. Together the shadows are 28%
of the mean frame (32% before the far-plane clip).

- **At stock clocks** (thermally limited ~2.5 GHz) the main pass measured 1.31 ms (cascades 0.61,
  local maps 0.70), with the foliage view at 2.68 ms. The far clip brings the local maps to 0.51 ms
  on average (stock A/B).
- **D16 and slice culling are clear wins**: D32F would add 0.7 ms; dropping the extruded-slice
  caster test 0.6 ms. Amortizing the far cascades saves only 0.03-0.09 ms here.
- **The cost is swaying foliage, and much of it was wasted work.**
  - Per-projector timers in the foliage view (stock clock): the lamp face looking into a tree
    canopy took 0.50 ms for 39 M pixel-shader invocations in 2.0 M texels (~20 leaf layers); three
    more faces took 0.21-0.25 ms, the other seven 0.01-0.09 ms.
  - Without the wind-swayed casters (trees, bushes, flower boxes, ivy), the atlas pass drops to
    0.05-0.11 ms in every view (stock): foliage is 77-94% of the cost wherever there is any, over
    90% in the three wide views.
  - Before the far clip, all foliage as solid cards removed nearly all pixel work (55 M -> 0.3 M
    invocations) yet saved no time (1.26 vs 1.27 ms). The conclusion then was "depth fill, not
    the alpha test".
  - Part of that fill was geometry beyond the far plane: clamped to depth 0 instead of clipped,
    then rasterized (and alpha-tested) for nothing (2.5). Clipping it cut the pass by 30% and the
    pixel-shader work by ~20%.
  - With that waste gone, solid cards save 36% (0.55 vs 0.86 ms) and distant foliage as cards
    (20 m) now wins overall (-0.21 ms frame, cascades -0.11, local maps -0.12). The alpha test, and
    the late depth writes its discard forces, are a real share of the remaining cost.
  - Both card options change the look (blob shadows), so they stay A/Bs.
- **Caster order matters.** Drawing the local casters back to front instead of front to back costs
  +45% in the overview (stock), with 31% more pixel-shader invocations. Early depth rejection does
  work for the alpha-tested casters, but only against depth that earlier draws already wrote.
- **Alpha-test sampling**: shadow passes sample the alpha with plain trilinear instead of the 16x
  anisotropic sampler (`ps_shadow_alpha`), which cut the local maps by 13-17% in the wide views.
- **Opacity-only BC4: not worth it here.** A diagnostic build took the alpha from a coarse mip
  (almost no texture traffic). That saved 0.05 ms on the local maps and 0.14 ms on the cascades;
  solid cards saved 0.32 + 0.15 ms in the same run. BC4 halves the bytes of the BC7 albedo, so it
  would save less than that bound, ~0.1 ms at best. The remaining alpha-test cost is the discard
  and the late depth writes it forces.
- **Fitted vs fixed**: a fixed 1024 per face is cheaper on average (0.60 vs 0.86 ms) but
  undersamples receivers near the camera; a fixed resolution with the fitted maps' worst-case
  density (2048 per face) does not fit the 8192² atlas. Halving the density gives 0.62 ms.
- **Caching static views** (2.10) gains nothing: 0.86 ms with and without. Every view that matters
  contains swaying foliage. Re-rendering moving casters only every 2nd frame gives 0.70 ms (-19%)
  at the price of a one-frame lag in their shadows. With wind off the cache saves 16% (0.85 -> 0.72).
  That is consistent with TI's position that re-rendering beats caching with copies on current
  hardware. Here even copy-free caching has little to work with.
  - Since the receiver clip became the default, turning the cache on also means clipping at the
    far plane (2.5): 0.83 ms against 0.77 without the cache, and 0.72 against 0.76 with wind off
    (-6%; 3.2, `bench/wind-pass-2026-10-18/`).
- **Distance fade** (2.9): never triggers in the courtyard; the smallest whole-view size at any
  viewpoint is ~385 texels.
- **CPU**: the local setup costs 70-80 µs per view (0.39 ms for 5 views, 1.5-1.8 ms for 23 at the
  overview). Each projector's caster list loops over every instance, so it scales with
  views x instances. The cache doesn't reduce it: a kept view still builds its caster list to
  check it.
- Not tried: coarser foliage LODs or proxies in shadow maps (a visible trade-off against TI's
  main-view LOD rule; the tree crowns have no LODs, since simplifying disconnected leaf cards
  barely removes any).

---

## 4. Settings, UI, CLI

| Setting | Default | UI | `--shadow` key |
|---|---|---|---|
| `shadows` | true | Shadows (gates cascades and local shadows) | |
| `cascade_count` | 4 | Cascades (1-4) | |
| `cascade_size` | 2048 | Cascade size (512-4096) | |
| `shadow_distance` | 120 m | Shadow distance (20-400 m) | |
| `shadow_d16` | true | D16 shadow depth (cascades and atlas) | `depth=16\|32` |
| `shadow_filter` | `PCF_TENT_4TAP` | PCF: "3x3 tent (4 taps)" / "5x5 (9 taps)" | `pcf=4\|9` |
| `cascade_slice_culling` | true | Slice caster culling | `slice_cull=0\|1` |
| `cascade_amortize` | false | Amortize far cascades | `amortize=0\|1` |
| `shadow_foliage_cards`, `shadow_foliage_distance` | false, 20 m | Distant foliage as solid cards; Foliage card distance (0-100 m) | `foliage_cards=0\|1`, `foliage_distance=m` |
| `local_shadows` | true | Local light shadows | `local=off` (`local=fitted\|fixed` turn it on) |
| `local_shadow_mode` | `LOCAL_SHADOWS_FITTED` | Local sizing | `local=fitted\|fixed` |
| `local_shadow_density` | 1.08 | Density target (0.25-3, fitted only) | `density=F` |
| `local_shadow_max_res` | 2048 | Max view size (512-4096, fitted only) | `max_res=N` |
| `local_shadow_fixed_res` | 1024 | Fixed size (512-4096, fixed only) | `fixed_res=N` |
| `shadow_atlas_size` | 8192 | Atlas size (4096 or 8192) | `atlas=N` |
| `local_shadow_clip` | `LOCAL_CLIP_RECEIVERS` | Clip casters: Off / At the far plane / Beyond the receivers | `far_clip=0\|1\|2` |
| `local_shadow_fade_px` | 96 | Fade lights below (0-512 px; 0 = never) | `fade_px=F` |
| `local_shadow_cache` | false | Cache static views | `cache=0\|1` |
| `local_shadow_dynamic_interval` | 1 | Moving casters every (1-4 frames; with the cache) | `dynamic_interval=N` |

`--shadow` takes one `key=value` per flag and can be repeated. `depth=` is D16 unless the value is
32, `pcf=` is the 4-tap kernel unless it is 9. CLI values are not range-checked (section 5).

Other flags:

- `--shadow sun=F` multiplies the sun's top-of-atmosphere illuminance (`Settings::sun_intensity`,
  default 1); `sun=0` shows the local lights alone.
- `--view 16` opens the density view; `--viewpoint NAME` starts at a named viewpoint.
- `--camera px,py,pz,tx,ty,tz` places the camera.
- `--exposure EV` fixes the exposure (auto exposure off); with `--no-awb`, `--no-animate` and
  `--screenshot`, runs give comparable images (the A/B image checks in 2.5 and 2.10).
- `--no-wind` turns the wind sway off (the cache's wind-off measurements); `--time T` freezes the
  scene at time T, which fixes the wind phase for screenshots (section 5).
- `--stats` turns on pipeline statistics; `--profile-dump out.csv` and `--bench NAME` as in 3.1.
- The UI's stats line shows local views (and how many came from the cache), rendered atlas
  texels, draws and the setup's CPU time.

---

## 5. Deviations from the research, and known issues

- **Receivers instead of a depth min/max read-back.** R3 4.2 intersects the camera frustum with
  the light pyramid and optionally a GPU depth range (SDSM-like). We clip per receiver instance
  instead: large receivers exactly, small ones by their bounds inside the pyramid. A single box
  around all receivers is useless for point lights: it contains the light, and every face becomes
  full size.
- **Atlas default 8192² D16, as R3 4.8 recommends.** With the courtyard's 8 shadowed lights, a
  fixed 2048² baseline would need up to 28 views = 117 M texels, more than the atlas's 67 M; the
  overflow rule would halve every tile to 1024². The benchmark's baseline is therefore fixed 1024
  (29 M texels, fits). A fixed 2048 run needs `--shadow atlas=16384` (512 MiB in D16). Overflow
  coarsens *every* projector one level; R3 4.8 suggests coarsening the least important ones first.
- **Distance fade to unshadowed only** (2.9, R3 4.9 phase 1). Phase 2, fading to Bend-style
  screen-space shadows, is not done.
- **No receiver-plane depth bias, no opacity-only (BC4) texture** for alpha-tested casters. The
  shadow passes sample the albedo texture's alpha (BC7) with plain trilinear filtering. The
  measured upper bound for BC4 is ~0.1 ms per frame (3.3).
- **Wind sway: bounded, and inside the instance bounds (fixed).** `wind_offset()`
  (`geometry.hlsli`) bends an instance along a fixed direction by `sway(t) x bend x h²`, with h the
  object-space height and |sway| <= 0.85, plus a leaf flutter of at most 0.0057 h.
  - **The problem.** The bend was a fixed 0.04, which suits plants of a metre or two. On the
    courtyard's tall meshes it moved the top by up to 4.4 m (the 11.4 m linden trees), 2.6 m (the
    8.8 m narrow trees), 1.5 m (the 6.5 m small trees) and 1.5-2.0 m (the 6.7-7.7 m wall ivy):
    whole crowns leaned and sheared like jelly
    ([../images/wind_sway_before_after.jpg](../images/wind_sway_before_after.jpg), top row). The
    instance bounds didn't include the sway, and the local fit padded receivers by only
    `RECEIVER_PAD` = 10 cm. So a swaying receiver could leave its fitted window and read a
    neighbouring tile, or move beyond the far plane and read as shadowed. A swaying caster could
    leave the caster volume, and main-view culling could drop a visible treetop.
  - **The fix.** Each wind-swayed instance gets its own bend (`wind_bend()` in `scene.cpp`, passed
    in `DrawData::wind_bend`): 0.04, or less for meshes taller than 2.5 m, so that their top
    sways at most 0.25 m (peak 0.85 x 0.25 = 21 cm, plus up to 6.5 cm of flutter on the
    lindens). Grass, flowers and bushes (up to 2 m) keep their old motion, 1-14 cm at the top.
    `instance_bounds()` grows the bounds of wind-swayed instances by the farthest the sway can
    move a vertex, `0.85 x bend x H² + 0.0057 H` for a mesh of height H: 28 cm for the lindens,
    about 1 cm for grass. Culling (main view, cascades, local views) and the local fit are now
    conservative; `RECEIVER_PAD` only covers the normal offset. `Instance::radius`, which selects
    LODs, stays that of the mesh at rest: the padding would have grown it by 5-8% for trees,
    bushes and ivy and moved their LOD switches for no reason.
  - The bounds made the receiver-depth clip of 2.5 safe, and it is now the default.
  - Left: the wall ivy still sways up to ~20 cm at its top, partly into and off its wall (the
    wind direction is fixed). A wall plant would want a smaller bend or flutter only. The glTF
    `wind` extra is a flag, not a per-instance strength.
- **Sun: no far-field 2048² cascade, no screen-space shadow mask, no Bend screen-space shadows,
  no blend band between cascades.** The sun keeps 4 inline-PCF cascades of 2048² instead of the
  research's 3 x 1296² plus a far field. The split distances are the engine's existing PSSM
  (lambda 0.75 over 120 m), not the research's UE exponent-3 scheme. Without a blend band, the PCF
  footprint and normal offset change size abruptly at each split (R3 5.6).
- **Cascade caster LODs are not dithered**, and cascades 2-3 use one LOD coarser than the main
  view. A cascade shadow can therefore pop at an LOD switch and differ slightly from the visible
  mesh, which TI warns about (34:40). The local views follow the main view exactly.
- **Foliage is the dominant shadow cost on the courtyard**, in the cascades and the local maps
  alike (3.3). With the far clip, drawing it as solid cards now saves 40% of the local maps, a
  visible trade-off (blob shadows). The real fix is proxy geometry or an impostor/sprite atlas
  for crowns (Days Gone, R3 5.8), not done.
- **Lights inside fixtures:** the courtyard's lamp lights sit inside their lanterns. The wall
  above a lantern is lit although the lantern has a cap. The cap is probably single-sided and
  back-face culled as seen from the light (shadow passes cull back faces); not verified. Either
  fixtures need back faces, or a light should be excluded from its own fixture.
- **Wide spots:** the projector pyramid stops at an outer cone of 1.3 rad, the light volume at
  1.5 rad. A spot wider than 1.3 rad would light pixels outside its pyramid, whose lookups land
  outside its tile (in a neighbouring tile). The courtyard's spots are 0.30-0.61 rad.
- **Moving lights** re-grid every frame (inherent; none in the scenes).
- **CLI limits.** `--shadow max_res` and `fixed_res` are clamped to at least 64 and `atlas` to
  1024-16384. Below those, the cap loop in `fit_projector()` or the packing retry could loop forever
  (tiles only shrink to their padding) and a tiny `fixed_res` gave a degenerate texel size.
- **Fixed in passing:** the procedural test scene built its "head" mesh from index data that
  `add_primitive()` had already remapped, so its triangles were scrambled. That showed up as
  self-shadow stripes. It now rebuilds the sphere before the second mesh.
