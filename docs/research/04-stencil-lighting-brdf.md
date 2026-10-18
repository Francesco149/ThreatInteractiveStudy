# R4: Deferred lighting with stencil light volumes, per-shading-model lighting draws, BRDF and material model

Research note for the D3D11 educational engine (FL 11_0/11_1, SM 5.0, no vendor SDKs, deferred,
2x MSAA, 1080p60, RTX 5060 8 GB + Ryzen 9 5900X). It covers the deferred lighting stage and the
material model proposed in Threat Interactive's "Tutorial: How To Optimize Almost Every Step In
Modern Game Rendering" (2026-08-01). The relevant parts of the video are 30:00-34:00 (G-buffer and
material model), 41:00-45:00 (stencil volume lighting) and 47:00-49:00 (eyes and hair).

I checked the content against primary sources: NVIDIA 2004, Guerrilla 2007/2009, Crytek 2010-2013,
AMD 2007/2011, NVIDIA 2008, Intel 2010, DICE 2011, Chalmers 2012, Epic 2013, Heitz 2014, Penner 2011,
Striking Distance 2023, the Filament docs and the D3D11.3 functional spec. I also extracted frames
from the video, transcribed the text of the pipeline-diagram boxes (§1.2), and read the per-draw
numbers from the frame captures shown in the video (§1.3). Anything that is my own derivation,
interpretation or design choice is marked **[ours]**.

---

## 0. TL;DR: decisions for our engine

1. **Light loop (point and spot lights):** for each visible light, do three things.
   - **(a)** Draw one closed, low-poly volume mesh into stencil only. Use the *two-sided z-fail
     parity* trick: `CullMode NONE`, `DepthFunc GREATER` (reverse-Z), and `StencilDepthFailOp =
     INVERT` on both faces with `WriteMask = 0x01`. One draw marks exactly the samples inside the
     volume. It handles the camera-inside case and near-plane intersection with no CPU test.
     `DepthClipEnable = FALSE`.
   - **(b)** For every shading model (SM) present in the frame, draw a full-screen triangle
     **restricted by a scissor rect** around the light. Use `StencilFunc EQUAL`, ref
     `(SM<<1)|1`, `ReadMask 0x0F`, and `StencilPassOp ZERO` with `WriteMask 0x01`. Each draw
     shades only "marked AND this SM" samples and reverts the mark. This is the Days Gone structure
     the video adopts. The scissor is our addition, and it is essential (see §4.2).
   - **(c)** No clear is needed afterwards.
2. **Directional light (sun):** one full-screen draw per SM, testing only the SM bits. There is an
   optional "mark lit samples from the shadow mask" pre-pass, as in Killzone 2 and the video's sun
   box (§4.5).
3. **Stencil byte [ours, but it follows the video's constraints]:**

   | bit | name | meaning |
   |---|---|---|
   | 0 | L | transient light-volume mark |
   | 1-3 | SM | shading model (0 = none) |
   | 4 | C | lighting channel |
   | 5-6 | FWD | forward-pass tags (eye, hair) |
   | 7 | E | MSAA per-sample edge bit, Crytek convention; only if we use the pixel/sample split |

   SM = 0 covers sky, unlit, forward-only pixels and "empty" redundant MSAA samples. Every one of
   these is excluded from the light marking by the stencil test.
4. **Shading models (video: "main 5 ... up to 7"):** 1 = Lit (dual material profile), 2 = Skin
   (Callisto-style dual normal), 3 = Foliage (two-sided), 4 = Subsurface, 5 = Cloth. Values 6 and 7
   are reserved. Each SM is a separate pixel-shader permutation that loads only its own inputs. For
   example, only Skin reads the dual-normal target and the skin LUT.
5. **BRDF baseline:**
   - Specular: GGX / Trowbridge-Reitz D, height-correlated Smith visibility (Heitz 2014; Hammon's
     fast form as an A/B option), Schlick Fresnel with Callisto's "specular Fresnel falloff"
     exponent, and the F0 / F90 2% rule.
   - Diffuse: Lambert, multiplied by Callisto's diffuse-Fresnel / retroreflection factor
     (defaults reduce exactly to Lambert + GGX).
   - Anisotropic GGX when the G-buffer aniso flag is set.
   - Optional multiscatter energy compensation.
6. **Dual Material Profile (DMP) [ours, implementing the video's description]:**
   - G-buffer `R8G8B8A8_UINT` holds the profile blend (the former metallic), the specular scale,
     and a 13-bit DMP ID (8192 slots).
   - The DMP table, `StructuredBuffer<uint>`, maps each ID to two profile indices. The profile
     table, `StructuredBuffer<BrdfProfile>`, holds per-profile BRDF parameters: high-precision F0
     from IOR (or complex IOR), conductor flag, Callisto controls, and SM tint.
   - Blending is a mixture of BRDF *results*. It is cheap because roughness, and therefore D·V, is
     per-pixel and shared: only F and the diffuse factor are evaluated twice.
7. **The video's headline claim is not settled by published data.** It says stencil volumes give a
   lower single-light-per-pixel cost than compute-tiled lighting, but nobody has published that
   number. Published data (Lauritzen 2010, Olsson 2012) shows tiled/clustered winning when there are
   many overlapping lights. We A/B it ourselves (§10): stencil vs no-cull vs scissor-only vs
   volume-mesh vs a tiled compute baseline, with 1/16/64 lights and MSAA off/2x, and we separate
   the per-light fixed cost from the per-lit-sample cost.

---

## 1. How the video positions it

### 1.1 Narration

- **(41:00)** The video claims: "Every time we find a game that uses compute shaders to process all
  lights in one draw, the singular light per pixel cost has always been slower than stencil volume
  lighting." It names Fox Engine, Crysis 3 and Days Gone as users of stencil volume lighting. The
  video gives no numbers for the compute side.
- **(41:30-42:30)** A cheap volume increments stencil values inside the light's range. The
  expensive shader runs only on the marked samples, and "the lighting draw simultaneously reverts
  the stencil volume values to what they were before".
  - Fox Engine draws the lighting with the same volume mesh. This causes quad overdraw along the
    mesh's internal edges.
  - Days Gone draws a full-screen triangle *per shading model*. Quad overdraw then appears only
    along shading-model edges. The pipeline adopts this.
- **(42:50-43:40)** Per-SM draws let each draw load only what it needs, e.g. only skin samples the
  dual-normal buffer, as only Days Gone's skin draws use the skin LUT. This "creates complexity room
  for advanced PCF ... and more complex BRDF shading models". In contrast, UE's branchy uber-shader
  "has to separate shadowing in a slower separated pass from lighting".
- **(44:20)** "Stencil volumes become slower with MSAA depth stencil buffers, which is why the volume
  itself needs to be rendered with early stencil testing to prevent invocations on sub-sample pixels
  where that empty / sky shared value is currently set." Since 37:00-38:00, a per-sample stencil
  carries the shading model, and empty/disabled samples share a value with sky.
- **(31:00-34:00) Material model:**
  - Normals are RGB10A2, with A2 holding the anisotropic flag and one lighting-channel bit.
  - Albedo + roughness are RGBA8.
  - A "dual material profile ID" selects a pre-authored pair of profiles (e.g. paint + iron).
  - The former metallic channel sets which profile is predominantly expressed.
  - There is an "8,000 slot limit".
  - The specular channel "always scale[s]" a high-precision IOR stored in the profile "to zero".
    Legacy specular/cavity maps must be preprocessed so values agree across profile edges.
  - The DMP ID is separate from the shading-model ID.
- **(47:40-48:30)** Eyes and hair are forward-rendered or tiled-forward. They use stencil values
  written during their base pass draws.

### 1.2 Diagram text, transcribed from frames at ~43:10 and ~31:20

**Box "Deferred Lighting Structure for Non-Cascaded Lights (Shadowed & Non-Shadowed)"**

Budget **1.50 ms**. The loop is "Iterate per visible light".

- **"First Step: Stencil Volume (Sample Frequency)":** "Render the retrospective dynamic or baked
  mesh-volume to increase/decrease the current stencil values by 1. Use early stencil testing with
  the sky value to prevent changes/invocations on redundant SV_Coverage pixels."
- **"Lighting Channel Stencil Pass":** "If the light has artist enabled 'channel restrictions' such
  as a 'character only light'; the channel mask can be referenced from the Normal RGB10A2 Alpha. It
  would be great if this could be referenced in the initial stencil volume."
- **"Remaining Sample Frequency Draws (Stenciled Fullscreen Triangles Draws Based On Shading Model
  Stencil)":**
  - "Draw a single fullscreen triangle with early stencil testing to shade the retrospective
    shading model of said stencil value."
  - "Only the relevant basepass g-buffers, shadow map (if shadowed), and retrospective BRDF ID map
    should be used as the pixel shader inputs."
  - "The diffuse & specular output will be stored in the Emissive RGBA16 & recently cleared
    specular RGBA16. The Emissive RGBA16 alpha will store the Luminance."
  - "Each draw reverts the stencil to it's original unlit value."
  - "The main 5 models will be: Lit (new metallic workflow?), Full Callisto Dual Normal, Foliage,
    Subsurface, & Cloth. Up to 7 models is manageable for a system that doesn't track existing
    model types within light boundaries."
  - A minimal PCF plus a screen-space shadow method run "in the same direct lighting pixel shader"
    (no shadow projection masks for local lights).
  - Targets: Emissive/Diffuse `RGBA16_FLOAT (2xMSAA)` and Specular `RGBA16_FLOAT (2xMSAA)`.
- **Side note "4x Non-MSAA RGBA16s?":** "Sample frequency shading increases the quad cost by N
  samples because quad shading does not work in parallel outside of the sample index (samples 0 &
  1). We need extensive testing across vendors to see if forcing pixel frequency draws to treat
  subsample results as another render target output increases performance."

**Sun box "Directional Light (Sun/Moon)"**

Budget 0.60 + 0.32 + 0.18 + 0.36 ms.

- The shadow data is projected into a native-resolution non-MSAA `RG8_UINT` (R = shadow, G =
  subsurface data).
- The "Remaining Sample Frequency Draws (Separated Draws Based On Shading Model Stencil)" box says:
  "It might be helpful to use the RG16 shadow mask as a full screen pass on top of the stencil to
  prevent shading on pixels with full shadowing ... The Crysis 3 cascaded shadow mask optimization
  should be used."

**Base pass G-buffer box (~31:20)**

- `Depth Stencil D32S8 (2xMSAA)`
- `Motion Vectors/MSAA Data RGBA16_FLOAT`
- `Normals & Custom Stencil RGB10A2_UNORM`
- `Albedo & Roughness RGBA8_UNORM_SRGB`
- Note: "UE4 G-Buffer Layout + 13 or 16 Bit BRDF ID + 12+ Bits for Depth Bias"
- Step 5 adds `Emissive/Diffuse RGBA16_FLOAT` and `Dual Normal & Skin Mask RGB10A2_UNORM`.

**DMP slide (~33:30):** "13/16 Bit Dual Material Profile ID", "8-Bit Metallic/Profile Blend",
"Lit Shading Model's DMP LUT (8000+ Dual Material Profiles)", and "1st profile: Paint BSDF/BRDF
expressions / 2nd profile: Iron BSDF/BRDF expressions".

### 1.3 Numbers visible in the video's frame captures

The tool looks like Intel GPA's Graphics Frame Analyzer. Neither the GPU nor the resolution is
shown.

| Capture | Draw | GPU time | Stats |
|---|---|---|---|
| Fox Engine (MGSV) | stencil volume, DrawIndexed #4700 | 28 µs | 50 prims, "Pixels Rendered" 1034k, PS invocations 0 |
| Fox Engine | lighting with the **same mesh**, #4701 | 113 µs | PS invocations 1040k |
| Days Gone | stencil volume, #4241 | 29 µs | 792 prims, "Pixels Rendered" 1534k, PS 0 |
| Days Gone | full-screen triangle, SM draw #4242 | 76 µs | 3 verts, PS invocations 476k |
| Days Gone | full-screen triangle, next SM draw #4246 | 34 µs | PS invocations 47.6k |

**[ours]** The two Days Gone SM draws give an inference. 10x fewer invocations cost only 2.2x
less, so we estimate ≈25-30 µs of fixed cost per unscissored full-screen stencil-tested
triangle on that machine. That is the cost of rasterizing and stencil-rejecting the whole screen.
With 5 SMs × N lights, that becomes the dominant cost, and it is why we add a scissor rect (§4.2).
Treat this as a hypothesis from two data points and measure it.

---

## 2. Background and history (what and why)

| Year | Source | Contribution relevant here |
|---|---|---|
| 2004 | Hargreaves & Harris, NVIDIA "Deferred Shading" (6800 Leagues) | Light volume geometry: sphere for point, cone for spot, quad or box for directional. **"Stencil cull"**: pass 1 renders the volume with color writes off, `Depth LESS`, stencil z-fail `REPLACE X`; pass 2 lights with stencil `EQUAL X`. "Rendering without color writes = 2x pixels per clock." For multiple materials: **"Re-render light multiple times, 1 for each BRDF"** versus a material-ID branch. This is the per-SM idea, 22 years earlier. GeForce 6800 guidance: "Don't switch z-test direction mid-frame", "Avoid frequent stencil reference / op changes". |
| 2005 | Shishkovtsov, *S.T.A.L.K.E.R.* (GPU Gems 2 ch. 9) | Material ID indexes a 3D light-response LUT (N·L, N·H, material), 64×256×4. Hardware filtering blends adjacent "layers". This is a precedent for ID-indexed BRDF profiles. A sun stencil mask (N·L × AO > 0) was estimated at ~3x and measured at "close to 2x" (GeForce 6800 Ultra, target 1024×768 @ 30 fps). |
| 2007 | Koonce, *Tabula Rasa* (GPU Gems 3 ch. 19) | Tight volumes, "greater" depth test with inverted winding. Light-volume stencil masking "was not worth it on average" under DX9 because of draw/state overhead. Marking sky pixels in stencil during the opaque pass is free and "can generate significant savings". |
| 2007/2009 | Valient, *Killzone 2* (Develop 07, GDC 09 notes) | The classic two-pass method. (1) Back faces, `GREATER_EQUAL` (normal Z), stencil write on depth pass marks "in front of the far boundary". (2) Front faces, `LESS_EQUAL` plus stencil `EQUAL` runs the light shader. Camera inside: single pass, no stencil. Lights smaller than **20% of the diagonal FOV** skip stencil and use only the depth-bounds test. Sun: a full-screen stencil pre-pass marks potentially lit pixels, "Approx. 50% of pixels skipped". |
| 2010 | Kaplanyan, *CryENGINE 3* | Artist-authored convex "clip volumes" per light, used as stencil culling geometry. "Stencil tagging is very fast on consoles." |
| 2011 | Sousa, Kasyan, Schulz, "Secrets of CryENGINE 3" | Light path depends on screen coverage. "FS quads with stencil volumes pre-passes: for global lights or when viewer is inside of light source. Else convex light volumes or even 2D quads + depth bounds when coverage not significant." |
| 2013 | Sousa et al., *Crysis 3* (GDC 13), CE3 Graphics Gems (SIGGRAPH 13) | Deferred lighting "still processed as usual". Stencil layout: 1 bit dynamic-geometry tag + 7 bits ambient areas. MSAA per-sample bit reserved (0x80) with `StencilWriteMask = 0x7F`; pixel-frequency passes read `~0x80`, sample passes read `0x80`. Tag the **entire 2×2 quad** as per-sample: "helps stencil culling efficiency ... in average about 1ms save". |
| 2011 | Thibieroz (AMD), "Deferred Shading Optimizations" | Camera inside: back faces with `Z = GREATER` (normal Z), "no need for stencil". Outside: front faces with `LESSEQUAL`, which wastes work on pixels behind the volume. Stencil: z-fail INCR back / DECR front, then "render where stencil != 0". Screen-aligned quads must bound the *ellipse* a sphere projects to. Discard zero-contribution pixels to save blending. Watch blend cost with fat formats and MSAA. |
| 2010 | Lauritzen (Intel), BPS course | "Full light meshes ... generally overkill", "**Two-pass stencil is more expensive than it is worth**", "Depth bounds test on some hardware, but not batch-friendly". Tiled shading wins with many lights (numbers in §5). |
| 2023 | TI video | Revives stencil volumes with per-SM full-screen draws (Days Gone), at sample frequency, with SM in stencil. |

The core trade-off is the same in every source. Stencil marking makes the expensive shader run on
*exactly* the lit samples, but each light costs extra draws, state changes and a full G-buffer
re-read and blend. That pays off when lights are large on screen but light few samples. It does
not pay off for many small or overlapping lights.

---

## 3. Stencil light volumes in detail

### 3.1 Conventions (read this first)

- **Depth: reverse-Z, `D32_FLOAT_S8X24_UINT`, infinite far plane.**
  - NVIDIA "Depth Precision Visualized" found zero comparison errors for float32 reverse-Z with an
    infinite far plane.
  - Near maps to 1, infinity maps to 0. Clear depth to 0.0; NVIDIA's clears guidance says use
    [0, 0.5) for GREATER-type tests.
  - The scene depth test is `GREATER` or `GREATER_EQUAL`.
  - View space here is **+Z forward (left-handed)**. In row-vector form (`mul(v, P)`) the
    projection is `P = [P00 0 0 0; 0 P11 0 0; 0 0 0 1; 0 0 n 0]`. So `depth = n / z_view` and
    `z_view = n / depth`.
- **D3D stencil test:**
  - The test is `(StencilRef & ReadMask) FUNC (stored & ReadMask)`. The **reference is the left
    operand**, the same convention as GL's `glStencilFunc`.
  - The order: if the stencil test fails, apply `StencilFailOp`. If it passes and depth fails,
    apply `StencilDepthFailOp`. If both pass, apply `StencilPassOp`.
  - The write is `new = (old & ~WriteMask) | (op(old) & WriteMask)`.
  - With `WriteMask = 0x01`, `INCR`/`DECR`/`INVERT` all become a toggle of bit 0.
  - Use **wrapping** `INCR`/`DECR`, never `_SAT`.
  - `D3D11_STENCIL_OP` values: KEEP, ZERO, REPLACE, INCR_SAT, DECR_SAT, INVERT, INCR, DECR.
- **DepthEnable = FALSE with StencilEnable = TRUE:** "the depth test is always pass when
  incorporated into the stencil operation" (D3D11.3 spec §17.9). A full-screen lighting triangle can
  therefore disable depth and still use the stencil.
- **DepthClipEnable = FALSE:**
  - Spec §15.4: primitives outside the depth range "are thus still rendered, but are given the value
    of the nearest limit of the viewport depth range. Even when Z clipping is disabled, primitives
    must be clipped such that only w > 0 vertices result."
  - So volume faces in front of the near plane get depth 1.0, faces beyond the far plane get 0.0,
    and geometry behind the eye is still clipped.
- **MSAA:** depth and stencil tests and updates are per sample, for both pixel-frequency and
  sample-frequency shaders (spec §16.2).

### 3.2 The marking test: two-sided z-fail parity (recommended)

**Rule.** For a closed (watertight) volume mesh, count the faces that lie *behind* the surface
sample along its view ray, i.e. the faces that **fail** the depth test. The sample is inside the
volume iff that count is odd. Faces are counted only when the stencil test passes; the test
excludes sky and empty samples.

Truth table for a convex volume (F = front face, B = back face):

| Situation | F fails? | B fails? | parity | inside? |
|---|---|---|---|---|
| surface in front of the volume | yes | yes | 0 | no |
| surface inside the volume | no | yes | 1 | **yes** |
| surface behind the volume | no | no | 0 | no |
| ray misses the volume | - | - | 0 | no |
| camera inside, surface inside (F behind eye, clipped) | - | yes | 1 | **yes** |
| camera inside, surface beyond the volume | - | no | 0 | no |
| F before the near plane (clamped to depth 1.0 → passes) | no | yes | 1 | yes, correct |

Why this works:

- It is Carmack's-reverse counting reduced to one bit. It is correct for *any* closed mesh, not
  only convex ones, so artist-made "baked" clip volumes work as well, as in Crytek's.
- It needs no CPU "is the camera inside?" test.
- The only requirement is that crossings *behind* the surface are never clipped away. With an
  infinite reverse-Z far plane they never are. With a finite far plane they are, so
  `DepthClipEnable = FALSE` is always set; it clamps instead of clipping.
- The mesh must be watertight: shared, bit-identical vertices and no T-junctions. D3D's top-left
  fill rule then covers each sample by exactly one of two triangles sharing an edge.

This is the video's "increase/decrease the current stencil values by 1": INCR on back faces and
DECR on front faces, with write mask 0x01, gives the same result as INVERT on both.

**D3D11 states for the mark pass:**

| Field | Value |
|---|---|
| Rasterizer | `FillMode SOLID`, **`CullMode NONE`**, **`DepthClipEnable FALSE`**, `ScissorEnable TRUE` (same rect as the lighting draws; harmless) |
| Depth | `DepthEnable TRUE`, `DepthWriteMask ZERO`, **`DepthFunc GREATER`** (same direction as the scene) |
| Stencil | `StencilEnable TRUE`, **`ReadMask 0x0E`** (SM bits), **`WriteMask 0x01`** |
| FrontFace = BackFace | `StencilFailOp KEEP`, **`StencilDepthFailOp INVERT`**, `StencilPassOp KEEP`, **`StencilFunc LESS`** |
| StencilRef | `0x00`. The test `0 < (stencil & 0x0E)` means SM ≠ 0, which skips sky and empty samples. |
| Channel-only light | Same, but `ReadMask 0x1E` and `StencilRef 0x10`. This passes iff C = 1 and SM ≥ 1, i.e. `0x10 < (stencil & 0x1E)`. This folds the video's "Lighting Channel Stencil Pass" into the volume, as the video wishes. |
| Output | Null pixel shader. Blend state with `RenderTargetWriteMask = 0`, or bind no RTVs. This gives depth/stencil-only rendering, "2x pixels per clock" since the GeForce FX era (NVIDIA 2004, 2008). |

**The INCR/DECR form (identical result):** use `FrontFace.StencilDepthFailOp = DECR` and
`BackFace.StencilDepthFailOp = INCR`. `FrontCounterClockwise` must then match the mesh winding;
INVERT does not care about winding.

**Cost caveats from vendor documents:**

- ATI/AMD (Persson 2007, "Depth in-depth"): "If either of z-fail or stencil-fail operations is not
  KEEP HiZ gets disabled", because tiles can't be rejected when a z-fail op must update them. The
  z-fail mark pass therefore gets no coarse-Z rejection on that hardware.
- The pass has no PS and runs at double rate. In the video's captures it costs ~28-29 µs for a
  large light.
- **Alternative that keeps HiZ:** use two draws with only *pass* ops (§3.3, variant B'). Measure
  before switching (§10).

### 3.3 Other marking variants, all in reverse-Z

**Variant B, the classic two-pass (Killzone 2), translated to reverse-Z:**

| Pass | Cull | DepthFunc | Stencil |
|---|---|---|---|
| 1. mark "in front of the far boundary" | `CullMode FRONT` (draw back faces) | `LESS_EQUAL` | Func ALWAYS (or the SM test), `PassOp REPLACE`, ref has L set, WriteMask 0x01 |
| 2. light | `CullMode BACK` (draw front faces) | `GREATER_EQUAL` | Func EQUAL L, `PassOp ZERO`, **and `StencilDepthFailOp ZERO`**, otherwise marked pixels in front of the volume stay marked |

- Camera inside: front faces get clipped at w ≤ 0, so pass 2 can't revert all marks. Valient
  therefore skips stencil in that case.
- **This variant does not combine with full-screen per-SM draws.** Those need the mark to encode
  *both* bounds, so use §3.2 or B' instead.

**Variant B' [ours], pass-ops only (HiZ-friendly on AMD per Persson 2007):**

1. Back faces, `DepthFunc LESS_EQUAL`, `PassOp INVERT` (or REPLACE 1): sets L where the surface
   is in front of the far boundary.
2. Front faces, `DepthFunc LESS`, `PassOp ZERO`: clears L where the surface is in front of the near
   boundary.

The result is exactly "inside" for a **convex** volume. With depth clamp, front faces before the
near plane get depth 1.0 and never clear anything, which is correct. Front faces behind the eye are
clipped, so they also don't clear, which is again correct: everything between the eye and the far
boundary is inside.

There are two catches:

- B' needs a convex mesh. It also needs two draws instead of one.
- Both passes test in the direction *opposite* to the scene (LESS_EQUAL / LESS vs the scene's
  GREATER). Coarse-Z units built for the scene direction can't reject for them (§4.4).

So whether B' is actually faster than the one z-fail draw is an open question. Measure it.

**Variant C, single-pass without stencil** (Thibieroz 2011, Valient's camera-inside path, also how
UE4/Unity-era engines draw lights):

- **Camera outside:** draw front faces with `DepthFunc GREATER_EQUAL`, in the scene direction, so
  HiZ works. This shades every surface behind the front face, including surfaces behind the volume
  (wasted; the attenuation is 0 there).
- **Camera inside or intersecting the near plane:** draw back faces with `DepthFunc LESS_EQUAL`
  (the "inverted" test). This shades every surface in front of the far boundary.
- Inside test **[ours]:** `distance(cameraPos, lightPos) < r_mesh + nearPlaneHalfDiagonal`.
- This is the "volume-mesh lighting, no stencil" A/B variant. With per-SM draws it must re-rasterize
  the mesh once per SM.

### 3.4 Restoring the stencil ("each draw reverts the stencil")

**Default, the video's design.** Each per-SM lighting draw uses `StencilFunc EQUAL`, ref
`(SM<<1)|1`, `ReadMask 0x0F`, `PassOp ZERO`, `WriteMask 0x01`. Every marked sample has some
SM ≥ 1, so the draw for that SM clears it.

**Rule:** issue a draw for **every SM present in the frame**. The CPU keeps a `smPresentMask` from
base-pass submissions. Otherwise marks leak into the next light, and the parity flips them back.
Add a debug "leak check" after the loop: a full-screen draw with stencil `EQUAL 1, ReadMask 0x01`
that writes magenta.

**Persson 2007 (ATI):** "there's nothing wrong with doing a partial clear of the stencil buffer by
using a stencil pass operation of ZERO ... can eliminate the need for a stencil clear and will work
fine with HiZ. For this to work it's important that the first pass tags pixels to render, rather
than pixels not to render." Our scheme follows that rule.

**NVIDIA counter-indication (GeForce 8/9 guide, 2008):**

- Coarse Z/stencil culling (ZCULL) "will not be able to cull any pixels ... If stencil writes are
  enabled while doing stencil testing (no stencil culling)".
- Fine-grained early-Z/stencil is still on. It is disabled only if, with depth/stencil writes on,
  the PS uses discard, alpha test, alpha-to-coverage, or `SampleMask != 0xFFFFFFFF`.
- So on that generation the restoring lighting draw loses *coarse* stencil rejection. Its cost
  becomes "rasterize + per-sample early stencil over the scissor rect", but invocations are still
  exact.
- Whether Blackwell behaves the same is undocumented.

That gives the A/B variant **V6**: lighting draws with `WriteMask 0x00` (no writes), followed by
one cheap "unmark" draw per light. The unmark draw is a scissored full-screen triangle with a null
PS, depth off, `StencilFunc EQUAL`, ref 1, `ReadMask 0x01`, `PassOp ZERO`, `WriteMask 0x01`.

**Never `discard` in a stencil-restoring lighting draw.** Discarded samples would keep their mark,
and discard also blocks early stencil writes. Thibieroz's "discard zero-contribution pixels" tip
applies only to draws that don't write stencil. Keep `SampleMask = 0xFFFFFFFF` in
`OMSetBlendState`.

`ClearDepthStencilView` cannot mask stencil bits, so a per-light *clear* would destroy the SM bits.
Restoring through stencil ops is the only clean option.

### 3.5 Coexistence with the other stencil bits

The full byte layout is in §9.1. These rules make it safe:

- **Base pass writes SM and C** with `StencilPassOp REPLACE`, `ReadMask 0x00`, `WriteMask 0x1E`,
  `StencilFunc ALWAYS`, and per draw `StencilRef = (SM<<1) | (C<<4)`.
  - Writes happen only where depth passes, so the visible surface wins, at sample granularity under
    MSAA.
  - No pixel-shader stencil export is needed. `SV_StencilRef` is optional D3D11.3 functionality
    (`PSSpecifiedStencilRefSupported`); query it, don't rely on it.
- Sky is never drawn, so it keeps SM = 0 from the frame clear.
- The MSAA "empty sample" step from R5/MSAA zeroes SM for redundant samples.
  - It runs *after* the base pass.
  - It is one sample-frequency full-screen pass that `discard`s non-redundant samples, with
    `PassOp REPLACE`, ref 0, `WriteMask 0x0E`.
  - That pass uses discard, but it writes the stencil only once per frame.
- Decals never write stencil. Fox Engine decals write everything except the SM ID.
- Every light-loop state uses `WriteMask 0x01`, so the L bit is the only bit ever modified there.
- Forward eye/hair passes use bits 5-6 with their own read masks.

### 3.6 MSAA specifics

- **Marking runs per sample automatically.** The stencil test `SM ≠ 0` makes sky and "empty"
  samples untouchable. That is exactly the video's "early stencil testing with the sky value to
  prevent changes/invocations on redundant SV_Coverage pixels".
- **Lighting frequency.** Three modes are worth supporting; the MSAA research note owns the choice.
  - **M1, the video's mode:** every lighting draw is sample-frequency (`SV_SampleIndex` input). Redundant
    samples of interior pixels have SM = 0, so early stencil kills them. Interior pixels run 1
    invocation and edge pixels run 2.
    - One draw per SM.
    - The resolve must copy sample 0 into the empty samples.
    - Cost risk: sample-frequency quads are formed per sample index, so helper lanes multiply
      (the video's "4x Non-MSAA RGBA16s?" note).
  - **M2, Crytek 2013:** E bit 0x80, tagged per 2×2 quad. For each SM, draw a pixel-frequency pass
    (`ReadMask 0x8F`, ref `(SM<<1)|1`) and a sample-frequency pass (ref `0x80|(SM<<1)|1`).
    Draw count doubles.
  - **M3, brute force:** every sample is shaded at sample frequency. Use it as the reference.
- **Depth for the lighting PS.** Read it from `Texture2DMS<float>` through an SRV
  (`R32_FLOAT_X8X24_TYPELESS`) while the DSV is bound **read-only-depth** (`D3D11_DSV_READ_ONLY_DEPTH`
  only, so stencil stays writable).
  - D3D11.3 spec §17.13: write enables are ANDed with the DSV flags, and the runtime unbinds
    conflicting SRVs.
  - UE4's "DepthRead_StencilWrite" mode relies on this combination.
  - **Verify** with the debug layer that the depth SRV is not being unbound.

### 3.7 Cost structure and batching

- **Per light:**
  - 1 mark draw: a ~80-320-triangle mesh, depth/stencil only.
  - Plus N_SM lighting draws: a full-screen triangle, scissored.
  - Plus optionally 1 unmark draw (V6).
- **State changes per light:** scissor rect, light constant buffer, two DS states plus StencilRef,
  RS, blend, VS/PS. With immutable state objects, 64 lights × 6 draws ≈ 400 draws, which is well
  within D3D11 CPU budgets on a 5900X. Measure CPU submit time anyway.
- **Light marks can't be batched.** One L bit and parity marking mean overlapping volumes in one
  draw would XOR each other. Batching disjoint lights would need per-pixel light identity, which
  is the tiled approach anyway. Not worth it at ≤ 64 lights.
- **Small lights.** Killzone 2 skipped stencil below 20% of the diagonal FOV, and Crytek used 2D
  quads + depth bounds "when coverage not significant". Lauritzen 2010 found two-pass stencil "more
  expensive than it is worth".
  - We keep a **"scissor-only" path (V1)** for lights whose scissor area is below a threshold, set
    by measurement. It skips the mark and uses SM-only stencil, so the shader runs for the whole
    rect and returns 0 outside the range.

---

## 4. Per-shading-model full-screen lighting (the Days Gone approach)

### 4.1 Structure

```
once per frame (after base pass, empty-sample step, shadow-map rendering):
    ClearRenderTargetView(specularRT, 0)            // video: "recently cleared specular RGBA16"
    OMSetRenderTargets({emissiveDiffuseRT, specularRT}, dsvReadOnlyDepth)
    bind G-buffer SRVs (incl. depth SRV), DMP + profile tables, shadow atlas
for each visible local light L (CPU frustum-culled):
    rect = SphereScissor(L)            ; if empty: skip
    RSSetScissorRects(rect); update LightCB(L)
    -- mark --
    RS_Volume, BS_NoColor, DSS_Mark{All|Channel}, VS_Volume, PS = null
    DrawIndexed(volume mesh of L)                   // icosphere or cone, see §6.2
    -- shade --
    RS_FullscreenScissor, BS_AddLight, VS_FullscreenTri
    for sm in 1..5 where smPresentMask has sm:
        PSSetShader(PS_Light[type(L)][shadowed(L)][sm])
        OMSetDepthStencilState(DSS_ShadeSM, (sm<<1)|1)
        Draw(3)                                      // reverts L on the samples it shaded
directional light: §4.5
```

**Full-screen triangle VS (no vertex buffer):**

```hlsl
cbuffer FullscreenCB : register(b2) { float g_TriangleDepth; }   // 0 unless using §4.3
float4 VS_FullscreenTri(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);                      // (0,0) (2,0) (0,2)
    return float4(uv * float2(2, -2) + float2(-1, 1), g_TriangleDepth, 1);  // covers [-1,1]^2
}
```

**Why a triangle and not a quad, or the volume mesh?**

- A quad has a diagonal seam, and 2×2 quads crossing it are shaded twice.
- A volume mesh has hundreds of internal edges, each causing partial quads. That is the video's
  Fox Engine criticism, shown with a wireframe: "Wireframe of lighting draw mesh = quad overdraw!".
- A triangle has no internal edges inside the screen.
- The gain for ALU-bound shaders is tiny: 0.2% on a GTX 1060 (30fps.net microbenchmark).
- For deferred lighting, the real cost of the mesh approach is the partial quads along *every*
  mesh edge in the light's footprint. With the triangle, partial quads appear only where the SM or
  the L mark changes inside a quad. Those helper lanes can't be avoided in D3D11.

### 4.2 Scissor rect: required, not optional [ours]

**Why.** A full-screen triangle rasterizes and stencil-tests every sample of the render target,
even when stencil rejects 99% of them. That is 1920×1080×2 ≈ 4.1 M sample tests per draw. Coarse
stencil culling (ZCULL/HiS) may be off while the draw writes stencil (§3.4). The Days Gone capture
in §1.3 suggests a ~25-30 µs floor per such draw. With 5 SMs × 64 lights that is ~10 ms, which is
hopeless. A scissor rect around the light's projected bounds makes that floor proportional to the
light's screen size. It makes "full-screen triangle + scissor" equivalent to a screen-aligned light
rectangle, but with no seams.

**Exact sphere bounds** come from Mara & McGuire 2013, as specialized by Kapoulkine 2023. The
formula below matches a brute-force projection to 4 decimals in our test. The inputs are the
view-space center `c` (+Z forward), the **mesh** radius `r` (§6.2), and the symmetric projection
terms `P00` and `P11`.

```cpp
// returns false if the sphere crosses the near plane (caller then uses the full viewport)
bool SphereNdcBounds(XMFLOAT3 c, float r, float zNear, float P00, float P11, XMFLOAT4& ndc /*minX,minY,maxX,maxY*/)
{
    if (c.z < r + zNear) return false;
    float czr2 = c.z * c.z - r * r;
    float vx = sqrtf(c.x * c.x + czr2), vy = sqrtf(c.y * c.y + czr2);
    float minx = (vx * c.x - r * c.z) / (vx * c.z + r * c.x);
    float maxx = (vx * c.x + r * c.z) / (vx * c.z - r * c.x);
    float miny = (vy * c.y - r * c.z) / (vy * c.z + r * c.y);
    float maxy = (vy * c.y + r * c.z) / (vy * c.z - r * c.y);
    ndc = { minx * P00, miny * P11, maxx * P00, maxy * P11 };   // NDC, +Y up
    return true;
}
// pixels: left = floor((minX*0.5+0.5)*W), right = ceil((maxX*0.5+0.5)*W),
//         top = floor((0.5-maxY*0.5)*H), bottom = ceil((0.5-minY*0.5)*H); clamp to viewport;
//         empty rect => light is off-screen => skip the light entirely.
```

- Spot lights: use the bounding sphere of the cone mesh. That is simple and conservative.
- `ScissorEnable TRUE` requires a scissor rect to be set for every draw. The sun uses a full
  viewport rect.

### 4.3 Optional emulated "near depth bound" [ours]

The depth bounds test (DBT) is not core D3D11. It is exposed only through NVAPI or AMD AGS
(UE4/Crytek/Killzone used DBT), which are out of scope. Two core-D3D11 substitutes exist:

- **Near bound for free.** Put the full-screen triangle at the light's nearest depth,
  `g_TriangleDepth = n / max(c.z - r_mesh, n)`, and enable `DepthFunc GREATER_EQUAL` (scene
  direction, no writes).
  - Tiles whose surfaces are all in front of the light are coarse-rejected by HiZ/ZCull, even when
    stencil culling is off. This is the "light behind a big foreground object" case.
  - Every marked sample is inside the volume, so it is behind its near bound and passes. Keep
    `StencilDepthFailOp KEEP`, because a non-KEEP z-fail op disables HiZ on AMD.
  - This depends on `r_mesh` being conservative. The debug leak check catches errors.
  - This is A/B variant **V7**. It is Thibieroz's "Set quad Z coordinates to Min Z of light" trick.
- **Far bound.** It is impossible in the same draw with one depth test. Stencil already handles it.

### 4.4 Hierarchical stencil: what is actually known

- **NVIDIA (GeForce 6+ ZCULL; 8+ fine-grained early Z and stencil; 2008 guide).**
  - Coarse Z/stencil culling is disabled by: no real depth clears, PS depth output, changing the
    depth test direction *while writing depth* (off until the next clear), and **stencil writes
    while stencil testing**.
  - It is "less efficient" when the depth buffer was written with a different test direction, with
    high-frequency depth, or with too many large depth buffers.
  - The 2004 deck: "Avoid frequent stencil reference / op changes".
  - Nothing newer is public. Treat these as hints and measure on the RTX 5060.
- **ATI/AMD (Radeon HD 2000+, 2007).**
  - HiZ covers depth *and* stencil.
  - z-fail or stencil-fail ops other than KEEP disable HiZ. Pass ops are fine.
  - Clear depth and stencil together.
  - Partial stencil "clears" with `PassOp ZERO` are fine.
  - GCN/RDNA HiS details are not publicly documented beyond patents (tile states pass / fail /
    unknown).
- **Crytek 2013:** tagging per-sample regions at **2×2-quad granularity** improved stencil-culling
  efficiency, "about 1ms save" on average. This shows coherence of stencil values matters to the
  coarse units. For the SM bits we can't choose granularity. For the E bit (mode M2) we can.
- **Practical conclusion for us:** rely only on fine-grained early stencil, which saves invocations
  and is guaranteed by `[earlydepthstencil]` or simply by having no discard, depth output or UAV.
  Bound the rasterized area with the scissor. Treat coarse stencil rejection as a bonus and measure
  V5 vs V6.

### 4.5 Directional light (sun or moon)

- **Per-SM draws:** one full-screen triangle per present SM, with `DepthEnable FALSE`,
  `StencilFunc EQUAL`, ref `SM<<1`, `ReadMask 0x0E`, `WriteMask 0`. No volume and no L bit.
- **Optional shadow-mask pre-mark** (video sun box; Killzone 2's "Approx. 50% of pixels skipped";
  S.T.A.L.K.E.R.'s "close to 2x"):
  - A full-screen sample-frequency pass reads the shadow mask (R = shadow) and **discards samples
    that are fully shadowed and have no subsurface term** (G channel).
  - Stencil: `StencilFunc LESS`, `StencilRef 0x01`, `ReadMask 0x0E` (that means SM ≠ 0, because
    `0x01 & 0x0E = 0`), `PassOp REPLACE`, `WriteMask 0x01`. This sets L on samples that need
    shading.
  - Then run the same per-SM draws as local lights: EQUAL `(SM<<1)|1`, restoring L.
  - Whether this wins depends on how much of the screen is in full shadow, so measure it.
- **A/B:** per-SM sun draws vs one uber-shader sun draw that reads SM from a stencil SRV
  (`X32_TYPELESS_G8X24_UINT`, `.g`) and branches. This directly tests the video's argument against
  branchy uber-shaders, without any light-culling confound.

### 4.6 What each SM draw loads (the video's "only the relevant inputs")

| SM | Inputs |
|---|---|
| 1 Lit | depth, GB0 normal+flags, GB1 albedo+roughness, GB2 DMP, DMP + profile tables, shadow (if shadowed) |
| 2 Skin | depth, GB0 (specular normal), GB1, GB2 (profile: skin spec params), **GB4 dual normal + curvature + skin mask**, **skin LUT**, shadow |
| 3 Foliage | depth, GB0, GB1, GB2 (profile: transmission tint), shadow; transmission needs the back-side shadow (§7.5) |
| 4 Subsurface | depth, GB0, GB1, GB2 (profile: subsurface tint/wrap), shadow; the sun also reads shadow-mask G |
| 5 Cloth | depth, GB0, GB1, GB2 (profile: sheen color), shadow |

Per-SM shaders also allocate registers only for their own path. That is the mechanism behind
"complexity room": GPU occupancy is limited by the *maximum* register count of any branch in an
uber-shader. To measure it, compare `dcl_temps` in `fxc /Fc` output, and actual registers per
thread and occupancy in Nsight Graphics.

---

## 5. Tiled and clustered alternatives: published numbers and a fair comparison

| Source (year) | Hardware / resolution | Result |
|---|---|---|
| Lauritzen 2010, BPS | GTX 480 and HD 5870, 1080p, Sponza with point lights | "Quad" deferred (screen-aligned quads, one-direction depth test) vs compute tiled. **Total:** slope ~20 µs/light vs ~4 µs/light. **4x MSAA:** ~35 µs vs ~5 µs per light. **Culling only:** ~7 vs ~0.5 µs/light. "Few lights overlap" in that scene. Conclusion: "Tile-based methods win going forward". |
| Andersson 2011, BF3 (GDC) | DX11 PC | Tiled CS: 16×16 tiles, min/max-Z per tile, groupshared light list, MSAA edge pixels re-packed. The stated motivation was traditional deferred's "massive overdraw & ROP cost when having lots of big light sources" and "expensive to have multiple per-pixel materials in light shaders". No per-light timing given. |
| Olsson, Billeter, Assarsson 2012, clustered (HPG) | GTX 480 | **Deferred with stencil optimization** ("light assignment will be exact per sample using a stencil test") does the **minimum** lighting computations, but in Sponza with 10k random lights it takes **97.1 ms**. TiledForward takes 23.6 ms, and clustered variants are under ~18 ms. Necropolis (~2500 lights): clustered 17 ms total (2.3 clustering + 1.5 assignment + 5.6 shading) vs tiled 26 ms (1.0 + 17.7 shading). Tiled did ~90 M lighting computations vs ~4-25 M for the others. |
| Killzone 2, 2009 | PS3 | Stencil not worth it below 20% of diagonal FOV. |
| Tabula Rasa, 2007 | DX9 | Stencil masking not worth it on average (draw/state overhead). |
| TI captures (§1.3) | unknown GPU and resolution | Days Gone: one light = 29 µs mark + 76 µs and 34 µs SM draws (+ more). Fox: 28 µs mark + 113 µs lighting. |

**How to read the claim fairly.** Stencil volumes are *work-optimal* per sample: Olsson's
"lighting computations" metric shows exactly one evaluation per lit sample. Their weaknesses are
per-light fixed costs (draws, marking, rasterizing the footprint) and per-light G-buffer re-reads
and blending. Each light re-reads ~20 B/sample and does an RGBA16F×2 read-modify-write per sample.
Tiled shading reads the G-buffer once per pixel and loops lights in registers. That amortizes
bandwidth when lights *overlap*, but it pays for tile-culling inefficiency (lights in a tile that
don't touch the pixel) and the uber-shader's occupancy.

So the video's claim ("single light per pixel cost") is plausible in its stated scope: few lights,
little overlap, heavy per-light shaders with shadows. It contradicts nothing published, but it is
also not demonstrated by any published number. Our A/B (§10) measures:

1. The **fixed cost per light** (intercept).
2. The **cost per lit sample-light pair** (slope).
3. The **overlap behavior**: N lights covering the same region.

All three are measured for both methods with the same BRDF code, the same shadows (none, for the
core test) and the same MSAA mode.

---

## 6. Light attenuation and volume geometry

### 6.1 Attenuation

**Karis 2013 (UE4).** "Physically accurate inverse-square falloff", windowed so it reaches 0 at the
light radius, "whilst still providing a soft transition":

```
falloff = saturate(1 - (d / r)^4)^2 / (d^2 + 1)
```

"The 1 in the denominator is there to prevent the function exploding at distances close to the
light source." A nice property is that changing the radius doesn't change the light's brightness.

**[ours]** The `+1` assumes meter units and visibly flattens the curve within ~1 m. Filament uses
`max(d², 1e-4)` (1 cm). We use Filament's form, with an optional per-light "source radius" clamp.

```hlsl
float DistanceFalloff(float3 posToLight, float invRadius)        // Karis window, Filament denominator
{
    float d2 = dot(posToLight, posToLight);
    float f  = d2 * invRadius * invRadius;                       // (d/r)^2
    float w  = saturate(1.0f - f * f);                           // 1 - (d/r)^4
    return (w * w) / max(d2, 1e-4f);
}
float SpotFalloff(float3 L, float3 spotDir, float spotScale, float spotOffset)  // Filament
{   // spotScale = 1/max(cos(inner)-cos(outer),1e-4); spotOffset = -cos(outer)*spotScale  (CPU)
    float a = saturate(dot(-L, spotDir) * spotScale + spotOffset);
    return a * a;
}
// radiance contribution: Lo = BRDF * (lightIntensity_cd * DistanceFalloff * SpotFalloff * shadow) * NoL
```

### 6.2 Volume meshes that conservatively bound the radius

The mesh's **inscribed** radius must be ≥ the light radius r. Scale a unit mesh, whose vertices lie
on the unit sphere, by `1 / inradius`. We computed the face-plane distances of subdivided
icospheres (vertices on the unit sphere):

| Mesh | vertices / triangles | inradius | scale factor |
|---|---|---|---|
| icosahedron (subdiv 0) | 12 / 20 | 0.7947 | 1.2584 |
| icosphere subdiv 1 | 42 / 80 | 0.9342 | 1.0705 |
| **icosphere subdiv 2 (recommended)** | 162 / 320 | 0.9822 | **1.0181** |
| icosphere subdiv 3 | 642 / 1280 | 0.9955 | 1.0045 |

- Subdiv 2 wastes ~3.7% of the projected area. Subdiv 1 wastes ~15%.
- For reference, the Days Gone capture used a 792-primitive volume and the Fox one 50.

**Spot light: closed cone [ours].**

- Apex at the light, axis along the spot direction, height = r (covers the spherical sector).
- Base radius = `r · tan(outer) / cos(π/N)` for an N-segment base polygon. N = 16 gives a factor of
  1.0196.
- Include the base cap, because parity marking needs a closed mesh.
- When `outer > ~45°` the cone gets wide (tan 45° = 1). Use the sphere mesh instead, which is
  always correct and smaller for wide cones.

**Baked or artist volumes** (Crytek clip volumes, the video's "dynamic or baked mesh-volume") work
with parity marking as long as they are closed; they don't have to be convex. Clipping a light to
such a volume needs no shader work, because the stencil mark already restricts the shading to it.

---

## 7. BRDF

Notation: α = perceptualRoughness² (Disney/Karis). `NoL`, `NoV`, `NoH`, `LoH` (= VoH) are clamped
dot products. Lighting is evaluated in the space the G-buffer normals are stored in.

### 7.1 Specular core (Heitz 2014, Karis 2013, Filament)

```hlsl
static const float PI = 3.14159265f;

float D_GGX(float NoH, float a)                        // Trowbridge-Reitz; a = alpha
{
    float a2 = a * a;
    float d  = (NoH * a2 - NoH) * NoH + 1.0f;          // NoH^2 (a2 - 1) + 1
    return a2 / (PI * d * d);
}
// Height-correlated Smith (Heitz 2014 eq. 99: G2 = 1 / (1 + Λ(v) + Λ(l)),
// Λ = (-1 + sqrt(1 + α² tan²θ)) / 2), folded with 1/(4 NoL NoV)  ->  V = G2 / (4 NoL NoV)
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0f - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0f - a2) + a2);
    return 0.5f / max(gv + gl, 1e-5f);
}
float V_SmithGGXCorrelatedFast(float NoV, float NoL, float a)   // Hammon 2017 (via Filament)
{
    return 0.5f / max(lerp(2.0f * NoL * NoV, NoL + NoV, a), 1e-5f);
}
// Schlick with Callisto "Specular Fresnel Falloff" n_s (SIGGRAPH 2023, slide 95):
//   F = f0 + t(2 - r(n_s)) (1 - f0)(1 - cosθ)^(5 r(n_s)),  r(x) = 2(1-x), t = saturate
//   n_s = 0.5 -> classic Schlick; n_s = 0 -> F = f0 (Fresnel removed). We generalize (1-f0) -> (f90-f0).
float3 F_Schlick(float3 f0, float f90, float VoH, float ns)
{
    float r = max(2.0f * (1.0f - ns), 1e-4f);
    return f0 + saturate(2.0f - r) * (f90 - f0) * pow(1.0f - VoH, 5.0f * r);
}
float F90From(float3 f0) { return saturate(50.0f * dot(f0, (0.33f).xxx)); } // "<2% is shadowing" (UE4/Filament)
float F0FromIOR(float n) { float x = (n - 1) / (n + 1); return x * x; }     // run on CPU in double, store F0 in the profile
// conductor F0 from complex IOR n + ik (per channel, CPU): ((n-1)^2 + k^2) / ((n+1)^2 + k^2)
```

- **Karis 2013** uses Schlick-Smith with `k = (Roughness+1)²/8` and an SG approximation of
  Schlick's power. The height-correlated form costs two sqrts more and is the better match to
  Heitz. Keep the fast form as an A/B option.
- **Diffuse.**
  - Karis evaluated Burley's diffuse and "saw only minor differences compared to Lambertian".
  - Callisto starts from Lambert and adds explicit controls (§7.2).
  - Burley, for reference (Filament form): `f90 = 0.5 + 2·α_perceptual·LoH²`,
    `Fd = (1/π)·F_Schlick(NoL,1,f90)·F_Schlick(NoV,1,f90)`.
  - **Our default: Lambert × Callisto c₁.**
- **Energy compensation (optional).**
  - Build a 2D LUT `E(NoV, α)` = directional albedo of the GGX lobe with F = 1.
  - Then `spec *= 1 + f0·(1/E − 1)` (Kulla-Conty, as used in Filament: `energyCompensation = 1.0 +
    f0 * (1.0 / dfg.y - 1.0)`).
  - It matters for rough metals. Add it after everything else works.
- **Anisotropic GGX, when the G-buffer aniso flag is set (Filament):**

```hlsl
float D_GGX_Aniso(float NoH, float ToH, float BoH, float at, float ab)
{
    float  a2 = at * ab;
    float3 v  = float3(ab * ToH, at * BoH, a2 * NoH);
    float  w2 = a2 / dot(v, v);
    return a2 * w2 * w2 / PI;
}
float V_SmithGGXCorrelated_Aniso(float at, float ab, float ToV, float BoV, float ToL, float BoL, float NoV, float NoL)
{
    float lv = NoL * length(float3(at * ToV, ab * BoV, NoV));
    float ll = NoV * length(float3(at * ToL, ab * BoL, NoL));
    return 0.5f / max(lv + ll, 1e-5f);
}
// at = max(a*(1+aniso), 0.001); ab = max(a*(1-aniso), 0.001); aniso in [-1,1] comes from the profile.
// Tangent [ours]: the G-buffer has no tangent, so when the aniso flag is set, GB2.y stores the tangent
// angle phi in [0,pi) (8 bits) relative to a deterministic basis built from N (Duff et al. 2017):
void BasisFromN(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0 ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z), c = n.x * n.y * a;
    t = float3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}
// T = cos(phi)*t + sin(phi)*b ; B = cross(N, T). The base pass encodes phi = atan2(dot(Tw,b), dot(Tw,t)) mod pi.
```

### 7.2 Callisto BRDF extensions (Jimenez & Petersen, SIGGRAPH 2023 slides 85, 90, 95)

The slide formula:

```
L_o = ( c1(θd,θh) f_lambert + f_ggx ) c2(θi) L_i cosθi
```

- **c₁** (diffuse Fresnel × retroreflection):
  - `c1 = lerp(1, ρ_f, α_f) · lerp(1, ρ_r, α_r)`
  - `h(θ,n,φ,m) = (1 − max(cosθ,0))^(5n) · max(cosφ,0)^(5m)`
  - `α_f = h(θd, r(n_f), θh, r(m_f))`
  - `α_r = h(θh, r(n_r), θd, r(m_r))`
  - `r(x) = 2(1−x)`
  - Here cos θd = L·H and cos θh = N·H.
  - Defaults: ρ = 1 (range [0,256], "exposed as intensity multiplied by a tint"), n = m = 0.75.
    With ρ = 1, c₁ = 1, which is exactly Lambert.
- **c₂** (smooth terminator):
  - `c2 = lerp(1, smoothstep(0, α_s·p, cosθi), α_s·o)`
  - `α_s = (1−(1−θd)³)(1−(1−θh)³)`
  - o ∈ [−1,1] with default 0 (off); p = 0.5.
  - **Ambiguity:** the slide does not say whether θ is in radians or normalized. Leave it off
    (o = 0) until we verify it against references.
- **"Modified Dual GGX Specular":** the parameters are a "Dual Specular Roughness Scale" and an
  "Opacity". The slides give no formula. **[ours]** `spec = lerp(GGX(α), GGX(min(α·scale,1)),
  opacity)`, sharing F, in the spirit of UE's dual-lobe skin.
- The authors note the "mathematical expression ... has slightly evolved since our prior GDC 2023
  presentation".

```hlsl
float CallistoH(float cosA, float n, float cosB, float m)
{
    return pow(max(1.0f - saturate(cosA), 1e-6f), 5.0f * n) * pow(max(saturate(cosB), 1e-6f), 5.0f * m);
}
float3 CallistoC1(float LoH, float NoH, float3 rhoF, float nf, float mf, float3 rhoR, float nr, float mr)
{
    float af = CallistoH(LoH, 2.0f * (1.0f - nf), NoH, 2.0f * (1.0f - mf));
    float ar = CallistoH(NoH, 2.0f * (1.0f - nr), LoH, 2.0f * (1.0f - mr));
    return lerp(1.0f, rhoF, af) * lerp(1.0f, rhoR, ar);
}
```

### 7.3 Skin: dual normal and pre-integrated scattering

**What the "dual normal buffer" holds.**

- Callisto (SIGGRAPH 2023 slide 115, "developed by Hampus Siversson and Miguel Rodriguez"): "We
  added support for dual normals, split for specular and diffuse. This was important to render sweat
  in a more accurate way."
- The video's base pass lists "Dual Normal & Skin Mask RGB10A2_UNORM (2xMSAA)".
- So GB0 holds the **specular** normal (sharp: pores, sweat), and GB4 holds a separate **diffuse**
  normal (smoother) for skin pixels only.
- Penner 2011's "per-channel bent normals", optimized form B: "Use two normal samples (blend the
  rest)... One normal-map, two samplers. Clamp one sampler at blurred mip threshold".

**Our GB4 layout [ours], `R10G10B10A2_UNORM`, written only by skin materials:**

| Channel | Content |
|---|---|
| RG | octahedral-encoded diffuse normal (10+10 bits is plenty for a blurred normal) |
| B | curvature `1/r`, normalized to the LUT range |
| A | 2-bit skin mask / profile index |

**Pre-integrated diffuse (Penner 2011, "Pre-Integrated Skin Shading", SIGGRAPH Advances).**

The scattering is baked into a 2D LUT indexed by `N·L` and curvature `1/r`. Penner: "store all
this data in a simple 2D texture that is parameterized by N.L and curvature". The ring integral,
offline or at startup, with x the angle around a ring of radius r (mm):

```
D(θ, r) = ∫_{-π}^{π} saturate(cos(θ + x)) · R(2r·sin(x/2)) dx  /  ∫_{-π}^{π} R(2r·sin(x/2)) dx
R(d) = Σ_i w_i · G(v_i, d),   G(v, d) = exp(-d² / (2v)) / (2π v)
```

Skin profile, sum of 6 Gaussians (d'Eon & Luebke, GPU Gems 3 ch. 14, fig. 14-13):

| variance (mm²) | R | G | B |
|---|---|---|---|
| 0.0064 | 0.233 | 0.455 | 0.649 |
| 0.0484 | 0.100 | 0.336 | 0.344 |
| 0.187 | 0.118 | 0.198 | 0 |
| 0.567 | 0.113 | 0.007 | 0.007 |
| 1.99 | 0.358 | 0.004 | 0 |
| 7.41 | 0.078 | 0 | 0 |

- LUT: u = N·L·0.5+0.5, v = curvature mapped to the range covered. Store it **linear**
  (R16G16B16A16_FLOAT, or UNORM for linear data).
- Curvature: Penner uses `c = length(fwidth(N)) / length(fwidth(P))` in the base pass. It is
  "constant within a triangle", so store per-vertex or in a texture if facets show. Convert units:
  the profile is in mm.
- Shading:
  - The lookup *replaces* `NoL` for diffuse; don't multiply by NoL again.
  - Per channel: R uses the diffuse (blurred) normal, B uses the specular normal, G uses their
    normalized midpoint. **[ours]** These weights are tunable; Penner says only "blend the rest".
  - Specular: dual-lobe GGX on the specular normal, with params from the skin profile.
- **Coordinate with the SSS note.** If screen-space SSS (Jimenez) blurs the diffuse later, the
  pre-integrated LUT double-counts scattering. Then either drop the LUT (use Lambert × c₁ on the
  diffuse normal) or use the LUT only on the blur's radius complement. The video mentions that Days
  Gone's skin draws "are the only shaders that use the custom skin shading lookup table".

### 7.4 Subsurface (generic) and cloth (Filament formulations)

- **Subsurface (wax, jade, marble).** Use energy-conserving wrap diffuse,
  `Fd_wrap = saturate((NoL + w)/(1+w)²)` (Filament: `(dot(n,l)+0.5)/2.25` for w = 0.5), multiplied
  by a subsurface tint `saturate(subsurfaceColor + NoL)`. The tint comes from the profile.
- **Cloth.**
  - Charlie sheen: `D = (2 + 1/α)·sin(θh)^(1/α) / (2π)` (Estevez & Kulla 2017; Filament's
    `D_Charlie`).
  - Neubelt visibility: `V = 1 / (4(NoL + NoV − NoL·NoV))`.
  - Sheen color from the profile; Lambert or wrap diffuse.

### 7.5 Foliage: two-sided transmission

UE4's two-sided foliage model [formula-level description]:

- Front side: normal Lit evaluation.
- Back side: wrapped inverted N·L times a GGX-shaped forward-scatter lobe around the light
  direction seen through the leaf.

```hlsl
float  wrap     = 0.5f;
float  wrapNoL  = saturate((-dot(N, L) + wrap) / ((1 + wrap) * (1 + wrap)));
float  scatter  = D_GGX(saturate(-dot(V, L)), 0.6f);            // alpha 0.6 lobe
float3 transmit = transmissionTint * albedo * wrapNoL * scatter;   // * light radiance * atten * shadowBack
```

**Pitfall:** for transmission, the shadow-map test at the leaf sees the leaf itself as the
occluder. Use a thickness-aware shadow lookup, e.g. Jimenez 2010 "Real-time realistic skin
translucency" (cited by Callisto for transmission), or a larger normal-offset bias for the
transmission term. In the sun path this is the video's RG8 shadow mask G channel ("related
subsurface properties").

---

## 8. Dual Material Profile (DMP): a simple, faithful D3D11 design

### 8.1 G-buffer [ours, built from the video's targets]

| Target (2xMSAA) | Format | Content |
|---|---|---|
| Depth/stencil | `R32G8X24_TYPELESS` (DSV `D32_FLOAT_S8X24_UINT`) | reverse-Z depth; stencil per §9.1 |
| GB0 normal | `R10G10B10A2_UNORM` | rgb = N·0.5+0.5 (UE4/Callisto style; octahedral is an option). A: bit0 = aniso flag, bit1 = lighting channel. Decode with `uint a = (uint)round(x.a * 3)`. |
| GB1 albedo+roughness | `R8G8B8A8_UNORM_SRGB` | rgb albedo (sRGB-decoded on load). A = perceptual roughness (alpha is never sRGB-converted). |
| GB2 material | **`R8G8B8A8_UINT`** | x = profile blend t (0-255, the former metallic). y = specular scale s (0-255), or the tangent angle when aniso. z = DMP ID bits 0-7. w = DMP ID bits 8-12, plus 3 spare bits. |
| GB3 | `R16G16B16A16_FLOAT` | motion vectors / MSAA data (other notes) |
| GB4 (skin only) | `R10G10B10A2_UNORM` | diffuse normal (octahedral), curvature, skin mask |
| Light accum 0 | `R16G16B16A16_FLOAT` | emissive (base pass) + diffuse (lights); A = luminance per the video |
| Light accum 1 | `R16G16B16A16_FLOAT` | specular (cleared before lighting) |

- **Why UINT for GB2:** IDs must never be filtered or blended. A UINT target makes accidental
  blending impossible, and decals simply leave that RT unwritten (per-RT write mask).
- Check `CheckFormatSupport` and `CheckMultisampleQualityLevels(R8G8B8A8_UINT, 2)` at startup. The
  fallback is `R8G8B8A8_UNORM` with `round(x*255)`.
- 13 bits give **8192** DMPs, matching the video's "8,000 slot limit" and "13/16 bit". The 3 spare
  bits allow 16 bits later.

### 8.2 Tables

```hlsl
struct BrdfProfile                    // 80 bytes; one per authored "material expression" (paint, iron, ...)
{
    float4 f0_conductor;     // rgb: F0 (linear) - dielectric: F0FromIOR(ior) computed on CPU from the
                             //      high-precision IOR; conductor: from complex IOR or measured;  w: 1 = conductor
    float4 diffFresnel_nf;   // rgb: Callisto rho_f (intensity*tint, default 1); w: n_f (0.75)
    float4 retro_nr;         // rgb: Callisto rho_r (default 1);                  w: n_r (0.75)
    float4 smTint_ns;        // rgb: SM-specific tint (foliage transmission / subsurface / sheen);  w: n_s (0.5)
    float4 misc;             // x: m_f (0.75), y: m_r (0.75), z: anisotropy [-1,1], w: f0FromAlbedo (0/1)
};
StructuredBuffer<BrdfProfile> g_Profiles : register(t5);   // e.g. up to 1024 profiles
StructuredBuffer<uint>        g_DmpTable : register(t6);   // 8192 entries: profileA | (profileB << 16)
```

- **Two-level indirection [ours].** A DMP is just a *pair* of profile indices. Profiles are shared:
  "paint" appears in many pairs. That keeps the table small (8192 × 4 B + 1024 × 80 B ≈ 112 KB).
- **Per-SM tables.** The video speaks of a "Lit Shading Model's DMP LUT", which suggests per-SM
  tables. One table where each SM interprets its own fields (`smTint`) is simpler and equivalent.
- **IOR precision.** The IOR lives on the CPU side (authoring data, double precision). The shader
  gets the derived F0. That is what "store a higher precision index of refraction variable in the
  material profile" buys over an 8-bit specular channel.

### 8.3 Blend semantics

**Option A: blend parameters.** `p = lerp(pA, pB, t)`, then evaluate once. This is the
metallic-workflow approach, which the video calls "too shallow": parameters like the conductor flag
or the falloff exponents don't interpolate meaningfully. Karis 2013 notes that parameter blending
is only safe when parameters are "perceptually linear".

**Option B, recommended: blend BRDF results.** `f = (1−t)·f_A + t·f_B`. This is the physically
correct model for a pixel footprint partly covered by each material, e.g. iron showing through
thinning paint.

Roughness and the normal come from the G-buffer, so `D·V` is shared. Only F and the diffuse
factor are evaluated twice, which costs a few extra ALU ops. A coherent branch skips the second
profile when t ∈ {0, 1}, which most pixels are.

### 8.4 The specular channel [ours, interpreting the video]

The video says: "store a higher precision index of refraction variable in the material profile and
have the specular channel always scale that value to zero."

- **Implementation:** `f0_eff = (s/255) · f0_profile` and `f90 = saturate(50·dot(f0_eff, 0.33))`.
  - s = 255 means "exactly the profile's IOR". This differs from UE4, where specular 0.5 maps to
    4%.
  - s → 0 removes the specular, F90 included. That is cavity / specular occlusion behaviour, which
    Karis 2013 says is what "Specular" was mostly used for in practice.
- **Alternative:** scale the IOR excess, `n_eff = 1 + s·(n − 1)`. That is less perceptually linear;
  A/B it if artists object.
- The video's preprocessing note follows from this: legacy spec/cavity maps must be remapped so
  the *effective* F0 matches across DMP-ID boundaries.

```hlsl
void EvalProfile(BrdfProfile p, float3 albedo, float s, float LoH, float NoH, float VoH, float DV,
                 out float3 diffuse, out float3 specular)
{
    float3 f0  = p.f0_conductor.rgb * ((p.misc.w > 0.5f) ? albedo : (1.0f).xxx) * s;
    float3 F   = F_Schlick(f0, F90From(f0), VoH, p.smTint_ns.w);
    float3 c1  = CallistoC1(LoH, NoH, p.diffFresnel_nf.rgb, p.diffFresnel_nf.w, p.misc.x,
                                      p.retro_nr.rgb,       p.retro_nr.w,       p.misc.y);
    diffuse  = (p.f0_conductor.w > 0.5f) ? 0.0f : albedo * c1 * (1.0f / PI);
    specular = F * DV;
}
// in PS_Light (SM_LIT), after decoding GB0..GB2 and computing L, H, attenuation, shadow:
uint   dmp = g_DmpTable[dmpId];
uint   pA  = dmp & 0xFFFF, pB = dmp >> 16;
float  DV  = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a);         // shared by both profiles
float3 diff, spec;
EvalProfile(g_Profiles[t < 1.0f ? pA : pB], albedo, s, LoH, NoH, VoH, DV, diff, spec); // pure pixels: 1 eval
[branch] if (t > 0.0f && t < 1.0f)                                                     // mixed pixels: 2nd eval
{
    float3 dB, sB;
    EvalProfile(g_Profiles[pB], albedo, s, LoH, NoH, VoH, DV, dB, sB);
    diff = lerp(diff, dB, t);
    spec = lerp(spec, sB, t);
}
float3 Li  = g_LightColor * DistanceFalloff(toLight, g_LightInvRadius) * shadow * NoL;
o.diffuse   = float4(diff * Li, 0);    // RT0 (additive)
o.specular  = float4(spec * Li, 0);    // RT1 (additive)
```

Decoding: `t = GB2.x / 255.0`, `s = GB2.y / 255.0`, `dmpId = GB2.z | ((GB2.w & 0x1F) << 8)`.

When the aniso flag (GB0.a bit 0) is set, GB2.y holds the tangent angle instead, so use `s = 1`.
Use the anisotropic D·V from §7.1, with the strength from profile A's `misc.z`.

---

## 9. Concrete recommendation for our engine

### 9.1 Stencil byte

| Bits | Mask | Name | Written by | Read by |
|---|---|---|---|---|
| 0 | 0x01 | L, light mark | light mark pass (INVERT), sun pre-mark (REPLACE) | per-SM lighting draws (restore with ZERO) |
| 1-3 | 0x0E | SM: 0 none · 1 Lit · 2 Skin · 3 Foliage · 4 Subsurface · 5 Cloth · 6-7 reserved | base pass (REPLACE via StencilRef); MSAA empty-sample pass (sets 0) | mark pass (`LESS`, ref 0); SM draws (`EQUAL`) |
| 4 | 0x10 | C, lighting channel (copy of GB0.a bit 1) | base pass | channel-only lights (mark pass ref 0x10) |
| 5-6 | 0x60 | forward tags (eye 0x20, hair 0x40) or free | eye/hair passes | forward eye/hair draws |
| 7 | 0x80 | E, per-sample edge (MSAA mode M2 only; Crytek convention) | MSAA classification | M2 pixel/sample SM draws |

Light-channel semantics with one compare: a light is either "all" or "channel-only". "Exclude
channel" lights can't be expressed without a second bit or pass.

### 9.2 Resources

- Depth texture `R32G8X24_TYPELESS`, 2x, bind flags `DEPTH_STENCIL | SHADER_RESOURCE`.
- `DSV_RW` and `DSV_ReadOnlyDepth` (`D3D11_DSV_READ_ONLY_DEPTH`), both `D32_FLOAT_S8X24_UINT`,
  `TEXTURE2DMS`.
- `SRV_Depth` `R32_FLOAT_X8X24_TYPELESS`, and a debug `SRV_Stencil` `X32_TYPELESS_G8X24_UINT`
  (`Texture2DMS<uint2>`, `.g`).
- Frame start: `ClearDepthStencilView(DSV_RW, DEPTH|STENCIL, 0.0f, 0)`.

### 9.3 D3D11 state objects (reverse-Z)

Depth-stencil states. FrontFace = BackFace unless noted. Columns: Fail / DepthFail / Pass / Func.

| Name | Depth | DepthFunc | DepthWrite | ReadMask | WriteMask | Ops (Fail/ZFail/Pass/Func) | StencilRef |
|---|---|---|---|---|---|---|---|
| `DSS_BasePass` | on | GREATER_EQUAL | ALL (or ZERO after a full prepass) | 0x00 | 0x1E | KEEP/KEEP/REPLACE/ALWAYS | `(SM<<1)|(C<<4)` |
| `DSS_MarkAll` | on | GREATER | ZERO | 0x0E | 0x01 | KEEP/**INVERT**/KEEP/**LESS** | 0x00 |
| `DSS_MarkChannel` | on | GREATER | ZERO | 0x1E | 0x01 | KEEP/INVERT/KEEP/LESS | 0x10 |
| `DSS_ShadeSM` (default) | **off** | - | ZERO | 0x0F | 0x01 | KEEP/KEEP/**ZERO**/**EQUAL** | `(SM<<1)|1` |
| `DSS_ShadeSM_NoWrite` (V6) | off | - | ZERO | 0x0F | 0x00 | KEEP/KEEP/KEEP/EQUAL | `(SM<<1)|1` |
| `DSS_Unmark` (V6) | off | - | ZERO | 0x01 | 0x01 | KEEP/KEEP/ZERO/EQUAL | 0x01 |
| `DSS_ShadeSM_NearBound` (V7) | on | GREATER_EQUAL | ZERO | 0x0F | 0x01 | KEEP/KEEP/ZERO/EQUAL | `(SM<<1)|1` |
| `DSS_ShadeSM_Pix` / `_Smp` (M2) | off | - | ZERO | 0x8F | 0x01 | KEEP/KEEP/ZERO/EQUAL | `(SM<<1)|1` / `0x80|(SM<<1)|1` |
| `DSS_SunSM` | off | - | ZERO | 0x0E | 0x00 | KEEP/KEEP/KEEP/EQUAL | `SM<<1` |
| `DSS_SunPreMark` (opt.) | off | - | ZERO | 0x0E | 0x01 | KEEP/KEEP/REPLACE/LESS | 0x01 |
| `DSS_LeakCheck` (debug) | off | - | ZERO | 0x01 | 0x00 | KEEP/KEEP/KEEP/EQUAL | 0x01 |

Rasterizer states. `FillMode SOLID`, `MultisampleEnable TRUE` (lines only on FL ≥ 10.1), no depth
bias.

| Name | CullMode | DepthClipEnable | ScissorEnable |
|---|---|---|---|
| `RS_Volume` | NONE | **FALSE** | TRUE |
| `RS_FullscreenScissor` | NONE | TRUE | TRUE (the sun sets a full-viewport rect) |
| V2/V3 volume lighting (A/B) | BACK (outside) / FRONT (inside) | FALSE | TRUE |

Blend states. Always `OMSetBlendState(bs, nullptr, 0xFFFFFFFF)`; the sample mask must stay all-ones
for early stencil with writes.

| Name | RT0 | RT1 |
|---|---|---|
| `BS_NoColor` | WriteMask 0 | WriteMask 0 |
| `BS_AddLight` | ONE/ONE/ADD (color and alpha), WriteMask ALL | same |

### 9.4 Shader permutations

- `VS_Volume`: unit mesh × (scale = r · inradius factor) → clip. `VS_FullscreenTri`.
- `PS_Light<TYPE ∈ {POINT, SPOT}, SHADOWED ∈ {0,1}, SM ∈ {1..5}, FREQ ∈ {SAMPLE, PIXEL}>` is 40
  variants. `PS_Sun<SM, FREQ>` is 10. Compile them from one `Lighting.hlsl` with `#define`s
  (`D3DCompile` at build time).
- Every lighting PS is declared `[earlydepthstencil]`. It does no `discard`, has no depth output and
  no UAVs. Inputs: `float4 pos : SV_Position`, plus `uint s : SV_SampleIndex` for FREQ = SAMPLE.
  Reads use `Texture2DMS<>.Load(int2(pos.xy), s)`.
- Position reconstruction per sample:

```hlsl
float2 spos = float2(pos.xy) + g_Depth.GetSamplePosition(s);   // 2x std pattern: (+.25,+.25), (-.25,-.25)
float2 ndc  = spos * g_InvViewport * float2(2, -2) + float2(-1, 1);
float  zv   = g_Near / g_Depth.Load(int2(pos.xy), s);           // reverse-Z, infinite far, +Z forward
float3 posV = float3(ndc.x * zv / g_P00, ndc.y * zv / g_P11, zv);
```

- Utility shaders: `PS_SunPreMark` (sample-frequency, discards fully shadowed samples),
  `PS_LeakCheck` (debug), and the tiled baseline `CS_TiledLighting` (§10).

### 9.5 CPU loop (C++ sketch)

```cpp
ctx->ClearRenderTargetView(specRTV, zero4);
ID3D11RenderTargetView* rtvs[2] = { emissiveDiffuseRTV, specRTV };
ctx->OMSetRenderTargets(2, rtvs, dsvReadOnlyDepth);
BindLightingSRVs(ctx);                                   // depth, GB0..GB4, tables, LUTs, shadow atlas
for (const Light& L : visibleLocalLights) {
    D3D11_RECT rc;
    if (!ScissorFor(L, rc)) continue;                    // §4.2; off-screen -> skip
    ctx->RSSetScissorRects(1, &rc);
    UpdateLightCB(ctx, L);                               // Map(WRITE_DISCARD)
    // mark
    ctx->RSSetState(rsVolume);
    ctx->OMSetBlendState(bsNoColor, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(L.channelOnly ? dssMarkChannel : dssMarkAll, L.channelOnly ? 0x10 : 0x00);
    ctx->VSSetShader(vsVolume, nullptr, 0); ctx->PSSetShader(nullptr, nullptr, 0);
    DrawVolumeMesh(ctx, L);                              // icosphere subdiv 2 or closed cone
    // shade, one full-screen triangle per present shading model; each reverts its L bits
    ctx->RSSetState(rsFullscreenScissor);
    ctx->OMSetBlendState(bsAddLight, nullptr, 0xFFFFFFFF);
    ctx->VSSetShader(vsFullscreenTri, nullptr, 0);
    ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (uint32_t sm = 1; sm <= 5; ++sm) {
        if (!(smPresentMask & (1u << sm))) continue;
        ctx->PSSetShader(psLight[L.type][L.shadowed][sm], nullptr, 0);
        ctx->OMSetDepthStencilState(dssShadeSM, (sm << 1) | 1);
        ctx->Draw(3, 0);
    }
}
```

---

## 10. A/B test plan

### 10.1 Variants

All variants use identical BRDF and attenuation code from one include, with shadows off for the
core comparison.

| ID | Method |
|---|---|
| V0 | **No culling:** per light, per SM, unscissored full-screen triangle (SM-only stencil); the shader returns 0 outside the radius |
| V1 | **Scissor only:** V0 + scissor rect |
| V2 | **Volume-mesh lighting, no stencil:** Thibieroz/UE-style front/back-face depth trick, one mesh draw per SM (SM stencil test) |
| V3 | **Fox Engine style:** parity mark + lighting with the *same volume mesh* per SM (stencil EQUAL + restore) |
| V4 | **Days Gone literal:** parity mark + per-SM *unscissored* full-screen triangle |
| V5 | **Recommended:** V4 + scissor |
| V6 | V5, but lighting draws don't write stencil, plus a separate unmark draw (tests ZCULL-with-writes) |
| V7 | V5 + emulated near depth bound (§4.3) |
| V8 | Two-pass pass-ops marking (B', §3.3) + V5 shading |
| T | **Tiled compute baseline:** BF3/Lauritzen style, 16×16 tiles, min/max depth, groupshared light list, one uber-kernel with `switch(SM)` read from the stencil SRV, MSAA per Lauritzen (sample 0 for all, then a packed list of edge pixels) |
| S1 / S2 | Sun: per-SM draws vs one branchy uber draw; with and without the shadow-mask pre-mark |

### 10.2 Workloads

- Light counts: **1 large** (~25% of screen), **1 small** (~1%), **16** (mixed sizes), **64**
  (mixed). Plus an **overlap stress** of 16 lights over the same screen region, and **occluded**
  lights (behind a wall).
- SM mix: only SM 1 present vs all 5 present.
- MSAA: off, 2x with M1, 2x with M2, 2x with M3.
- Fixed camera paths in a Sponza-class interior and a synthetic plane scene.

### 10.3 Measurement

- **GPU time.** Use `D3D11_QUERY_TIMESTAMP` pairs inside a `TIMESTAMP_DISJOINT` query.
  - Granularity: the whole light loop; per light for single-light tests; per draw for 1-light
    breakdowns (mark, SM draws).
  - Use a ring of 3-4 frames of queries and never stall.
- **Invocations.** `D3D11_QUERY_PIPELINE_STATISTICS` (`PSInvocations`, `CPrimitives`) per variant,
  and `D3D11_QUERY_OCCLUSION` for marked samples. Check the work optimality of V3-V8 against V0
  and T.
- **Cross-check** with Nsight Graphics GPU Trace on the RTX 5060: per-draw times, ZCULL/early-Z
  throughput, warp occupancy, registers per thread for per-SM vs uber shaders. Optionally use Intel
  GPA, which is what the video's captures appear to use, for like-for-like counters.
- **Stability.** Lock GPU clocks. `nvidia-smi --lock-gpu-clocks=<f>,<f>` needs admin. An
  alternative is creating a D3D12 device on the same adapter and calling `SetStablePowerState(TRUE)`
  in Windows developer mode. Warm up for 120 frames, then report the median and 95th percentile over
  300 frames. Disable the frame-rate cap and V-sync.
- **Derived metrics.** Fit `T_lights = a·N_lights + b·(Σ lit sample-light pairs) + c` per method.
  - a = per-light fixed cost.
  - b = **single-light-per-sample cost**, the video's claim: stencil methods should have the lower
    b. If not, the claim fails on our hardware.
  - For T, the work counter is "sample × lights in tile". The waste ratio is
    `(Σ tile-light pairs × samples) / (Σ actually lit pairs)`, Olsson's lighting-computations
    metric.
- **CPU submit time** per variant: QueryPerformanceCounter around the loop. It matters for 64
  lights × 6 draws.
- **Correctness gates** before timing:
  - The leak check (`DSS_LeakCheck`) must show nothing.
  - V3-V8 must produce bitwise-similar images to V0 (difference image; ε for blending order).
  - Camera inside a light, and lights crossing the near plane, must match V0.

### 10.4 Expected results (hypotheses to confirm or refute)

- V4 ≫ V5 cost with many lights, because full-screen raster per SM per light dominates
  (Days Gone 34 µs for 47.6k invocations).
- V5 < V3 for large lights (fewer helper lanes), and about equal for small ones.
- V1 ≤ V5 for small lights, where marking overhead dominates (Killzone 2's 20% FOV rule).
- T wins at 64 overlapping lights and loses or ties at 1-16 non-overlapping heavy lights.
- V6 vs V5 tells us whether stencil writes kill coarse stencil culling on Blackwell.

---

## 11. Pitfalls checklist

- [ ] Mark pass: `CullMode NONE`, `DepthClipEnable FALSE`, `WriteMask 0x01`, `DepthFunc` in the
      scene direction (GREATER). Use wrapping INCR/DECR or INVERT, never `_SAT`.
- [ ] The volume mesh is closed and watertight (indexed, shared vertices) and inflated by the
      inradius factor (1.0181 for icosphere subdiv 2).
- [ ] Draw every SM present in the frame, or marks leak. Keep the debug leak check.
- [ ] No `discard` in lighting draws that restore stencil. `SampleMask = 0xFFFFFFFF`.
- [ ] The DSV is read-only-*depth* only (stencil writable). The depth SRV uses
      `R32_FLOAT_X8X24_TYPELESS`. Confirm with the debug layer that it is not unbound.
- [ ] `ScissorEnable` requires a valid rect on every draw, including the sun (full viewport).
- [ ] Stencil test operand order: the reference is on the left. Prefer EQUAL / NOT_EQUAL where
      possible, and unit-test LESS-based masks with a readback.
- [ ] `R8G8B8A8_UINT` MSAA RT support: query it. The fallback is UNORM with explicit rounding.
- [ ] RGB10A2 alpha: decode with `round(a*3)`. Don't compare floats.
- [ ] sRGB targets convert only rgb. Roughness in alpha is stored linear.
- [ ] Penner LUT: linear storage. It replaces NoL for diffuse. Units are mm.
- [ ] Callisto `pow` with exponent 0: guard the base with ≥ 1e-6 to avoid NaN.
- [ ] Foliage transmission needs thickness-aware shadowing, or the leaf shadows itself.
- [ ] Changing depth-test *direction* while writing depth disables ZCULL until the next clear
      (NVIDIA 2008). Volume-mesh variants with the inverted test write no depth, so they are fine.
      Still, don't flip directions in depth-writing passes.

---

## 12. Open questions

1. **Coarse stencil on Blackwell.** Do NVIDIA ZCULL stencil and AMD HiS stay active while a draw
   both tests and writes stencil? This is undocumented for modern GPUs; V5 vs V6 answers it on our
   hardware.
2. **MSAA lighting mode.** M1 (the video's per-sample empty samples, one draw per SM) vs M2
   (Crytek, two draws per SM) vs the video's "4x non-MSAA RGBA16" pixel-frequency idea. The MSAA
   note owns this; the stencil layout above supports all of them.
3. **Specular channel semantics.** Does it scale F0 (ours) or the IOR excess? Is it 13 or 16 bits?
   One profile table, or one LUT per SM as the video's slide implies?
4. **Callisto details.** The smooth-terminator θ normalization and the exact "dual GGX" mix are not
   specified in the slides. We use defaults that reduce to Lambert + GGX.
5. **Skin.** Pre-integrated LUT vs screen-space SSS, and where curvature comes from (base-pass
   derivatives vs baked). This depends on the SSS note.
6. **Sun.** Does the shadow-mask pre-mark pay for itself (Killzone 2: 50% skipped) once subsurface
   G-channel pixels must still be shaded?
7. **Provenance.** The Days Gone per-SM full-screen draws and the Fox/Days Gone stencil volumes are
   documented only by TI's frame captures. No Bend or Kojima Productions talk describes them. Crysis
   2/3's "FS quads with stencil volume pre-passes" is documented by Crytek, in Secrets of CE3 (2011).

---

## 13. Sources

- Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering" (2026),
  https://www.youtube.com/watch?v=SnNm7rSSvlg. Transcript 29:00-50:00. Diagram boxes transcribed from
  extracted frames (~31:20, ~33:30, ~43:10, ~46:30). Per-draw capture numbers for Fox Engine and Days
  Gone (~42:00-42:40).
- Hargreaves & Harris, "Deferred Shading", NVIDIA 6800 Leagues (2004),
  https://download.nvidia.com/developer/presentations/2004/6800_Leagues/6800_Leagues_Deferred_Shading.pdf.
  Stencil-cull states, per-BRDF re-render, early-Z and stencil guidance.
- Valient, "Deferred Rendering in Killzone 2" (Develop 2007),
  https://www.guerrilla-games.com/media/News/Files/Develop07_Valient_DeferredRenderingInKillzone2.pdf.
  Two-pass back/front-face marking, sun pre-mark (~50% skipped).
- Valient, "Rendering Technology of Killzone 2", GDC 2009 extended notes,
  https://www.guerrilla-games.com/media/News/Files/GDC09_Valient_Rendering_Technology_Of_Killzone_2_Extended_Presenter_Notes.pdf.
  Camera-inside handling, depth bounds, the 20%-of-FOV small-light threshold, conditional rendering.
- Kaplanyan, "CryENGINE 3: Reaching the Speed of Light" (SIGGRAPH 2010),
  https://advances.realtimerendering.com/s2010/Kaplanyan-CryEngine3(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf.
  Artist clip volumes as stencil culling geometry.
- Sousa, Kasyan, Schulz, "Secrets of CryENGINE 3 Graphics Technology" (SIGGRAPH 2011),
  https://advances.realtimerendering.com/s2011/SousaSchulzKazyan%20-%20CryEngine%203%20Rendering%20Secrets%20((Siggraph%202011%20Advances%20in%20Real-Time%20Rendering%20Course).ppt.
  "FS quads with stencil volumes pre-passes ... convex light volumes or 2D quads + depth bounds".
- Sousa, Wenzel, Raine, "The Rendering Technologies of Crysis 3" (GDC 2013),
  https://www.slideshare.net/slideshow/rendering-technologies-from-crysis-3-gdc-2013/25052434 (read
  via the agent laptop). Stencil bit usage, 0x7F write mask, quad tagging (~1 ms).
- Sousa, "CryENGINE 3 Graphics Gems" (SIGGRAPH 2013),
  https://advances.realtimerendering.com/s2013/Sousa_Graphics_Gems_CryENGINE3.pptx. Also
  https://www.yumpu.com/en/document/read/51951811/cryengine-3-graphics-gems-crytek/5. Per-sample
  stencil bit and read/write masks, pixel/sample-frequency passes.
- Thibieroz, "Deferred Shading Optimizations" (AMD, GDC 2011),
  https://web.archive.org/web/2015/http://developer.amd.com/wordpress/media/2012/10/Deferred%20Shading%20Optimizations.pps.
  Camera-inside and outside depth tricks, z-fail INCR/DECR stencil, quad bounds of projected
  spheres, blending and discard advice.
- Persson, "Depth in-depth" (ATI/AMD, 2007),
  https://web.archive.org/web/2015/http://developer.amd.com/wordpress/media/2012/10/Depth_in-depth.pdf.
  HiZ/HiStencil rules: z-fail ops disable HiZ, `PassOp ZERO` partial clears are HiZ-friendly.
- NVIDIA, "GeForce 8 and 9 Series GPU Programming Guide" (2008),
  http://developer.download.nvidia.com/GPU_Programming_Guide/GPU_Programming_Guide_G80.pdf. ZCULL
  and EarlyZ enable/disable conditions, double-speed z/stencil, depth bounds test.
- NVIDIA, "Advanced API Performance: Clears", https://developer.nvidia.com/blog/advanced-api-performance-clears/.
  Reverse-Z clear values.
- Reed (NVIDIA), "Depth Precision Visualized", https://developer.nvidia.com/content/depth-precision-visualized.
  Reverse-Z with float depth and an infinite far plane.
- Microsoft, D3D11.3 Functional Specification,
  https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm. §15.4 depth
  clip, §16.2 per-sample tests, §16.12 forced early depth/stencil, §17.9 DepthEnable/StencilEnable,
  §17.13 read-only DSV.
- Microsoft Learn, "Configuring Depth-Stencil Functionality",
  https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-depth-stencil.
  Two-sided stencil in D3D11.
- Koonce, "Deferred Shading in Tabula Rasa" (GPU Gems 3 ch. 19),
  https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-19-deferred-shading-tabula-rasa.
  Stencil masking not worth it under DX9; sky stencil is free.
- Shishkovtsov, "Deferred Shading in S.T.A.L.K.E.R." (GPU Gems 2 ch. 9),
  https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-9-deferred-shading-stalker.
  Material-ID BRDF LUT, sun stencil mask ~2x.
- Courrèges, "Metal Gear Solid V - Graphics Study" (2017),
  https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/. Fox Engine G-buffer
  (material ID in the specular RT), reversed float depth, per-light volume draws.
- Lauritzen, "Deferred Rendering for Current and Future Rendering Pipelines" (SIGGRAPH 2010 BPS),
  https://web.archive.org/web/2016/http://bps10.idav.ucdavis.edu/talks/12-lauritzen_DeferredShading_BPS_SIGGRAPH2010.pdf.
  Quad vs tiled per-light slopes, "two-pass stencil is more expensive than it is worth", MSAA.
- Andersson, "DirectX 11 Rendering in Battlefield 3" (GDC 2011),
  https://web.archive.org/web/2013/http://www.dice.se/wp-content/uploads/GDC11_DX11inBF3_Public.pdf.
  Tiled CS algorithm and motivation.
- Olsson, Billeter, Assarsson, "Clustered Deferred and Forward Shading" (HPG 2012),
  https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf. Stencil deferred vs
  tiled/clustered timings and lighting-computation counts.
- Persson, "Practical Clustered Shading" (SIGGRAPH 2013),
  https://advances.realtimerendering.com/s2013/Practical%20Clustered%20Shading.ppt. Context on
  moving from classic deferred to clustered.
- Mara & McGuire, "2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere" (JCGT 2013),
  https://jcgt.org/published/0002/02/05/; Kapoulkine, "Approximate projected bounds" (2023),
  https://zeux.io/2023/01/12/approximate-projected-bounds/. Exact sphere screen bounds (verified
  numerically).
- 30fps.net, "Full screen triangle optimization", https://30fps.net/pages/twotris/. Triangle vs
  quad measurement (0.2%, GTX 1060).
- Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013 notes),
  https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf. GGX, the
  Schlick-Smith k, SG Fresnel, Lambert vs Burley, windowed inverse-square falloff, Specular → Cavity,
  shading-model branch, perceptually linear blending.
- Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs" (JCGT 2014),
  https://jcgt.org/published/0003/02/03/paper.pdf. Height-correlated G2 (eq. 99), GGX Λ (eq. 72/86).
- Google Filament, "Physically Based Rendering in Filament", https://google.github.io/filament/Filament.md.html.
  Verified code for D_GGX, V_SmithGGXCorrelated(+Fast, Hammon), F_Schlick, Fd_Burley, anisotropic
  GGX, energy compensation, D_Charlie, wrap diffuse, attenuation, F0 from IOR.
- Jimenez & Petersen, "The Rendering of The Callisto Protocol" (SIGGRAPH 2023 Advances),
  https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf.
  Callisto BRDF formulas (slides 85/90/95, rendered and read), dual normals for specular vs diffuse
  (slide 115), example parameter sets.
- Jimenez, Longhi, Contel, "The Character Rendering Art of The Callisto Protocol" (GDC 2023),
  https://gdcvault.com/play/1029339/The-Character-Rendering-Art-of. Only the session page was
  accessible (laptop). Slides are not public; content was taken from the SIGGRAPH 2023 deck instead.
- Penner, "Pre-Integrated Skin Shading" (SIGGRAPH 2011 Advances),
  https://advances.realtimerendering.com/s2011/Penner%20-%20Pre-Integrated%20Skin%20Rendering%20(Siggraph%202011%20Advances%20in%20Real-Time%20Rendering%20Course).pptx.
  LUT over N·L and curvature, curvature from derivatives, two-sample bent normals.
- d'Eon & Luebke, "Advanced Techniques for Realistic Real-Time Skin Rendering" (GPU Gems 3 ch. 14),
  https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-14-advanced-techniques-realistic-real-time-skin
  (figure 14-13 image). Six-Gaussian skin profile.
- Duff et al., "Building an Orthonormal Basis, Revisited" (JCGT 2017),
  https://jcgt.org/published/0006/01/01/. Branchless basis from N, used to encode the tangent angle.
