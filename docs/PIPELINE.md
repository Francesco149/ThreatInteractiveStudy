# The frame, pass by pass

This is the frame as `renderer_frame()` (`src/renderer.cpp`) executes it, mapped to the chapters of
Threat Interactive's video and to the per-pass budget of the video's pipeline chart (1080p 60 fps
on an RTX 3060, transcribed in [research/00-video-pipeline-chart.md](research/00-video-pipeline-chart.md)).
Every pass is a GPU profiler scope with the same name, shown in the UI next to its budget.
Measured costs: [PERF.md](PERF.md). Details per technique: [techniques/](techniques/).

## Frame setup

1. **Animation** (`scene_animate`): rotating/swinging/sliding objects; every instance keeps last
   frame's matrix for motion vectors.
2. **Frame constants**: camera, jitter, exposure; the sun's colour at the camera comes from the
   atmosphere's transmittance (`sky_sun_illuminance`), so a low sun turns orange by itself.
3. **Sky LUTs** (GPU, profiler scope "Sky LUTs"): the Hillaire 2020 transmittance and
   multi-scattering LUTs, the sky-view LUT and the sky ambient cube, re-baked only when the sun
   moves (or the camera rises or falls by more than 1 m); bound at t20-t22 for the whole frame
   (t23, the multi-scattering LUT, only while baking).
   [tonemap-sky-bloom](techniques/tonemap-sky-bloom.md)
4. **GI probes** (GPU, only when needed, "GI capture" / "GI relight"): the probe grid is captured
   once after loading (a few seconds, paced in blocks) and re-lit from a whole-scene sun shadow map
   when the sun moves (~12 ms). It runs after the sky LUTs because probes that see the sky are lit
   by them. [gi-ao](techniques/gi-ao.md)
5. **Draw lists** (`build_draw_lists`): frustum culling, LOD selection with dithered transitions,
   pre-pass selection, base-pass buckets, instancing of identical consecutive items. All per-draw
   data of the frame goes into one structured buffer, uploaded once. No per-draw constant buffers.
6. **Shadow views** (`shadows_setup`): sun cascades with caster culling against the camera slice
   extruded toward the sun; local lights get frustums fitted to what the camera sees, sized by the
   "1080p + 8%" density rule and packed into one atlas (casters clipped beyond the deepest visible receiver,
   distant lights faded out, optional cache of static views).
7. **Lights** (`lights_setup`): culling, scissor rectangles, volume transforms.
8. **VFX** (`vfx_setup`): glass draw list (back to front), particles (a pure function of time).

## GPU

| # | Pass (profiler scope) | Video | TI budget | What it does here |
|---|---|---|---|---|
| 1 | Shadows | 39:00 | 0.60 sun + 1.00 local | Sun cascades (D16) and density-synced local-light projectors in one D16 atlas, depth-only from the 12-byte position stream; floors never cast. [shadows](techniques/shadows.md) |
| 2 | Clears | 8:00 | 0.15 | Hardware clears of depth/stencil and the partially written targets only (emissive). Nothing else is ever cleared. [geometry](techniques/geometry.md) |
| 3 | Depth pre-pass | 14:21 | 0.30 | Partial, non-alpha-tested: large/close occluders only, no pixel shader. |
| 4 | Base pass | 28:53 | 2.80 | G-buffer (2x MSAA): close alpha-tested, other opaque, remaining alpha-tested, then pre-passed objects with depth EQUAL. Writes the shading model into stencil per draw, motion vectors for everything, specular AA, texture jitter compensation. |
| 5 | Decals | 36:44 | 0.30 | Box-projected decals straight into the G-buffer albedo; no decal buffer. |
| 6 | MSAA edge stencil | 36:44 | 0.50 | "G-buffer resolve": marks pixels whose samples shade differently; everything after this shades once per pixel elsewhere. |
| 7 | GTAO | 49:45 | 1.00 (GI's screen-space share) | Ground-truth AO from depth and normals (full or half resolution), 4x4 denoise; applied to indirect light only. [gi-ao](techniques/gi-ao.md) |
| 8 | Sun + ambient | 45:00 | 0.54 | One draw per shading model (pixel + sample frequency), overwrites both lighting buffers (no clear). Sun shadows; indirect light from the probe grid (sky ambient cube where there is no valid probe) with GTAO multi-bounce and specular occlusion. [lighting](techniques/lighting.md) |
| 9 | Local lights | 41:00 | 1.50 | Stencil volume mark, then a scissored full-screen triangle per shading model that lights and unmarks; shadows from the local atlas. |
| 10 | Subsurface scattering | 53:48 | 0.30 | Separable SSS on skin pixels only (stencil), full resolution. |
| 11 | Compose | 57:21 | 0.12 | diffuse x albedo + emissive onto the specular buffer, with height fog (lit by the atmosphere's horizon radiance) in the same draw (dual-source blending). |
| 12 | Sky | 1:01:00 | 0.18 | Sky samples only (stencil 0): one sky-view LUT fetch plus the analytic sun disk. [tonemap-sky-bloom](techniques/tonemap-sky-bloom.md) |
| 13 | Transparents + VFX | 1:02:00 | 0.15 + 1.00 | Forward glass; particles at half resolution against a checkerboard min/max depth (hijack-cleared target), nearest-depth upsample; responsive mask. [vfx-and-output](techniques/vfx-and-output.md) |
| 14 | Exposure | 1:16:00 | (in tone map) | Histogram of the diffuse-only (albedo-free, emissive-free) lighting buffer, Fox Engine style; the same buffer drives auto white balance. |
| 15 | Bloom | 1:57:40 | 0.38 | Call of Duty: Advanced Warfare bloom: 6 levels from half resolution (first downsample in compute), 13-tap downsamples, tent upsamples; no threshold. Deviation: built from the HDR scene before tone mapping, where TI recycles the post-motion-blur image (shared with SSR). |
| 16 | Tone LUT bake | 1:04:45 | - | Bakes GT7's curve into a 64^3 LUT, only when the tone mapper changes. |
| 17 | Tone map | 1:04:45 | 0.25 | Exposure and white balance as one 3x3 matrix, bloom lerp, the GT7 curve through the LUT, per MSAA sample into two 8-bit layers: the MSAA resolve happens after tone mapping. Velocity and AA masks. |
| 18 | Anti-aliasing | 1:19:38 | 0.38 + 0.25 | SMAA S2x on both sample layers, then the two-frame temporal resolve (previous SMAA output, never an accumulation buffer), edge-only, responsive-masked. [antialiasing](techniques/antialiasing.md) |
| 19 | Motion blur | 1:42:40 | 0.30 | Velocity pack with depth-limited camera blur, trailing-only gather, separable second pass. [motion-blur](techniques/motion-blur.md) |
| 20 | Final output + UI composite | 1:57:29 | 0.15 + 0.10 | Scaling, chromatic aberration, vignette, grain, display calibration and the UI overlay in one draw. |

The GI's world-space share (1.00 ms) is spent in "GI relight" when the sun moves and in the probe
lookups inside "Sun + ambient"; with a static sun a frame pays only the lookups.

Debug passes (only when selected): quad overdraw counting, debug views.

## Render targets (1920x1080)

| target | format | samples | written by | cleared |
|---|---|---|---|---|
| depth/stencil | D32_FLOAT_S8X24 (reversed Z) | 2 | pre-pass, base pass; stencil: base pass, edges, light volumes | yes |
| G-buffer albedo | RGBA8 sRGB (a = roughness) | 2 | base pass, decals | no |
| G-buffer normal | RGB10A2 (octahedral, specular) | 2 | base pass | no |
| G-buffer material | RGBA8 (profile pair, blend, AO, shading model) | 2 | base pass | no |
| motion | RG16F | 2 | base pass | no |
| emissive | R11G11B10F | 2 | base pass (emissive draws only) | yes |
| diffuse lighting | RGBA16F | 2 | sun (overwrite), lights (add), SSS | no |
| specular lighting / scene colour | RGBA16F | 2 | sun, lights, compose, sky, transparents, VFX | no |
| GTAO view depth, raw, denoised | R32F, R8, R8 (half size in half-res mode) | 1 | GTAO | no |
| bloom chain | R11G11B10F, 6 levels from half size | 1 | bloom | no |
| LDR sample layers x2 | RGBA8 typeless (UNORM + sRGB views) | 1 | tone map | no |
| velocity, AA masks, SMAA edges/weights/outputs | various | 1 | tone map / SMAA | SMAA edges/weights yes |

Persistent resources: sun cascades (D16 array, 4x2048^2), local shadow atlas (D16, 8192^2), sky
LUTs (RGBA16F: transmittance 256x64, multi-scattering 32x32, sky-view 192x108) and the sky ambient
cube (6 float4), the GI grid (two RGBA16F Texture3D of nx x ny x 6nz, ping-ponged for bounces, plus
a static emissive grid), the probes' sensors (R32G32_UINT atlas, 8x8 texels per cube face), the
GI's own 2048^2 whole-scene sun shadow map, and the 64^3 RGBA16F tone LUT.

Stencil byte: bits 0-2 = shading model + 1 (0 = sky/empty sample), bit 3 = MSAA edge, bit 7 =
"inside the current light volume" (set by the volume draw, cleared by the lighting draw).
