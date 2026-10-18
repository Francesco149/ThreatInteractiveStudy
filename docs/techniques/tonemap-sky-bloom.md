# Sky, tone mapping, exposure, white balance and bloom

Video chapters: 57:21 "Direct Lighting Compositing & Effects Rendering" (sky and clouds at 1:01:10),
1:04:45 "Tone Mapping & Exposure Mechanics", 1:57:29 "Motion Effects, Composite Effects, & UI/Gamma"
(bloom at 1:57:40). Research:
[../research/06-tonemap-exposure-bloom.md](../research/06-tonemap-exposure-bloom.md) (curves, LUT,
exposure, white balance, bloom),
[../research/05-sss-gi-reflections-sky.md](../research/05-sss-gi-reflections-sky.md) sections 5 (fog)
and 9 (sky), [../research/00-video-pipeline-chart.md](../research/00-video-pipeline-chart.md) sections
2.16, 2.20 and 2.25 (budgets). Code: `src/r_sky.cpp`, `shaders/atmosphere.hlsl`,
`shaders/atmosphere.hlsli`, `shaders/sky.hlsl`, `src/r_tonemap_lut.cpp`, `shaders/tonemap_lut.hlsl`,
`shaders/tonemap_ops.hlsli`, `shaders/tonemap.hlsl`, `src/r_post.cpp` (`pass_exposure`,
`pass_tonemap`), `shaders/exposure.hlsl`, `src/r_bloom.cpp`, `shaders/bloom.hlsl`. Fog is in
`shaders/compose.hlsl` ([lighting.md](lighting.md)); the MSAA separation in the tone-map pass is
covered in [antialiasing.md](antialiasing.md).

TI's requirements. **Sky** (chart: 0.18 ms, clouds 2.90 ms on top): drawn after direct lighting,
overwriting the sky samples, which the stencil identifies; replicate GT7's photographically
validated sky. **Tone mapping** (chart: 0.25 ms target, 0.93 ms for The Callisto Protocol's stage
without bloom): linear mid-tones, a contrast toe comparable to Uchimura's, hue preservation (bright
orange should go to yellow, then white); GT7's curve, which clips sooner but is, TI argues, closer to
naked-eye perception; correct exposure above all. The curve goes through a small 3D LUT updated in a
first draw (Callisto: 64^3 RGBA16; Days Gone: 32^3 RGB10A2), with GT7's "power-of-4 nonlinearity
anti-banding" in any case, and one merged draw that tone maps each MSAA sample into its own 10-bit
image. **Auto exposure and auto white balance** from the diffuse-only RGBA16 lighting buffer (Fox
Engine), excluding emissives and SSS, with a min/max white-balance range and an artist temperature
bias. **Bloom** (chart: 0.38 ms): Jimenez's Call of Duty: Advanced Warfare chain, placed after motion
blur so it reacts to the blurred image and shares its mips with stochastic SSR; TI calls this part
work in progress.

## Where the passes run

1. **Sky LUTs** (frame setup, `pass_sky_luts`, before `gi_update`): bakes when needed, then binds the
   sky for every later pixel and compute shader of the frame.
2. **Sun + ambient** (fallback ambient), **Compose** (height fog), **Sky** (sky samples) and
   **Transparents + VFX** read it.
3. **Exposure**: histogram + white balance from the diffuse lighting buffer, one 3x3 camera matrix.
4. **Bloom**: from the HDR scene colour (after transparents), before exposure.
5. **Tone LUT bake**: only when the curve changes.
6. **Tone map**: bloom lerp, camera matrix, LUT fetch, per MSAA sample.

## Sky and atmosphere (Hillaire 2020)

Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020), ported
from the MIT reference code (github.com/sebh/UnrealEngineSkyAtmosphere) with Y up. Earth constants
from the paper's table 1 (Bruneton's 2017 values), in km and 1/km: ground radius 6360, top 6460;
Rayleigh scattering (5.802, 13.558, 33.1)e-3, scale height 8 km, phase 3(1 + cos^2)/(16 pi); Mie
scattering 3.996e-3, extinction 4.440e-3 (albedo 0.9), scale height 1.2 km, Henyey-Greenstein
g = 0.8; ozone absorption (0.650, 1.881, 0.085)e-3 with a tent profile `max(0, 1 - |h - 25| / 15)`
(10-40 km); ground albedo 0.3. The planet's centre is straight below the camera; the camera altitude
is `clamp(y / 1000, 0.001, 90)` km. Geometry is written in altitudes instead of radii (`r^2 - R^2 =
(h - H)(r + R)`), because `r * r` with r ~ 6360 km loses all precision in fp32 a few metres above
the ground.

Every LUT stores values for a **unit sun illuminance at the top of the atmosphere**; the real one
travels in `FrameCB.sun_color.w`, so the sun intensity slider never re-bakes anything.

| LUT (textures RGBA16F) | Size | Kernel | Content | Baked |
|---|---|---|---|---|
| Transmittance (t21) | 256x64 | 8x8 threads, Dispatch(32, 8) | `exp(-optical depth)`, 40 midpoint steps to the top of the atmosphere or the ground; Bruneton 2017 parameterisation (x = distance to the top remapped between its minimum and maximum for this altitude, y = rho / H) | once |
| Multi-scattering (t23) | 32x32 | 64 threads per texel, Dispatch(32, 32) | x = sun zenith cosine, y = altitude; 64 stratified directions (8x8) x 20 steps, isotropic phase; second order `L2` and transfer factor `f_ms` averaged over the sphere, stored as `L2 / (1 - f_ms)` (paper eq. 10: all orders >= 2 as a geometric series) | once |
| Sky-view (t20) | 192x108 | 8x8 threads, Dispatch(24, 14) | single scattering with the real phase functions + multiple scattering from the LUT above, earth shadow, sun light bounced by the ground; 4-32 steps (32 for paths >= 100 km) | sun or camera altitude change |
| Ambient cube (t22) | 6 float4 (structured buffer) | 256 threads, Dispatch(1, 1, 1) | see below | after each sky-view bake |

The ray march (`integrate`, the reference code's `IntegrateScatteredLuminance`) spreads its samples
quadratically along the ray (`t = t_max (s / N)^2`, so the dense air near the camera gets most of
them), samples each segment at 0.3 of its length, and integrates the in-scattering analytically
over the segment, `(S - S exp(-sigma_t dt)) / sigma_t` (Hillaire 2015, "Physically Based and Unified
Volumetric Rendering in Frostbite"), which stays energy-conserving with few steps.

**Sky-view parameterisation** (sun-relative longitude/latitude around the camera, half the azimuth
circle since the sky is symmetric about the sun's vertical plane): `u = sqrt(0.5 - 0.5 cos(dphi))`
puts more texels towards the sun; `v` is split at the horizon with a squared mapping on each side,
sky `v = 0.5 (1 - sqrt(1 - theta / Z))`, ground `v = 0.5 + 0.5 sqrt((theta - Z) / beta)` with
`beta = acos(sqrt(h (2R + h)) / (R + h))` and `Z = pi - beta`, then a half-texel remap. Lookups clamp
`v` to their own side of the horizon: below it the LUT holds the haze in front of a dark ground, and
bilinear filtering across the boundary drew a dark line just above the horizon.

**Re-bake rules** (`pass_sky_luts`): transmittance + multi-scattering once, and again after a shader
reload (the shader object pointer is compared); sky-view + ambient cube when the sun direction changes
(by more than 1e-6), the camera moves more than 1 m vertically (the horizon moves < 0.04 degrees), or
after a reload. Afterwards t20 (sky-view), t21 (transmittance) and t22 (ambient cube) are bound to
both the pixel and compute stages for the whole frame; t23 (multi-scattering) is bound only to the
compute stage while the sky-view LUT is baked. `pass_sky_luts` runs before `gi_update`, so a GI
relight in the same frame sees this frame's sky.

### Sun colour (CPU)

`sky_sun_illuminance` (`r_sky.cpp`) evaluates the same 40-step optical-depth integral as the
transmittance LUT, on the CPU, for the one direction that matters, so the sun colour is a plain
constant in `FrameCB.sun_color.rgb` (no GPU readback) and agrees with the sky by construction. The
fraction of the disk above the horizon is `saturate((asin(mu) - asin(mu_horizon)) / (2 r) + 0.5)`
with `r = 0.004651` rad (0.2665 degrees), so the sun fades out over half a degree at sunset instead of
switching off. The top-of-atmosphere illuminance is the scene's directional light intensity times
`sun_intensity` (courtyard: 3756.5; no scene file: 8). The glTF light's *colour* is ignored: a low
sun turns orange through the transmittance alone.

### Sky pass

`ps_sky` draws a full-screen triangle with stencil EQUAL 0 under the shading-model mask (`0x07`), so
only sky samples are written (pixel frequency; the stencil is per sample). Radiance = one bilinear
fetch of the sky-view LUT + the analytic sun disk (the LUT is far too coarse for a 0.53 degree disk,
paper section 5.3):

- disk radiance = `sun_color.rgb / (pi sin^2 r) * limb`, with linear limb darkening
  `(1 - 0.6 (1 - mu)) / (1 - 0.6 / 3)` normalised to keep the illuminance;
- capped at `min(16 * E_toa, 60000)`: the real disk is ~1e5 times brighter than the sky, beyond fp16
  (65504) and far more than the bloom should spread. With the courtyard's 3756.5 the fp16 cap binds
  (16 x 3756.5 = 60104);
- below the horizon (`hits_ground`), the ray ends on the virtual ground sphere and is fogged with the
  same `height_fog` as the geometry, so a finite floor continues into the distance without a seam.
  The ground term adds `0.3 * E_up/pi * transmittance` (skylight reflected by the ground, which the
  LUT leaves out) to the LUT's sun-lit ground.

### Sky ambient cube

`cs_sky_ambient` integrates the sky over 4096 Fibonacci-sphere directions (golden angle, 16 per
thread), with tree reductions in groupshared memory, in two passes:

1. `E_up / pi = (4 / N) sum_{d.y > 0} L(d) d.y`: the irradiance on an upward-facing surface divided
   by pi (the Monte Carlo estimate `(4 pi / N) sum L cos`, over pi).
2. For each axis +X -X +Y -Y +Z -Z: `(4 / N) sum L(d) max(+-d_axis, 0)`, where the lower hemisphere
   now includes the ground lit by `E_up / pi` from pass 1. Downward-facing surfaces therefore get
   ground bounce instead of sky blue (TI's warning against ambient that collapses into flat sky blue).

Stored as irradiance / pi, the radiance a white Lambertian surface reflects, which is what the
diffuse lighting buffer holds. `sky_ambient(n)` evaluates it as an ambient cube with `n^2` weights
(McTaggart 2004, Valve). 64 threads took 0.027 ms (latency-bound); 256 threads take 0.011 ms, only
when the sky-view LUT is re-baked.

### Who reads the sky

| Reader | Function | What |
|---|---|---|
| sky pass | `sky_radiance` + `sun_disk` | the visible sky |
| compose (fog) | `sky_horizon_radiance` | fog in-scatter: the sky's last row above the horizon in the ray's azimuth |
| sun + ambient (`gi.hlsli`, `indirect_light`) | `sky_ambient(N)`, `sky_ambient(dominant dir)` | diffuse and specular ambient when GI is off or no probe is valid (blend `saturate(20 * validity)`) |
| GI relight (`gi_relight.hlsl`) | `sky_radiance(dir)` | probe sensors that saw the sky; no sun disk, the sun arrives as shadowed direct light |
| particles, glass (`vfx.hlsl`) | `sky_ambient` | smoke: `sky_ambient(+Y) * 0.8` plus a little sun; glass: `sky_ambient(reflect)` with Fresnel |

FrameCB's old `ambient_sky` / `ambient_ground` constants are gone.

### Height fog (compose pass)

Analytic exponential height fog, as UE's ExponentialHeightFog: for a ray of length `d` with vertical
extent `dy` from camera height `h0`, `tau = 0.001 * exp(-0.12 h0) * (1 - exp(-k)) / k * d` with
`k = max(0.12 dy, -80)` (density 0.001/m at height 0, ~4 km visibility; falloff 0.12/m; the clamp
  keeps `exp(-k)` finite for rays more than ~650 m downwards), transmittance
`T = exp(-tau)`, in-scatter `sky_horizon_radiance(ray) * (1 - T)`. Matching the fog colour to the sky
just above the horizon (Wenzel 2006) makes distant geometry fade into the sky instead of a grey wall,
and backlit haze glows warm at golden hour. The old density 0.0035 turned backlit views into a white
veil once the in-scatter came from the real horizon radiance (bright Mie aureole). The compose pass
applies it with dual-source blending ([lighting.md](lighting.md)).

## Auto exposure (diffuse-only metering)

This part predates the sky and tone-mapping work and is unchanged by it. `cs_histogram` (16x16
threads) reads the **diffuse lighting buffer** (albedo-free, no emissive) at one pixel in 16
(`4 * id + 2`, MSAA sample 0): 480x270 points in 30x17 groups at 1080p. Sky samples (depth 0) and
unlit points (luminance <= 1e-6) are skipped. Diffuse lighting D is the radiance of a white
Lambertian surface, so an 18% grey card reads 0.18 D: **exposure = 1 / average(D)** maps it to 0.18
whatever the albedos, like an incident light meter (Fox Engine, "Photorealism Through the Eyes of a
FOX", GDC 2013).

- 256 bins, bin 0 unused: `bin = 1 + floor(254 * saturate((log2 Y + 10) / 24))`, i.e. 2^-10..2^14,
  0.094 EV per bin. Centre-weighted integer weights (`uint(lerp(4, 1, saturate(r^2)) + 0.5)`: rings of
  4 / 3 / 2 / 1, r^2 in [0, 2] from screen centre to corner). Groupshared histogram per group, merged with one
  `InterlockedAdd` per non-empty bin into a 256-uint raw buffer. Each group also writes
  `(sum w * rgb, sum w)` for the white balance (no float atomics in SM 5.0).
- `cs_adapt` (one group of 256): mean log2 luminance between the 40th and 95th (weighted)
  percentiles, target = `1 / exp2(mean)`; exponential adaptation in log2, frame-rate independent,
  2.5/s when the exposure drops (scene got brighter) and 1.2/s when it rises (eyes adapt to light
  faster than to dark); the first frame snaps. When nothing was metered (only sky on screen) the
  exposure holds, like the white balance. Resets the histogram for the next frame.
- Applied in the tone-map pass as `metered * 2^EC` (compensation slider); manual mode uses
  `1 / (1.2 * 2^EV100)` (Lagarde & de Rousiers 2014, "Moving Frostbite to PBR").

The metering pass also runs with auto exposure off: the white balance needs it. GPU-only, used in
the same frame, no readback. `--view 9` shows the metered buffer.

## Auto white balance (Bradford, mired range and bias)

In `cs_adapt`, after exposure (research note 06 section 6). With albedo removed, the diffuse buffer's
mean colour *is* the light colour (direct + bounce), so no grey-world assumption is needed: a red
wall is not mistaken for red light.

1. Light = `sum(w rgb) / sum(w) + 1e-4` (a faint grey "virtual light" that only matters in near
   darkness, GT7 slide 222). Rec.709 -> XYZ -> xy.
2. Correlated colour temperature by McCamy 1992, `n = (x - 0.3320) / (0.1858 - y)`,
   `CCT = 449 n^3 + 3525 n^2 + 6823.3 n + 5520.33`, clamped to 1667-25000 K. Tint = xy minus the
   Planckian locus at that CCT (Kim et al. 2002 cubic fit), its length clamped to 0.01 so strongly
   green or magenta light is only partly corrected.
3. Target = `clamp(10^6 / CCT, 10^6 / max_K, 10^6 / min_K)` mireds (default 3000-10000 K = 100-333
   mired): lights outside the range are corrected only to its limit, like a camera's AWB, so candle
   light stays orange. Mireds because equal steps look about equally large.
4. Mired and tint adapt at 1/s (state in buffer entry 3); the first frame snaps; when nothing is
   metered (only sky or emissives on screen) the previous state is held.
5. Von Kries in Bradford cone space towards D65, with `LMS(xy) = B (x/y, 1, (1-x-y)/y)` (Y = 1, so
   the luminance of white is preserved and white balance does not fight exposure):
   `g = lerp(1, LMS_D65 / LMS(planck(10^6 / mired) + tint), strength)` (strength 0.6: a low sun stays
   warm), times the artist bias `LMS_D65 / LMS(D65_xy + planck(10^6 / (153.75 - warmth)) -
   planck(6504 K))`. A positive warmth assumes a bluer light than D65, which warms the picture; it is
   applied in full even with AWB off (`--no-awb` only sets the strength to 0).
6. `M_wb = M_XYZ->709 B^-1 diag(g) B M_709->XYZ`, written as three rows.

## Tone mapping

### The pass (`tonemap.hlsl`, `pass_tonemap`)

One full-screen draw. Per MSAA sample:

```
exposed = (exposure * M_wb) * lerp(hdr, bloom_level0 / 6, bloom_intensity)
out     = LUT(lut_uvw((exposed / 64)^(1/4)))      // sRGB-encoded
```

The camera matrix (exposure x white balance, one 3x3 from a structured buffer) is applied before
the lookup, so the LUT only depends on the curve and is never re-baked for exposure. With
`LAYERS = 2` (SMAA S2x and 4x, the default) each sample is written to its own RGBA8 layer through the
UNORM view (the LUT already stores encoded values), plus the 1x velocity and the AA mask (MSAA edge
when any channel of the two encoded samples differs by more than 2/255; responsive = max of the
samples' alpha). With `LAYERS = 1` the samples are decoded, averaged in linear and written through
the sRGB view: the resolve still happens after tone mapping. Details: [antialiasing.md](antialiasing.md).

### GT7 (`tonemap_gt7`, default)

HLSL port of Polyphony Digital's `gt7_tone_mapping.cpp` v1.0 (Suzuki, Yasutomi, Uchimura, SIGGRAPH
2025 Physically Based Shading course; MIT, notice kept in `tonemap_ops.hlsli`), SDR path:

1. Rec.709 -> Rec.2020, times 2.5: GT frame-buffer units (1.0 = 100 cd/m^2) with a 250-nit SDR
   paper white, so exposed 1.0 = paper white.
2. **Skewed**: the GT V2 curve per channel. Toe `0.538 (x / 0.538)^1.28` blended into the identity by
   `smoothstep(0, 0.538, x)`; linear up to `0.444 * 2.5 = 1.11`; then the shoulder
   `kA + kB exp(kC x)` with kA = 2.963333, kB = -3.373351, kC = -0.539568 (alpha = 0.25), C1-continuous
   with the linear part.
3. **Untwisted**: in ICtCp (BT.2100, matrices exactly as in the sample), the intensity of the skewed
   colour with the chroma of the original, faded to white by `1 - smoothstep(0.98, 1.16, I / I_peak)`
   (`I_peak` = 0.602559, the I of (2.5, 2.5, 2.5)).
4. `min(lerp(skewed, untwisted, 0.6), 2.5) / 2.5`, back to Rec.709, `saturate` as the gamut clip.

Mostly hue-preserving with a camera-like hint of twist: bright orange goes to yellow, then white
(GT7 slides 139-150). The shoulder tends to 1.185 x peak, so the `min` clips: a neutral colour reaches
SDR white at exposed ~1.47, about **3.0 stops above mid grey**. Research simulation of the port:
0.18 -> 0.179, 1.0 -> 0.835, >= 1.47 -> 1.0. Exposure has to be right; there is little headroom.
The PQ `pow(x, 78.84)` needs fp32.

### Comparison operators (`--tonemapper N`)

Each takes exposed linear Rec.709 and returns display-linear Rec.709; those that do not map 0.18 to
~0.18 are pre-scaled, so the A/B compares curves rather than exposures (as GT7 slide 154 does for
ACES). Headroom = stops above mid grey to reach 0.99 (research note 06 section 3.4, simulation).

| N | Operator | Source | Pre-scale | Headroom |
|---|---|---|---|---|
| 0 | GT7 | above | (x2.5 internally) | 3.0 |
| 1 | GT | Uchimura 2017 (Gran Turismo Sport): P = 1, a = 1, m = 0.22, l = 0.4, c = 1.33, b = 0, per channel | none | 3.7 |
| 2 | Frostbite display mapping | Fry, GDC 2017: desaturation with ICtCp intensity, no toe, shoulder from 0.25 blended 60/40 between hue-preserving (max channel) and per channel, then up to 30% of the original chroma restored in the darker range | none | 4.3 |
| 3 | ACES | Hill's RRT + sRGB ODT fit (BakingLab) | x1.462 | 6.5 |
| 4 | Reinhard | `x / (1 + x)` per channel | x1.22 | 9.1 |
| 5 | Linear clip | `saturate(x)` | none | 2.5 |
| 6 | AgX | minimal version (Wrensch; three.js / Filament), Rec.2020, log2 range [-12.47, 4.03], 6th-order polynomial | x0.808 | 6.4 |

### The 64^3 LUT and the per-sample ALU A/B

`cs_bake` (4x4x4 threads, Dispatch(16, 16, 16) = 262,144 texels) writes a 64^3 RGBA16F Texture3D
(2 MB): texel `i` holds `linear_to_srgb(op(64 (i / 63)^4))`. The index is GT7's **pow(4) shaper**
(slide 173), `u = (x / 64)^(1/4)`: an HDR input spans ~20 stops, and uniform indexing would spend
almost every texel on highlights. With `X_max = 64` (~8.5 EV above 0.18) mid grey sits at texel
~14.5, exposed 1.0 at ~22.3 and GT7's clip at ~24.5; the spacing is ~0.4 EV per texel at mid grey,
0.26 at 1.0 and 0.09 at the top. Inputs above 64 clamp (only very bright, saturated lights, which
converge to white anyway). `lut_uvw` remaps to texel centres, or the ends of the range would be
interpolated towards the clamp border. sRGB-encoded storage halves the interpolation error of linear
storage; the research simulation put 64^3 pow(4) at ~2-3 sRGB 8-bit codes (p99) on saturated colours
against the ALU curve.

The bake runs only when the tone mapper changes or its shader is reloaded (`pass_tonemap_lut`,
profiler scope "Tone LUT bake"). `--tonemap-alu` (`tonemap_lut = false`) selects the `TONEMAP_LUT = 0`
permutation, which evaluates `linear_to_srgb(tonemap_operator(op, x))` for every MSAA sample instead
of the fetch: two evaluations per pixel with 2x MSAA, through a uniform `switch` over all seven
operators. The research's `fxc` count for a two-sample GT7 pixel shader was 234 instruction slots
(22 `exp` and 18 `log`) against 34 for the LUT version.

## Bloom (Call of Duty: Advanced Warfare)

Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014
Advances). Non-thresholded: the whole HDR scene blooms by its own intensity. Filtering on the way
down *and* up keeps it stable under motion; a plain bilinear chain aliases and pulses (Jimenez,
citing Mittring 2012).

- **Target**: one R11G11B10F texture at half resolution with 6 mips (at 1080p 960x540, 480x270,
  240x135, 120x67, 60x33, 30x16: 0.69 M texels), one RTV and one single-mip SRV per level so a pass
  never reads the level it writes, and a UAV on level 0.
- **Input**: the HDR scene colour after compose, sky and transparents, *before* exposure; MSAA samples
  box-averaged, clamped to [0, 65504] (one overflowed specular sample or NaN would otherwise spread
  over the screen; D3D11 `min`/`max` return the non-NaN operand, so NaN becomes 0).
- **Level 0, compute** (`cs_downsample_first`, 8x8 threads, Dispatch(120, 68) at 1080p): the 13-tap
  filter = five overlapping 2x2 boxes, the centre box (taps at +-1 texel) weighted 0.5 and four corner
  boxes (taps at 0 / +-2) 0.125 each. Each bilinear tap of the original sits on a texel corner, i.e.
  is a 2x2 average, so over an MSAA source (no hardware filtering) it needs the 6x6 full-res texels
  under the footprint. An 8x8 group loads (and resolves) its 20x20 tile into groupshared memory
  once: 12.5 sample loads per output instead of 72 for a pixel shader. A **partial Karis average**,
  weight `1 / (1 + luma)` per 2x2 box, renormalised, on this level only: it removes the fireflies a
  single very bright sample would turn into a pulsing blob, without darkening large bright areas.
- **Levels 1-5** (`ps_downsample`): the same 13 bilinear taps with the same weights, no Karis.
- **Upsample** (`ps_upsample`, levels 5 -> 1): 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16, radius one
  source texel, blended ONE + ONE onto the next larger level, which still holds its own downsample:
  `E' = E, D' = D + blur(E'), ...`. Repeated tents converge to a Gaussian; level 0 ends up holding
  the sum of all six levels.
- **Mix** (tone-map pass): `lerp(scene, level0 / 6, 0.04)`. Both inputs are averages of scene
  radiance, so the lerp conserves energy, and since it happens before the camera matrix the chain is
  exposure-independent.

## Measurements

From the final benchmark pass (2026-10-18; RTX 5060 with the GPU clock locked at 1485 MHz, about an RTX 3060's shader throughput; at stock clocks every
pass is 1.4-1.8x faster, [../perf-tables-stock.md](../perf-tables-stock.md); 1920x1080, 2x MSAA; mean
over the 8 courtyard viewpoints and the camera path; full tables in
[../perf-tables.md](../perf-tables.md), method in [../PERF.md](../PERF.md)).

| Profiler scope | Measured (ms) | TI chart (ms) |
|---|---|---|
| Sky LUTs (not re-baked) | ~0 | - |
| Sky | 0.016 (worst view 0.04) | 0.18 |
| Exposure (histogram + adapt + WB) | 0.044 | in the tone-map box |
| Bloom | 0.22 | 0.38 |
| Tone LUT bake (curve unchanged) | ~0 | - |
| Tone map (LUT) | 0.13 | 0.25 (upper bound 0.93) |

`--bench post` (frame and tone-map pass, mean over views):

| Configuration | frame (ms) | vs default | Tone map | Bloom |
|---|---|---|---|---|
| GT7 through the 64^3 LUT (default) | 7.48 | | 0.132 | 0.226 |
| GT through the LUT | 7.52 | +0.04 | 0.133 | 0.224 |
| ACES through the LUT | 7.49 | +0.01 | 0.133 | 0.228 |
| GT7 per sample, no LUT | 7.76 | +0.28 | 0.396 | 0.225 |
| bloom off | 7.26 | -0.22 | 0.135 | |
| auto white balance off | 7.47 | -0.01 | 0.135 | 0.225 |
| auto exposure off (manual EV100 12) | 7.28 | -0.20 | 0.131 | 0.226 |

- At stock clocks: tone map 0.10 ms through the LUT vs 0.24 ms per sample, bloom 0.13 ms. The tone
  map gains least from the higher clock (1.4x vs 1.7x for most passes): it reads every MSAA sample of
  the HDR buffer, so it is largely bandwidth-bound.
- The LUT saves 0.26 ms of tone mapping (-67%) and makes the curve's cost irrelevant: every
  operator costs the same through it.
- The metering pass runs in every configuration (the white balance needs it), so the AWB row costs
  the same as the default. The auto-exposure row's -0.20 ms is not an exposure cost: the fixed
  EV100 12 image is darker, with fewer luma edges, and SMAA's weight pass drops from 0.20 to
  0.05 ms.
- Two earlier back-to-back A/Bs from the commits: the ambient cube bake took 0.027 ms with 64
  threads and 0.011 ms with 256; the bloom downsample chain took 0.17 ms with a pixel-shader level 0
  and 0.10 ms with the compute version.

## Deviations and known issues

- **Sky**: no clouds (the chart's 2.90 ms); no photographic validation against reference photos
  (GT7's discipline); aerial perspective is the height fog, not Hillaire's froxel LUT. The sky-view
  LUT is baked only when the sun or the camera altitude changes, where the paper bakes it every frame.
- **Tone map**: 8-bit RGBA8 sample layers instead of TI's 10-bit RGB10A2 (see
  [antialiasing.md](antialiasing.md)). The ALU path has no "both samples equal" shortcut, so it pays
  two curve evaluations on every pixel.
- **Exposure**: point-sampled 1/16 of the pixels with no prefilter (the research planned a 4x4 average
  downsample first); no EV clamp and no GT7-style long-term adaptation. There is no per-pixel
  metering weight: skin after its SSS blur and foliage transmission are metered (TI wants SSS
  excluded). D contains the diffuse weight `(1 - F)(1 - metal)`, so pure metals (D = 0) are skipped
  and partly metallic pixels read slightly dark.
- **Bloom placement**: built from the HDR scene before tone mapping (research variant B1), not from
  the post-motion-blur image TI's chart uses (and shares with SSR), so it does not react to motion
  blur. Level 0 is the full 13-tap with partial Karis, closer to CoD:AW than the research's 2x2 Karis
  box. On odd-sized sources (135 -> 67, 67 -> 33, 33 -> 16 at 1080p) the 2-texel offsets no longer
  land exactly on texel corners.
- **Fog**: not applied to glass or particles.
- **Sun colour and the GI**: `sun_color.rgb` depends on the camera's altitude (transmittance in
  `sky_sun_illuminance(sun_dir, cam.pos.y, ...)`), so the GI relight is triggered by the sun's
  direction and its top-of-atmosphere illuminance (`sun_color.w`) instead; the first merged version
  compared `sun_color.rgb` and re-lit the probes (~12 ms) on most frames of a climbing or descending
  camera. A relight lights the probes with the sun colour at the camera's altitude at that moment
  (a negligible difference over a few metres).

## Settings, UI and command line

| Setting (`Settings`) | Default | UI | Command line |
|---|---|---|---|
| `sun_intensity` | 1.0 | Lighting: "Sun intensity" 0-4 (x the scene's illuminance) | `--shadow sun=F` |
| `sun_elevation`, `sun_azimuth`, `sun_from_scene` | 12, 230 degrees, true | "Sun elevation" -5..90, "Sun azimuth" 0..360 (editing overrides the scene's sun) | `--sun ELEVATION,AZIMUTH` |
| `tonemapper` | `TM_GT7` | Post: "Tone mapper" | `--tonemapper N` (0 GT7, 1 GT, 2 Frostbite, 3 ACES, 4 Reinhard, 5 linear clip, 6 AgX) |
| `tonemap_lut` | true | "Bake curve into 3D LUT" | `--tonemap-alu` |
| `auto_exposure`, `exposure_ev` | true, 0 | "Auto exposure"; "Exposure comp." -4..4 EV or "Exposure EV100" 0..16 | none |
| `auto_white_balance`, `awb_strength` | true, 0.6 | "Auto white balance", "AWB strength" | `--no-awb` |
| `awb_min_kelvin`, `awb_max_kelvin` | 3000, 10000 | "AWB range" 1700-20000 K | |
| `wb_warmth` | 0 | "WB warmth" -100..+100 mired | |
| `bloom`, `bloom_intensity` | true, 0.04 | "Bloom", "Bloom intensity" 0-0.2 | `--no-bloom` |
| `gi` | true | "Probe GI" (off: the sky ambient cube alone) | `--gi 0` |

The profiler panel shows "Sky", "Tone map" and "Bloom" next to their chart budgets (`ti_budget_ms`
in `src/bench.cpp`); "Sky LUTs", "Exposure" and "Tone LUT bake" have no chart entry.
