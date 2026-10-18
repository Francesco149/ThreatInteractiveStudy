# R5: SSS, lighting composition, fog, GI, AO, reflections and sky

Research note for the educational D3D11 prototype of the pipeline in Threat Interactive (TI), *"Tutorial: How To Optimize Almost Every Step In Modern Game Rendering"* (2026-08-01). It covers the video's "WIP" layers:

- screen-space subsurface scattering (SSS)
- diffuse and specular composition
- fog
- global illumination (GI)
- ambient occlusion (AO)
- reflections
- sky

Target: Windows, D3D11 FL 11_0/11_1, SM 5.0, deferred, 2x MSAA, 1080p60, RTX 5060 8 GB.

How claims are labelled:

- **[src]**: verified in the cited primary source (I downloaded or read it).
- **[TI]**: from the video's narration, or from TI's on-screen pipeline diagram and measurements. I transcribed the diagram from frames at 49:45–57:40. TI's capture hardware is not stated on screen.
- **[est]**: my own estimate. These are not measured, so the A/B plan in §10 is how we confirm them.
- **[proposal]**: my design choice for our engine.

All HLSL and C++ below was written for this note from the published math. Nothing is copied from the reference code. The Separable SSS kernel generator reproduces the reference tables exactly (checked numerically, §3.3).

---

## 0. TL;DR: decisions for our engine

| Area | Decision (v1) | Runtime cost at 1080p on RTX 5060 [est] |
|---|---|---|
| Lighting buffers | Diffuse and specular are separate 2xMSAA **RGBA16F**. Diffuse holds lighting *without* albedo, and the composite multiplies base color (post-scatter texturing, Crysis 3 style). | n/a |
| Composite | One full-screen pass writes a 2xMSAA RGBA16F lit buffer. It computes `diffuse*albedo + specular`, applies height fog and skips sky via stencil. Interior pixels run at pixel frequency; MSAA-edge pixels run at sample frequency. | 0.10–0.15 ms (TI budget 0.12 ms) |
| SSS | Jimenez separable SSS (2 pixel-shader draws), full res, stencil = skin. It reads sample 0 with an ID-aware center sample, and the per-sample stencil makes writes MSAA-correct. Pre-integrated kernel of 11 or 17 taps, built at startup. Ping-pongs through the composite buffer as TI's diagram says. Optional 2-position 2x2 Bayer jitter. | 0.05–0.15 ms third-person, ~0.3–0.5 ms full-screen face |
| Fog | Analytic exponential height fog, merged into the composite. The inscatter color is matched to the sky horizon. | ~0.01 ms of ALU |
| GI | Baked **ambient-cube irradiance probe grid**, generated at load on the GPU. Cube captures of the direct-lit scene plus sky, iterated 3 bounces. Leak handling: invalid-probe detection and dilation, plus normal/view bias. Sampled in the per-shading-model sun/ambient draw, so static and dynamic objects are lit the same way. v2: per-frame **shadow-map-driven relighting** of baked sensors (TI's shadow-maps-first idea). | sampling 0.03–0.06 ms; v2 relight 0.05–0.2 ms |
| AO | GTAO (radiometric horizon AO), full res, pixel frequency. 2 slices × 4 steps per side. 4×4 Bayer slice rotation plus 4×4 depth-aware spatial denoise, **no temporal history**. Applied to *indirect only*, with the multi-bounce fit and specular occlusion. | 0.25–0.35 ms (half res ~0.12 ms) |
| Reflections | Deterministic (non-stochastic) screen-space ray march (McGuire & Mara DDA), pixel frequency. Glossy uses a mip pyramid of the previous frame's lit image. Falls back to parallax-corrected, GGX-prefiltered cubemaps (Lagarde 2012, Karis 2013) and then the sky cube. No history buffer. | 0.3–0.5 ms full res, 0.1–0.2 ms half res |
| Sky | Hillaire 2020 LUT atmosphere: transmittance 256×64, multi-scattering 32×32, sky-view 192×108 per frame. The sun color comes from transmittance. Sky ambient feeds probe captures and a fallback ambient cube. | ~0.1 ms total |

The sum of these layers is about 1.0–1.6 ms. TI's diagram budgets 1.0 ms for world-space GI plus 1.0 ms for screen-space techniques, 0.3 ms for SSS and 0.12 ms for the composite.

---

## 1. What the video proposes

Everything in this section is paraphrased. Timestamps refer to the local transcript, and the diagram notes were read from video frames.

### 1.1 Narration ([TI], timestamps)

- **49:45 GI (marked WIP).**
  - Find indirect lighting from the existing shadow-map information first, and fall back to a world-space trace only where that is not enough.
  - Do **not** build reflective shadow maps (RSMs). TI puts them at 0.3–0.6 ms per frame.
  - Instead feed the indirect evaluation from the existing shadow maps plus baked world-space data, as Days Gone and The Division do.
  - Target: noiseless results inside about 2 ms.
- **50:52 Baked data.**
  - Baked grids or noiseless probes (The Division, Days Gone, Fox Engine) are preferred over lightmaps.
  - They need noiseless SSDO/SSGI plus a separate large-scale occlusion system to look good.
  - Static and dynamic objects can be lit by the same baked data: Alan Wake 2 uses a sparse grid, Days Gone baked voxel GI plus occlusion, and The Phantom Pain probes.
  - The editor can run a dynamic or ray-traced version while the shipped game uses the bake. TI notes that DDGI already offers this workflow.
- **53:30 Reflections.** The same layering: non-ray-traced methods first, then world-space tracing.
  - The world-space representation should be close to a simplified voxel scene.
  - Dynamic cube maps remain an option.
- **53:48 SSS.**
  - The Phantom Pain blurs aggressively at native resolution, which damages shadows.
  - Callisto Protocol does full-res RGBA16 SSS with a per-pixel profile-ID lookup.
  - Silent Hill 2 Remake uses half res and comes out too blurry, with upscaling artifacts.
  - Callisto's half-res trick blends 80% of a half-res blur with the native unblurred diffuse. TI says it looks almost identical at about three times the speed, but upscale artifacts remain. Half res is only acceptable with an ID- and depth-aware upscale.
  - TI recommends a Crysis-3-like pixel-shader method: it can use stencil testing, needs only an ID buffer for per-profile color, and measured roughly 3× faster than Callisto with RGBA16 inputs. TI thinks it can be merged with the following composite pass.
  - If banding appears, use the two-position 2x2 Bayer jitter and rely on 60 fps persistence.
  - Expected cost in normal third-person play is about 0.3 ms.
- **57:21 Composition.**
  - Diffuse and specular are only kept separate so SSS can blur standalone diffuse.
  - UE checkerboards diffuse and specular inside an RG11B10 buffer; TI rejects that.
  - TI wants **RGBA16** lighting. KCD2, Dead Space Remake and The Phantom Pain have the best-looking lighting contrast. KCD2 still puts skin diffuse in RG11B10; Dead Space Remake keeps it RGBA16.
  - Crysis 3 keeps diffuse and specular in two RG11B10 buffers and only goes to RGBA16 when combining with base color, because Crytek argues the precision matters only once material color is applied. TI wants to verify this before adopting it.
  - Skin specular must remain separate because Callisto Gaussian-blurs it. TI calls this step poorly documented.
- **1:00:50 Fog.** Fog belongs in the composite pass. NFS 2015 and The Phantom Pain use fog atlases there; an ALU-only approach may be better.
- **1:01:10 Sky and clouds.**
  - These are rendered after direct lighting is resolved.
  - Per-pixel cloud cost should scale with base-pass coverage.
  - TI points to the GT7 GDC 2023 sky talk, praising its heavy photographic validation.

### 1.2 TI's on-screen pipeline diagram (paraphrased, [TI])

**GI block, labelled "cascaded probe lighting".** Stages in order:

1. Stream GI data.
2. Find visible radiance probes from depth, and maybe normals.
3. Find propagation paths from off-screen probes.
4. **Shadow-map tracing.** Each relevant probe updates its local "irradiance sensors" against the existing shadow maps. Several positions around each sensor are sampled to avoid popping. Sensors outside every shadow map are flagged for world-space software ray tracing.
5. **Fallback.** Temporal software RT over SDFs, voxels, BVH or micro shadow maps. One ray per sensor, with its start position changing every frame if one ray is too expensive.
6. **Feed sensors into probes.**
   - Baked per-sensor data: the most unoccluded direction and an average albedo of the nearby static geometry facing it.
   - Dynamic per-sensor data: direct-light occlusion.
7. Propagate light across probes.
8. Optionally update a world-space reflection environment.
9. Optionally output a simplified spherical map on a precomputed atlas (a Fox Engine trick).
10. **Pixel-frequency screen-space layers: SSGI at 0.55 ms and probe-guided SSAO at 0.40 ms.**
    - The timings reference Days Gone's 1080p SSGI and optimized SSAO, measured before denoising.
    - Notes on these layers: avoid CIAO because of halos; never evaluate AO on sky or let it affect direct light; use an ALU Bayer pattern for non-temporal effects and blue noise only for temporal ones.
11. First separable denoise step.
12. **Shade indirect lighting into the existing 2xMSAA RGBA16 diffuse buffer at sample frequency**, with indirect SDF shadows.

Block budget: **1.0 ms world-space GI plus 1.0 ms for screen-space techniques.**

"Learned from" notes on the GI block:

- **Days Gone** updates baked local probes from shadow-map data in a compute pass costing about 0.3 ms.
- **Crysis 3 / Tesseract** use RSMs: about 0.3 ms to render plus another 0.3 ms to process, for a poor result.
- **MGSV** uses small GI atlases fed by micro 30×30 orthographic shadow views from the light toward each sensor.
- **The Division, MGSV and Days Gone** together show that world-space GI need not be noisy or slow.

Workflow notes:

- Artists flag static geometry for baking.
- The engine places probes and sensors automatically.
- An editor-only version keeps re-baking with ray tracing.
- Direct lighting matters most in the output. Everything else may be approximated, as long as ambient light does not collapse into a flat sky-blue.

**Reflections block (budget 0.5 + 1.0 + 0.30 ms).** Stages in order:

1. Write resolved normals and roughness into two existing 1x RGB10A2 targets.
2. Compute-shader classification for stochastic SSR, producing a native R8_UINT trace mask.
3. **Pixel-frequency stochastic SSR**: trace a mipmapped R11G11B10 copy of last frame's post-VFX image with blue-noise sampling in about 0.40 ms. Outputs are RG16 specular motion vectors and a noisy native R11G11B10 result.
4. Build a world-space tracing mask from roughness, ID/sky and the SSR result.
5. **World-space RT reflections** for the masked pixels, at 760p (RG16 plus R11G11B10).
6. **One merged temporal-reprojection and composite draw.**
   - Inputs: a native SSR history (R11G11B10), the traces, specular motion vectors, the 2xMSAA motion vectors, the 2xMSAA depth-stencil (early testing on the sky value) and the 2xMSAA normals.
   - Outputs: the 2xMSAA RGBA16 specular composite and next frame's SSR history.
   - The temporal SSGI/SSAO resolve could be merged here too.
7. Real-time cubemaps are a possible later topic. Days Gone shows they can be cheap.

"Learned from" notes on reflections:

- **NFS 2015**: native SSR tracing at about 0.30 ms.
- **Detroit: Become Human**: a good resolve thanks to blue noise and RG16F motion vectors.
- **Crysis 3 Remastered**: blends pixel-frequency SSR into the multisampled image cleanly, and merges SSR denoising into the composite.

**SSS block: diffuse only, native resolution, possibly pixel frequency, 0.3 ms.**

- The first separable pass overwrites the 2xMSAA RGBA16 buffer that the composite will later overwrite anyway.
- The second pass writes back into the original input.
- Only two draws.

**Composite block: diffuse plus specular, with fog, 0.12 ms.**

- Reads the diffuse and specular RGBA16 buffers plus the needed G-buffer and ID data, and overwrites a 2xMSAA RGBA16 target.
- Skipping sky pixels would help.
- A simple SSR could live here, for Fresnel-anti-aliased edges.
- Possibly split the draws by shading model with stencil. Skin specular needs a Gaussian smoothing step; Callisto has no separate draw for it, so it was probably merged into the composite.
- A Fox-Engine-style early-tonemapped 2xMSAA RGB10A2 resolve is worth investigating.

**Example "marketing posts" (the claims TI wants an engine to be able to make):**

- **GI**: four layers, all built from current-frame shadow maps with no RSM generation:
  - world-space large-scale occlusion;
  - world-space large-scale indirect propagation;
  - TAA-independent directional SSAO from the normal buffer, without halos;
  - lit-scene-driven AO (SSGI).

  Total under 0.5 ms per frame at 1080p.
- **Reflections**: a stochastic-SSR-assisted world-space mask, blue noise, RG16F motion vectors, R11G11B10F results, native screen-space tracing, and native or interleaved world-space tracing.
- **SSS**:
  - two separable blur draws;
  - a jittered 2x2 Bayer pattern against banding at 60 fps;
  - full res, or half res blended with the native unblurred 2xMSAA diffuse through a depth- and ID-aware upscaler.

**TI SSS measurements on screen (55:30–56:52; hardware not stated; close-up faces unless noted):**

| Case | GPU time |
|---|---|
| Callisto Protocol, native RGBA16 SSS (UE `r.SSS.SampleSet 2`) | 1.4 ms |
| Callisto, `r.SSS.SampleSet 0` (TI: weak scattering, banding) | 0.78 ms |
| Days Gone, native RG11B10 | 1.00 ms |
| Days Gone, inputs forced to RGBA16 | 1.35 ms |
| Crysis 3, native RGBA16 (2 draws) | 0.62 ms |
| Crysis 3, native RG11B10 | 0.49 ms |
| Callisto, medium shot: SampleSet 0 / 1 / 2 | 0.23 / 0.31 / 0.39 ms |

TI also notes that Alan Wake 2's direct-lighting cost comes from reading 3× more inputs. Separate diffuse and specular outputs by themselves should cost only about 5% (perhaps 15%).

---

## 2. Frame integration: buffers, stencil, pass order, MSAA frequency

### 2.1 Buffers and formats [proposal]

| Resource | Format | Size at 1080p | Notes |
|---|---|---|---|
| Depth-stencil | `R32G8X24_TYPELESS` with a DSV of `D32_FLOAT_S8X24_UINT`, 2xMSAA | 33 MB | SRVs: depth `R32_FLOAT_X8X24_TYPELESS` (`Texture2DMS<float>`); stencil `X32_TYPELESS_G8X24_UINT` (`Texture2DMS<uint2>`, stencil in `.y`). Bind it as a **read-only DSV** (`D3D11_DSV_READ_ONLY_DEPTH \| D3D11_DSV_READ_ONLY_STENCIL`) when it is also an SRV. |
| Diffuse lighting | `R16G16B16A16_FLOAT`, 2xMSAA | 33 MB | Σ lights of (1/π)·E·(diffuse energy terms), **no albedo**, plus indirect diffuse. Written without a clear; the first full-screen draw per shading model overwrites. |
| Specular lighting | `R16G16B16A16_FLOAT`, 2xMSAA | 33 MB | Direct and indirect specular. Alpha is free. |
| Lit / composite | `R16G16B16A16_FLOAT`, 2xMSAA | 33 MB | Final HDR before transparents. Also the SSS horizontal-pass temp (TI's diagram). |
| Linear depth, 1x | `R32_FLOAT` (or R16F), from sample 0, plus mips | 8 MB + mips | Built in the G-buffer resolve stage. Used by AO, SSR and SSS. |
| View normal plus roughness, 1x | `R10G10B10A2_UNORM` or RGBA8, from sample 0 | 8 MB | AO and SSR. TI also proposes 1x RGB10A2 targets as pixel-frequency placeholders. |
| AO | `R8_UNORM`, 1x (raw and denoised) | 2 × 2 MB | |
| SSR | `R16G16B16A16_FLOAT`, 1x | 16.6 MB | rgb = radiance, a = confidence. Half res is 4.1 MB. |
| Previous-frame color pyramid | `R11G11B10_FLOAT`, 1x, full mips | ~11 MB | Average of the 2 samples of last frame's lit buffer (post-VFX, pre-tonemap). |
| Probe grid | 6 × `Texture3D` `R11G11B10_FLOAT` (or RGBA16F) | e.g. 32×8×32 is about 0.2 MB (0.4 MB RGBA16F) | Ambient cube: irradiance along ±X, ±Y, ±Z. |
| Sky LUTs | transmittance 256×64 RGBA16F; multi-scatter 32×32 RGBA16F; sky-view 192×108 R11G11B10F | < 0.2 MB | |
| Env cubes | sky cube 128² × 6 RGBA16F with mips; local cubes 128² × 6 × N | ~0.5 MB each | GGX-prefiltered. |
| Env BRDF LUT | `R16G16_FLOAT` 128×128 | 64 KB | Or the analytic approximation. |

### 2.2 Stencil bit layout [proposal]

This must be coordinated with the G-buffer and lighting notes. The layout is chosen so every test in this note is a single `EQUAL` test.

```
bits 0-2 : shading model ID   (0 = none/sky, 1 = default lit, 2 = skin, 3 = forward (eyes/hair), ...)
bits 3-5 : reserved for stencil-volume light marking
bit  6   : GEOMETRY  (1 = opaque geometry sample, 0 = sky / empty sample)
bit  7   : EDGE      (pixel's samples differ -> needs sample-frequency shading; written to all samples of the pixel)
```

| Selection | ReadMask | Ref | Func |
|---|---|---|---|
| geometry, interior (pixel-frequency) | `0xC0` | `0x40` | EQUAL |
| geometry samples of edge pixels (sample-frequency) | `0xC0` | `0xC0` | EQUAL |
| sky / empty samples | `0x40` | `0x00` | EQUAL |
| all skin samples (SSS) | `0x47` | `0x42` | EQUAL |

Stencil testing is **per sample** on an MSAA depth-stencil buffer, so a pixel-frequency full-screen draw writes its single output only to the samples that pass. This is the key mechanism for MSAA-correct SSS and composite writes. `DepthEnable = FALSE` does not disable the stencil test in D3D11.

### 2.3 Pass order (per frame) [proposal]

Passes covered by other notes are in parentheses.

1. (Pre-pass, G-buffer base pass, 2xMSAA.)
2. (G-buffer resolve.) Write the stencil bits from §2.2. Write 1x linear depth (sample 0) plus mips, and 1x view normal and roughness.
3. (Shadow maps.) v2 GI: also refresh the GI shadow map (whole static level, orthographic) only when the sun moves.
4. **Sky LUTs.** Transmittance and multi-scatter only when atmosphere parameters change. Sky-view every frame, or when the sun or camera altitude changes. Compute the sun color (CPU or GPU).
5. **GI relight (v2 only).** Sensor relighting compute pass updates the probe grid, possibly time-sliced.
6. **GTAO**: main pass, then the 4×4 spatial denoise. 1x, pixel frequency.
7. **SSR trace**: 1x, pixel frequency. It needs only the G-buffer, depth and *last frame's* color, so it can run before lighting.
8. **Lighting** (other note), one full-screen draw per shading model for sun + ambient.
   - It **overwrites** diffuse and specular and **also adds**:
     - indirect diffuse = probe irradiance × GTAO multi-bounce;
     - indirect specular = lerp(cube, SSR, confidence) × EnvBRDF × specular occlusion.
   - Then the stencil-volume local lights add on top.
   - This merges TI's separate indirect-shading step (indirect light added into the 2xMSAA diffuse buffer) into an existing draw.
9. **SSS H**: reads Diffuse, writes the composite buffer (as temp) on skin samples.
10. **SSS V**: reads the composite buffer, writes Diffuse on skin samples.
11. **Composite and fog**: geometry samples only. Draw A covers interior pixels at pixel frequency; draw B covers edge pixels at sample frequency. It writes the composite buffer. Skin specular smoothing happens here (optional).
12. **Sky**: sky samples only, writes the composite buffer.
13. (Decals, transparents and VFX with their own fog evaluation, then tonemap and AA.)
14. **Previous-color pyramid**: average the 2 samples into R11G11B10F mip 0, then `GenerateMips`. This is SSR input for the next frame.

### 2.4 Pixel frequency versus sample frequency under 2x MSAA

| Pass | Frequency | Why |
|---|---|---|
| GTAO, SSR trace, sky LUTs, probe relight | 1x (from sample 0) | Low-frequency or screen-space. Edge error is invisible after blur and composite. |
| Lighting incl. indirect | Pixel frequency on interior plus sample frequency on EDGE pixels (other note) | Per-sample normals at silhouettes matter for anti-aliased lighting. |
| SSS H and V | Pixel frequency; **writes per-sample via the stencil test** | SSS is a blur, so per-sample evaluation adds nothing. The stencil keeps non-skin samples of edge pixels intact. |
| Composite plus fog | Pixel frequency on interior plus sample frequency on EDGE | Must not mix albedo or fog across silhouettes. |
| Sky | Pixel frequency, stencil = sky samples | |
| Previous-color pyramid | Pixel frequency (average of samples) | SSR source only. |

This mirrors Crytek's documented deferred-MSAA scheme ([src] Sousa 2013 "CryENGINE 3 Graphics Gems", slides 5–7):

- after the G-buffer, a custom MSAA resolve stores sample 0 for pixel-frequency passes;
- a sub-sample mask goes into a reserved stencil bit, tagging whole pixel quads for better stencil culling;
- pixel-frequency passes set the stencil read mask to the per-pixel bit;
- sample-frequency passes use the per-sample bit and `SV_SampleIndex`.

Crytek also states that R11G11B10F is enough for lighting in most cases.

---

## 3. Screen-space subsurface scattering

### 3.1 What and why (sources)

- **Jimenez, Sundstedt, Gutierrez 2009**, "Screen-Space Perceptual Rendering of Human Skin", ACM TAP 6(4) [src, iryoku.com/sssss].
  - Skin pixels are tagged in stencil, and the diffuse irradiance is blurred in screen space with the d'Eon six-Gaussian skin profile, using repeated 1D passes.
  - Kernel width scales with `1/depth`, and each tap lerps back to the center color as `min(k·|z_center − z_tap|, 1)` (the follow-surface correction).
  - The earliest form of what CryEngine 3 and UE3 shipped.
- **d'Eon & Luebke 2007**, GPU Gems 3 ch. 14. The skin profile as a sum of six Gaussians with variances `0.0064, 0.0484, 0.187, 0.567, 1.99, 7.41 mm²` and red weights `0.233, 0.100, 0.118, 0.113, 0.358, 0.078`.
- **Jimenez et al. 2015**, "Separable Subsurface Scattering", CGF 34(6) [src, paper PDF].
  - Replaces the 12 1D convolutions of the sum-of-Gaussians with **one separable kernel (2 passes)**, either pre-integrated or the artist model `a(x) = w·G(x,σn) + (1−w)·G(x,σf)`. Parameters:

    | Fit | σn (RGB) | σf (RGB) | w |
    |---|---|---|---|
    | close fit | [0.034, 0.029, 0.016] | [1, 0.48, 0.2] | 0.37 |
    | production | [0.25, 0.08, 0.02] | [1, 0.3, 0.05] | 0.6 |

  - Importance-sampled offsets plus per-pixel random rotation of the filter axes. The paper reports that seven samples per pixel often suffice, at 16 instructions per sample.
  - **0.498 / 0.348 / 0.304 ms at 1080p on AMD Radeon HD 7970** for close-up, middle and long shots.
  - Jittering a radius of 10% of the kernel suffices at portrait distances; extreme close-ups need 30%.
- **Open-source reference**: github.com/iryoku/separable-sss [src]. **License: BSD 2-clause style.**
  - Retain the copyright notice in source.
  - Binary distributions must reproduce the attribution sentence given in `LICENSE.txt` (in the docs or credits).
  - The repo README says the demo predates the 2015 paper's importance sampling and jitter.
  - `SeparableSSS.h` provides a 2-pass blur with presets of 11, 17 and 25 taps (the quality levels are rated for 1080p; the 11-tap one suffices at 720p), `SSSS_FOLLOW_SURFACE`, and a shadow-map thickness transmittance function.
  - Its documentation asks for MSAA-resolved color input, and a depth resolved as a plain average to avoid silhouette artifacts.
- **Golubev 2018**, "Efficient Screen-Space Subsurface Scattering Using Burley's Normalized Diffusion in Real-Time", SIGGRAPH 2018 Advances. This is **Unity HDRP**, not Activision as the brief said [src slides].
  - Uses Burley's normalized diffusion, importance-sampled on a disk (non-separable), as a full-screen compute shader with LDS.
  - Stencil-tags the material for hierarchical-stencil tile rejection.
  - LOD: no filtering for sub-pixel disks, 21 samples for medium, 55 for large. Per-pixel random rotation.
  - Offers post-scatter or pre- and post-scatter albedo.
  - **PS4, 21 samples: 1.16 ms** for the compute pass that convolves *and* merges the diffuse and specular buffers. Relies on TAA for noise.
  - Burley profile [src Christensen & Burley 2015 talk]: `R(r) = A·s·(e^(−s·r/ℓ) + e^(−s·r/(3ℓ))) / (8π·ℓ·r)`, with `s = 1.85 − A + 7·(0.8 − A)³` (searchlight, mean free path ℓ). Its CDF for importance sampling is `1 − ¼e^(−s·r/ℓ) − ¾e^(−s·r/(3ℓ))`.
- **Crysis 3 (2013)**: no published SSS paper found. The Graphics Gems deck (Sousa 2013) covers deferred MSAA only. TI's frame analysis [TI] shows a **2-draw pixel-shader SSS** with RGBA16 output: 0.62 ms close-up, versus 0.49 ms with RG11B10 inputs.
- **Callisto Protocol (GDC 2023, Jimenez, Longhi, Contel)**: the GDC Vault talk has no public slides (members-only video). TI's analysis: UE compute SSS, RGBA16, profile-ID lookup, a half-res option blended 80% with native, and a skin-specular Gaussian blur. The SIGGRAPH 2023 Advances deck on Callisto rendering [src] covers the BRDF (dual GGX, Fresnel falloff, retroreflection) and has no SSS cost data.

### 3.2 How the video positions it

Full resolution, a pixel shader with stencil (skin only), an ID-buffer lookup per profile, and 2 draws. Ping-pong through the soon-to-be-overwritten composite RGBA16 buffer. Optionally merge with the composite. Use a 2-position 2x2 Bayer jitter if banding appears. Cost about 0.3 ms in third-person. No half res unless the upsample is ID- and depth-aware. UE does not use normals in this pass, and TI is not sure they matter.

### 3.3 Exact kernel (pre-integrated separable, Jimenez reference) [src → re-implemented]

Build the kernel on the CPU at startup, or whenever the profile changes, and upload it to a constant buffer. Each tap is `float4(weightR, weightG, weightB, offset_mm)`, with the center tap first.

```cpp
// SSS kernel generator (same math as Jimenez's SeparableSSS reference; see license note in 3.1).
// Profile: d'Eon & Luebke skin, red-channel Gaussian weights used for all channels, each channel stretched by
// 'falloff'; the narrowest Gaussian (0.0064 mm^2) is left out because 'strength' keeps that part unblurred.
#include <cmath>
#include <vector>
#include <algorithm>
struct float3_ { float x, y, z; };
struct Tap { float r, g, b, offsetMm; };

static float Gaussian(float variance, float r) {
    return std::exp(-(r * r) / (2.0f * variance)) / (2.0f * 3.14159265f * variance);
}
static float SkinProfile(float r, float falloff) {        // r in mm
    float rr = r / (0.001f + falloff);
    return 0.100f * Gaussian(0.0484f, rr) + 0.118f * Gaussian(0.187f, rr) +
           0.113f * Gaussian(0.567f,  rr) + 0.358f * Gaussian(1.99f,  rr) +
           0.078f * Gaussian(7.41f,   rr);
}
std::vector<Tap> BuildSssKernel(int numTaps /*odd: 11, 17, 25*/,
                                float3_ strength = {0.48f, 0.41f, 0.28f},
                                float3_ falloff  = {1.0f, 0.37f, 0.3f}) {
    const float range = numTaps > 20 ? 3.0f : 2.0f;            // kernel support in mm
    std::vector<Tap> k(numTaps);
    for (int i = 0; i < numTaps; ++i) {                          // quadratic spacing: dense near the centre
        float t = -range + 2.0f * range * float(i) / float(numTaps - 1);
        k[i].offsetMm = (t < 0.0f ? -1.0f : 1.0f) * t * t / range;
    }
    for (int i = 0; i < numTaps; ++i) {                          // weight = profile * width covered by the tap
        float left  = i > 0           ? std::fabs(k[i].offsetMm - k[i - 1].offsetMm) : 0.0f;
        float right = i < numTaps - 1 ? std::fabs(k[i].offsetMm - k[i + 1].offsetMm) : 0.0f;
        float area  = 0.5f * (left + right);
        k[i].r = area * SkinProfile(k[i].offsetMm, falloff.x);
        k[i].g = area * SkinProfile(k[i].offsetMm, falloff.y);
        k[i].b = area * SkinProfile(k[i].offsetMm, falloff.z);
    }
    std::rotate(k.begin(), k.begin() + numTaps / 2, k.begin() + numTaps / 2 + 1); // centre tap -> index 0
    float sr = 0, sg = 0, sb = 0;
    for (const Tap& t : k) { sr += t.r; sg += t.g; sb += t.b; }
    for (Tap& t : k) { t.r /= sr; t.g /= sg; t.b /= sb; }       // normalise: energy-conserving blur
    k[0].r = (1 - strength.x) + strength.x * k[0].r;             // blend with the unblurred image
    k[0].g = (1 - strength.y) + strength.y * k[0].g;
    k[0].b = (1 - strength.z) + strength.z * k[0].b;
    for (int i = 1; i < numTaps; ++i) { k[i].r *= strength.x; k[i].g *= strength.y; k[i].b *= strength.z; }
    return k;
}
```

**Unit test.** These values were checked numerically against the reference header presets.

| Taps | Center weight | Outermost tap |
|---|---|---|
| 11 | (0.560479, 0.669086, 0.784728) | (0.00471692, 0.000184771, 5.07566e-05) at −2 mm |
| 17 | (0.536343, 0.624624, 0.748867) | (0.00317394, 0.000134823, 3.77269e-05) at −2 mm |
| 25 | (0.530605, 0.613514, 0.739601) | (0.000973793, 1.11862e-05, 9.43437e-07) at −3 mm |

Notes:

- The 17-tap offsets are ±{0.03125, 0.125, 0.28125, 0.5, 0.78125, 1.125, 1.53125, 2} mm.
- `strength` is how much of the irradiance is scattered per channel. `falloff` is the per-channel width scale, and red scatters furthest.
- Per-profile colors (TI's "ID buffer lookup"): store 1 to 8 kernels in the cbuffer (`kernels[profile][tap]`) and index them by a profile ID from the G-buffer. A dynamic cbuffer index is fine in SM 5.0.

### 3.4 Kernel footprint, follow-surface and jitter

**Physical footprint [proposal].** The reference uses an artist `sssWidth` with ad-hoc factors. We use real units.

```
pixelsPerMeter(z) = 0.5 * P._22 * screenHeight / z          // z = linear view depth; isotropic in x and y
tapOffsetPixels   = offsetMm * 0.001 * worldUnitsPerMeter * scatterScale * pixelsPerMeter(z)
```

`scatterScale = 1` means a d'Eon-sized profile. TI thinks Callisto's scattering is too much, so start at 1 and compare against reference photos.

Example: at 1080p, a 60° vertical FOV and a face 0.5 m away, the ±2 mm support is about ±3.7 px. In a close-up at 0.25 m it is about ±7.5 px. Golubev's LOD: if `2 * tapOffsetPixels(max) < 1`, **skip the blur** (return the center). This makes distant characters free.

**Follow surface.** Per tap:

```
s = saturate(|z_tap − z_center| / depthTolerance)
color = lerp(tap, center, s)
```

Use `depthTolerance` of about 1–2 cm [est; tune]. The reference uses `saturate(300·cot(fov/2)·sssWidth·|Δz|)` in its own units.

**Taps on non-skin pixels are rejected** the same way (`s = 1`). Detect them with the stencil SRV, or with an alpha sentinel (§3.6). Without this, eyebrows, collars and stale temp data bleed in.

**Jitter.** Jimenez 2015 randomly rotates the two filter axes per pixel. With MSAA inputs (no hardware filtering) a rotated axis costs four loads per tap, so instead use **TI's 2-position 2x2 Bayer radial jitter**, which keeps the axes aligned [proposal]:

```hlsl
static const float BAYER2[4] = { 0.0, 0.5, 0.75, 0.25 };   // [[0,2],[3,1]] / 4, index (x&1) + 2*(y&1)
float Bayer2x2(int2 p, uint frame) { return frac(BAYER2[(p.x & 1) + 2 * (p.y & 1)] + 0.5 * (frame & 1)); }
// kernel scale per pixel: 1 + jitterAmount * (Bayer2x2(p, frame) - 0.375)   (jitterAmount ~ 0.15-0.3; 0 = off)
```

### 3.5 MSAA: what to read and where to write

Facts:

- MSAA textures cannot be filtered, so `Texture2DMS` only offers `Load`.
- The reference expects resolved input.
- Crytek pre-resolves sample 0 for pixel-frequency passes.
- Stencil tests run per sample.

Options considered:

1. **Average the samples** for the input. This mixes background into skin at silhouettes and produces halos. Reject it.
2. **Sample frequency everywhere** (`SV_SampleIndex`). This is 2× cost for no visible gain, because SSS is a blur.
3. **Pixel frequency with an ID-aware center and per-sample stencil writes** [proposal, recommended]:
   - **Center** = the first sample whose stencil is skin. At least one is, or the PS would not run.
   - **Taps** = sample 0, linearly interpolated between the two pixel centers along the axis, with 2 `Load`s, because sub-pixel offsets need filtering. Skin masks come from the stencil SRV.
   - **Write** goes to skin samples only, through stencil EQUAL skin. Non-skin samples of edge pixels keep their own unblurred diffuse.

### 3.6 D3D11 implementation (2 draws, TI's ping-pong)

**Draw H**: SRV `Diffuse` (2xMSAA), RTV `Composite` (2xMSAA, used as temp; its stale contents never matter).
**Draw V**: SRV `Composite`, RTV `Diffuse`.

Both draws:

- read-only DSV of the main 2xMSAA depth-stencil, plus SRVs of its depth and stencil;
- depth-stencil state: `DepthEnable=FALSE`, `DepthWriteMask=ZERO`, `StencilEnable=TRUE`, `StencilReadMask=0x47`, `StencilWriteMask=0`;
- `FrontFace = BackFace = {KEEP, KEEP, KEEP, EQUAL}`, stencil ref `0x42`;
- no blending, `CullMode=NONE`;
- a full-screen triangle from `SV_VertexID`.

Early stencil rejection holds because the shader neither writes depth nor discards.

```hlsl
// sss_blur.hlsl : one separable SSS pass (dispatch twice: g_Axis = (1,0) then (0,1)). Pixel frequency. SM 5.0.
#include "common.hlsli"   // g_ProjParams (x=P._11, y=P._22, z=near, w=far), g_ScreenSize, uint g_FrameIndex,
                          // Bayer2x2() from section 3.4

Texture2DMS<float4, 2> g_Src     : register(t0);  // H: diffuse lighting   V: result of H (in the composite buffer)
Texture2DMS<float,  2> g_Depth   : register(t1);  // device depth
Texture2DMS<uint2,  2> g_Stencil : register(t2);  // .y = stencil

cbuffer SssCB : register(b1)
{
    float4 g_Kernel[25];        // rgb = weights, w = offset in mm; centre first
    uint   g_NumTaps;           // 11 or 17 (25 = reference)
    float2 g_Axis;              // (1,0) or (0,1)
    float  g_WorldUnitsPerMm;   // 0.001 for metres
    float  g_ScatterScale;      // 1 = physically sized profile
    float  g_InvDepthTolerance; // 1 / (~0.015 m)
    float  g_JitterAmount;      // 0 = off
};

float LinearViewZ(float d) { float n = g_ProjParams.z, f = g_ProjParams.w; return n * f / (f - d * (f - n)); } // standard Z
// reversed Z: n * f / (n + d * (f - n))

bool IsSkin(int2 p, int s)
{
    return (g_Stencil.Load(p, s).y & 0x47u) == 0x42u;   // GEOMETRY | MODEL_SKIN
}

// Sample 0 of two neighbouring pixels along the axis, interpolated; rejects non-skin texels.
bool FetchTap(float2 centrePix, float offsetPx, out float3 color, out float viewZ)
{
    float  fl = floor(offsetPx);
    float  a  = offsetPx - fl;
    int2   p0 = int2(centrePix + g_Axis * fl);
    int2   p1 = p0 + int2(g_Axis);
    float  w0 = (1.0 - a) * (IsSkin(p0, 0) ? 1.0 : 0.0);
    float  w1 = a         * (IsSkin(p1, 0) ? 1.0 : 0.0);
    float  ws = w0 + w1;
    color = (w0 * g_Src.Load(p0, 0).rgb + w1 * g_Src.Load(p1, 0).rgb) / max(ws, 1e-5);
    viewZ = LinearViewZ(g_Depth.Load(w0 >= w1 ? p0 : p1, 0));
    return ws > 0.0;
}

float4 PS_SssBlur(float4 svpos : SV_Position) : SV_Target
{
    int2  pc = int2(svpos.xy);
    int   sc = IsSkin(pc, 0) ? 0 : 1;                     // first skin sample (edge pixels)
    float3 centre = g_Src.Load(pc, sc).rgb;
    float  zc     = LinearViewZ(g_Depth.Load(pc, sc));

    float pxPerMm = g_WorldUnitsPerMm * g_ScatterScale * 0.5 * g_ProjParams.y * g_ScreenSize.y / zc;
    pxPerMm *= 1.0 + g_JitterAmount * (Bayer2x2(pc, g_FrameIndex) - 0.375);   // 2-position 2x2 Bayer, section 3.4
    if (abs(g_Kernel[g_NumTaps - 1].w) * pxPerMm < 0.5)    // sub-pixel footprint: nothing to scatter
        return float4(centre, 0);

    float3 sum = centre * g_Kernel[0].rgb;
    [loop] for (uint i = 1; i < g_NumTaps; ++i)
    {
        float3 c; float z;
        bool   ok = FetchTap(svpos.xy, g_Kernel[i].w * pxPerMm, c, z);
        float  s  = ok ? saturate(abs(z - zc) * g_InvDepthTolerance) : 1.0;  // follow surface / reject
        sum += g_Kernel[i].rgb * lerp(c, centre, s);
    }
    return float4(sum, 0);
}
```

Optimizations, to be A/B-tested:

- **(a) Alpha sentinel instead of the stencil SRV in the tap loop.**
  - The lighting writes `Diffuse.a = −1` for skin and `0` otherwise. Draw H writes `−1` into the temp alpha. A tap is skin if `alpha < −0.5`.
  - This cuts 2 stencil loads per tap. `−1` can never come from the responsive-mask alpha that VFX write later into the composite buffer.
- **(b) Use the 1x linear-depth texture** from the G-buffer resolve instead of linearizing MSAA depth per tap.
- **(c) Option C, a "merge with composite" variant.**
  - Draw H writes a **1x** RGBA16F temp, which needs a 1x skin stencil built in the G-buffer resolve with a discard plus stencil REPLACE draw.
  - Draw V reads that temp with **hardware bilinear**, 1 sample per tap, and outputs the *final composite* for skin samples: `blurredDiffuse·albedo + smoothedSpecular`, with fog.
  - The main composite then skips skin. This saves one full read and write over skin pixels, at the cost of 16.6 MB and a resolve draw. This is TI's suggestion of merging SSS with the next stage.

### 3.7 Inputs that make SSS correct

- **Post-scatter texturing.** The Diffuse buffer holds lighting *without* albedo, and the composite multiplies the albedo afterwards. This preserves albedo detail (Golubev recommends post-scatter for scanned albedo) and matches Crysis 3.
  - An optional pre- and post-scatter variant multiplies `sqrt(albedo)` before and after.
- **Indirect diffuse must be in the Diffuse buffer before SSS.** The probe GI is added in the sun/ambient draw (§2.3), so skin ambient also scatters.
- **Translucency of thin regions** (ears, nostrils) is optional. Evaluate it in the skin lighting draw (Jimenez 2010, reference `SSSSTransmittance`):
  - Thickness `d = scale·|z_shadowmap − z_receiver|` with `scale = 8.25·(1 − translucency)/width`, after shrinking the position 0.005 units along −N.
  - `T(d) = Σ_i w_i·exp(−d²/v_i)` using the d'Eon RGB weights (variances 0.0064…7.41).
  - Contribution: `albedo·lightColor·atten·T(d)·saturate(0.3 + dot(L, −N))`.
- **Skin specular smoothing** (TI/Callisto, undocumented) is experimental. In the composite for skin samples, use a 5-tap cross of the specular buffer (sample 0, ±1 px, Gaussian weights such as 0.4/0.15/0.15/0.15/0.15), with the same depth and skin rejection.

### 3.8 Half-resolution variant (only as a scalability A/B)

Callisto's approach [TI]: blur at half res, then in the resolve `final = lerp(nativeDiffuse, upsampledBlur, 0.8)`.

TI requires a **depth- and ID-aware upsample**:

- For each full-res sample, take the 4 nearest half-res texels.
- Weight each as `bilinear × [ID equal] × exp(−|Δz|/σz)`.
- If the weights sum to near zero, fall back to native.

TI says this should reuse the half-res min/max depth resources of the VFX stage.

### 3.9 Pitfalls

- The reference header warns that blurring in non-linear (gamma) space ruins SSS. Our buffers are linear RGBA16F.
- The kernel axes become visible as star or cross patterns under very sharp light (Jimenez 2015, fig. 10). Jitter helps.
- Stale temp contents: always reject non-skin taps.
- Edge pixels: never take the center from sample 0 blindly.
- Very large footprints thrash the cache (Golubev). Clamp `pxPerMm`, for example to a max footprint of 40 px.

### 3.10 Expected cost and how to measure

**[est]:**

- 17 taps × (2 color + 2 stencil + 1 depth) loads per skin pixel, two passes, written only to skin samples.
- Third-person (5–10% skin coverage): 0.05–0.15 ms.
- Full-screen face: 0.3–0.5 ms.
- Compare Jimenez's 0.5 ms close-up on an HD 7970 (about 4–5× slower than an RTX 5060 in FP32, about 1.7× lower bandwidth), and TI's 0.23–0.39 ms medium shot for Callisto.

Measure with GPU timestamps around the H+V pair, and use `D3D11_QUERY_PIPELINE_STATISTICS.PSInvocations` to prove the stencil culling (§10).

---

## 4. Diffuse and specular composition (with fog)

**What and why.** Lighting is accumulated in separate diffuse (no albedo) and specular buffers only so that SSS can blur standalone diffuse. The composite folds them into one HDR buffer. Crysis 3 kept diffuse and specular in R11G11B10F and composed into RGBA16 with base color [TI]. Crytek's slide says R11G11B10F suffices for lighting in most cases [src].

TI wants RGBA16F everywhere until that is validated. **v1: RGBA16F**, with RG11B10F as an A/B.

**Algorithm** [proposal]:

```hlsl
// composite.hlsl : Draw A = interior pixels (pixel frequency, stencil mask 0xC0 ref 0x40);
//                  Draw B = edge pixels (sample frequency via SV_SampleIndex, stencil mask 0xC0 ref 0xC0).
Texture2DMS<float4, 2> g_Diffuse, g_Specular;
Texture2DMS<float4, 2> g_BaseColor;     // G-buffer RGBA8_UNORM_SRGB view -> linear on load; a = metalness (example)
Texture2DMS<float,  2> g_Depth;

float4 Composite(int2 p, uint s, float2 pixelPos)
{
    float3 diff  = g_Diffuse.Load(p, s).rgb;           // already includes SSS for skin
    float3 spec  = g_Specular.Load(p, s).rgb;
    float4 bc    = g_BaseColor.Load(p, s);
    float3 albedo = bc.rgb * (1.0 - bc.a);             // diffuse albedo; metals -> 0
    float3 lit   = diff * albedo + spec;

    float3 wpos  = WorldPosFromDepth(pixelPos, g_Depth.Load(p, s));
    return float4(ApplyHeightFog(lit, wpos), 0);       // section 5
}
float4 PS_CompositePixel (float4 pos : SV_Position) : SV_Target { return Composite(int2(pos.xy), 0, pos.xy); }
float4 PS_CompositeSample(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target { return Composite(int2(pos.xy), s, pos.xy); }
```

Details:

- Sky samples are skipped via stencil. The sky pass writes them.
- No clear: every sample of the composite buffer is written by either the composite or the sky.
- Skin specular smoothing (§3.7) goes in this pass, behind a branch or a separate stencil-selected draw (TI is unsure stencil-splitting by shading model is worth it).
- If TI's simple SSR for Fresnel-anti-aliased edges moves here, the composite also needs normal, roughness and F0. v1 keeps SSR in the lighting draw (§2.3).

**Cost [est].** Interior reads sample 0 of 3 MSAA buffers plus depth, about 28 B in and 16 B out per pixel, roughly 90 MB at 1080p. That is about 0.1–0.15 ms at 448 GB/s, and MSAA color compression lowers it further. TI's budget is 0.12 ms.

Running everything at sample frequency doubles PS invocations and loads. This is A/B #C2.

**Pitfall.** The base color must be loaded from the same sample index as diffuse and specular, or silhouettes get albedo from the wrong surface.

---

## 5. Fog: analytic exponential height fog in the composite

**Sources.**

- **Wenzel 2006**, "Real-time Atmospheric Effects in Games", SIGGRAPH 2006 course, ch. 6.4 [src]. CryEngine 2 global volumetric fog with density `f(z) = b·e^(−c·z)`, integrated analytically along the view ray. The fog color is matched to the average sky horizon color so geometry blends into the sky.
- **Quilez, "Better fog"** (iquilezles.org) [src]. The same integral, plus a sun-directional inscatter lobe.
- **UE ExponentialHeightFog** [src via Flax Engine's `ExponentialHeightFog.hlsl`]:
  - The same math in base 2: `exp2`, `(1 − 2^(−f))/f` with a Taylor branch for |f| < 0.01, and a falloff clamped at −127.
  - User parameters are internally divided by 1000.
  - `FogAtViewPosition = density·2^(−falloff·(camY − fogHeight))`.
  - Extras: start distance, cutoff distance, max opacity, and directional inscattering `color·pow(saturate(dot(V, L)), exp)` with its own start distance.

**Derivation** (natural-exp form) [src math, own derivation].

Density is `σ(h) = a·e^(−b(h − h0))`, where a is the density at height h0 [1/m] and b is the falloff [1/m]. The camera is at c, the shaded point is at p, `v = p − c` and `D = |v|`.

```
τ = ∫0..D a·e^(−b(c_y + s·v_y/D − h0)) ds = a·e^(−b(c_y − h0)) · D · (1 − e^(−k)) / k ,   k = b·v_y
(1 − e^(−k))/k ≈ 1 − k/2   for |k| < 1e-3
T = e^(−τ)         L = L_surface·T + L_in·(1 − T)
```

```hlsl
cbuffer FogCB { float g_FogDensity; float g_FogFalloff; float g_FogHeight; float g_FogMaxOpacity;
                float3 g_SunDir; float g_SunLobeExponent; float3 g_SunInscatter; float g_FogStartDistance; };
Texture2D<float3> g_SkyViewLut; SamplerState g_LinearClamp;

float3 ApplyHeightFog(float3 lit, float3 wpos)
{
    float3 v = wpos - g_CameraPos;
    float  D = length(v);
    float  dEff = max(D - g_FogStartDistance, 0.0);               // simple start distance
    float  k = max(g_FogFalloff * v.y, -80.0);                    // keep exp() finite
    float  shape = abs(k) > 1e-3 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k;
    float  tau = g_FogDensity * exp(-g_FogFalloff * (g_CameraPos.y - g_FogHeight)) * dEff * shape;
    float  T = max(exp(-tau), 1.0 - g_FogMaxOpacity);

    float3 V = v / D;
    float3 Lin = SampleSkyHorizon(V)                                  // Wenzel: match the sky at the horizon
               + g_SunInscatter * pow(saturate(dot(V, g_SunDir)), g_SunLobeExponent); // UE-style sun lobe
    return lit * T + Lin * (1.0 - T);
}
```

`SampleSkyHorizon(V)` is one bilinear fetch of the sky-view LUT (§8) at the view azimuth and elevation 0. It makes the fogged geometry converge on the actual sky color, which removes the horizon seam that otherwise appears because sky pixels are not fogged.

A more physical alternative: `Lin = fogAlbedo·(E_sun·HG(dot(V,L), g)·T_atm + E_skyAmbient/(4π))`.

Transparents call the same function in their own pixel shaders.

**How the video positions it.** Merge fog into the composite. Fog atlases are an alternative, but TI suspects an ALU approach is better.

**Cost.** Pure ALU (one exp, one division, one LUT fetch) inside a bandwidth-bound pass, so about 0.01 ms [est]. As a separate pass it would cost an extra read-modify-write of the 33 MB 2xMSAA buffer, about 0.1–0.15 ms [est]. That is A/B #F1.

**Pitfalls.**

- Without the clamp, `exp(−k)` overflows for steep downward rays.
- Do not fog the sky pixels a second time, because the sky LUT already contains atmospheric scattering.
- With large `b`, the fog layer's top becomes visible when the camera crosses h0. That is expected.

---

## 6. Global illumination (noiseless, cheap, simple)

### 6.1 Background and positioning

- **RSMs (Dachsbacher & Stamminger 2005).** RSM plus LPV or similar. TI says **don't**: 0.3–0.6 ms per frame, a visually poor result in Crysis 3 and Tesseract [TI].
- **The Division (Stefanov, GDC 2016)** [src for the abstract only; slides not public]. Radiance transfer probes give real-time bounce lighting from fully dynamic lights on consoles and PC, with indoor probe bleeding handled.
  - The predecessor, Far Cry 3's Deferred Radiance Transfer Volumes (GDC 2012): a sparse volume of probes storing **SH transfer matrices** that are relit on the fly for time-of-day and local lights, with shading in screen space.
  - My understanding of the 2016 system (not re-verified, slides inaccessible): baked surface points (surfels) per probe are relit each frame from shadow maps, sky and lights, then gathered into probe irradiance. That is exactly TI's sensor design.
- **Days Gone** [TI]: baked local probes are updated from shadow-map data by compute shaders in about 0.3 ms.
- **Fox Engine / MGSV** [TI]: baked probes, small GI atlases, and micro 30×30 orthographic shadow views toward irradiance sensors.
- **Hooker 2016, "Volumetric Global Illumination at Treyarch"** (SIGGRAPH Advances, Black Ops 3) [src]:
  - Irradiance volumes in **ambient cube** format (Valve, McTaggart 2004). Of the six colors only three are sampled per normal, which the talk says is cheaper than 4-coefficient SH with more precision. Three `Texture3D`s with the + and − halves side by side, using hardware trilinear filtering. Evaluation is `Σ n_i²·sample_i`.
  - Voxels are baked by reprojecting the reflection-probe cubemaps.
  - Leak prevention:
    - Normal-offset sampling (Silvennoinen 2015) was *not* enough for them, so they added artist-placed convex **attenuation volumes**.
    - Voxels that see a backface or caulk geometry are ignored at bake and in-painted from valid neighbors.
    - Sample points are pushed away from geometry before sampling.
  - Resolution was about 1 voxel per 8 inches. Cost: under 2 ms for reflections and GI inside a 4 ms deferred shader on PS4.
- **DDGI (Majercik et al. 2019, JCGT 8(2))** [src]:
  - Probe grids with octahedral irradiance (8×8 interior texels) and mean and mean² depth (16×16, RG16F).
  - The robust interpolant, verified in RTXGI `Irradiance.hlsl`:
    - biased position `P + N·normalBias − V·viewBias` (V here points from the camera);
    - per-probe weight `((dot(dirToProbe, N) + 1)/2)² + 0.2`;
    - Chebyshev visibility `σ²/(σ² + (r − μ)²)`, cubed, with a floor of 0.05;
    - a crush of weights below 0.2 (`w *= w²/0.04`);
    - multiply by trilinear and renormalize.
  - It needs RT to update, but its *offline/editor* workflow is what TI cites. RTX 2080 Ti, 1080p: 6 ms per frame for the whole method.
- **The video's goal** [TI]: layered noiseless GI under 1 ms world space plus 1 ms screen space; static and dynamic lit identically; editor dynamic and ship baked; shadow maps first, world-space fallback.

### 6.2 Recommendation v1: GPU-baked ambient-cube probe grid [proposal]

**Why this one.** It is noiseless by construction, because nothing is stochastic at runtime. The runtime cost is about 3–6 trilinear fetches inside an existing full-screen draw. It is D3D11-native (rasterized cube captures, no RT), and the same capture step is the foundation of v2.

**Grid.**

- An axis-aligned box over the playable volume, with spacing of 1–2 m for a demo (e.g. 32×8×32 at 1.5 m). Probe `i` sits at `gridMin + i·spacing`.
- Storage: 6 `Texture3D` of size Nx×Ny×Nz, in R11G11B10F or RGBA16F. Each holds **irradiance E** for one axis direction (+X, −X, +Y, −Y, +Z, −Z).

**Bake procedure (at load or on demand; cache to disk):**

1. **Capture.** For each probe, render a cube with 16×16 per face. For each face:
   - Camera at the probe, 90° FOV, near 0.05 m, D3D cube face orientations.
   - Draw opaque static geometry with a simple forward bake shader:
     - `L = emissive + albedo/π · (E_sun·max(0, N·L)·shadow + E_prev(P, N))`.
     - `E_prev` is the previous bounce's grid, sampled with the runtime function below. It is zero on bounce 0.
     - Shadows come from a whole-level sun shadow map rendered once for the bake.
   - Draw the sky into the background from the sky-view LUT. Sky light is then included automatically.
   - Also output a **backface flag**: cull NONE, and the PS writes `!SV_IsFrontFace` to a second R8 target.
   - Batching: a geometry shader with `SV_RenderTargetArrayIndex` renders the 6 faces in one draw. On D3D11.3 hardware with `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`, use VS instancing instead.
2. **Project to the ambient cube.** Compute: one thread group per probe, 64 threads, a groupshared reduction over 6·16² texels.

   ```hlsl
   // texel (x,y) of face f at resolution R:  u = (x+0.5)/R*2-1,  v = (y+0.5)/R*2-1  (v down)
   // D3D cube directions: +X ( 1,-v,-u)  -X (-1,-v, u)  +Y ( u, 1, v)  -Y ( u,-1,-v)  +Z ( u,-v, 1)  -Z (-u,-v,-1)
   // solid angle: dw = (2/R)^2 / (1 + u*u + v*v)^1.5
   // E_axis = sum over texels of L(w) * max(0, dot(w, axis)) * dw         (6 axes; each texel feeds <= 3 of them)
   // validity: backRatio = sum(isBackface * dw) / (4*pi)
   ```

3. **Iterate bounces.** Repeat steps 1 and 2 three times, ping-ponging the grid. That gives TI's 3+ bounces.
4. **Fix invalid probes.** Probes with `backRatio > 0.25` sit inside geometry (DDGI uses a similar classification; Hooker skips voxels that see backfaces). Overwrite them with the average of valid 6-neighbors, and repeat until filled (Hooker's in-painting). Do this after **every** bounce, before the next bounce samples the grid, so inside-geometry probes never feed the next bounce.
5. **Upload** the 6 grids as `Texture3D` (or keep the UAV results).

**Bake cost [est].**

- Example: 32×8×32 = 8192 probes × 6 faces × 3 bounces.
- Raster time is trivial. CPU draw-call overhead dominates: a 300-draw scene × 8192 probes ≈ 2.5 M draws per bounce with the GS 6-face path, around 3–5 s per bounce in D3D11.
- Mitigations:
  - merge static geometry per material for the bake (tens of draws, so under 1 s);
  - use coarser spacing outdoors;
  - cache the result.
- The projection compute is under 10 ms total.

**Runtime sampling** (inside the per-shading-model sun/ambient draw, so static and dynamic objects get the same GI):

```hlsl
Texture3D<float3> g_AmbXP, g_AmbXN, g_AmbYP, g_AmbYN, g_AmbZP, g_AmbZN;
cbuffer GiCB { float3 g_GridMin; float g_ProbeSpacing; float3 g_InvGridCount; float g_NormalBias; float g_ViewBias; };

float3 ProbeIrradiance(float3 P, float3 N, float3 toCamera)
{
    // push the lookup point off the surface (DDGI/Silvennoinen-style bias; ~0.2-0.5 x spacing)
    float3 Pb  = P + (N * g_NormalBias + toCamera * g_ViewBias) * g_ProbeSpacing;
    float3 uvw = ((Pb - g_GridMin) / g_ProbeSpacing + 0.5) * g_InvGridCount;   // probe centres at texel centres
    float3 n2  = N * N;
    float3 ex  = N.x >= 0 ? g_AmbXP.SampleLevel(g_LinearClamp, uvw, 0) : g_AmbXN.SampleLevel(g_LinearClamp, uvw, 0);
    float3 ey  = N.y >= 0 ? g_AmbYP.SampleLevel(g_LinearClamp, uvw, 0) : g_AmbYN.SampleLevel(g_LinearClamp, uvw, 0);
    float3 ez  = N.z >= 0 ? g_AmbZP.SampleLevel(g_LinearClamp, uvw, 0) : g_AmbZN.SampleLevel(g_LinearClamp, uvw, 0);
    return n2.x * ex + n2.y * ey + n2.z * ez;                                   // irradiance E(N)
}
// in the sun/ambient draw (diffuse buffer stores lighting without albedo):
//   diffuse += (1/PI) * ProbeIrradiance(P, N, V) * GTAOMultiBounce(ao, albedo)   (section 7)
// outside the grid: fall back to the sky ambient cube (section 8.4)
```

Both selection branches may compile to two samples each. That is fine, because the tiny 3D textures stay in cache.

L1 SH in 3 RGBA16F textures (`E(n) = π·c0·Y00 + (2π/3)·Σ c1m·Y1m(n)`) needs 3 fetches and no select, but can ring negative under a strong sun. Keep it as A/B #G3.

**Leak policy for v1:**

1. Detect and dilate invalid probes (bake).
2. Normal and view bias (runtime).
3. Choose a probe spacing smaller than wall thickness × 2 where it matters.
4. Optional: Hooker-style axis-aligned attenuation boxes that scale the GI to 0 outside a room.

Escalate to DDGI-style per-probe visibility only if the leak test scene still fails. Visibility means a 16×16 octahedral mean and mean² depth per probe, captured from the same cube renders, plus a manual 8-probe interpolation using the RTXGI weights above. Escalating loses hardware trilinear filtering: 8 probes × (1 depth + 3 irradiance) fetches.

### 6.3 Recommendation v2: shadow-map-driven relighting (shadow maps first) [proposal]

This makes the v1 grid react to a moving sun, time of day and lights without re-rendering captures. It is a simplified radiance-transfer-probe scheme (Far Cry 3 / The Division lineage) and TI's irradiance sensors.

1. **At bake**, from a low-res capture (8×8 per face = 384 texels per probe, or 4×4 for 96), store one **sensor** per texel in a `StructuredBuffer`:
   - world position (probe position + direction × hit distance);
   - normal (oct16);
   - albedo (RGBA8);
   - solid angle `dω`;
   - direction;
   - an `isSky` flag.

   TI's diagram describes per-sensor data as a dominant unoccluded direction plus an average albedo, which is the same idea, clustered.
2. **Each frame** (or a quarter of the probes per frame), a compute pass with one group per probe:

   ```
   for each sensor s of probe p:
     L_s = isSky ? SkyRadiance(dir_s)                                             // sky-view LUT
                 : albedo_s/π · ( E_sun · max(0, n_s·l) · ShadowVis(pos_s)           // <- shadow-map tracing
                                 + ProbeIrradiance_prevFrame(pos_s, n_s)            // multi-bounce feedback
                                 + Σ localLights ) + emissive_s
     E_axis(p) += L_s · max(0, dir_s·axis) · dω_s                                   // 6 axes
   ```

   `ShadowVis` tries the camera cascades first, then a **static whole-level GI shadow map** (rendered only when the sun moves). Sensors outside both keep their last value. That is TI's world-space fallback, implemented without RT. TI's sampling of several positions around a sensor maps to a 4-tap PCF here.
3. Bounce feedback from last frame's grid converges in a few frames. It is world-space and deterministic, so it is noiseless. The lag is about (bounces × update period) under sudden light changes.

**Cost [est].** 8192 probes × 96 sensors is about 0.8 M sensor evaluations (one PCF fetch plus 3 grid fetches each), roughly 0.1–0.2 ms. Time-sliced over 4 frames, about 0.03–0.05 ms. Memory is about 25 MB at 32 B per sensor.

Compare Days Gone's 0.3 ms [TI] and TI's target of under 0.5 ms at 1080p for GI.

### 6.4 Large-scale occlusion and dynamic objects

- **Static large-scale occlusion** is inherent in the bake: sky visibility and bounce are occluded by static geometry.
- **Dynamic objects** do not occlude the grid. GTAO covers the near field (§7). Characters could later add analytic capsule or sphere occluders (the indirect SDF shadows in TI's diagram), but that is out of scope for v1.
- **SSGI** (driven by the lit scene, not the albedo buffer [TI]; 0.55 ms in Days Gone) is out of scope for v1.
  - If added, gather previous-frame lit radiance along the GTAO horizons. "Screen Space Indirect Lighting with Visibility Bitmask" (Therrien et al. 2023) is a GTAO-compatible formulation.
  - It must be spatially denoised only.

### 6.5 Pitfalls

- Light leaks through thin walls, so use the leak test room in §10.
- Seams where grids meet; use one grid for the demo.
- Probes inside geometry produce black splotches, so dilate them.
- Double occlusion: the bake already contains static occlusion, so do not multiply the grid by baked AO again. Frostbite notes the same for their radiosity AO [src Lagarde & de Rousiers 2014 fn. 49]. Screen-space AO on top is fine because it captures near-field and dynamic occlusion; take `min` with any baked AO, as GTAO §4.3 does.
- AO must never darken **direct** light [TI; Frostbite occlusion table agrees].

---

## 7. Ambient occlusion: GTAO, spatial-only and noiseless

**Sources.**

- **Jimenez, Wu, Pesce, Jarabo 2016**, "Practical Real-Time Strategies for Accurate Indirect Occlusion", Activision tech report ATVI-TR-16-01 / SIGGRAPH 2016 PBS [src].
  - Horizon-based, radiometrically correct (cosine-weighted) AO with an analytic inner integral.
  - Multi-bounce from AO and albedo, plus GTSO specular occlusion and bent normals.
  - **0.5 ms on PS4 at 1080p with a half-res AO buffer**: 1 direction (12 steps) per pixel, 4×4 spatial noise, 4×4 bilateral, and 6 temporal rotations, giving 96 effective directions.
- **XeGTAO** (Intel, MIT license) [src README]:
  - Full res by default: 3 slices × 3 steps per side (18 spp), or 2×2 in the lower preset.
  - **5×5 depth-aware spatial denoiser that works without TAA**.
  - Depth MIP chain (as in McGuire SAO 2012) and a pow-2 sample distribution.
  - **0.56 ms at 1080p on RTX 2060 (High)**, 1.4 ms at 4K on RTX 3070, 2.2 ms (High) / 1.5 ms (Medium) on GTX 1050. Bent normals add about 25%.
- **SSDO** (Ritschel, Grosch, Seidel 2009, I3D): directional occlusion with one indirect bounce in image space. TI's TAA-independent directional SSAO is satisfied by GTAO plus bent normals.
- **TI positioning** [TI]: 0.40 ms probe-guided SSAO; avoid CIAO because of halos; never on sky or direct lighting; ALU Bayer pattern for non-temporal effects.

### 7.1 Algorithm (per pixel, view space, LH +z forward)

For each slice angle φ, the screen direction is ω = (cos φ, sin φ). The view-space direction is `dirV = (ω.x, −ω.y, 0)` because screen y points down.

```
V      = normalize(−P)                         axis = cross(dirV, V)
projN  = N − axis·dot(N, axis)                 cosN = saturate(dot(projN, V)/|projN|)
n      = sign(dot(dirV − dot(dirV,V)·V, projN)) · acos(cosN)
horizon search per side (side −1, +1): c = max over samples of dot(normalize(S − P), V), init −1,
       with near-field falloff lerping each sample's cosine toward −1
h(−1) = n + clamp(−acos(c−) − n, −π/2, π/2)     h(+1) = n + clamp(+acos(c+) − n, −π/2, π/2)
â     = ¼·(−cos(2h − n) + cosN + 2h·sin n)  for each side         A += |projN|·(â(−1) + â(+1))
A    /= sliceCount
```

This is paper equation 7 with Algorithm 1 [src].

```hlsl
// gtao.hlsl  (pixel frequency, 1x R8 output). Inputs: 1x linear view depth (sample 0) + 1x view normal.
static const float PI = 3.14159265;
static const float BAYER4[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };   // /16
Texture2D<float> g_ViewZ; Texture2D<float4> g_NormalVS;
cbuffer AoCB { float g_Radius; float g_FalloffRange; float g_MaxRadiusPx; uint g_SliceCount; uint g_StepsPerSide; };

float3 ViewPos(float2 pix, float z)
{
    float2 ndc = float2(pix.x * g_InvScreenSize.x * 2 - 1, 1 - pix.y * g_InvScreenSize.y * 2);
    return float3(ndc.x * z / g_ProjParams.x, ndc.y * z / g_ProjParams.y, z);
}

float PS_GTAO(float4 svpos : SV_Position) : SV_Target
{
    int2   pix = int2(svpos.xy);
    float  z   = g_ViewZ.Load(int3(pix, 0));
    float3 P   = ViewPos(svpos.xy, z);
    float3 N   = normalize(g_NormalVS.Load(int3(pix, 0)).xyz);
    float3 V   = normalize(-P);
    float  rPx = min(g_Radius * 0.5 * g_ProjParams.y * g_ScreenSize.y / z, g_MaxRadiusPx);
    if (rPx < 1.0) return 1.0;

    float noise = BAYER4[(pix.x & 3) + 4 * (pix.y & 3)] / 16.0;   // slice rotation per 4x4 tile
    float stepJ = frac(noise * 7.0 + 0.5);                        // decorrelated step offset (deterministic)
    float vis = 0;
    [loop] for (uint s = 0; s < g_SliceCount; ++s)
    {
        float  phi   = (s + noise) * PI / g_SliceCount;
        float2 omega = float2(cos(phi), sin(phi));
        float3 dirV  = float3(omega.x, -omega.y, 0);
        float3 axis  = cross(dirV, V);
        float3 projN = N - axis * dot(N, axis);
        float  pl    = max(length(projN), 1e-4);
        float  cosN  = saturate(dot(projN, V) / pl);
        float  n     = sign(dot(dirV - dot(dirV, V) * V, projN)) * acos(cosN);

        float hc[2] = { -1.0, -1.0 };
        [unroll] for (int side = 0; side < 2; ++side)
        {
            float sgn = side == 0 ? -1.0 : 1.0;
            [loop] for (uint k = 0; k < g_StepsPerSide; ++k)
            {
                float  t    = (k + stepJ) / g_StepsPerSide;  t *= t;                 // denser near centre
                float2 sPix = floor(svpos.xy + sgn * omega * max(t * rPx, k + 1.0)) + 0.5;
                float  sz   = g_ViewZ.SampleLevel(g_PointClamp, sPix * g_InvScreenSize, 0);
                float3 d    = ViewPos(sPix, sz) - P;
                float  len  = length(d);
                float  c    = dot(d / len, V);
                c = lerp(-1.0, c, saturate((g_Radius - len) / g_FalloffRange));      // near-field falloff
                hc[side] = max(hc[side], c);
            }
        }
        float h0 = n + clamp(-acos(hc[0]) - n, -0.5 * PI, 0.5 * PI);
        float h1 = n + clamp( acos(hc[1]) - n, -0.5 * PI, 0.5 * PI);
        float sn = sin(n);
        vis += pl * 0.25 * ((cosN + 2 * h0 * sn - cos(2 * h0 - n)) + (cosN + 2 * h1 * sn - cos(2 * h1 - n)));
    }
    return saturate(vis / g_SliceCount);
}
```

**Denoise (spatial only)** [proposal].

- Use a 4×4 window (offsets −2..+1). Any contiguous 4×4 window contains each of the 16 Bayer rotations exactly once, so 2 slices × 16 rotations give 32 effective directions with **no temporal history**. This is the spatial half of the GTAO paper's scheme.
- Weight each neighbor by plane distance: `w = saturate(1 − |dot(N_c, P_q − P_c)| / (0.02·z_c + 0.01))`.
- Optional two-position variant: alternate the Bayer offset by +8/16 on odd frames, so 60 fps persistence doubles the effective directions. That is TI's trick, and it needs no history buffer.

**Defaults.**

- Radius 0.75 m (near field only; the probes carry the far field).
- Falloff range 0.25 m, max radius 64 px.
- Slices 2 (3 for "High"), steps per side 4.

**Apply to indirect only.**

```hlsl
float3 GTAOMultiBounce(float ao, float3 albedo)            // GTAO paper eq. 33 (cubic fit, 3-bounce reference)
{
    float3 a =  2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c =  2.7552 * albedo + 0.6903;
    return max(ao, ((ao * a + b) * ao + c) * ao);
}
float SpecularOcclusion(float NoV, float ao, float roughness)   // Lagarde & de Rousiers 2014
{
    return saturate(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}
```

Bent normals (Algorithm 2) can come later for directional probe lookups, at about +25% cost.

**Half res** (A/B #A2): trace at 960×540 on the min-depth or sample-0 downsample, denoise, then do a bilateral 4-tap depth-aware upsample. That is the original GTAO's choice and XeGTAO's FAQ suggestion.

**Cost [est].** Full res with 2 slices × 8 samples plus the denoise: ~0.25–0.35 ms on RTX 5060, extrapolating XeGTAO's 0.56 ms on RTX 2060 with 18 spp. Half res: ~0.1–0.15 ms. TI's budget is 0.40 ms.

**Pitfalls.**

- Thin occluders over-occlude; the height-field assumption is inherent. Keep the radius small.
- Halos at depth discontinuities if the denoiser is not depth-aware.
- Skip sky pixels: geometry stencil, or `z ≥ far`.
- Use sample 0 consistently for depth and normal.

---

## 8. Reflections: SSR first, then cubemaps, then sky

**Sources.**

- **McGuire & Mara 2014**, "Efficient GPU Screen-Space Ray Tracing", JCGT 3(4) [src]:
  - Perspective-correct 2D DDA. It interpolates `k = 1/w` and `Q = P·k` linearly in screen space, iterates over the major axis, and swaps axes if |Δy| > |Δx|.
  - Clips to the near plane, uses a **zThickness** voxel model, and has `stride` and `jitter` parameters with optional binary refinement.
  - **1.2 ms at 1080p on a GeForce Titan (Kepler) for 25 iterations at every pixel**, 6.4 ms on a 650M.
  - Recommends stride > 1 only for rough surfaces.
- **Uludag 2014**, "Hi-Z Screen-Space Cone-Traced Reflections", GPU Pro 5:
  - A min-Z pyramid traversal (fewer steps) plus cone tracing over a blurred color mip chain for glossy reflections.
  - Cone angle from the specular power with lobe constant u = 0.244 (cited in GTAO Appendix C [src]).
- **Lagarde & Zanuttini 2012**, "Local Image-based Lighting With Parallax-corrected Cubemap" (SIGGRAPH 2012 talk and blog) [src]. A box proxy and ray-box intersection to correct the lookup direction.
- **Karis 2013**, "Real Shading in Unreal Engine 4" [src]:
  - Split-sum: `∫L·f·cos ≈ (prefiltered L at roughness) × (F0·A + B)`.
  - Prefiltering uses GGX importance sampling under the n = v = r assumption.
  - A 2D LUT for (A, B) over (NoV, roughness).
- **Karis 2014** "Physically Based Shading on Mobile" [src]: the analytic `EnvBRDFApprox` (constants c0 = (−1, −0.0275, −0.572, 0.022), c1 = (1, 0.0425, 1.04, −0.04), `a004 = min(r.x², exp2(−9.28·NoV))·r.x + r.y`, `AB = (−1.04, 1.04)·a004 + r.zw`).
- **TI positioning**: non-RT first; stochastic SSR with blue noise plus one-frame temporal reprojection (a native SSR history buffer); world-space fallback at 760p; a 0.5 + 1.0 + 0.30 ms budget; NFS 2015 native SSR at 0.30 ms; real-time cubemaps as an option. That is *a history buffer*, though only one previous frame.

### 8.1 Recommendation v1: deterministic SSR plus parallax cube fallback, no history [proposal]

**SSR trace** (1x, pixel frequency, before lighting, pixels with roughness < 0.6):

- **Inputs:** 1x linear depth (sample 0), 1x view normal and roughness, the previous-frame color pyramid (R11G11B10F, mips), `PrevViewProj`.
- **Ray:** `R = reflect(−V, N)` (mirror direction; deterministic, so no noise). Start at P + N·ε.
- **DDA** (McGuire-Mara):
  - Clip to the near plane and `maxDistance` (e.g. 30 m).
  - `stride = 1` px for roughness < 0.2, otherwise 2 px. `maxSteps` = 32 (full res) or 24 (half res).
  - **Jitter** = Bayer2x2 × stride (deterministic, TI-style) instead of blue noise.
  - At each step, the ray covers `[rayZmin, rayZmax]` within the pixel. It hits if `rayZmax ≥ sceneZ && rayZmin ≤ sceneZ + thickness` (LH, larger z is farther), with `thickness = max(0.1 m, 0.02·sceneZ)` [est; tune].
  - **Binary refinement:** 4 iterations between the last miss and the hit.
- **Hit validation:**
  - reject when the hit normal faces away (`dot(N_hit, R) > 0`);
  - fade at screen edges: `saturate(min(min(uv, 1 − uv))/0.1)`;
  - fade with ray length: `1 − (t/maxDist)²`;
  - fade with roughness: `saturate((0.6 − r)/0.2)`.
  - The product of these is `confidence`.
- **Color:**
  - Reproject the hit with last frame's matrix: `hitWorld → PrevViewProj → prevUV`, and invalidate it if off-screen.
  - Glossy blur by **mip selection** (Uludag-style cone):
    - `α = roughness²`, `n = 2/α² − 2`, `θ = acos(0.244^(1/(n+1)))`;
    - `footprintPx = 2·t·tan θ·pixelsPerMeter(z_hit)`;
    - `mip = clamp(log2(footprintPx), 0, maxMip)`;
    - trilinear sample.
  - Noise-free, at the cost of approximate glossy shapes.
- **Output:** RGBA16F (rgb, confidence).

**Cube fallback** (lighting draw):

- One global **sky cube** (from the sky-view LUT, 128² GGX-prefiltered, updated when the sun moves), plus up to 4–8 **local box-projected cubes**.
- Local cubes are captured at load with the same capture path as the GI bake (128², bake shader including probe GI) and prefiltered.
- Prefilter mip m with roughness `r_m = m/(M−1)` and look up `mip = r·(M−1)`. The mapping only needs to be consistent.
- Use 64–256 GGX samples per texel with PDF-based source-mip selection (Colbert & Křivánek, GPU Gems 3 ch. 20) to avoid fireflies.
- Parallax correction (Lagarde):

```hlsl
float3 BoxProjectedDir(float3 P, float3 R, float3 boxMin, float3 boxMax, float3 cubeCenter)
{
    float3 tA = (boxMax - P) / R, tB = (boxMin - P) / R;
    float3 tFar = max(tA, tB);
    float  t = min(min(tFar.x, tFar.y), tFar.z);
    return (P + R * t) - cubeCenter;          // sample the cube with this direction
}
```

**Combine** (in the per-shading-model sun/ambient draw):

```hlsl
float3 envSpec = lerp(SampleCube(R, roughness), ssr.rgb, ssr.a);
float2 AB      = g_EnvBrdfLut.SampleLevel(g_LinearClamp, float2(NoV, roughness), 0); // or EnvBRDFApprox
specular      += envSpec * (F0 * AB.x + AB.y) * SpecularOcclusion(NoV, ao, roughness);
```

**Previous-frame color pyramid.** At the end of the frame (post-VFX, pre-tonemap), average the 2 samples of the lit buffer into R11G11B10F mip 0, then `GenerateMips` (box filter; acceptable). The first frame has no history, so use confidence 0.

**Cost [est].**

- Full res, about 50% reflective coverage, 32 steps: **0.3–0.5 ms**. This extrapolates McGuire's 1.2 ms on a Kepler Titan for 25 steps at all pixels; the RTX 5060 has about 4× FP32 and 1.5× bandwidth.
- Half-res trace plus depth-aware upsample: 0.1–0.2 ms.
- Pyramid: ~0.05 ms.
- Cube sampling plus LUT: ~0.03 ms inside the lighting draw.
- TI quotes NFS 2015 native SSR at 0.30 ms.

**v2 (TI's design, optional):** stochastic GGX-sampled rays with Bayer or blue-noise jitter, plus *one-frame* reprojection blend using specular motion vectors (RG16F). This is still not a long accumulation buffer, but it is a history buffer. Only add it if deterministic glossy SSR looks wrong on rough materials.

**Pitfalls.**

- Missing information off-screen and behind objects is why the cube fallback exists.
- Thickness that is too large produces streaks; too small lets rays pass through thin objects.
- Self-intersection: offset the start and skip the first pixel.
- Hits on the sky (`z = far`) must be rejected so the cube supplies the sky.
- MSAA: SSR is 1x, applied at pixel frequency (as TI says Crysis 3 Remastered does, blending pixel-frequency SSR into the multisampled image). On edge samples, per-sample normals still drive the cube fallback.

---

## 9. Sky and atmosphere

### 9.1 Options

- **Analytic models.** Preetham et al. 1999 and Hosek & Wilkie 2012 fit a ground-observer sky luminance to turbidity and sun elevation. They are cheap, but:
  - they give no consistent aerial perspective, sun transmittance or multiple-scattering control;
  - they cover a single (Earth) atmosphere;
  - Preetham is known to be poor at low sun.

  They save almost nothing against Hillaire's ~0.1 ms.
- **Hillaire 2020**, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR / CGF 39(4) [src paper; reference code MIT: github.com/sebh/UnrealEngineSkyAtmosphere]. Physically based, with small LUTs, a non-iterative multiple-scattering approximation, fully dynamic parameters, and a sun-relative sky-view LUT.
- **GT7, GDC 2023 "Sky rendering"** (Polyphony Digital) [src PDF]. The sky is computed **offline** with a measurement-based, full-spectrum simulator: aerosol layers from AERONET size distributions, measured refractive indices and density profiles, and validated against photographs. It is stored as runtime **LUTs**:
  - a 3D texture per weather, with 64 sun-elevation slices × (32×128) spherical directions, **R11G11B10F** (the talk says BC6H was not accurate enough for color);
  - adaptive mapping with more texels near the sun and the horizon, aligned so slices interpolate for any sun elevation;
  - no altitude axis (about 300 m of variation in GT7).

  Clouds are ray-marched into a cloud G-buffer amortized over 32 frames (PS5) or 64 frames (PS4). Lap-average cost is **+0.18 ms on PS5 and +0.59 ms on PS4**.

  Takeaway for us: GT7's runtime is just a LUT fetch. What makes it good is the **offline physical model plus photographic validation**. Hillaire gives us a dynamic physical model at similar runtime cost. We adopt GT7's *validation discipline* (compare against reference photos at matching sun elevations) rather than its offline simulator.

### 9.2 Atmosphere model (Earth, Hillaire Table 1) [src]

| Constant | Value |
|---|---|
| Ground radius Rb | 6360 km |
| Top radius Rt | 6460 km |
| Ground albedo | 0.3 |
| Rayleigh scattering | (5.802, 13.558, 33.1)·10⁻³ km⁻¹, no absorption, density `e^(−h/8 km)`, phase `3(1+cos²θ)/(16π)` |
| Mie | scattering 3.996·10⁻³ km⁻¹, absorption 4.40·10⁻³ km⁻¹, density `e^(−h/1.2 km)`, Cornette-Shanks (or Henyey-Greenstein) phase with g = 0.8 |
| Ozone absorption | (0.650, 1.881, 0.085)·10⁻³ km⁻¹, density `max(0, 1 − \|h − 25\|/15)`, no scattering |

`σ_s = σ_Rs·d_R + σ_Ms·d_M` and `σ_t = σ_Rs·d_R + (σ_Ms + σ_Ma)·d_M + σ_Oa·d_O`.

### 9.3 LUT passes (pixel shaders on full-screen triangles into small RTs; compute is also fine)

1. **Transmittance LUT, 256×64 RGBA16F, 40 steps.** Rebuild only when the atmosphere changes. Hillaire's GTX 1080 time was 0.01 ms.
   - Bruneton parameterization, texel to (r, μ):
     - `H = √(Rt² − Rb²)`, `ρ = H·v`, `r = √(ρ² + Rb²)`;
     - `d_min = Rt − r`, `d_max = ρ + H`, `d = d_min + u·(d_max − d_min)`;
     - `μ = d = 0 ? 1 : (H² − ρ² − d²)/(2·r·d)`.
   - Lookup (r, μ) to uv: `ρ = √(r² − Rb²)`, `d = max(0, −r·μ + √(r²(μ² − 1) + Rt²))`, `u = (d − d_min)/(d_max − d_min)`, `v = ρ/H`.
   - Integrate `σ_t` along the ray to the top sphere, then `T = exp(−∫σ_t)`.
2. **Multi-scattering LUT, 32×32 RGBA16F.** Rebuild only when the atmosphere changes; 0.07 ms on a GTX 1080.
   - Texel: `cos θ_sun = 2u − 1`, `h = v·(Rt − Rb)`.
   - For each of **64 uniform directions**, march 20 steps (analytic per-step integration `S·(1 − e^(−σt·Δt))/σt`, then `throughput *= e^(−σt·Δt)`), accumulating:
     - `L′ += throughput · σ_s · (1/4π) · T_sun(x)`, plus a ground hit term `T · T_sun(ground) · max(0, μ_sun) · albedo/π`;
     - `L_f += throughput · σ_s` (unitless; no phase, no sun term).
   - `L2nd = Σ L′/64`, `f_ms = Σ L_f/64`, and `Ψ_ms = L2nd/(1 − f_ms)` (geometric series, Hillaire eq. 9–10).
3. **Sky-view LUT, 192×108 R11G11B10F, 30 steps, every frame** (or when the sun or camera altitude changes). Paper: 200×100 at 0.05 ms on a GTX 1080.
   - Sun-relative latitude and longitude around the local up.
   - `u = √(0.5 − 0.5·cosAz_rel)` (more texels toward the sun).
   - `v` is split at the horizon with a non-linear (√) mapping. Let `β = acos(√(r² − Rb²)/r)` and `Z = π − β`:
     - sky: `v = 0.5·(1 − √(1 − acos(μ)/Z))`;
     - ground: `v = 0.5 + 0.5·√((acos(μ) − Z)/β)`;
     - then a half-texel sub-UV remap.
   - Per step: `S = E_sun·(T_sun(x)·(σ_Rs·d_R·p_R + σ_Ms·d_M·p_M) + Ψ_ms(x)·σ_s(x))`, integrated with view throughput as above.
4. **Sky draw** (stencil: sky samples):
   - `L = SkyViewLUT(V)`.
   - Add the **sun disk**: if `dot(V, L_sun) > cos(0.2665°)`, add `E_sun/Ω_sun · T(camera, V)` with `Ω_sun = 2π(1 − cos 0.2665°) ≈ 6.8·10⁻⁵ sr`, plus optional limb darkening.
   - The disk is composited separately because the LUT is too low-res (paper §5.3).
5. **Aerial perspective.** v1 uses height fog with sky-matched color (§5). v2 is Hillaire's 32×32×32 camera-froxel LUT (in-scatter RGB plus mean transmittance A, 32 km depth, 0.04 ms), applied in the composite.

The paper's total for sky and aerial perspective was **0.31 ms at 1280×720 on a GTX 1080**, or 0.14 ms for the on-screen part. For us: ~0.1 ms at 1080p with the transmittance and multi-scatter LUTs cached [est].

Units: the shader works in km for the atmosphere. Camera altitude = `Rb + cameraY_m/1000 + 0.001` (keep the camera above the ground sphere).

### 9.4 How the sky participates in lighting

- **Sun color** = `E_sun_TOA · T(r_cam, μ_sun)`, from the transmittance LUT (or the same 40-step integral ported to C++ once per frame; simplest, no readback). This drives the directional light, so the sunset color comes for free.
- **Sky ambient.**
  - When the sun changes, render the sky-view LUT into a small cube (32²×6), then project it to an ambient cube (and L2 SH if wanted).
  - It feeds: the background of all probe captures (so sky light is in the GI); the fallback ambient outside the probe grid; and the **sky reflection cube** (128², GGX-prefiltered).
  - TI warns against ambient that collapses into flat sky blue. The bounce from the sun-lit ground in the probes is what prevents that.
- **Fog inscatter color** comes from the sky-view LUT horizon row (§5).
- **Clouds**: out of scope for v1. TI notes the cost should scale with base-pass coverage. GT7's amortized cloud G-buffer is the reference if they are added.

---

## 10. Measurement and A/B test plan

**Infrastructure** [proposal]:

- **Per-pass GPU time:** `D3D11_QUERY_TIMESTAMP_DISJOINT` per frame, plus two `D3D11_QUERY_TIMESTAMP` queries around each pass, in a ring of 4–5 frames to avoid stalls. Report the median of 300 frames with V-Sync off. Optionally lock GPU clocks.
- **Work done:** `D3D11_QUERY_PIPELINE_STATISTICS` → `PSInvocations` per pass. This proves stencil culling: SSS should be close to the number of skin pixels, and the composite should be close to the number of geometry pixels plus edge pixels × 2.
- **Fixed camera bookmarks:**
  - S1: full-screen face close-up;
  - S2: third-person medium shot (the character fills about 10% of the screen);
  - S3: long shot;
  - S4: leak test room (closed box with one door, sun outside);
  - S5: reflective floor and corridor;
  - S6: sunset with the horizon and fog.
- **Image metrics:** capture HDR (RGBA16F) and tonemapped LDR. Compare against a reference configuration with RMSE/PSNR in HDR and NVIDIA FLIP (BSD-3) in LDR, and keep an in-engine A/B toggle key for flip tests. TI's criterion: the less change you perceive when switching to the reference, the better.

| ID | Test | Variants | Metric and expectation |
|---|---|---|---|
| S-1 | SSS cost versus taps | 11 / 17 / 25 (reference) taps, S1–S3 | ms, FLIP vs 25-tap. Expect 17 taps under 0.3 ms in S2. |
| S-2 | Stencil-masked versus unmasked | stencil EQUAL skin vs full screen with an in-shader ID early-out | ms and PSInvocations. Expect masked to scale with skin area; unmasked to have a full-screen floor. |
| S-3 | Full res versus half res | full; half res + 80% native blend (Callisto); half res + depth/ID-aware upsample | ms and FLIP vs full res, especially at hair and eye edges (TI). |
| S-4 | MSAA edge handling | center from sample 0 vs ID-aware center; taps from sample 0 vs sample average | Visual at silhouettes (halos), FLIP on an edge crop. |
| S-5 | Jitter | off / 2x2 Bayer static / 2-position | Banding visibility in S1 at 11 taps. |
| S-6 | Diffuse precision | RGBA16F vs R11G11B10F diffuse and specular (Crysis 3 claim) | ms (expect −20% SSS, per TI's 0.62 vs 0.49 ms) and HDR error in dark skin gradients. |
| S-7 | Merge with composite | TI 2-draw ping-pong + separate composite vs Option C merged | ms over skin pixels. |
| C-1 | Fog merged versus separate | in-composite vs separate full-screen fog pass | ms. Expect merged ≈ +0.01 ms and separate ≈ +0.1–0.15 ms. |
| C-2 | Composite frequency | interior pixel-frequency + edge sample-frequency vs all sample-frequency | ms and PSInvocations. Edge AA must be identical. |
| C-3 | Sky skip | stencil-skip sky vs process all | ms in sky-heavy views. |
| G-1 | Probes on and off | no GI (sky ambient only) / v1 grid | ms (expect +0.03–0.06 ms in the lighting draw), visual in S4 and outdoors. |
| G-2 | Leak handling | none / dilation / + normal-view bias / + DDGI visibility | Leak luminance on the inner walls of S4 (mean luminance of a wall region); ms. |
| G-3 | Encoding | ambient cube vs L1 SH vs L2 SH | Error vs a 64²-face reference capture at random points; fetch count. |
| G-4 | v2 relight | full update vs 1/4 time-sliced | ms, and convergence lag in frames after a sun jump. |
| A-1 | AO cost | full res 2×4, 3×4; half res + upsample | ms (target ≤ 0.40 ms per TI). |
| A-2 | AO quality | with and without the 4×4 denoise; two-position on and off | Noise (temporal std-dev of a static frame should be 0) and FLIP vs a 16-slice × 16-step no-denoise reference. |
| R-1 | SSR resolution and steps | full / half res; 16 / 32 / 64 steps; stride 1 / 2 | ms and FLIP vs 128-step stride-1. |
| R-2 | Fallback | cube only / SSR + cube | Visual in S5 (sky-colored speculars in occluded areas without SSR). |
| K-1 | Sky cost | sky-view each frame vs on change; LUT sizes | ms. Expect under 0.1 ms total. |
| K-2 | Sky validation (GT7 discipline) | Hillaire output vs reference photos at matched sun elevations | Side-by-side and horizon color, no metric. |

---

## 11. Open questions

1. **R11G11B10F versus RGBA16F for diffuse and specular** (Crytek's argument against TI's preference). This needs S-6 on a good HDR monitor, as TI says.
2. **Skin specular Gaussian smoothing** (Callisto): purpose, radius and weights are undocumented, and the GDC 2023 slides are not public. Test whether it is visible at all.
3. **Normals in SSS** (UE does not use them; Golubev's disk orientation and Mikkelsen 2010's cross-bilateral do). Is a slope-aware kernel stretch worth the normal fetch?
4. **SSS kernel size**: physically sized d'Eon (2 mm support) versus artist-tuned. TI thinks Callisto over-scatters, so we need photo references.
5. **Probe density and storage**: 1 m versus 2 m spacing, and whether a sparse or cascaded grid (TI labels its GI as cascaded probe lighting) is needed beyond the demo.
6. **v2 relight fallback**: what replaces TI's temporal software RT for sensors outside every shadow map in a large level (static GI shadow map coverage versus voxel or SDF tracing)?
7. **SSGI layer**: whether a horizon-based, lit-scene SSGI at about 0.5 ms adds enough over probes and GTAO multi-bounce to be worth it.
8. **Deterministic versus stochastic glossy SSR**: whether mip-blur glossy is good enough or TI's one-frame-history stochastic SSR is required.
9. **Fox Engine early-tonemapping RGB10A2 resolve** in the composite (TI flagged it to investigate; it interacts with VFX compositing).
10. **The Division GI internals** (surfel/brick details, costs): slides not accessible. If the talk becomes reachable, verify the v2 design against it.

---

## 12. Sources

| Source | What it provided |
|---|---|
| TI video, youtube.com/watch?v=SnNm7rSSvlg (2026-08-01); local transcript and frames 49:45–1:03 | Positioning, narration quotes, pipeline-diagram notes (GI, reflections, SSS, composite boxes; budgets 0.3 / 0.12 / 1.0+1.0 / 0.5+1.0+0.30 ms), SSS measurements (Callisto, Days Gone, Crysis 3), example "marketing posts". |
| Jimenez et al. 2015, "Separable Subsurface Scattering", CGF; iryoku.com/separable-sss/downloads/Separable-Subsurface-Scattering.pdf | Separable pre-integrated and artist models, jitter and importance sampling, parameter table, 0.498/0.348/0.304 ms at 1080p on HD 7970. |
| github.com/iryoku/separable-sss (`SeparableSSS.h`, `Demo/Code/SeparableSSS.cpp`, `Demo/Shaders/*.fx`, `LICENSE.txt`) | Kernel generation algorithm and presets (used to validate our generator), follow-surface formula, stencil states, transmittance function, MSAA/resolve guidance, BSD-style license terms. |
| iryoku.com/sssss/ (Jimenez, Sundstedt, Gutierrez 2009, ACM TAP) | Original screen-space skin SSS: stencil, depth-scaled width, follow-surface lerp. |
| iryoku.com/screen-space-subsurface-scattering/ | Adoption in CryEngine 3 and UE3; blur and correction code description. |
| d'Eon & Luebke 2007, GPU Gems 3 ch. 14, developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-14-advanced-techniques-realistic-real-time-skin | Six-Gaussian skin profile (variances, weights). |
| Golubev 2018, advances.realtimerendering.com/s2018/Efficient%20screen%20space%20subsurface%20scattering%20Siggraph%202018.pdf | Unity HDRP Burley SSS: disk importance sampling, stencil tiles, LOD (21/55 samples), post-scatter texturing, PS4 1.16 ms. |
| Christensen & Burley 2015, SIGGRAPH talk (history.siggraph.org/.../2015-Talks-Christensen_An-Approximate-Reflectance-Profile...pdf) | Burley profile with `s = 1.85 − A + 7(0.8 − A)³` and its CDF. |
| Jimenez & Petersen 2023, advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf | Callisto BRDF context (no SSS cost data). |
| gdcvault.com/play/1029339 (Callisto GDC 2023) and gdcvault.com/play/1023273 (The Division GI, GDC 2016) | Abstracts only; slides not available. |
| Sousa 2013, "CryENGINE 3 Graphics Gems", advances.realtimerendering.com/s2013/Sousa_Graphics_Gems_CryENGINE3.pptx | Deferred MSAA: sample-0 pre-resolve, per-sample stencil bit, pixel versus sample passes, "R11G11B10F suffices". |
| Wenzel 2006, advances.realtimerendering.com/s2006/Chapter6-Real-time%20Atmospheric%20Effects%20in%20Games.pdf | Analytic height-fog integral; fog color matched to the sky horizon. |
| iquilezles.org/articles/fog/ | Height-fog closed form and sun-lobe inscatter. |
| github.com/FlaxEngine/FlaxEngine `Source/Shaders/Lighting/ExponentialHeightFog.hlsl` and `ExponentialHeightFog.cpp` | UE-style exponential height fog details (exp2 form, Taylor branch, clamps, /1000 parameter scaling, directional inscatter). |
| Jimenez, Wu, Pesce, Jarabo 2016, activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf | GTAO equations, Algorithm 1 and 2, multi-bounce fit (eq. 33), implementation (half res, 4×4 spatial, 6 temporal), 0.5 ms PS4 at 1080p. |
| github.com/GameTechDev/XeGTAO (README) | Spatial-only 5×5 denoise, full-res defaults, depth MIPs, timings (RTX 2060, RTX 3070, GTX 1050), MIT license. |
| Lagarde & de Rousiers 2014, "Moving Frostbite to PBR", seblagarde.wordpress.com/wp-content/uploads/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf | Specular occlusion formula; occlusion applied to indirect only. |
| Hooker 2016, advances.realtimerendering.com/s2016/Volumetric%20Global%20Illumination%20at%20Treyarch.pptx | Ambient-cube volumes, trilinear evaluation, leak handling (backface voxels, in-painting, attenuation volumes), under 2 ms GI and reflections. |
| Majercik et al. 2019, DDGI, jcgt.org/published/0008/02/01/; RTXGI `rtxgi-sdk/shaders/ddgi/Irradiance.hlsl` (github.com/NVIDIAGameWorks/RTXGI-DDGI) | Probe visibility interpolant (backface wrap + 0.2, Chebyshev, crush, bias), probe texture sizes, 6 ms on RTX 2080 Ti at 1080p. |
| Silvennoinen & Timonen 2015, advances.realtimerendering.com/s2015/SIGGRAPH_2015_Remedy_Notes.pdf | Normal-offset probe sampling (as cited by Hooker). |
| McGuire & Mara 2014, jcgt.org/published/0003/04/04/paper.pdf | Screen-space DDA ray march, thickness, stride and jitter, 1.2 ms at 1080p on Titan (25 steps). |
| Lagarde 2012, seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/ | Parallax-corrected (box-projected) cubemaps. |
| Karis 2013, blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf | Split-sum IBL, prefiltered environment map, env BRDF LUT. |
| Karis 2014, unrealengine.com/blog/physically-based-shading-on-mobile | `EnvBRDFApprox` constants. |
| Hillaire 2020, sebh.github.io/publications/egsr2020.pdf; github.com/sebh/UnrealEngineSkyAtmosphere (MIT) | Atmosphere coefficients, LUT sizes and steps, multiple-scattering approximation, LUT parameterizations, GTX 1080 and iPhone 6s timings. |
| GT7 GDC 2023, s3.amazonaws.com/gran-turismo.com/pdi_publications/GDC2023_GT7_SKY_RENDERING.pdf | Measurement-based offline sky, LUT format and size (64 × 32×128 R11G11B10F, adaptive mapping), cloud G-buffer amortization and cost (+0.18 ms PS5, +0.59 ms PS4). |
