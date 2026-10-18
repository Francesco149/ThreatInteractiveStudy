# R7: Anti-aliasing, "SMAA 4x" (S2x + T2x), modernized

Research note for the D3D11 educational engine (FL 11_0/11_1, SM 5.0, no vendor SDKs, deferred,
2x MSAA, 1080p60, RTX 5060 + Ryzen 9 5900X). It covers the anti-aliasing stage proposed in Threat
Interactive's "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering" (2026-08-01),
minutes 1:19-1:43, plus the responsive mask at 1:03-1:04.

Everything below was checked against primary sources: SMAA source code and demo, SMAA paper, Filmic
SMAA and CoD:AW decks (text, speaker notes and slide images), CryENGINE 3 AA slides, Karis 2014,
Decima 2017, the archived TSCMAA article, D3D/HLSL docs and the AGS header. When something is my own
derivation or a reconstruction, the note says so.

---

## 0. TL;DR: decisions for our engine

1. **Base:** the iryoku/smaa reference shader (`SMAA.hlsl` v2.8, MIT), used as written. The
   only change is our own sampler/porting macros (`SMAA_CUSTOM_SL`), so that D3D11 works without the
   Effects framework.
2. **Spatial part = SMAA S2x.** The tonemap pass reads the two MSAA samples and writes
   *two* tonemapped LDR layers. We run full SMAA 1x on each layer, with the subsample indices of the
   frame and sample. The layers are averaged by hardware blending in an sRGB render target, using
   blend factors 1.0 and then 0.5.
3. **Temporal part = SMAA T2x two-frame blending.** Each frame blends the current S2x output with
   the *previous frame's S2x output*, never with the previous temporal output. The camera jitter
   alternates between (-0.125,-0.125) and (+0.125,+0.125) px, so S2x + T2x give the SMAA 4x pattern.
   The resolve is narrow: one point sample per frame, no reconstruction filter.
4. **Modernizations, each one switchable for A/B testing:**
   - Velocity weighting, using the SMAA/CryENGINE formula.
   - Motion-gated neighborhood color clamp.
   - Responsive mask from VFX alpha, current and previous frame.
   - Edge-only blending mask = (MSAA edge or SMAA-processed pixel) in current or previous frame.
   - Unjittered material UVs and normals in the base pass (`EvaluateAttributeSnapped`).
   - Optional Filmic SMAA "morphological edge suppression" in edge detection.
   - Optional sharper history resampling ("fast smootheststep").
5. **Not adopted (by design, per the video):** an exponential history or accumulation buffer, wide
   reconstruction filters, and Filmic SMAA's separate temporal filter.
6. **Programmable sample positions are not available** in core D3D11. Only NVAPI offers them on D3D11,
   and AGS does not. There is a D3D11-legal experiment (E3, section 8.4) that builds a Decima-like
   4-sample "edge-midpoint" pattern in *one* frame. It uses standard 2x MSAA plus a *constant*
   (-0.25,+0.25) px camera offset.

---

## 1. How the video positions AA (timestamps)

- **1:19-1:21.** AA comes after tonemapping. It is "mostly based on SMAA 4x". SMAA 4x runs after an
  SMAA S2x pass, which is "running SMAA twice, once for each MSAA sub-sample layer". MSAA is there "to
  super sample temporally unstable edges", and MLAA (SMAA) handles jaggies. The original Crysis 3 SMAA
  is the quality reference.
- **1:21-1:22.** Worst case S2x in grass is "almost a full millisecond", and usually much cheaper. The
  SMAA author published optimizations worth about 40%. Crysis 3 did not use the smallest edge and LUT
  buffers. The biggest optimizations came from MiniEngine's FXAA.
- **1:22-1:24.** The Filmic SMAA visual fixes:
  - High-contrast edges that the original missed ("explained and fixed in the presentation with sample
    code").
  - Improved line detection, which keeps textures unaffected.
  - Smoothing of certain patterns for the temporal stability of the *spatial* component.
- **1:25-1:28.** Temporal = T2x. Blend only the current frame with the *previous frame's SMAA output*,
  with no accumulation buffer. The jitter follows the 2x MSAA pattern on even and odd frames. The
  video names Ghost of Tsushima PC's T2x as a recent "semi-correct" example.
- **1:28-1:33.** A narrow resolve gives a clean two-step gradient. UE's wide filter
  (`r.TemporalAA.FilterSize`) blurs edges and textures. A slight sharpen is acceptable if it is merged
  into the TAA pass (NIS as reference).
- **1:33-1:37.** Issues and fixes:
  - VFX ghosting: responsive mask, an artist flag on materials.
  - Foliage ghosting: full-scene motion vectors, including alpha-tested and animated foliage, plus
    velocity weighting "enhanced" by color clamping. Filmic SMAA additionally used current and
    previous depth.
- **1:37-1:40.** Texture blur: jitter texture UVs in sync with the camera jitter, in the base pass.
  As a last precaution, blend temporally only on edges (Intel TSCMAA blended only on CMAA edges). We
  can use "the custom MSAA mask saved in our motion vector buffer".
- **1:40-1:42.** Lighting still shifts with jitter, for example shadows. The options are programmable
  sample locations or a Decima-like pattern: 2x MSAA samples on the pixel's horizontal and vertical
  axes, so that neighbors share samples and each pixel gets 4 samples without temporal blending.
  "Any temporal logic should only enhance the existing MSAA edges, sort of like a colored CSAA. We
  might even want to store the last frame's relevant SV coverage data." Luma edge detection with
  lowered peripheral contrast is "only a scalability option".
- **0:09 and 0:35-0:38.** MSAA coverage information is stored with the motion vectors and cleared each
  frame. It is a custom SV_Coverage-derived mask, without flat shared-edge discontinuities.
- **1:03-1:04.** All VFX and transparent draws write a responsive mask into alpha. It "will reduce the
  amount of temporal anti-aliasing blending". Older engines' responsive masks "still look bad because
  the TAA shader accumulates more than a single frame".

---

## 2. Conventions and sample geometry (read this first, it prevents most sign bugs)

- **Pixel space:** x right, y **down**, origin at the top-left. Pixel centers are at +0.5. Offsets
  are given in pixels relative to the center.
- **NDC (D3D):** x right, y **up**. A projection offset of `(2*jx/W, 2*jy/H)` moves the geometry by
  `(jx, -jy)` pixels in pixel space.
- **"Projection jitter" `j`:** in this note, `j` is the value written into the projection exactly as
  the SMAA demo does it, in NDC-oriented pixels (y up):
  `viewProjJ = view * proj * XMMatrixTranslation(2*j.x/W, 2*j.y/H, 0)` (row-vector DirectXMath).
  Equivalently: `proj._31 += 2*j.x/W; proj._32 += 2*j.y/H;` for a standard LH perspective, which is
  Karis's `ProjMatrix[2][0/1]` form.
- **Geometry shift in pixel space** is `g = (j.x, -j.y)`. The **effective sample shift** relative to
  the geometry is `-g`.
- **D3D11 standard 2x pattern** (`DXGI_SAMPLE_DESC{2, D3D11_STANDARD_MULTISAMPLE_PATTERN}`):
  - sample 0 = (+4,+4)/16 = (+0.25,+0.25) px
  - sample 1 = (-0.25,-0.25) px, y down
  - In y-up terms this is S0 (0.25,-0.25) and S1 (-0.25,0.25), as in the SMAA demo comment and the
    paper's Fig. 11.
  - **Verify at startup.** Either call `Texture2DMS.GetSamplePosition(s)` in a tiny shader and read
    the result back, or run the SMAA demo's `detectMSAAOrder()` test: render the left half of a 1x1 2x
    MSAA target and read sample 0. The SMAA demo itself creates its MSAA targets with quality 0 and
    relies on that detection. We use the standard-pattern quality value **and** verify.

**SMAA 4x net sample positions.** These come from the SMAA.hlsl `@SUBSAMPLE_INDICES` table, converted
to pixel space with y down. "Camera jitter" in the SMAA table means the sample shift (= -g).

| frame | layer (MSAA sample) | projection jitter j (demo `getJitter`) | net sample pos (y down) | subsampleIndices |
|---|---|---|---|---|
| 0 | 0 | (-0.125, -0.125) | (+0.375, +0.125) | (5, 3, 1, 3) |
| 0 | 1 | (-0.125, -0.125) | (-0.125, -0.375) | (4, 6, 2, 3) |
| 1 | 0 | (+0.125, +0.125) | (+0.125, +0.375) | (3, 5, 1, 4) |
| 1 | 1 | (+0.125, +0.125) | (-0.375, -0.125) | (6, 4, 2, 4) |

T2x (1x MSAA): frame 0 uses `j=(-0.25,+0.25)` with indices (1,1,1,0). Frame 1 uses `j=(+0.25,-0.25)`
with indices (2,2,2,0). S2x (no jitter): sample 0 uses (1,1,1,0) and sample 1 uses (2,2,2,0). 1x uses
(0,0,0,0). The index meanings (from `SMAA.cpp`) are:

- orthogonal: [0] 0, [1] -0.25, [2] +0.25, [3] -0.125, [4] +0.125, [5] -0.375, [6] +0.375
- diagonal: [0] (0,0), [1] (0.25,-0.25), [2] (-0.25,0.25), [3] (0.125,-0.125), [4] (-0.125,0.125)
- slot layout: `{ |, --, /, \ }`

For 4x, the demo computes the layer index as `2*frameParity + msaaReorder(layer)`.

Note: the SMAA 4x set has the same x and y coordinates as the D3D11 4x MSAA pattern,
(-0.125,-0.375), (0.375,-0.125), (-0.375,0.125), (0.125,0.375), but two of its samples are mirrored
in y. Both are 4-rook (sparse grid) patterns. This matches the video's remark that SMAA 4x is "not
totally accurate" relative to 4x MSAA.

---

## 3. SMAA reference implementation (Jimenez et al. 2012; code v2.8, 2013)

### 3.1 License and files

- Repository: https://github.com/iryoku/smaa. The last commit is from November 2013 (v2.8).
- License: **MIT**, with a clarification that no notice is required in binary distributions.
  Switched from earlier terms on 2013-10-17.
- Files we need:
  - `SMAA.hlsl`: the whole algorithm, about 1360 lines, header-style. It works as both HLSL and GLSL.
  - `Textures/AreaTex.h`: 160x560, stored R8G8. Load as `DXGI_FORMAT_R8G8_UNORM`, or BC5.
  - `Textures/SearchTex.h`: 64x16, R8. Load as `DXGI_FORMAT_R8_UNORM`, or BC4.
  - Optional: `Scripts/AreaTex.py` and `SearchTex.py`, to regenerate or tweak the LUTs.
- `Demo/DX10/Code/SMAA.cpp` and `Demo/DX10/Shaders/SMAA.fx` are the integration reference. They are
  D3D10 Effects code, which we translate to plain D3D11.

### 3.2 The three passes, with formats and states

All passes draw an oversized fullscreen triangle. Samplers are clamp, with LINEAR =
`MIN_MAG_LINEAR_MIP_POINT` and POINT = `MIN_MAG_MIP_POINT`.

| pass | VS / PS entry points | inputs | output | depth-stencil state |
|---|---|---|---|---|
| 1. Edge detection | `SMAAEdgeDetectionVS` / `SMAAColorEdgeDetectionPS` (or `Luma...`, `Depth...`) | color, **gamma (non-sRGB) view** | `edgesTex` RG8_UNORM (left, top) | depth off, stencil ALWAYS/REPLACE ref=1 (PS `discard`s non-edges, so stencil=1 only on edges) |
| 2. Blend weights | `SMAABlendingWeightCalculationVS` / `SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, subsampleIndices)` | edgesTex (linear), areaTex, searchTex | `blendTex` RGBA8_UNORM | depth off, stencil EQUAL ref=1, keep |
| 3. Neighborhood blending | `SMAANeighborhoodBlendingVS` / `SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex[, velocityTex])` | color, **sRGB view**, blendTex | output (sRGB RTV) | depth and stencil off; runs on all pixels |

Rules from the SMAA.hlsl header:

- `edgesTex` and `blendTex` **must be cleared every run, including alpha**. The edge pass discards
  pixels and the weights pass is stencil-masked, while the next passes read neighbors.
- "All texture reads and buffer writes must be non-sRGB, with the exception of the input read and
  the output write in `SMAANeighborhoodBlending`". Edge detection must read the **non-sRGB** (gamma)
  view.
  - So the per-layer color texture is `R8G8B8A8_TYPELESS`, with an SRV `UNORM` for pass 1, an SRV
    `UNORM_SRGB` for pass 3, and an RTV `UNORM_SRGB` for whoever writes it.
- Stencil optimization: the edge pass writes stencil (the discard is already in the code), the
  weights pass tests it, and the last pass runs everywhere. The 1x depth-stencil must be **cleared to
  0 before each layer**.
- RG8 edges: the 2013 code avoided R8G8 on NVIDIA ("slower than RGBA8"). That advice is about
  2013-era GPUs, so measure both formats on the RTX 5060.
- areaTex and searchTex can be BC5 and BC4 compressed, for a "non-perceptible quality decrease, and a
  marginal performance increase".
- Inputs must be **tonemapped LDR**. The thresholds are in gamma-space units, 0..1.

### 3.3 Presets and defines

From `SMAA.hlsl`:

| preset | SMAA_THRESHOLD | MAX_SEARCH_STEPS | MAX_SEARCH_STEPS_DIAG | CORNER_ROUNDING | notes |
|---|---|---|---|---|---|
| LOW (60% quality) | 0.15 | 4 | diag off | corner off | |
| MEDIUM (80%) | 0.10 | 8 | diag off | corner off | |
| HIGH (95%) | 0.10 | 16 | 8 | 25 | |
| ULTRA (99%) | 0.05 | 32 | 16 | 25 | |

Other defaults:

- `SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR` 2.0
- `SMAA_DEPTH_THRESHOLD` = 0.1*THRESHOLD
- `SMAA_PREDICATION` 0, with THRESHOLD 0.01, SCALE 2.0, STRENGTH 0.4
- `SMAA_REPROJECTION` 0
- `SMAA_REPROJECTION_WEIGHT_SCALE` 30.0, range 0-80
- Header comment: "If temporal supersampling is used, 0.2 could be a reasonable value [threshold], as
  low contrast edges are properly filtered by just 2x."

**What we define.** We use our own porting layer, so that samplers have explicit registers. The
`SMAA_HLSL_4` path declares FX-style `SamplerState LinearSampler { Filter = ... }`, which only
Effects honor.

```hlsl
// smaa_config.hlsli  (include before SMAA.hlsl)
#define SMAA_RT_METRICS float4(1.0/1920.0, 1.0/1080.0, 1920.0, 1080.0) // recompile on resize, or use a cbuffer float4
#define SMAA_CUSTOM_SL 1
SamplerState LinearSampler : register(s0); // MIN_MAG_LINEAR_MIP_POINT, CLAMP
SamplerState PointSampler  : register(s1); // MIN_MAG_MIP_POINT, CLAMP
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_PRESET_HIGH            // A/B against SMAA_PRESET_ULTRA
#define SMAA_REPROJECTION 1         // only matters for the neighborhood pass: packs velocity length into alpha
#include "SMAA.hlsl"
```

Wrappers. The neighborhood pass with `SMAA_REPROJECTION 1` needs the velocity texture argument.

```hlsl
cbuffer SMAAConstants : register(b0) { float4 SubsampleIndices; };
Texture2D ColorGamma : register(t0); // pass 1: UNORM view of the layer
Texture2D EdgesTex   : register(t0); // pass 2
Texture2D AreaTex    : register(t1); // pass 2
Texture2D SearchTex  : register(t2); // pass 2
Texture2D ColorSRGB  : register(t0); // pass 3: UNORM_SRGB view of the layer
Texture2D BlendTex   : register(t1); // pass 3
Texture2D Velocity   : register(t2); // pass 3: RG16F, UV units, (current - previous), jitter-free
// (compile each pass as its own shader so the register reuse is legal)

void FullscreenTri(uint id, out float4 pos, out float2 uv) {
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct V3 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 offset[3] : TEXCOORD1; };
struct VW { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 offset[3] : TEXCOORD2; };
struct V1 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 offset : TEXCOORD1; };

V3 EdgeVS(uint id : SV_VertexID) { V3 o; FullscreenTri(id, o.pos, o.uv); SMAAEdgeDetectionVS(o.uv, o.offset); return o; }
float2 EdgePS(V3 i) : SV_Target { return SMAAColorEdgeDetectionPS(i.uv, i.offset, ColorGamma); }

VW WeightVS(uint id : SV_VertexID) { VW o; FullscreenTri(id, o.pos, o.uv); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.offset); return o; }
float4 WeightPS(VW i) : SV_Target {
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.offset, EdgesTex, AreaTex, SearchTex, SubsampleIndices);
}

V1 BlendVS(uint id : SV_VertexID) { V1 o; FullscreenTri(id, o.pos, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.offset); return o; }
void BlendPS(V1 i, out float4 color : SV_Target0, out float4 mask : SV_Target1) {
    color = SMAANeighborhoodBlendingPS(i.uv, i.offset, ColorSRGB, BlendTex, Velocity);
    // "SMAA touched this pixel" flag for the edge-only temporal mask (section 6):
    float4 a;
    a.x  = BlendTex.SampleLevel(LinearSampler, i.offset.xy, 0).a;
    a.y  = BlendTex.SampleLevel(LinearSampler, i.offset.zw, 0).g;
    a.wz = BlendTex.SampleLevel(LinearSampler, i.uv, 0).xz;
    mask = float4(0, 0, dot(a, 1.0) >= 1e-5 ? 1.0 : 0.0, 0); // written to aaMask.b only
}
```

The library also provides:

- `SMAAResolvePS(texcoord, current, previous[, velocity])`: the T2x temporal resolve. Section 4
  replaces it with an extended version that keeps its formula.
- `SMAASeparatePS`: splits a 2x MSAA texture into two textures using `Texture2DMS<float4,2>.Load`.
  We fold this into the tonemap pass, which the header recommends: it "feed[s] tone mapped
  subsamples to SMAA, which will yield better results".

### 3.4 S2x and 4x in practice (demo `runSMAA`)

```
S2x:  separate(MS -> L0, L1)
      go(L0, ..., dst=out, pass 0)  // blend factor 1.0 (overwrite)
      go(L1, ..., dst=out, pass 1)  // blend factor 0.5 -> out = 0.5*SMAA(L0) + 0.5*SMAA(L1)
4x:   same as S2x but dst = previousRT[current], then
      reproject(previousRT[current], previousRT[previous], velocity, backbuffer)
```

Notes:

- Blend state for `go`:
  - `SrcBlend = BLEND_FACTOR`, `DestBlend = INV_BLEND_FACTOR`, `BlendOp = ADD`.
  - Factor 1.0 for pass 0 and 0.5 for pass 1.
  - The sRGB RTV makes the average linear.
  - In D3D11, also set `SrcBlendAlpha = BLEND_FACTOR`, `DestBlendAlpha = INV_BLEND_FACTOR`, so that
    the velocity code in alpha is averaged too.
- The demo resolves the MS velocity with `ResolveSubresource` (average) into an RG16F 1x texture,
  and both layers use it.
- The demo computes velocity with **both** current and previous matrices jittered by the *current*
  jitter, so the jitter cancels. Velocity = `(currNDC - prevNDC) * (0.5, -0.5)`, in UV units,
  current minus previous. `SMAAResolvePS` negates it to fetch the previous frame.

### 3.5 Published performance (SMAA paper, 2012)

- GeForce GTX 470, 1080p:
  - Typical cost: 1.02 ms for SMAA 1x, 1.32 ms for T2x, 2.04 ms for S2x, 2.34 ms for 4x. The Fig. 1
    caption gives 1.3 ms for T2x and 2.6 ms for 4x in Crysis 2.
  - Breakdown: local contrast adaptation 0.08 ms; diagonals 0.12 ms; temporal supersampling 0.3 ms;
    filtering the second MSAA sample 1.02 ms; rendering the scene at 2x adds about 1.57 ms on average.
  - The SMAA 4x delta on top of a 2x forward render is 2.09 ms.
- The paper's speed comparison: in forward rendering, 4x and T2x are about 1.46x and 4.09x faster
  than MSAA 8x, respectively.
- 2012 numbers on a 2010 GPU. Expect a small fraction of that on an RTX 5060, but **measure**
  (section 11).

### 3.6 Pitfalls

- Feeding HDR, or sRGB-decoded linear values, into edge detection gives wrong thresholds.
- Forgetting to clear edges, blend, or stencil per layer: stale edges from the other layer or from
  the previous frame.
- Wrong MSAA sample order makes S2x areas wrong and gives a blurry, "glowy" result. Verify it (section 2).
- Using quads instead of a fullscreen triangle (minor overshading on the diagonal).
- The `SamplerState` initializers in the stock `SMAA_HLSL_4` path do nothing outside Effects. Bind
  real samplers (section 3.3).
- Subsample indices that do not match the actual jitter give gradients that do not converge between
  frames, which shows as flicker in T2x and 4x.

---

## 4. SMAA T2x / 4x temporal resolve (two-frame blending)

### 4.1 The reference formula (paper eq. 6-7; `SMAAResolvePS`)

```hlsl
// neighborhood pass (SMAA_REPROJECTION=1) stored: color.a = sqrt(5 * length(velocityUV))
float2 v    = -VelocityTex.Sample(Point, uv).rg;          // velocity is (cur - prev); negate to go back
float4 cur  = CurrentTex.Sample(Point, uv);
float4 prev = PreviousTex.Sample(Point, uv + v);          // previous frame's SMAA output, NOT history
float delta  = abs(cur.a * cur.a - prev.a * prev.a) / 5.0; // = | |v_cur| - |v_prev| |  (UV units)
float weight = 0.5 * saturate(1.0 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE); // K = 30
return lerp(cur, prev, weight);
```

That is, `w = 0.5 * max(0, 1 - K*sqrt(| |v_c| - |v_p| |))` and `c = (1-w)*c_c + w*c_p`.

**Why the velocity goes through SMAA.** "Morphological antialiasing is actually blending pixels from
both sides of the silhouette ... However, the velocity map remains aliased", which "leaves trails of
blended pixels behind objects in motion. The solution is to apply SMAA also over the velocity buffer".
The neighborhood pass does this by blending the velocity with the same weights and storing its
length in alpha.

**Scale feel at 1080p (my numbers).** K=30 on UV units means:

- a velocity-magnitude difference of 1 px (0.00052 UV) gives `w ≈ 0.16`
- 0.25 px gives `w ≈ 0.33`
- 0 gives `w = 0.5`

The test is resolution dependent and compares magnitudes only: two objects with the same speed but
opposite directions pass it.

**Origins.** CryENGINE 3 / Crysis 2 (Sousa 2011) used "reprojection range clamping":
`fBlendW = saturate(1 - fVLen/fVMaxLen)`. It also stored coarse depth in the sub-sample buffer alpha
and masked out when `fVLen > fMaxVThreshold && fCurrD > fPrevD`, but noted that 8 bits were
insufficient for depth. SMAA 2012 replaced this with the velocity-difference weight above.

### 4.2 Two-frame blending vs accumulation (the video's core argument)

- **SMAA T2x** keeps two ping-pong buffers of *SMAA output*. Frame N's resolve reads S(N) and S(N-1)
  and writes to the display chain. Nothing is fed back.
  - Static scene: the output is `0.5*(A+B)` in both frames, so it is perfectly stable after 2 frames.
  - Failure modes: a rejected pixel shows 1 frame of the 2x SMAA result. A wrongly accepted pixel
    ghosts for **one** frame (16.7 ms at 60 fps).
- **Exponential history** (Karis 2014; Filmic's "temporal filter"):
  `h_t = alpha*x_t + (1-alpha)*h_{t-1}` is "nearly infinite number of samples with fixed storage".
  Every acceptance error persists, so ghosting and diffusion blur build up, which then needs clamps,
  clipping, anti-flicker heuristics and bicubic history resampling.
- **Decima 2017** independently chose two-frame reuse: "we chose to only reuse the INPUT to the AA
  system from the previous frame, but not the previous output ... we reach a stable and final result
  in only two frames". Its reasons: thin geometry, AA after VFX and post effects, a half-res motion
  buffer, and objects without motion vectors.
- **Early UE4 mobile AA** (Smedberg and Lottes, 2014): "2x temporal super-sampling ... Blending two
  jittered frames after tonemapping (32 bpp, perceptual color space) ... not using motion
  re-projection".

**Narrow resolve.**

- SMAA T2x: each output pixel is `lerp(cur(p), prev(p - v), w)` with point sampling, so there is no
  spatial reconstruction kernel.
- UE4 TAA (Karis 2014): "Gaussian fit to Blackman-Harris 3.3", `W(x) = exp(-2.29 x^2)`, "Support is
  ~2 pixels wide". This is the "wide resolve" the video criticizes.

### 4.3 Modern additions (what the video asks for)

**a) Color clamp, used to enhance velocity weighting, not to replace it.**

- Clamp `prev.rgb` to the min/max of the current S2x output's 3x3 neighborhood.
- Known problem with jitter (Filmic slides 71-72): a subpixel feature present in only one jitter
  phase gets clamped away in the other phase, so a static image flickers at 30 Hz.
  - Without the clamp, the static two-frame blend is exactly stable.
  - Filmic SMAA's answer was to apply clamping only in a separate accumulation filter, after the
    2x resolve, which we reject.
  - Our answer: **gate the clamp by motion**, `clampAmount = saturate(speedPx / 0.5)`. A static pixel
    gets no clamp, and a pixel moving at 0.5 px/frame or more gets the full clamp.
- The S2x neighborhood already contains 2x-antialiased values, so the box is wider than a 1x box.
  That reduces false clamps, which is Filmic's argument on slides 84-85.

**b) Depth test (Filmic SMAA, optional).**

- Slide 101: "we use depth testing to detect disocclusions. Compare current and previous frame
  depth. Half resolution nearest depth." The notes add: with jittered input, "a conservative depth
  comparison need to be performed: comparing nearest-to-camera depth in the current and previous
  frame neighborhoods".
- No formula was published. A simple version: keep a 1x linear view-depth, the nearest of the 2
  samples, for this frame and the previous one. Then
  `zc = min3x3(curDepth, p)` and `zp = min3x3(prevDepth, p - v)`, and reject (`w = 0`) if
  `abs(zp - zc) > depthTol * zc`.
- Camera motion along the view axis changes depth for static geometry, so near-camera pixels need a
  tolerance or a predicted previous depth. That is an open question; ship it off by default.

**c) Responsive mask.** Section 9.

**d) Edge-only mask.** Section 6.

**e) Sharper resampling of the previous frame (Filmic slides 104-106).**

- Point sampling, as in the reference, is sharp but snaps to the nearest texel in motion.
- Plain bilinear blurs.
- Filmic used Inigo Quilez's trick with a "fast smootheststep". It costs 1 bilinear tap:

```hlsl
float2 SmootheststepUV(float2 uv, float2 size) {      // size = (W, H)
    float2 p = uv * size + 0.5;
    float2 i = floor(p);
    float2 f = p - i;
    f = saturate((f - 0.2) / 0.6);                    // "u = max(min((x-0.2)/(1-0.4),1),0)"
    f = f * f * (3.0 - 2.0 * f);                      // "u^2 (3 - 2u)"
    return (i + f - 0.5) / size;                      // then SampleLevel(LinearClamp, ..., 0)
}
```

**f) Better velocity weighting (optional V2).**

- Keep last frame's 1x velocity texture.
- Compare vectors instead of lengths:
  `w = 0.5 * saturate(1 - K * sqrt(length(vCur - vPrevAt(p - vCur))))`, with K = 30 in UV units.
- Removes the "same speed, opposite direction" blind spot. Costs one 7.9 MiB RG16F texture.

### 4.4 Published temporal costs

- SMAA 2012: temporal supersampling adds 0.3 ms (GTX 470, 1080p).
- The video: UE5.8 TSR with a 4x history costs 2.2 ms, "more than 18 times more expensive than the
  temporal reprojection logic used for the original temporal SMAA", so roughly ≤0.12 ms. The
  hardware is not stated in the transcript.
- Filmic SMAA: the whole Filmic T2x costs 0.75-1.05 ms on PS4 at 1080p.

---

## 5. Filmic SMAA (Jimenez, SIGGRAPH 2016) and CoD:AW (2014): every concrete change

Source: `Filmic-SMAA-v8.pptx`, 143 slides including hidden ones, with speaker notes. Goals:
"Sharpness, Robustness, Performance". The deck covers "Filmic SMAA T2x, which is what we used in
production for consoles". S2x and 4x were "not practical for our games".

### 5.1 Morphological quality

**1. Morphological edge suppression (slides 20-31).** This is the video's "high-contrast edges the
original missed".

- **Problem:** local contrast adaptation suppresses an edge if a neighbor edge has
  `SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR` (2x) more contrast. Sometimes the stronger edge "doesn't
  lead to any meaningful pattern", so a real jagged line loses an edge and the line search breaks.
- **Fix:** score every small pattern that passes through the current edge, and every pattern that
  does not. A pattern's score is the sum of its edges' absolute differences minus the sum of the
  edges that must be absent. Keep the edge only if the best passing pattern wins and exceeds a
  threshold.
- The slide gives the naive and the factorized code. The **index layout is not printed**. I
  reconstructed it from the pattern diagrams on slides 26-27 and checked algebraically that the
  factorized code equals the naive one under this layout.

```hlsl
// d(a, b): edge strength between two pixels
//   luma:  abs(La - Lb)          color: max3(abs(Ca - Cb))   (gamma-space values, as in SMAA)
// Offsets (x right, y down) relative to the current pixel C = (0,0).
// For the LEFT edge of C (the edge between (-1,0) and (0,0)):
//   v[0] = d((-1,-1),(0,-1))   vertical edge above        h[0] = d((-1,-1),(-1,0))  top edge of left pixel
//   v[1] = d((-1, 0),(0, 0))   CURRENT edge               h[1] = d(( 0,-1),( 0,0))  top edge of C
//   v[2] = d((-1,+1),(0,+1))   vertical edge below        h[3] = d((-1, 0),(-1,1))  bottom edge of left pixel
//   v[4] = d(( 0, 0),(1, 0))   right edge of C            h[4] = d(( 0, 0),( 0,1))  bottom edge of C
//   v[5] = d((-2, 0),(-1,0))   left edge of left pixel    (v[3], h[2] unused)
// For the TOP edge of C use the same function with x and y swapped (transposed neighborhood).
float max3(float a, float b, float c) { return max(a, max(b, c)); }
bool KeepEdge(float v[6], float h[5], float k, float thresholdPattern) {
    float t1 = v[0] - h[0] - h[1];
    float t2 = v[2] - h[3] - h[4];
    float t3 = h[0] - h[1];
    float t4 = h[3] - h[4];
    float t5 = v[0] - v[1];
    float t6 = v[2] - v[1];
    float t7 = v[0] + t1;
    float t8 = v[2] + t2;
    float t9 = v[2] + t5;
    float pattern1 = max(t1 + t2 + v[1],
                         max(abs(t3 + t4),
                             max3(t3 + t8 - v[5],
                                  t4 + t7 - v[5],
                                  max3(-t4 + t7, -t3 + t8, abs(t3 - t4) - v[5]) - v[4])) - t9);
    float pattern2 = max(max(-t1, -t2) - v[1], k * max(abs(t3) + t5, abs(t4) + t6));
    return max(thresholdPattern, pattern2) < pattern1;   // slide: SMAA_MORPHOLOGICAL_THRESHOLD_PATTERN
}
```

- The 9 passing patterns are: straight line, two U shapes, two Z shapes, and four L shapes. The 6
  non-passing patterns are: two straight horizontal lines through the edge's end vertices, and four
  corners at the end vertices, weighted by `k`.
- **`k` and `SMAA_MORPHOLOGICAL_THRESHOLD_PATTERN` are unpublished.** Start with `k = 1.0` and
  `thresholdPattern = SMAA_THRESHOLD`, then tune by viewing `edgesTex` on the slide-23 style failure
  case (a weak horizontal edge next to a strong vertical one).
- **Integration:** in `SMAA*EdgeDetectionPS`, keep the threshold test and the discard. Replace the
  last line, `edges.xy *= step(finalDelta, LCA * delta.xy)`, with
  `edges.x *= KeepEdge(vLeft, hLeft, ...)` and `edges.y *= KeepEdge(vTop, hTop, ...)`.
  - It needs the 3x3 neighborhood plus (-2,0) and (0,-2): 11 fetches.
  - Filmic preloaded R8 lumas into LDS with Gather. For our PoC, plain point loads are fine.

**2. Silhouette line detection (slides 32-35).** "Texture details sometimes confused with
morphological patterns ... Extend the pattern recognition. Make sure it is a silhouette line ... Less
filtered pixels, sharper results." The diagrams show the short end segments of a staircase being left
unfiltered. **No code or rule is given.** Not reproducible as published; treat it as an open item.

**3. U-pattern smoothing (slides 36-40).**

- "Small U-Patterns are temporally unstable ... We are using a LUT. We can tweak to taste." The fix
  sits between the triangle and the flat-line revectorization.
- **The public SMAA v2.8 already contains a version of this.** Commit fddb76d (2013-09-08, "Make
  u-shaped patterns smoother") added to `AreaTex.py`:
  - `smootharea(d, a1, a2)`: `b = sqrt(2a) * 0.5`, `lerp(b, a, saturate(d / 32))`
  - applied to patterns 3 and 12
- So the shipped `AreaTex.h` already smooths short U shapes. To tweak further, edit
  `SMOOTH_MAX_DISTANCE` or `smootharea()` and regenerate (Python 3 plus Pillow).

### 5.2 Morphological performance (the video's "40%")

- **Deferred queues** (slides 44-49).
  - Pixel shaders run in 2x2 quads, and stencil culling only rejects whole quads, so many lanes
    idle on sparse edges.
  - Taken from James Stanard's **MiniEngine FXAA** (`FXAAPass1CS.hlsli` / `FXAAPass2CS.hlsli`): edge
    pixels are appended to a buffer ("about 2500 edges on a typical scene"), then consumed by an
    indirect dispatch.
  - D3D11 FL11_0 has everything needed: `AppendStructuredBuffer` or a `RWByteAddressBuffer` counter,
    `CopyStructureCount`, and `DispatchIndirect`.
- **Dual edge blending** (slides 50-56). A compute shader can scatter, so each pattern is searched
  once and both pixels are written. Race conditions are resolved by doing vertical edges then
  horizontal ones, and by making edges agree "during edge detection". This removes the third pass.
- **Using the LDS** (slide 57). Output R8 lumas in the previous pass, preload them into LDS with
  Gather4. This "saved 0.14 ms on PS4@1080p".
- **Results** (slide 58, PS4 at 1080p): deferred queues plus dual edge blending saved 0.16 ms, and LDS
  saved 0.14 ms. The morphological component went from **0.774 ms to 0.475 ms** (-39%, the video's
  "40%"), "faster than FXAA" (without diagonals). The final Filmic version is "two times faster than
  our original implementation".
- **Bilinear fetch and decode** (slides 59-60): two edges in one fetch. This is already in v2.8
  (`SMAADecodeDiagBilinearAccess`).
- **"Smallest buffer sizes"** (video): by v2.8 the edges are RG8, searchTex is a power-of-two 64x16 R8
  (commit 8d8beba, "BC4-compressible"), and areaTex is RG8 or BC5. Crysis 3 shipped in Feb 2013,
  before these September 2013 changes.
- **Our plan:**
  - Phase 1 is the 3-pass pixel-shader reference with stencil.
  - Phase 2 (optional) is a compute "deferred queue" for the weights pass only: edge pixels are
    appended in the edge pass via a UAV, then an indirect dispatch runs over them.
  - Keep the PS version as the readable baseline.

### 5.3 Temporal: what Filmic changed, and whether we adopt it

| Filmic change (slides) | what it is | adopt? |
|---|---|---|
| Separate temporal supersampling and temporal filtering (83-86) | T2x resolve first, then an exponential-history "temporal filter" | **No.** The video forbids accumulation. Keep the insight that clamping 2x-resolved data flickers less. |
| Temporal contrast, Catmull-Rom 5-tap history (87-92), temporal spatial-contrast tracking (94-97), FPS-aware alpha `exp(-dt/tau)` (98) | all serve the accumulation buffer | No. Useful only for the accumulation baseline (section 11). The 5-tap Catmull-Rom code is in slide 92. |
| Depth testing to extend the clamp (99-102) | disocclusion via current vs previous nearest depth; "Responsive AA" for alpha-blended objects | **Yes, optional** (4.3b) |
| Improved supersampling resampling: fast smootheststep (103-109) | sharper previous-frame fetch | **Yes, optional** (4.3e) |
| Contrast-aware temporal quincunx (110-116) | quincunx only where contrast is high: "~0.02 ms on PS4@1080" | No. It blurs, and our 4x pattern already has 4 samples. |
| Supersampling derivatives (117-125) | mip bias `log2(0.7071) = -0.5`, or unjitter texcoords (slide 121 code) | **Yes**, the unjitter variant (section 7) |
| Improved color weighting (126-133) | compare luma of frame N vs N-2 and N-1 vs N-3, 2x2 gather, closest-match | Not by default. A candidate fix for VFX without velocity if the responsive mask is not enough. |
| Low-profile velocity (134-136) | 8-bit two-piece-linear velocity, half-res nearest velocity: 7.91 MB to 0.98 MB | No. RG16F is fine on our target. |
| Exploiting the HVS (137) | lower morphological and temporal quality at the screen sides | **Scalability only**, as the video says |
| Temporal upsampling (138-140) | rearranged sample pattern | No |

### 5.4 CoD: Advanced Warfare (Jimenez, SIGGRAPH 2014)

- The deck is mostly about DOF, motion blur and bloom. The relevant AA rule (slide 134): with
  temporal supersampling such as SMAA T2x, temporally dither noise in sync with the 2-frame cycle:
  `frameTick.x * InterleavedGradientNoise(sv_position) + frameTick.y`, with `frameTick.x = 0.5` and
  `frameTick.y = 0.0 or 0.5` on odd and even frames.
- IGN is `frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))))`.
- For our engine: every dithered effect (shadow filter rotation, LOD dither, SSAO) should either
  alternate in phase with the 2-frame jitter, or be static. Never use a longer cycle, because
  two-frame blending cannot average it.

---

## 6. Edge-only temporal blending (TSCMAA, then our MSAA mask)

**TSCMAA (Intel, about 2018; archived article; BSD-3 sample, 76.6 MB, no longer downloadable).** The
article says:

- CMAA plus TAA. The TAA runs "only for edge candidates identified by edge detection, resulting in
  fast indirect dispatch".
- Edge threshold 1/22 by default. "We use 50 percent of CMAA edge candidates for TAA".
- History is reprojected from depth and view-projection, sampled with "a five-tap approximation for a
  Hermite/Catmull-Rom bicubic filter".
- "Variance clipping in YCoCg space".
- "Blending ... is also done only for TAA edge candidates with a blend weight of 0.8f and other non
  TAA edge pixels are directly taken from CMAA pixels". The output becomes the history, so it is an
  accumulation.
- No jitter. It was designed for VR head motion.
- Performance: Intel HD 630 at 1150 MHz, 1280x1280 per eye. "MSAA4x quality with around a 40
  percent cost reduction and is equivalent to MSAA2x performance". With 30% fewer or more edges, time
  scales x0.86 or x1.08. At 2Kx2K, time is x1.59 (same edge count) or x2.12 (edge count x1.91).
- The video adds that FFXIV's TSCMAA-like path uses heavy formats.

**Our adaptation.** The video says to use the MSAA mask instead of the SMAA edge mask. My analysis is
that **both are needed**, for this reason:

- SMAA 4x changes the area-texture subsample offsets every frame (section 2 table).
- So along long, shallow edges SMAA modifies pixels whose 2 MSAA samples are *identical*. Those
  pixels sit inside the revectorized gradient but are not MSAA edges.
- Frame A and frame B produce *different* values there, by design: their average is the 4x result.
- If such pixels are excluded from blending, they alternate at 30 Hz, which is flicker.
- Likewise, an edge can straddle the samples in one jitter phase and not in the other. So the mask
  must be the union over **current and previous** frames. This is the video's "store the last
  frame's SV coverage".

```
edgeMask(p) = max( msaaEdge_cur(p), msaaEdge_prev(p - v), smaaTouched_cur(p), smaaTouched_prev(p - v) )
w_final     = w * lerp(1, edgeMask, EdgeOnly)
```

- **msaaEdge**: computed in the tonemap pass as
  `any(abs(ldr0 - ldr1) > 2/255)`. Two MSAA samples that differ visibly means a geometric edge
  (or per-sample shading). This is free, exact for what is visible, and independent of how the
  G-buffer stage builds its custom coverage mask. If the engine already stores a coverage flag next
  to the motion vectors, use that instead; it should agree, except where both sides have the same
  color, which does not matter.
- **smaaTouched**: computed in the neighborhood pass as "any blend weight ≥ 1e-5", meaning SMAA
  modified the pixel. It is written to `aaMask.b` with a write mask. Layer 0 overwrites, and layer 1
  uses `BlendOpAlpha/BlendOp = MAX` on that RT.
- Interior pixels, meaning texture and shading areas without edges, keep `w = 0`. They are exactly
  the current frame, so textures can never be blurred by the temporal pass. This is the "last
  precaution".
- High-contrast *shading* edges (shadow boundaries) are usually caught by SMAA color edge detection,
  so they are smaaTouched and get blended. That stabilizes the lighting sub-pixel shift the video
  worries about at 1:40.

---

## 7. Texture (and shading) jitter compensation

**Prior art: Jimenez 2016, Filmic SMAA slide 121, "Supersampling Derivatives".** "Alternatively,
unjitter the diffuse and normal map texture coordinates using the barycentric position of the
subsamples (as in MSAA). Loses shading antialiasing. A good tradeoff is to only do it for the
diffuse. Free."

```hlsl
input.texcoord += ddx(input.texcoord) * subpixelOffset.x;
input.texcoord -= ddy(input.texcoord) * subpixelOffset.y;   // subpixelOffset = projection jitter j (y up), in pixels
```

The alternative from the same deck (slides 119-120) is a mip bias of `log2(0.7071) = -0.5` (sampler
`MipLODBias`). Karis 2014 also says "Mipmap bias all textures". This fixes blur from wrong
derivatives, but not the texture *motion* between frames.

**Exact SM5 variant (my recommendation).**

- `EvaluateAttributeSnapped(attr, int2 offset)` evaluates an interpolant at an offset from the pixel
  center on a 16x16 grid. The range is [-8,7]/16 px, pixel shader only, SM5.
- Our jitters are ±2/16 (4x) and ±4/16 (T2x), so they are **exactly representable**.
- To evaluate at the un-jittered pixel center, the offset is the geometry shift `g = (j.x, -j.y)`
  (y down) times 16.

```hlsl
cbuffer FrameCB : register(b1) { int2 UnjitterOffset16; /* = int2(round(16*j.x), round(-16*j.y)) */ };
// 4x, frame 0: j = (-0.125,-0.125) -> int2(-2, +2);  frame 1: j = (+0.125,+0.125) -> int2(+2, -2)
float2 uv  = EvaluateAttributeSnapped(input.uv,      UnjitterOffset16); // albedo/normal/roughness UVs
float3 nrm = EvaluateAttributeSnapped(input.normalW, UnjitterOffset16); // optional: vertex normal, tangent
// fallback / cross-check (Jimenez 2016): uv = input.uv + ddx(input.uv) * j.x - ddy(input.uv) * j.y;
```

Notes and pitfalls:

- Sign check: with `j = (-0.125,-0.125)`, Jimenez's formula gives an evaluation point of
  (-0.125,+0.125) px (y down), the same as the snapped offset. Verify on screen: a static
  high-frequency texture must not move between even and odd frames (frame-difference test,
  section 11).
- The MS docs page title says "pixel centroid with an offset", but the parameter text says "offset
  from the pixel center". Verify with a debug interpolant that holds the NDC position. If the compiler
  rejects a non-literal offset, compile two permutations, one per frame parity.
- **Alpha-tested cutouts: do *not* unjitter the opacity fetch.** Otherwise cutout silhouettes are
  identical in both frames and get no temporal supersampling. Better still, do a per-sample alpha
  test: `EvaluateAttributeAtSample(uv, s)` for each sample, building the `SV_Coverage` output. Then
  cutouts get real 2x MSAA while shading stays at pixel frequency.
- **Lighting is still jittered.**
  - The G-buffer depth comes from the jittered raster, so deferred lighting reconstructs the
    jittered surface point.
  - Shadow and light boundaries therefore shift by ≤0.125 px between frames.
  - Mitigation one: section 6 (SMAA-touched shading edges are blended).
  - Mitigation two (experimental): in the lighting pass, move the reconstructed depth to the pixel
    center with the depth plane, `z_c ≈ z - ddx(z)*g.x - ddy(z)*g.y`. This is valid only inside
    continuous surfaces, and edges are blended anyway.
  - Measure the residual with the static-camera test before adding complexity.

---

## 8. Sample positions on D3D11 (PSL), and the Decima pattern

### 8.1 Availability

- **Core D3D11:** sample positions are fixed. Use `DXGI_SAMPLE_DESC.Quality =
  D3D11_STANDARD_MULTISAMPLE_PATTERN (0xffffffff)` or `D3D11_CENTER_MULTISAMPLE_PATTERN (0xfffffffe)`.
  "Hardware must support 1, 4, and 8 sample counts ... if vendors support 2, 4(required), 8(required),
  or 16, they must also support the corresponding standard pattern or center pattern."
- **NVAPI only:** `NvAPI_D3D11_RASTERIZER_DESC_EX` has `ProgrammableSamplePositionsEnable`,
  `SampleCount`, `SamplePositionsX[16]` and `SamplePositionsY[16]` (via
  `NvAPI_D3D11_CreateRasterizerState`). Excluded by our "no vendor SDK" rule.
- **AMD AGS:** `amd_ags.h` was checked. It has no sample-position API for DX11 (only depth bounds,
  MDI, UAV overlap, clip rects, and similar).
- PSL exists in D3D12 (`SetSamplePositions`, tiers 1 and 2) and in Vulkan (`VK_EXT_sample_locations`).
- **The CENTER pattern** puts all samples at the pixel center. Coverage is then identical for every
  sample, so it gives **no edge AA**. It is only useful for layered or stochastic tricks, so it does
  not help us.

### 8.2 What PSL would buy (the video's 1:41 idea)

With PSL you could alternate the 2x sample positions per frame, using samples 0 and 1 of the 4x
pattern on even frames and samples 2 and 3 on odd frames. Fully covered pixels would then still shade
at the true center, with no texture or lighting jitter. Even better, you could place the samples
Decima-style, giving 4 samples per pixel with no temporal reuse at all (8.3). Neither is possible in
core D3D11.

### 8.3 Decima / Horizon Zero Dawn (de Carpentier and Ishiyama, SIGGRAPH 2017)

- "We never render at the pixel centers, but only on the pixel's edges. In fact, we render
  HORIZONTALLY BETWEEN the pixel centers for odd frames, and VERTICALLY between pixel centers for even
  frames." This uses camera jitter only: (0.5, 0) px and (0, 0.5) px, with no MSAA or EQAA.
- Then FXAA runs on those samples. They are "resampled from the pixel's edges to the pixel's centers"
  with "a 4-tap resampling kernel with negative lobes ... amount of sharpening is determined based on
  local contrast".
- The previous sharpened frame is blended 50/50, giving "effectively 4 samples of data per pixel".
  On rejection, the current unsharpened pixel is output.
- The pattern is "similar to FLIPQUAD ... sharper ... No extra mipmap logic. No MSAA/EQAA hardware
  needed. Inferior in removing jaggies from lines and grids (solved by FXAA)".
- Cost: FXAA + TAA + UI is ≤1 ms on PS4 at 1080p. SMAA "can take up to 2 milliseconds when looking
  down at the grass in Horizon. And that's 3 times slower than just looking up at a clear sky."

### 8.4 Experiment E3: the Decima 4-sample pattern in ONE frame, D3D11-legal (my derivation, untested)

- The standard 2x samples are at (+0.25,+0.25) and (-0.25,-0.25) px.
- Add a **constant** effective sample shift of (+0.25,-0.25) px. That is a geometry shift
  `g = (-0.25,+0.25)` (y down), i.e. projection jitter `j = (-0.25,-0.25)` every frame.
- Sample 0 then lands on the **right-edge midpoint** (+0.5, 0) and sample 1 on the **top-edge
  midpoint** (0, -0.5).
- So each pixel center is surrounded by four samples at distance 0.5: its own S0 (right) and S1 (top),
  the left neighbor's S0 (its left edge), and the bottom neighbor's S1 (its bottom edge). This is
  exactly Decima's "Ours" pattern, achieved with no temporal blending.

```
edge pixels (msaaEdge/smaaTouched): out(x,y) = 0.25 * ( L0(x,y) + L1(x,y) + L0(x-1,y) + L1(x,y+1) )
other pixels:                       out(x,y) = L0(x,y)   (fully covered: L0 == L1)
```

- Here `L0` and `L1` are the per-sample layers, after SMAA 1x on each layer. Each layer is a regular
  grid, so use subsample index 0. Offsets of ±0.5 are not in SMAA's area textures.
- Shading for fully covered pixels happens at the shifted center. Unjitter the material interpolants
  with `EvaluateAttributeSnapped(..., int2(-4, +4))`. The remaining constant 0.35 px shift of
  lighting is a uniform, static image offset and is invisible.
- There is no ghosting and no velocity dependence.
- Open questions: interaction with SMAA (morphological AA on shifted grids), and quality versus 4x.
  Worth an A/B run (section 11, M7).

---

## 9. Responsive mask path (VFX ghosting)

- **Video (1:03-1:04, 1:33-1:34):** VFX and transparent materials write a responsive value into alpha.
  It reduces temporal blending, and the material flag is set by artists.
- **Prior art:**
  - Karis 2014: the "Responsive AA" material flag "Sets stencil when rendering translucency. Temporal
    AA pass tests stencil and uses minimal feedback. Unfortunately need >0 feedback to prevent visible
    jittering", because of accumulation.
  - FSR2 reactive mask: 0 to 1, where 1 means "fully reactive". Apps "should write alpha to the
    reactive mask", and the guidance is "clamping the maximum reactive value to around 0.9".
- **Our path:**
  1. The HDR lighting target needs an alpha channel (RGBA16F). R11G11B10 has no alpha; in that case
     use a separate R8 MSAA target.
  2. Opaque and deferred lighting write alpha = 0.
  3. Transparent/VFX blend state:
     - Color is unchanged.
     - Alpha: `BlendOpAlpha = D3D11_BLEND_OP_MAX` (SrcBlendAlpha and DestBlendAlpha are ignored for
       MAX, but set them to ONE).
     - Shader alpha = `responsiveness * coverage`, where `responsiveness` is the material parameter in
       0..1 and `coverage` is the sprite opacity.
  4. Tonemap pass: `aaMask.g = max(alpha_s0, alpha_s1)`.
  5. Temporal pass: `w *= 1 - max(aaMask_cur.g, aaMask_prev.g at p - v)`.
     - The *previous* value matters because a particle without velocity ghosts at its *old* position.
     - Two-frame blending needs no "<0.9" clamp: a value of 1 just shows the current 2x-SMAA'd frame.
- **Requirement for everything else:** full-scene motion vectors, including skinned meshes,
  vertex-animated and alpha-tested foliage (wind evaluated at both the previous and the current time),
  and moving instances. They must be jitter-free.

---

## 10. Recommended implementation for our engine

### 10.1 Inputs from earlier stages (requirements on other notes)

- HDR scene color: 2x MSAA RGBA16F, standard pattern, alpha = responsive mask.
- Motion vectors: 2x MSAA RG16F, **jitter-free** (computed from unjittered current and previous
  view-projection matrices), in UV units, current minus previous. That is
  `(cNDC/cW - pNDC/pW) * float2(0.5, -0.5)`, the same convention as the SMAA demo.
- Optional: the engine's per-pixel MSAA coverage/edge flag (video), and a 1x nearest linear depth for
  the optional depth test.

### 10.2 Jitter sequence

```cpp
// frame parity p = frameIndex & 1;  jitter applied ONLY to the main camera's raster projection
// (not shadow maps, not UI, not velocity matrices). Mode 4x by default.
static const float2 kJ4x[2]   = { {-0.125f, -0.125f}, {+0.125f, +0.125f} };   // SMAA demo getJitter(MODE_SMAA_4X)
static const float2 kJT2x[2]  = { {-0.25f,  +0.25f}, {+0.25f, -0.25f} };      // MODE_SMAA_T2X (1x MSAA)
static const float4 kIdx4x[4] = { {5,3,1,3}, {4,6,2,3}, {3,5,1,4}, {6,4,2,4} }; // [2*p + msaaReorder(layer)]
float2 j = kJ4x[p];
XMMATRIX viewProjJittered = view * proj * XMMatrixTranslation(2.0f * j.x / W, 2.0f * j.y / H, 0.0f);
int2 unjitter16 = { (int)lroundf(16.0f * j.x), (int)lroundf(-16.0f * j.y) };   // section 7
```

### 10.3 Resources (1920x1080)

| resource | format | size | notes |
|---|---|---|---|
| layer[2] | R8G8B8A8_TYPELESS | 2 x 7.91 MiB | RTV UNORM_SRGB (tonemap writes), SRV UNORM (edge detect), SRV UNORM_SRGB (blend) |
| velocity1x | R16G16_FLOAT | 7.91 MiB | custom resolve: average of the 2 samples (do not `ResolveSubresource` if coverage bits are packed in the same buffer) |
| aaMask[2] (ping-pong) | R8G8B8A8_UNORM | 2 x 7.91 MiB | r = msaaEdge, g = responsive, b = smaaTouched (a unused). RG8 + R8 works too. |
| edgesTex | R8G8_UNORM | 3.96 MiB | cleared per layer (A/B vs RGBA8) |
| blendTex | R8G8B8A8_UNORM | 7.91 MiB | cleared per layer |
| smaaStencil | D24_UNORM_S8_UINT (1x) | 7.91 MiB | stencil cleared per layer; reuse any existing 1x DS |
| areaTex / searchTex | R8G8_UNORM 160x560 / R8_UNORM 64x16 | 175 KB / 1 KB | from the headers; BC5/BC4 optional |
| smaaOut[2] (ping-pong) | R8G8B8A8_TYPELESS | 2 x 7.91 MiB | RTV/SRV UNORM_SRGB; alpha = velocity code. **This, not the temporal output, is "previous".** |
| aaOut | R8G8B8A8_UNORM_SRGB (or whatever post takes) | 7.91 MiB | temporal pass output, consumed by DOF/motion blur/UI; never read back next frame |

Total is about 83 MiB, or about 75 MiB if `aaOut` is an existing post-chain target. Either way it is
about 1% of 8 GB.

### 10.4 Pass list per frame

```
P0  Tonemap+Separate (fullscreen PS, pixel frequency)
      in : HDR Texture2DMS<float4,2> (Load s0, s1), MV Texture2DMS<float2,2>, tonemap LUT
      out: RT0 layer[0] = tonemap(s0)  (sRGB RTV)
           RT1 layer[1] = tonemap(s1)
           RT2 velocity1x = 0.5*(mv0 + mv1)
           RT3 aaMask[cur].rg = (any(abs(gamma0-gamma1) > 2/255), max(a0, a1))   (write mask RG)
P1-3 SMAA on layer[0]: clear edges, blend, stencil; Edge (stencil REPLACE) -> Weights (stencil EQUAL,
     SubsampleIndices = kIdx4x[2p + reorder(0)]) -> Neighborhood (RT0 smaaOut[cur], blend OFF;
     RT1 aaMask[cur] write-mask B, blend OFF)
P4-6 SMAA on layer[1]: same, SubsampleIndices = kIdx4x[2p + reorder(1)]; Neighborhood with
     RT0 blend BLEND_FACTOR/INV_BLEND_FACTOR (color and alpha), factor 0.5; RT1 write-mask B, BlendOp MAX
P7  Temporal (fullscreen PS): smaaOut[cur], smaaOut[prev], velocity1x, aaMask[cur], aaMask[prev]
     -> aaOut.  (optional: depth test, sharpen)
swap cur/prev
```

`IndependentBlendEnable = TRUE` is needed in P0, P3 and P6, because the render targets use different
write masks or blend ops. Edge detection defaults to `SMAAColorEdgeDetectionPS`; Luma is the
scalability option, per the video and Filmic slide 137.

### 10.5 Temporal pass (P7)

```hlsl
Texture2D<float4> CurTex   : register(t0); // smaaOut[cur]  (UNORM_SRGB view -> linear)
Texture2D<float4> PrevTex  : register(t1); // smaaOut[prev] (previous frame's SMAA S2x output)
Texture2D<float2> VelTex   : register(t2); // velocity1x, UV units, cur - prev, jitter-free
Texture2D<float4> MaskCur  : register(t3); // aaMask[cur]
Texture2D<float4> MaskPrev : register(t4); // aaMask[prev]
SamplerState LinearClamp : register(s0);
SamplerState PointClamp  : register(s1);
cbuffer TemporalCB : register(b0) {
    float4 RtMetrics;      // 1/W, 1/H, W, H
    float  WeightScale;    // 30 (SMAA_REPROJECTION_WEIGHT_SCALE)
    float  HistoryValid;   // 0 on first frame, camera cut, resize, mode switch
    float  EdgeOnly;       // 1 = section 6 mask
    float  ClampSpeedPx;   // 0.5; <= 0 disables the clamp
    float  SharpResample;  // 1 = fast smootheststep fetch instead of point
};

float4 TemporalPS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int3   p    = int3(pos.xy, 0);
    float4 cur  = CurTex.Load(p);
    float2 v    = VelTex.Load(p);
    float2 puv  = uv - v;                                     // where this pixel was last frame
    float4 prev = SharpResample > 0
                ? PrevTex.SampleLevel(LinearClamp, SmootheststepUV(puv, RtMetrics.zw), 0)
                : PrevTex.SampleLevel(PointClamp, puv, 0);

    // 1. SMAA T2x velocity weighting (alpha = sqrt(5*|v|), antialiased by the neighborhood pass)
    float delta = abs(cur.a * cur.a - prev.a * prev.a) / 5.0;
    float w = 0.5 * saturate(1.0 - sqrt(delta) * WeightScale);

    // 2. responsive mask (current and previous position) + 3. edge-only mask
    float4 mc = MaskCur.Load(p);
    float4 mp = MaskPrev.SampleLevel(PointClamp, puv, 0);
    w *= 1.0 - max(mc.g, mp.g);
    float edge = max(max(mc.r, mc.b), max(mp.r, mp.b));
    w *= lerp(1.0, edge, EdgeOnly);
    w *= HistoryValid * (all(puv == saturate(puv)) ? 1.0 : 0.0);

    [branch] if (w > 0.0 && ClampSpeedPx > 0.0) {
        // 4. motion-gated 3x3 clamp on the current S2x output
        float3 mn = cur.rgb, mx = cur.rgb;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = CurTex.Load(p, int2(x, y)).rgb;
            mn = min(mn, c); mx = max(mx, c);
        }
        float speedPx = length(v * RtMetrics.zw);
        prev.rgb = lerp(prev.rgb, clamp(prev.rgb, mn, mx), saturate(speedPx / ClampSpeedPx));
    }
    return float4(lerp(cur.rgb, prev.rgb, w), 1.0);
}
```

- `Load` with `int2` offsets is legal for constant offsets in [-8,7].
- Optional sharpening, merged here (video 1:33): apply a contrast-adaptive cross-shaped unsharp mask
  to `cur` using the 4 orthogonal taps already loaded for the clamp, with the weight scaled down by
  local min/max contrast (CAS-style). Keep it **off by default** and compare against NVIDIA NIS
  sharpen-only. Decima sharpened each frame *before* the 50/50 blend; doing that would need the same
  kernel on `prev`.

### 10.6 Order of implementation (each step testable)

1. SMAA 1x on a 1x target. Validate `edgesTex` and `blendTex` visually, then add stencil.
2. S2x: tonemap MRT layers, sample-order check, two runs with blend factor.
3. T2x/4x reference: jitter, ping-pong `smaaOut`, `SMAAResolvePS` formula. Check that a static scene
   is stable after 2 frames.
4. Texture unjitter (section 7).
5. Responsive mask, motion-gated clamp, edge-only mask.
6. Optional: Filmic edge suppression, smootheststep, depth test, compute deferred queue, E3.

---

## 11. A/B test plan

**Modes.** Every mode is switchable at runtime. Use the same camera paths and scenes.

| id | mode |
|---|---|
| M0 | no AA, 1x |
| M1 | SMAA 1x (HIGH; also ULTRA) |
| M2 | SMAA S2x (2x MSAA, no jitter) |
| M3 | SMAA T2x (1x + two-frame, reference resolve) |
| M4 | SMAA 4x reference (S2x + T2x, full-screen blend, velocity weighting only, no UV unjitter) |
| M5 | **ours**: M4 + UV unjitter + edge-only mask + responsive + motion-gated clamp (+ Filmic edge suppression as M5b) |
| M6 | accumulation TAA baseline (Karis-2014 style) on 1x: Halton(2,3) 8-sample jitter, 3x3 nearest-depth velocity dilation, bilinear history (also Catmull-Rom 5-tap from Filmic slide 92), 3x3 YCoCg min/max clip, `out = lerp(history, cur, 0.1)`, output fed back as history |
| M7 | experiment E3 (constant-jitter Decima pattern, single frame) |

**Reference images.**

- Add an offline capture mode: freeze scene time, render 16 sub-frames with a 4x4 stratified jitter
  and no AA, and box-average them in linear space. That gives a 16x SSAA ground truth for each frame
  of a sequence, so it works for motion too.
- A no-jitter 1x frame is the texture-sharpness reference.

**Scenes.**

- S1: sky with distant geometry (cheap case).
- S2: typical interior/exterior with long shallow edges and thin wires.
- S3: worst case: dense alpha-tested, wind-animated grass covering the screen (the video's ~1 ms S2x
  case).
- S4: textured plane with high-frequency text or checker under a sharp shadow edge.
- S5: ghosting rig: a dark box crossing a bright checker at 2, 8 and 16 px/frame, plus an additive
  particle emitter moving without velocity.

**Measurements.**

1. **Cost.** D3D11 timestamp queries (`D3D11_QUERY_TIMESTAMP_DISJOINT` plus per-pass
   `D3D11_QUERY_TIMESTAMP`, read back 3 frames later). Take the median and p95 of 600 frames per
   pass: P0, each SMAA pass per layer, P7, and the M6 resolve. Report each scene at 1080p.
   - Targets: S2x+T2x total ≤1 ms worst case (S3), as the video's budget implies.
   - Also report the edge-pixel count, i.e. the stencil coverage via an occlusion query on the weights
     pass. The Filmic, Decima and TSCMAA numbers all scale with edge count.
2. **Static stability** (camera fixed, jitter on, 120 frames):
   - Flicker energy F = mean over pixels of `|out_t - out_{t-1}|`, in 8-bit units, plus % of pixels
     with difference > 1/255.
   - Expected: M2 is 0; M3, M4 and M5 are about 0 for blended pixels.
   - M5's non-masked pixels show the residual *lighting* jitter (section 7). This is the key number
     for the 1:40 concern, and S4 isolates it.
   - M6 shows clamp flicker.
3. **Motion stability** (slow pans at 0.1, 0.25 and 0.5 px/frame):
   temporal error `TE = mean |(out_t - out_{t-1}) - (ref_t - ref_{t-1})|` against the 16x reference
   sequence.
4. **Spatial quality** against the 16x reference: PSNR and NVIDIA ꟻLIP (BSD-3, `pip install
   flip-evaluator`), over the full frame and separately on edge pixels (msaaEdge or smaaTouched) and
   non-edge pixels. On S4, compare non-edge PSNR to the no-jitter 1x frame: texture blur shows here.
   Expect M4 < M5 ≈ M2.
5. **Ghosting (S5).** Trail length = maximum distance behind the moving object where
   `|out - ref| > 0.05`, plus integrated ghost energy. Expect M3/M4/M5 at 1 frame or less, and M6 at
   several frames. For VFX, compare M5 with and without the responsive mask, and with and without the
   previous-frame mask term.
6. **Foliage (S3).** M5 with and without foliage motion vectors, and with velocity weighting only vs
   velocity weighting plus clamp.
7. **Visual.** Side-by-side crops and frame-by-frame video at 200-400% zoom. The two-step gradient
   test from 1:30: a near-horizontal edge with SMAA disabled must show exactly the 4x MSAA-like steps
   under M4/M5, and a wide blur under M6.

Dump frames through staging textures to PNG (linear-to-sRGB as displayed). Compute metrics offline in
Python (numpy, flip-evaluator).

---

## 12. Open questions

1. Filmic edge-suppression constants (`k`, pattern threshold) are unpublished, and the index layout
   is reconstructed. Tune and validate on synthetic failure cases.
2. Silhouette line detection (Filmic) has no published rule. Skip, or design our own.
3. Motion-gated clamp threshold (0.5 px/frame) and K=30: both need tuning at 1080p.
4. Lighting sub-pixel shift on non-masked pixels. How visible is it after UV/normal unjitter? Do we
   need depth-plane re-centering in deferred lighting (section 7)?
5. Depth test: is a predicted previous depth needed to avoid false rejections near the camera?
6. RG8 vs RGBA8 edges and BC5/BC4 LUTs on an RTX 5060 (2013 NVIDIA advice may be stale).
7. E3 (constant-jitter Decima pattern): interaction with per-layer SMAA, and whether it beats 4x
   without any temporal component.
8. Whether `EvaluateAttributeSnapped` accepts a dynamic offset on our fxc/driver, and its
   center-vs-centroid semantics. Verify with a debug interpolant.
9. Interaction with motion blur and DOF ordering (video: DOF after AA, motion blur partly before
   tonemapping). The temporal pass must run on the same image the velocity describes.

---

## Sources

- Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering"
  (2026-08-01), https://www.youtube.com/watch?v=SnNm7rSSvlg. Local transcript, minutes 0:09, 0:35-0:38,
  1:03-1:04, 1:19-1:43. Provided the requirements and positioning.
- iryoku/smaa (Jimenez, Echevarria, Masia, Navarro, Gutierrez; v2.8, 2013, MIT),
  https://github.com/iryoku/smaa.
  - `SMAA.hlsl`: passes, presets, defines, entry points, sRGB rules, stencil, S2x/T2x/4x tables,
    `SMAAResolvePS`, `SMAASeparatePS`.
  - `Textures/AreaTex.h` and `SearchTex.h`: formats and sizes.
  - `Demo/DX10/Code/SMAA.cpp`, `Demo.cpp`, `Shaders/SMAA.fx`, `Simple.fx`: blend factor, ping-pong
    flow, jitter values, subsample indices, MSAA order detection, velocity convention.
  - `Scripts/AreaTex.py`: U-pattern `smootharea`.
  - Commit history (MIT switch, BC4 SearchTex, U-shape smoothing, R8G8 note).
- Jimenez, Echevarria, Sousa, Gutierrez, "SMAA: Enhanced Subpixel Morphological Antialiasing",
  Eurographics 2012,
  https://www.iryoku.com/smaa/downloads/SMAA-Enhanced-Subpixel-Morphological-Antialiasing.pdf.
  Provided eq. 6-7 (velocity weighting, K=30), the S2x/T2x/4x rationale, velocity-through-SMAA, and
  GTX 470 1080p timings.
- Jimenez, "Filmic SMAA: Sharp Morphological and Temporal Antialiasing", SIGGRAPH 2016 Advances,
  https://www.iryoku.com/downloads/Filmic-SMAA-v8.pptx. Text, notes and rendered slides including
  hidden ones. Provided morphological edge suppression code, silhouette lines, U-patterns, deferred
  queues, dual edge, LDS, the 0.774→0.475 ms figure, the temporal toolset, the depth test, fast
  smootheststep, quincunx, texcoord unjitter code, color weighting, velocity packing, and PS4 budgets.
- Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014,
  https://www.iryoku.com/downloads/Next-Generation-Post-Processing-in-Call-of-Duty-Advanced-Warfare-v18.pptx.
  Provided interleaved gradient noise and 2-frame temporal dithering with SMAA T2x.
- Sousa, "Anti-Aliasing Methods in CryENGINE 3", SIGGRAPH 2011,
  http://iryoku.com/aacourse/downloads/13-Anti-Aliasing-Methods-in-CryENGINE-3.pdf. Provided Crysis 2
  temporal AA: reprojection range clamping `fBlendW` and the depth mask.
- Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014,
  https://de45xmedrsdbp.cloudfront.net/Resources/files/TemporalAA_small-59732822.pdf. The
  counterexample: Halton jitter, exponential history, Blackman-Harris wide filter, neighborhood clamp
  and clip, velocity weighting dismissed, Responsive AA stencil, mip bias.
- de Carpentier and Ishiyama, "Decima Engine: Advances in Lighting and AA", SIGGRAPH 2017,
  https://advances.realtimerendering.com/s2017/DecimaSiggraph2017.pdf. Provided the edge-midpoint
  2-frame pattern, two-frame input reuse, 1D adaptive sharpen, FLIPQUAD comparison, and PS4 costs.
- Intel, "Temporally Stable Conservative Morphological Anti-Aliasing (TSCMAA)" (archived 2018-02-28),
  https://web.archive.org/web/20180228205640/https://software.intel.com/en-us/articles/temporally-stable-conservative-morphological-anti-aliasing-tscmaa.
  Provided edge-candidate-only TAA, the 0.8 blend weight, 5-tap bicubic, YCoCg variance clipping, HD
  630 scaling numbers, and the BSD-3 license. The code download is not archived, and GitHub has no
  TSCMAA repository (CMAA2 is at https://github.com/GameTechDev/CMAA2).
- Microsoft MiniEngine FXAA compute shaders,
  https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine/Core/Shaders
  (`FXAAPass1CS.hlsli`, `FXAAPass2CS.hlsli`, `FXAAResolveWorkQueueCS.hlsl`). Provided the deferred
  work-queue pattern that Filmic SMAA adopted.
- Smedberg (and Lottes), "Next-Generation AAA Mobile Rendering", GDC 2014,
  https://gdcvault.com/play/1020756/Next-Generation-AAA-Mobile. Members-only. The content was read
  from the Game Connection 2014 version,
  https://www.slideshare.net/slideshow/next-generation-mobile-gp-us-and-rendering-techniques-niklas-smedberg/38879047:
  early UE4 mobile 2-frame jittered blend after tonemapping, no reprojection.
- Microsoft Docs: `D3D11_STANDARD_MULTISAMPLE_QUALITY_LEVELS`,
  https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_standard_multisample_quality_levels
  (standard and center patterns, support rules). `EvaluateAttributeSnapped`,
  https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/evaluateattributesnapped (16x16 offset
  grid, SM5, pixel shader).
- WebGPU CTS `multisample_info.ts`,
  https://chromium.googlesource.com/external/github.com/gpuweb/cts/+/HEAD/src/webgpu/multisample_info.ts.
  Secondary confirmation of the 1/16 standard grid and the 4x positions.
- NVAPI `NvAPI_D3D11_RASTERIZER_DESC_EX`,
  https://docs.nvidia.com/nvapi/structNvAPI__D3D11__RASTERIZER__DESC__EX.html. The D3D11 PSL fields
  (vendor-only, excluded).
- AMD AGS SDK header, https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK/blob/master/ags_lib/inc/amd_ags.h.
  Confirms there is no DX11 sample-position API.
- AMD FidelityFX FSR2 README, https://github.com/GPUOpen-Effects/FidelityFX-FSR2. Reactive mask
  semantics (alpha as reactivity, 0.9 cap), cited as prior art for the responsive mask.
