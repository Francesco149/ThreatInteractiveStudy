# R8: Motion vectors, velocity packing, motion blur ("velocity compensation"), half-res VFX, composite effects

Scope: research note for the educational D3D11 engine (FL 11_0, SM 5.0, deferred, 2x MSAA, 1080p60,
RTX 5060 8 GB). It covers the base-pass motion vectors (plus the MSAA coverage bits next to them), the
velocity packing pass, the motion blur (trailing-only, depth-aware, uniform sampling plus a second
"separable" pass, depth-limited camera blur), half-resolution min/max-depth VFX, and the half-res
composite-effect buffers. Video minutes: 1:02-1:04 and 1:42-1:59, plus the motion-vector remarks at
0:08-0:09, 0:28 and 1:34-1:39.

Conventions follow note 07 (`07-antialiasing-smaa.md`, section 2):

- **Pixel space:** y down, pixel centers at +0.5.
- **NDC:** y up.
- **Motion vectors:** RG16F, UV units, *current minus previous*, jitter-free:
  `(cNDC/cW - pNDC/pW) * float2(0.5, -0.5)`.
- **Matrices:** row-vector DirectXMath style. HLSL uses `mul(float4(p,1), M)` with `row_major` matrices.
- **Trail vector:** the motion-blur code uses the per-pixel *trail vector* `v` in pixels. It points
  in the direction of motion, and the blur trail extends from the pixel *backwards* along `-v` over
  `|v|` pixels.

Status: these are research results and a design. Nothing here has been run. Every number marked
"estimate" must be measured (section 12).

---

## 0. TL;DR: decisions

| # | Decision | Default |
|---|---|---|
| 1 | Motion vectors come from the **base pass for everything**: static, rigid moving, skinned (previous bone palette), vertex-animated and alpha-tested foliage (wind evaluated at `Time` and `PrevTime`). Static objects are *not* skipped. The coverage bits have to be written anyway, so skipping would save almost nothing. | MV = `R16G16_FLOAT` 2x MSAA (note 07 contract), plus `R8_UINT` 2x MSAA "motion bits" (bits 0-1 = SV_Coverage, bit 2 = CAMERA_TRACKED, bit 3 = NO_MOTION_BLUR). Fallback if the MRT count is tight: one `R16G16B16A16_FLOAT` with z = coverage, w = flags. Both cleared to 0 each frame. |
| 2 | **Velocity pack** is one compute dispatch that also produces the tile max ("same draw", as in UE4/CryEngine 5/Fox). It reads the MSAA MV and depth, picks the front-most sample, rebuilds the camera-only velocity from depth, removes it, and adds it back scaled by the **depth-limited camera weight `w(z)`** (U-shape). It then applies the trail time (frame-rate independent "shutter") and clamps to `MaxTrailPx`. | `PackedVel` `R16G16B16A16_FLOAT` 1920x1080: xy = trail vector px, z = linear depth m. `TileMax` `R16G16B16A16_FLOAT` 60x34 (32 px tiles). |
| 3 | **Neighbor max** is **one-sided**: a neighbor tile counts only if its motion points away from the center tile, so its trail lies toward the center. It is used **only to find foreground trails**, never as a pixel's blur length. That removes the UE-style "thick dilated outline". | 3x3, tile = 32 px = `MaxTrailPx` |
| 4 | **Motion blur pass 1 = trailing-only gather.** Each pixel samples *ahead* along a motion vector (sources whose trails end on it). Each sample is accepted only if *its own* trail geometrically covers the pixel and it is in front or on the same motion layer. There are two layers: the pixel's own surface (along its own vector) and closer foreground trails (along neighbor max), composited "over". There is no background reconstruction (inner blur): the object stays opaque, its leading edge stays sharp, and the trail fades behind it. | N = 8 uniform taps per layer, no noise, `TrailFalloff` 0.5 |
| 5 | **Motion blur pass 2 = the "separable" pass** (Crytek 2008 iterative idea, as used in Crytek 2013, UE `r.MotionBlurSeparable` and Fox). It runs the same direction again on pass 1's output, over **one pass-1 stride** (`|v|/N`), depth/motion-aware. 8 x 8 gives 64 uniform virtual taps without any noise pattern. | M = 8, `Pass2Span = 1/N` (A/B: 1.5/N, 1 = Crysis-style tent) |
| 6 | **Placement:** after the AA temporal output (note 07: `aaOut` feeds motion blur), and **before bloom**, so that bloom reacts to the motion-blurred image. Running in HDR before tonemap would keep bright streaks, but it conflicts with per-sample SMAA S2x. See open question Q1. | |
| 7 | **Half-res VFX:** checkerboard min/max depth downsample of the 2x MSAA depth into a half-res D32 plus R32F linear depth. Low-frequency particles go to a half-res RGBA16F with premultiplied alpha, transmittance in alpha. The composite is a nearest-depth upsample (bilinear away from depth edges) into the MSAA HDR target, with alpha `MAX` = responsive mask. Fine particles stay native. | 960x540 |
| 8 | **Composite effects** (flares, light glows, rain/lens drops) go to half-res RGBA16F buffers, depth-tested against the same half-res depth. They are composited in the final post pass together with bloom. | |
| 9 | Expected cost at 1080p on the RTX 5060 (**estimate**, measure): pack + tiles 0.10-0.2 ms, MB pass 1 0.15-0.45 ms, pass 2 0.1-0.2 ms, depth downsample about 0.03 ms, VFX composite about 0.05-0.15 ms. Half-res VFX save about 75% of the particle PS invocations. | |

---

## 1. How the video positions these topics

- **0:08-0:09.** Clear any buffer where MSAA coverage information is stored. "It made sense to store it
  with motion vector data as both will be relevant in later anti-aliasing stages." Our MB output
  passes write every pixel, so they are never cleared. The video criticizes UE for clearing the MB
  output: "Seventh-gen games like Crysis 3 did not waste time doing this. They just overwrote the
  buffer." (1:44)
- **0:28.** Some content is "motion vector pre-passed" (dual-normal objects) to keep the base-pass
  output count below the limit. That is a G-buffer-note concern; our MV layout must not add more than
  2 RTs.
- **1:34-1:35.** Foliage ghosting happens because foliage had no motion vectors. Motion vectors "can
  be provided at a near free cost if produced in the base pass", including alpha-tested animated
  foliage.
- **1:39.** The custom MSAA mask saved in the motion vector buffer is used for edge-only temporal
  blending (note 07).
- **1:42-1:44 (Days Gone, UE4.11 motion blur at max quality).**
  - MB starts before tonemap and auto exposure.
  - A **velocity packing** draw combines native object motion vectors and native depth (camera
    velocity reconstructed from depth) into a native-res **RG11B10**, so the MB reads one resource.
    "Since our depth and velocity are multi-sampled, making this resource, which will not be
    multi-sampled, will end up being a big help, and we can implement some special logic here."
  - A **120x68 max-tile RGBA16** is generated in the same draw (UE4, CryEngine 5).
  - A small draw dilates it.
  - The MB draw reads scene color, the packed velocity and the dilated tiles.
- **1:45.** Noise patterns used against step artifacts:
  - White noise (NFS 2015): "one of the better ones".
  - Interleaved gradient noise (later UE4, UE5): "looks awful".
  - Blue noise (Alan Wake 2, Wicked Engine, Ghost of Tsushima): awful.
  - Simple checkerboard (Jimenez's suggestion; early UE4/Days Gone, Dead Space Remake, KCD2).
- **1:46-1:47.** The **max-tile path is red-coded**.
  - It produces a "thick motion blur outline around the character" with "massive blocky artifacts".
  - Without dilation the outline is slimmer, but tile shapes remain and some edges lose accuracy.
  - NFS 2015 has no tile resource and no such artifact. Instead, "the road velocity behind the barrel
    smears on top of it, instead of the barrel occluding the road velocity".
  - "Maybe we can keep the tiled approach ... but we need a solution."
- **1:47-1:49.** **Depth-limited camera motion blur**, done in the velocity packing stage.
  - Per-object MB "is just removing the camera movement from the velocity input".
  - We should "only allow camera motion blur up to a certain depth with decreasing intensity",
    about 5-10 ft.
  - Production needs "more of a U shape". "A low-resolution mask will probably act as the best way to
    approach such a shape, and all depth beyond this point should be blurred under extreme camera
    motion".
  - Expose artist variables for the cutoffs.
- **1:49-1:51.** **Single-direction trailing.**
  - The blur should trail only behind the motion, "like our naked eye perception". NFS 2015 does
    this: distant lights "stretch along the screen from a clearer point of origin".
  - Centered or bidirectional sampling "resembles behavior you only see in camera footage" (Days Gone).
  - Background streaking behind an object dilutes the object's point of origin.
  - "Single direction velocity trailing with plausible depth aware motion blur is going to catapult
    graphics."
- **1:51-1:56 (Fox Engine, MGSV:TPP, 4K capture with half-res MB at 1080p).**
  - A CS pass sorts the motion vectors into tiles and builds "neighborhood tiles" (the transcript says
    "120 by 168"; at a 1080p internal resolution this is presumably 120x68 = 16 px tiles).
  - A half-res RGBA8 holds:
    - RG = velocity, "packed in a way that gives closer objects higher precision";
    - BA = the max neighborhood tiles, "bilinearly upscaled".
  - So the MB pass samples only this resource and the scene color.
  - The MB pass uses **uniform sampling, no pattern, "three times per direction"** (centered, like
    Days Gone).
  - It is followed by a **separable pass**: the same packed velocity, with the uniformly blurred
    output as input. This pass costs about the same as the first pass, and 40% more than UE's
    separable pass.
  - "UE's separable motion blur is sort of make-believe."
  - The Fox HLSL "straight-up copy" Crytek's 2008 sample code.
  - Wanted: "a competent, aggressive separable pass that builds off of Crytek's sample code".
  - Open R&D: sample count, possibly a checkerboard plus separable, and "uniform sampling to prevent
    any chance of noise".
- **1:56-1:57.** Trail length must be a constant matching perception, not a "shutter speed". MGSV's
  *medium* setting is "much more believable". The proposed calibration is a digital hand on screen
  driven by a real hand in a motion-capture glove. The term is "velocity compensation", meaning what a
  monitor cannot send to the eyes, independent of fps.
- **1:57.** "We want bloom that reacts to the motion blurred result" (shiny weapons, spinning wheels).
- **1:02-1:04.** The **"Phantom Pain's half-resolution min-max depth effect rendering for
  low-frequency effects and sprites"**.
  - It is "the biggest performance optimization" of the transparents stage. It "massively reduce[s]
    pixel shader invocations ... without common low-resolution depth discontinuities".
  - Fine particles cannot be half-res.
  - The two half-res buffers must be cleared between the native and the half-res VFX.
  - Its resources should be reused for half-res SSS if that route is taken.
  - All VFX/transparents write a **responsive mask in alpha**, which is easy for half-res effects
    because their blend mask can be recycled.
- **1:58-1:59.** **Composite effects** (flares, light-source blooming).
  - They use half-res compositing buffers (Fox). Crysis 3's native-res composite buffer "completely
    chokes performance for pretty much no noticeable gain".
  - NFS 2015 also composites at half res, in RG11B10. Ours uses the available half-res RGBA16s.
  - The pass depth-tests against a half-res hardware depth, as does a half-res RGBA16 holding rain
    and lens drops. That buffer is composited "with bloom so that those watery effects look like
    they're absorbing the local light", in the same pass as film grain, grading and chromatic
    aberration.

---

## 2. Pass list and resources (1920x1080, 2x MSAA)

```
Base pass (MSAA 2x)   : ... + RT_MV (R16G16_FLOAT, MSAA2) + RT_MotionBits (R8_UINT, MSAA2)   [cleared to 0]
Lighting / sky / fog  : (other notes)
Transparents & VFX    : 1. decals, glass, fine native-res particles -> HDR MSAA (alpha MAX = responsive)
                        2. DepthDownsample PS @960x540: checkerboard min/max -> HalfDepth D32 + HalfLinZ R32F
                        3. Clear HalfVfx RGBA16F to (0,0,0,1)
                        4. Half-res low-frequency VFX (premultiplied) -> HalfVfx, depth test vs HalfDepth
                        5. Composite PS @full res: nearest-depth upsample -> HDR MSAA (alpha MAX)
Tonemap + SMAA S2x/T2x: note 07 -> aaOut
Motion blur           : 6. VelocityPack+TileMax CS  (reads MV MSAA, MotionBits MSAA, depth MSAA)
                        7. NeighborMax PS @60x34
                        8. MB pass 1 (trailing gather) -> MbTemp RGBA16F
                        9. MB pass 2 (separable smoothing) -> MbOut
Bloom                 : reads MbOut (bloom note)
Composite effects     : flares/glows -> HalfFlare RGBA16F (additive, HalfDepth test); lens drops -> HalfDrops
Final composite       : bloom + flares + drops + grain + grading + CA, then UI (other notes)
```

| resource | format | size | written by | notes |
|---|---|---|---|---|
| RT_MV | R16G16_FLOAT, MSAA 2 | 15.8 MiB | base pass | UV units, cur - prev, jitter-free (note 07) |
| RT_MotionBits | R8_UINT, MSAA 2 | 4.0 MiB | base pass | bits 0-1 SV_Coverage, bit 2 CAMERA_TRACKED, bit 3 NO_MOTION_BLUR |
| PackedVel | R16G16B16A16_FLOAT | 15.8 MiB | pack CS (UAV) | xy = trail px, z = linear depth (m), w = 0 |
| TileMax | R16G16B16A16_FLOAT | 60x34 | pack CS (UAV) | xy = longest trail in the 32x32 tile |
| NeighborMax | R16G16B16A16_FLOAT | 60x34 | PS | xy = dominant foreground trail |
| MbTemp | R16G16B16A16_FLOAT | 15.8 MiB | MB pass 1 | rgb, a = foreground-trail alpha (pass-2 direction hint) |
| MbOut | whatever bloom takes | | MB pass 2 | can alias an existing post target |
| HalfDepth | R32_TYPELESS (DSV D32_FLOAT) | 2.0 MiB | downsample (SV_Depth) | reused by composite effects |
| HalfLinZ | R32_FLOAT | 2.0 MiB | downsample (SV_Target) | soft particles + upsample |
| HalfVfx | R16G16B16A16_FLOAT | 4.0 MiB | half-res VFX | rgb premultiplied, a = transmittance |
| HalfFlare, HalfDrops | R16G16B16A16_FLOAT | 2 x 4.0 MiB | composite effects | |

`R16G16B16A16_FLOAT` typed UAV *stores* are required on FL 11_0 hardware (Microsoft format-support
table). The CS never *loads* from a typed UAV, so nothing optional is used.

---

## 3. Base-pass motion vectors

### 3.1 What and why

- Per-pixel velocity from a velocity render target bound during the color pass dates from Crysis
  (Sousa 2008, DX10 only).
  - Crysis wrote "screen space velocity, surface depth and instance ID into a FP16 RT".
  - It skipped unmoved geometry by comparing the previous and current matrices or bones.
  - Previous bone matrices did not fit in the 256 DX9 vertex constants (70 bones x 2 registers x 2
    frames = 280), so the effect was DX10-only.
- McGuire 2012 renders V "at full resolution to an additional draw buffer bound during the color
  rendering pass" on modern GPUs.
- Sousa 2013 (Crysis 3): "Output object velocity in G-Buffer (only when required)".
  - Rigid geometry writes it when the object distance is below a threshold.
  - Deformable geometry writes it when its movement exceeds a threshold.
  - "Moving geometry rendered last". R8G8, "velocity encoded in gamma 2.0 space".
- MGSV (Courrèges 2017):
  - Writes a velocity map for dynamic meshes, with a mask in R.
  - Computes static velocity from depth and the previous matrices.
  - Composites the two.
  - Treats animated characters as static, to avoid a second skinning pass.
- The video wants **full-scene** motion vectors, including alpha-tested, wind-animated foliage, for
  the TAA velocity weighting (note 07) and for motion blur.

### 3.2 Exact recipe (D3D11, SM 5.0)

Per object, keep `World` and `PrevWorld`. Per skinned mesh, keep the current and previous bone
palettes. Per frame, keep `ViewProj` (jittered, used for raster per note 07), `ViewProjNoJitter`,
`PrevViewProjNoJitter`, `Time` and `PrevTime`.

```hlsl
cbuffer PerFrame : register(b0) {
    row_major float4x4 ViewProj;              // raster (may contain note-07 jitter)
    row_major float4x4 ViewProjNoJitter;      // motion vectors only
    row_major float4x4 PrevViewProjNoJitter;  // last frame's ViewProjNoJitter (camera-relative origin!)
    float Time, PrevTime;                     // PrevTime = exactly the Time used last frame
};
cbuffer PerObject : register(b1) {
    row_major float4x4 World;
    row_major float4x4 PrevWorld;             // = World on the first visible frame / after teleport
    uint MotionFlags;                         // CAMERA_TRACKED = 4, NO_MOTION_BLUR = 8
};

struct VSOut {
    float4 pos      : SV_Position;
    float4 curClip  : TEXCOORD6;   // perspective-correct interpolation (NOT noperspective)
    float4 prevClip : TEXCOORD7;
    // ... uv, normal, etc.
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p     = SkinOrRigid(v, Bones);        // skinned: current palette
    float3 pPrev = SkinOrRigid(v, PrevBones);    // skinned: previous palette (rigid: same as p)
    float3 wp     = mul(float4(p,     1), World).xyz;
    float3 wpPrev = mul(float4(pPrev, 1), PrevWorld).xyz;
    wp     += WindOffset(v, wp,     Time);       // identical function and inputs except time
    wpPrev += WindOffset(v, wpPrev, PrevTime);
    o.pos      = mul(float4(wp, 1), ViewProj);
    o.curClip  = mul(float4(wp, 1), ViewProjNoJitter);
    o.prevClip = mul(float4(wpPrev, 1), PrevViewProjNoJitter);
    return o;
}

struct PSOut { /* G-buffer targets ... */ float2 mv : SV_Target4; uint bits : SV_Target5; };

// in the base-pass PS (alpha-tested materials: clip() first, the outputs below are unchanged)
void WriteMotion(VSOut i, uint coverage /* : SV_Coverage */, inout PSOut o)
{
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.mv   = (cur - prev) * float2(0.5, -0.5);      // UV units, y down, cur - prev
    o.bits = (coverage & 3u) | (MotionFlags & 0xCu);
}
```

Notes:

- **Coverage.** `uint coverage : SV_Coverage` as a PS *input* is SM 5.0. It does not change the
  shading frequency. What exactly goes into the 2 coverage bits (raw coverage vs the video's
  "shared flat edge" filtered mask) is decided by the AA/G-buffer notes. This note only reserves the
  bits.
  - Our pack pass uses `bits != 0` as "this sample was written by geometry". The buffer is cleared to
    0, so the sky stays 0.
- **Camera-tracked flag.** The player character, the player's vehicle and the first-person weapon set
  `CAMERA_TRACKED`. These keep screen-relative motion in the depth-limited camera blur (section 5.3).
- **No jitter in MVs.** Use the unjittered matrices on both sides, as note 07 requires.
  Alternatively, apply the same current jitter to both sides, as the SMAA demo does.
- **Skinned meshes.** Skinning twice in the VS doubles the skinning ALU. That is the simplest
  approach and the right one for a prototype.
  - Alternative: CS skinning into a position buffer, keeping last frame's buffer (ping-pong). This
    costs memory instead of ALU.
- **Instancing.** The instance buffer needs a `PrevWorld` per instance.
- **Particles and other non-opaque draws** do not write MVs. They use the responsive mask instead
  (note 07, section 9). See also the motion-blur limitation in 7.6.

### 3.3 Static objects: always write, or reconstruct from depth?

| option | how | saves | costs / risks |
|---|---|---|---|
| **A. Always write (recommended)** | Same shader, `PrevWorld = World` | nothing to manage | +1 matrix multiply per vertex, +8 interpolator floats, MV export (RT write) |
| B. Skip static, reconstruct | Clear MV to a sentinel; the pack pass rebuilds the camera velocity from depth for unwritten pixels (UE4's VelocityFlatten does exactly this: `Velocity.x > 0 ? decode : camera reprojection`) | VS math and interpolators of static draws | The RT write still happens if the MV and coverage RTs are bound, so bandwidth is the same. It needs per-draw MRT write masks to save export bandwidth, plus two shader permutations. |

Option B saves almost nothing here, because the coverage bits force a write to the motion targets for
*every* pixel anyway. The reconstruction is still needed in the pack pass, but for two other reasons:
(1) the sky/cleared pixels, and (2) splitting each pixel's motion into camera and object parts for the
depth-limited camera blur (section 5).

### 3.4 Pitfalls

- **PrevWorld timing.**
  - Copy `World` to `PrevWorld` *after* the frame is submitted.
  - Objects culled last frame still need their previous transform updated every frame, or a
    "first visible frame" flag that forces `PrevWorld = World`. Otherwise objects that pop into view
    streak.
- **Camera cuts and teleports.** For that frame, set `PrevViewProjNoJitter = ViewProjNoJitter` and
  `PrevWorld = World` on the affected objects. Note 07's temporal pass wants the same reset.
- **Precision.** Use camera-relative world space for both matrices. Otherwise large world
  coordinates quantize both clip positions and the MV becomes noise.
  - RG16F in UV units has a relative precision of 2^-11. For a 20 px velocity at 1920 that is about
    0.01 px, which is fine.
- **Wind.** `PrevTime` must be the time actually used last frame, not `Time - dt` recomputed. The
  wind function must not read anything else that changes per frame (gust textures, scroll offsets)
  unless the previous value is also passed.
- **Interpolation.** `curClip` and `prevClip` are float4 with default (perspective) interpolation, and
  the divide happens in the PS.
  - At MSAA edge pixels the pixel-center evaluation may extrapolate outside the triangle. That is
    still exact for the triangle's plane, so `centroid` is not needed.

### 3.5 Cost and how to measure

- Cost is a few dozen VS ALU per vertex (doubled skinning for skinned meshes), 8 interpolator floats,
  and the export of 4 B + 1 B per sample. At 2x MSAA 1080p that is about 20 MiB of extra RT writes
  per frame, before compression.
- Measure the base-pass time (timestamp queries) with the MV and bits RTs bound vs not bound. Also
  measure skinned characters with the previous-palette skinning on vs off.
- The video's claim is "near free". On a 448 GB/s GPU, 20 MiB costs about 0.05 ms of pure bandwidth.

---

## 4. Prior art, with the exact algorithms

### 4.1 Crysis (Sousa, GDC 2008, "Crysis Next-Gen Effects")

**Camera MB.**

- Render a sphere around the camera. Interpolate between the previous and current camera transforms
  by a shutter ratio, then compute screen-space current and previous positions per vertex.
- In the PS, step along `vStep = vPrev - vCurr` with `s` from -1 to 1 (centered).
- Per tap, a **velocity mask** (from depth) scales the offset. Pixels closer than a near threshold
  get mask 0, used "for blending out blur from first person arms/weapons". The blur therefore "scales
  at distance", and camera rotation can override the mask.
  - This is the *opposite* of the video's depth limit (near blurred, far sharp).
- **"Iterative sampling approach: first pass uses 8 samples, ping-pong results, second pass uses
  blurred results, this results in 8 x 8 samples (virtually 64); 3rd = 512 samples, 4th = 4096."**
  This is the origin of the "separable" motion blur the video praises.
- Skip camera MB entirely when there is no movement. Estimate the required sample and pass count from
  the camera's translation and rotation speed.
- About **1 ms average at 1280x1024 on a G80**.

**Object MB (DX10).**

- Velocity, depth and ID go into an FP16 RT, with near-zero velocities written as 0.
- An 8x smaller mask (Gaussian-blurred velocity length) is used to skip pixels.
- Edge dilation runs in 4 separable passes (2 horizontal, 2 vertical). A pixel dilates only
  "if world depth > surface depth".
- The final blur accumulates N samples centered on the pixel (`(i/N) - 0.5`) along the *clamped*
  velocity ("Very important to avoid ugly results, especially with jerky animations"). It lerps toward
  the result by `saturate(2 * accumulated alpha mask)`.

### 4.2 Crysis 2 (Sousa, Gamefest 2011)

- Camera velocity is now "re-projection + velocity buffer": static velocity from depth and the
  previous view-projection, dynamic geometry outputs velocity.
- Full res costs at least about 3 ms (console worst case), so it runs **half res** (inspired by KZ2).
- Dilation is done "on the fly, on same pass as when applying motion blur".
- Done "in HDR, before tone mapping ... bright streaks are kept and propagated".
- **9 taps on consoles with max-range clamping, 24 taps on PC.**

### 4.3 McGuire, Hennessy, Bukowski, Osman (I3D 2012), "A Reconstruction Filter for Plausible Motion Blur"

- **Inputs.**
  - C: HDR color (RGB16F, post MSAA resolve).
  - V: velocity, the *half-spread* `q = 0.5 (X - X') * exposure * framerate`, clamped to
    `[0.5 px, k]`, stored biased in RG8.
  - Z: linear depth in R16F.
- **TileMax** (w/k x h/k): `vmax` over each k x k tile, where vmax is the *longest vector*, not the
  per-component max.
- **NeighborMax**: vmax over the 3x3 tiles. Because `|V| <= k`, every pixel then knows the farthest
  neighbor that can reach it, under the dominant-velocity assumption.
- **Reconstruction at pixel X** (paraphrase of their Listing 1):
  ```
  vN = NeighborMax[X/k];  if |vN| <= eps + 0.5px: return C[X]
  weight = 1/|V[X]|;  sum = C[X] * weight
  j = random(-0.5, 0.5)                        // per-pixel jitter of the whole filter
  for i in [0,S), i != (S-1)/2:
      t = mix(-1, 1, (i + j + 1)/(S + 1));  Y = round(X + vN * t)
      f = softDepthCompare(Z[X], Z[Y])          // Y in front of X
      b = softDepthCompare(Z[Y], Z[X])          // Y behind X
      aY = f * cone(Y, X, V[Y])                 // case 1: blurry Y in front spreads over X
         + b * cone(X, Y, V[X])                 // case 2: X is blurry -> see background through it
         + cylinder(Y, X, V[Y]) * cylinder(X, Y, V[X]) * 2   // case 3: both blurry
      weight += aY;  sum += aY * C[Y]
  return sum / weight
  ```
  where:
  - `cone(X,Y,v) = clamp(1 - |X-Y|/|v|, 0, 1)`
  - `cylinder(X,Y,v) = 1 - smoothstep(0.95|v|, 1.05|v|, |X-Y|)`
  - `softDepthCompare(za,zb) = clamp(1 - (za-zb)/SOFT_Z_EXTENT, 0, 1)` (their negative-z convention)
  - `SOFT_Z_EXTENT` = 1 mm to 10 cm.
- **Parameters.**
  - k = 20, S = 15 at 1280x720.
  - S = 5 is sufficient on consoles (TV filtering); S = 15 "yields a perceptible increase" on
    monitors at nearly the same cost.
  - The ratio k/S should stay constant as resolution grows.
- **Performance (1280x720, full-screen camera motion).** GeForce 480: 6.2 ms (k = 31), 3.0 ms (k = 15),
  2.8 ms (k = 7), 2.7 ms (k = 5). Xbox 360: 1.5 ms (k = 5).
- **Related remarks.**
  - When the camera tracks an object, that object stays sharp and the rest blurs.
  - Valve recommends 15% of a full-frame shutter and a max velocity of 4% of the screen width
    (Vlachos 2008).
  - For third person, McGuire recommends using "only 0-15% of the previous camera" (an interpolated
    camera in X'), ramping it up for falls and jump pads. This is a precursor of the video's
    "remove camera velocity".
  - Artistic per-mesh velocity scales and velocity-only "particles".
  - A width-k guard band is technically needed.

### 4.4 Guertin, McGuire, Nowrouzezahrai (HPG 2014; NVIDIA tech report 2013), "A Fast and Stable Feature-Aware Motion Blur Filter"

Their fixes to the single-velocity artifacts (tiles, overlapping motions, thin features):

- Sample along **two directions**: the tile's vmax, and a per-pixel center direction
  `vc = lerp(v(p), perp(vmax), (|v(p)| - 0.5)/gamma)`.
  - Samples are split by tile variance. A conservative N/2 split gives "roughly equal quality".
- **Velocity-direction-aware weights.**
  - Case 1 is multiplied by `dot(sample velocity dir, sampling dir)`.
  - Case 2 is multiplied by `dot(vc, vmax)`.
  - Case 3 is multiplied by the max of the two.
- **Stochastic tile lookup** near tile borders, which trades banding for noise.
- **NeighborMax ignores diagonal tiles** whose vmax does not point at the center tile. CryEngine 5
  ships this check.
- **Center weight** `wp = N/(kappa * |vc|)`, with kappa = 40. The midpoint sample is not skipped.
  This makes the result independent of N and fixes thin-object ghosting.
- Halton-jittered sample positions, with the jitter reduced as N grows.
- **Relative depth compare** `zCompare(za,zb) = clamp(1 - (za-zb)/min(za,zb), 0, 1)`, which is
  scene-scale independent.
- **Encoding fix.** McGuire's RG8 encoding clamped x and y separately to +-r. Encode the vector
  length-normalized instead.
- They suggest "a post-process AA filter designed ... for motion blur ... that re-blurs in the
  direction of motion (as in [Sou08])".
- **Performance (Sponza, all pixels moving, GTX 780).** Single-direction values in parentheses.

  | N | 720p (ms) | 1080p (ms) |
  |---|---|---|
  | 19 | 1.11 (0.93) | 1.96 (2.06) |
  | 27 | 1.41 (1.19) | 3.17 (2.71) |
  | 35 | 1.68 (1.52) | 3.77 (3.44) |
  | 47 | 2.18 (2.02) | 4.91 (4.54) |

  Parameters were N = 35 and r (tile) = 40. Intermediates were UINT8, point sampled.

### 4.5 Jimenez (SIGGRAPH 2014), "Next-Generation Post-Processing in Call of Duty: Advanced Warfare"

It is based on McGuire 2012/2013 and Sousa 2013's vectorization.

- **Tiles.**
  - TileMax over 20x20 px, computed separably: [w x h] to [w/20 x h], then to [w/20 x h/20].
  - Then TileNeighborhood = max over 3x3.
  - Then a full-res variable-width blur in the tile velocity direction.
- **Packing.** Velocity and depth go together into one **R11G11B10** ("reduces taps per sample from 3
  to 2").
  - Tile coordinates are dithered to reduce tiling artifacts.
  - Point sampling is used to avoid bleeding.
- **Sample weight.** Paraphrased (Δz = sample depth - center depth, positive = sample farther):
  - `DepthCmp = saturate(0.5 + (+s, -s) * Δz)`. This gives a (background, foreground) pair that sums
    to 1.
  - `SpreadCmp(offset, (centerSpread, sampleSpread)) = saturate(scale * spread - offset + 1)`, in sample
    units. With half-sample offsets it is `... - max(offset - 1, 0)`.
  - `weight = dot(DepthCmp, SpreadCmp)`.
  - So a background sample is weighted by the *center's* spread (inner blur), and a foreground sample
    by the *sample's* spread (outer blur).
- **Center as background, single direction.** Accumulate `sum.rgbw` over samples, then
  `sum *= 1/SAMPLE_COUNT` and `color = sum.rgb + (1 - sum.w) * center.rgb`. This removes McGuire's
  discontinuity between the inner and outer gradients.
- **Mirrored background reconstruction.** Samples are taken in symmetric pairs, and the weights are
  swapped or mirrored depending on `(depth1 > depth2, vel2 > vel1)`. This blurs the known background
  as much as the reconstructed one.
- **Sampling patterns tried.**
  - Uniform (noise = 0).
  - Random (a fast hash).
  - Dither. The best was a simple checkerboard: `±0.25` from `(pos & 1)`, sign-flipped per row.
  - The take-away: "Random noise better if you are heavily undersampling", while dither "works better
    in other cases ... to fill smaller sampling gaps like in motion blur ... a 'finisher'".
  - The same deck introduces **interleaved gradient noise** (`frac(52.98 * frac(dot(pos, (0.0671,
    0.0058))))`), but for its DOF sample rotation, not for motion blur. UE later used IGN for motion
    blur.
- **Fast paths.** Tile min and max are both stored. Tiles with `max - min` small converge to a plain
  color average with no depth or velocity tests. Tiles with tiny max exit early.
- **Cost.** **0.52 ms on Xbox One** (running scene), 1080p.

### 4.6 Sousa (SIGGRAPH 2013), "Graphics Gems from CryENGINE 3": the 2-pass "separable" variant

- **Inputs.**
  - Scene in R11G11B10F.
  - "Bake ||V|| and 8 bit depth into a R8G8 target".
  - Object velocity R8G8 in "gamma 2.0 space", composited with the camera velocity.
- **Make it separable, 2 passes [Sousa08].**
  - **1st iteration: 6 taps (0.236 ms). 2nd iteration: 6 taps (+0.236 ms; 36 taps accumulated).**
    Measured at 1080p on an AMD HD 7970.
  - The inner loop is "a couple of mads": a vectorized DepthCmp/VelCmp with McGuire's three cases
    collapsed into `dot(cmp_z.xy, cmp_v.xy) + cmp_v.z * cmp_v.w * 2`.
  - The "tile max velocity [is used] as center velocity tap".
- **Sampling.** "Fat buffers sampling are half rate on GCN hw with bilinear (point filtering is
  fullrate, but doesn't look good due to aliasing)".
- **Ordering.** DOF before MB introduces less error when MB happens on the focus.

### 4.7 Unreal Engine 4 (4.1x source, which Days Gone's 4.11 MB resembles)

Read from a 4.15-era engine tree (`PostProcessVelocityFlatten.usf`, `PostProcessMotionBlur.usf/.cpp`,
`PostProcessing.cpp`). Described here, not copied.

- **VelocityFlatten (CS, 16x16 threads).**
  - For each pixel, use the encoded object velocity if present. Otherwise reconstruct the camera
    velocity from depth with a precomputed "CameraMotion" polynomial.
  - Convert to **polar (length, angle)** and write **R11G11B10 = (length, angle/2π + 0.5, linear
    depth)**. The float format has no sign bit, hence the polar encoding.
  - In the same dispatch, a groupshared reduction of (min length, max length) per 16x16 tile writes a
    float4 (min.xy, max.xy). At 1080p that is the video's **120x68 RGBA16**.
- **Dilation.** It is either a gather CS (each tile looks at ±3 tiles and includes a neighbor if the
  center lies inside that neighbor's velocity-oriented swept quad) or a scatter (one oriented quad per
  tile, depth-sorted by velocity length).
  - Velocities may exceed a tile (MotionBlurMax default 5% of the width, ±48 px at 1080p), so
    dilation spans up to 7x7 tiles. This is the "inflated" look.
- **MainPS.**
  - Step counts per quality: 4, 6, 8, 16, split half per direction (centered, ±).
  - The tile lookup is jittered by ±0.25 tile.
  - Per-pixel random: IGN in 4.15. The checkerboard dither code is still present but disabled, which
    matches the video's "early UE4 used checkerboard".
  - **Skip path:** tile max < 0.5 px.
  - **Fast path:** tile min > 0.4 x tile max. It does a *plain color average* along the ±tile-max
    vector, with no depth or velocity weights.
  - Otherwise, Jimenez weights with mirroring, and `out = sum.rgb + (1 - sum.a) * center`.
- **`r.MotionBlurSeparable`** adds a second MainPS pass on the first pass's output.
  - Pass 0 scale is `1 - 0.5/N`.
  - Pass 1 scale is `(1/N) * r.MotionBlur2ndScale`, at quality 3. Its span is exactly **one stride**
    of pass 0.
  - Mathematically this composes N uniform taps with N sub-taps into N^2 uniform taps over the same
    length, which is the right idea. It still uses the tile-max search vector, the fast path and the
    noise. That is our best guess at why the video found it "less competent": the stepping is
    reduced, but the noise and tile artifacts remain.
- **Scale.** `MotionBlurAmount` (default 0.5) x velocity, applied in both directions. The total blur
  length is Amount x per-frame displacement, i.e. a 180° shutter, centered.

### 4.8 CryEngine 5 (source-available, `MotionBlur.cfx` + `MotionBlur.cpp`)

- **PackVelocities PS.**
  - Linear depth gives the world position, which is reprojected with `mViewProjPrev`.
  - The object velocity replaces it when present.
  - It is scaled by `exposureTime / frameTime` (constant exposure regardless of fps) and clamped to a
    max length.
  - Optional radial and directional artistic blur is added.
  - Output: xy = encoded velocity, z = `sqrt(len * 32)` (gamma-2 length, more precision for small
    velocities), w = linear depth, all RGBA8.
- **VelocityTileGen** is separable, 2 passes. **VelocityTileNeighborhood** uses 3x3, with diagonal
  tiles included only if they point at the center (Guertin).
- **MotionBlurPS.**
  - 6 (low), 14 (medium) or 24 (high) samples, centered ±.
  - Checkerboard dither ±0.25.
  - The tile lookup is randomized near tile borders, only along one axis per checkerboard parity.
  - McGuire/Jimenez weights with mirroring.
  - The output `acc` (rgb plus weight in a) is blended with `ONE, INV_SRC_ALPHA` onto the scene.

### 4.9 Fox Engine (MGSV) and NFS 2015

The only public descriptions are the video and Courrèges 2017.

- **Courrèges.** MGSV's velocity map is 8-bit. The motion blur is "based on [MHBO 2012]" with a
  low-res max-velocity tile map. MB runs after DOF and before color grading. Transparents and
  particles are drawn *after* tonemapping, in LDR.
- **The video** adds the uniform 3-taps-per-direction plus separable pass, and the half-res RGBA8
  packing with bilinear neighbor tiles (1.4 above).
- **NFS 2015** (video): white-noise sampling, no tile resource, single-direction trailing, and
  background-over-foreground smearing (barrel).

### 4.10 Why tile max gives a "thick blocky outline": analysis (our reasoning, not from a source)

In the UE4 path the tile vector is used as more than a search hint:

1. **Fast path.** Whole tiles average colors along ±tile max *without per-sample tests*. A
   background tile next to a fast character, under camera motion, gets the character's blur length
   and direction.
2. **Inner blur.** Background samples are accepted with the *center's* spread, and dilation inflates
   the area where that happens.
3. **Dilation beyond 3x3.** UE allows velocities larger than a tile.

Together these produce a blurred band of tile-quantized width around moving objects. Disabling
dilation shrinks the band (the video observes exactly this), but the tile shapes from (1) and (2)
remain.

**Remedy.**

- (a) Use the tile vector only to *find* candidates.
- (b) Validate every sample with its own trail and depth.
- (c) Clamp trails to the tile size, so 3x3 is exact.
- (d) Have no fast path that uses tile lengths.
- (e) Keep a second sampling direction (the pixel's own), so regions whose motion differs from the
  tile max keep their own blur (Guertin).

Our pass 1 does (a)-(e).

---

## 5. Velocity packing pass, with depth-limited camera blur (ours)

### 5.1 Separating camera and object motion

For each pixel (the front-most of its 2 samples):

- `vTot` is the base-pass MV. For unwritten (sky) samples it is `vCam`.
- `vCam` is the motion this point *would* have if it were static. It is reconstructed from depth with
  `CurClipToPrevClip = inverse(ViewProjNoJitter) * PrevViewProjNoJitter` (row-vector order).
- `vObj = vTot - vCam` is the object's own motion, the part per-object MB keeps. If it is below
  0.25 px, set it to 0: that is reprojection noise from the sample-position and pixel-center mismatch.
- `v = vObj + w(z) * vCam`.
  - w = 1 gives classic per-pixel MB.
  - w = 0 gives "per-object" MB, which the video calls "removing the camera movement from the
    velocity input".

### 5.2 The U-shaped weight w(z): artist variables

```
wNear(z) = 1 - smoothstep(NearStart, NearEnd, z)      // 1 close to the eye, 0 beyond ~10 ft
wFar(z)  = smoothstep(FarStart, FarEnd, z) * FarAmount // only under extreme camera motion
w(z)     = CameraBlurScale * max(wNear(z), wFar(z))
FarAmount = smoothstep(Omega0, Omega1, cameraAngularSpeed)   // CPU, per frame
```

| variable | default | meaning |
|---|---|---|
| NearStart / NearEnd | 1.5 m / 3.0 m (5 ft / 10 ft) | the video's "5 or 10 ft" |
| FarStart / FarEnd | 20 m / 60 m | the far arm of the "U" |
| Omega0 / Omega1 | 2 rad/s (115°/s) / 6 rad/s (344°/s) | what counts as "extreme camera motion" |
| CameraBlurScale | 1.0 | global camera-blur amount |

- **Camera angular speed:** `acos(clamp(dot(fwdPrev, fwdCur), -1, 1)) / dt`, or the quaternion delta
  if roll matters.
- **Why rotation only.** Rotation-induced screen velocity is depth-independent, so it is what makes
  far content streak. Translation-induced velocity falls off as 1/z by itself.
- **The video's "low-resolution mask" idea.** We use an analytic per-pixel w(z) instead. It is smooth
  in depth and costs nothing. The R&D alternative is to evaluate w on the 60x34 tile grid (tile min
  depth) and sample it bilinearly, which spatially softens the cutoff at depth discontinuities. That
  is an A/B item.

### 5.3 The camera-tracked pitfall (important)

- Take a car the camera follows. Its screen motion `vTot` is about 0, so `vObj = -vCam`.
- With `w = 0` (the car beyond 3 m), its velocity becomes `-vCam`: **the tracked car blurs while the
  world stays sharp**. That is inverted from what an eye tracking the car sees.
- The same happens to the third-person character in the 1.5-3 m transition band.
- **Fix:** objects flagged `CAMERA_TRACKED` use `vTot` (screen-relative), whatever their depth.
- A first-person weapon has `vTot` of about 0, so it is sharp either way. That reproduces Crysis 2008's
  "blend out blur from first person arms" without a special depth mask.
- `NO_MOTION_BLUR` (bit 3) forces `v = 0`. This is McGuire's "under-blur signage the eye would track".

### 5.4 Trail time ("velocity compensation") and clamping

- `TrailScale = TrailTime / FrameDt`, where FrameDt is the real time between the two frames the MVs
  span. This is CryEngine 5's `exposureTime / timeStep`, and it makes the trail length independent of
  frame rate, as the video demands ("doesn't matter if your content is at 30 or 500 fps").
- **Default `TrailTime` = 8 ms.** That is about a 50% (180°) shutter at 60 fps.
  - R&D range 2-16 ms. Valve's 15% shutter is 2.5 ms at 60 fps.
  - The mocap-glove calibration from the video decides the final value (Q4).
- **Clamp:** `|v| <= MaxTrailPx` = 32 px at 1080p. Scale it with resolution height. The tile size
  must equal MaxTrailPx so that the 3x3 neighbor max is exact (McGuire). Values below 0.5 px become 0.

### 5.5 HLSL: velocity pack + tile max in one dispatch

```hlsl
// VelocityPackCS.hlsl  cs_5_0   Dispatch(ceil(W/32), ceil(H/32), 1)
#define TILE 32
#define TPG  16                      // 16x16 threads, each thread packs a 2x2 pixel quad
#define CAMERA_TRACKED 4u
#define NO_MOTION_BLUR 8u

Texture2DMS<float2, 2> MotionMS : register(t0);   // RT_MV
Texture2DMS<uint,   2> BitsMS   : register(t1);   // RT_MotionBits
Texture2DMS<float,  2> DepthMS  : register(t2);   // R32_FLOAT SRV of the MSAA depth
RWTexture2D<float4> PackedVel   : register(u0);   // xy trail px, z linear depth
RWTexture2D<float4> TileMax     : register(u1);   // xy longest trail in tile

cbuffer VelocityPackCB : register(b0) {
    row_major float4x4 CurClipToPrevClip;  // inverse(ViewProjNoJitter) * PrevViewProjNoJitter
    float2 ScreenSize, InvScreenSize;
    float  TrailScale, MaxTrailPx;
    float  NearStart, NearEnd, FarStart, FarEnd;
    float  FarAmount, CameraBlurScale;
    float4 DepthParams;                    // see LinearDepth(); shared helper with the VFX passes
};

// View-space depth (meters) from device depth d, for a finite perspective projection:
//   standard Z (near->0, far->1): DepthParams = (n*f, f, n - f, 0)
//   reversed Z (near->1, far->0): DepthParams = (n*f, n, f - n, 0)
float LinearDepth(float d) { return DepthParams.x / (DepthParams.y + d * DepthParams.z); }

float CameraBlurWeight(float z)
{
    float wNear = 1.0 - smoothstep(NearStart, NearEnd, z);
    float wFar  = smoothstep(FarStart, FarEnd, z) * FarAmount;
    return CameraBlurScale * max(wNear, wFar);
}

groupshared float3 gsMax[TPG * TPG];      // xy = trail, z = squared length

float2 PackPixel(int2 pix)
{
    // front-most of the two MSAA samples (foreground priority at edges)
    float d0 = DepthMS.Load(pix, 0), d1 = DepthMS.Load(pix, 1);
    float z0 = LinearDepth(d0),       z1 = LinearDepth(d1);
    int   s  = (z1 < z0) ? 1 : 0;
    float d  = s ? d1 : d0;
    float z  = min(z0, z1);
    uint  bits = BitsMS.Load(pix, s);

    float2 uv       = (pix + 0.5) * InvScreenSize;
    float4 prevClip = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1), CurClipToPrevClip);
    float2 prevUV   = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
    float2 vCam     = uv - prevUV;                                 // UV, cur - prev
    float2 vTot     = (bits != 0) ? MotionMS.Load(pix, s) : vCam;  // sky / unwritten -> camera only
    float2 vObj     = vTot - vCam;
    if (dot(vObj * ScreenSize, vObj * ScreenSize) < 0.0625) vObj = 0;   // < 0.25 px: noise

    float  w  = (bits & CAMERA_TRACKED) ? 1.0 : CameraBlurWeight(z);
    float2 v  = (vObj + w * vCam) * ScreenSize * TrailScale;       // trail vector in pixels
    float  len = length(v);
    if (len > MaxTrailPx) v *= MaxTrailPx / len;
    if (len < 0.5 || (bits & NO_MOTION_BLUR)) v = 0;

    PackedVel[pix] = float4(v, z, 0);
    return v;
}

[numthreads(TPG, TPG, 1)]
void VelocityPackCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    int2   base = int2(gid.xy) * TILE + int2(gtid.xy) * 2;
    float3 best = 0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        int2 pix = base + int2(i & 1, i >> 1);
        if (any(pix >= int2(ScreenSize))) continue;
        float2 v = PackPixel(pix);
        float  l2 = dot(v, v);
        if (l2 > best.z) best = float3(v, l2);
    }
    gsMax[gi] = best;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = TPG * TPG / 2; step > 0; step >>= 1)
    {
        if (gi < step && gsMax[gi + step].z > gsMax[gi].z) gsMax[gi] = gsMax[gi + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) TileMax[gid.xy] = float4(gsMax[0].xy, 0, 0);
}
```

- **Depth SRV.** The MSAA depth is created as `R32_TYPELESS` with `BIND_DEPTH_STENCIL |
  BIND_SHADER_RESOURCE`, a `D32_FLOAT` DSV and an `R32_FLOAT` SRV. A D24S8 depth works the same way
  with `R24_UNORM_X8_TYPELESS`. Unbind the DSV, or bind it read-only, before reading it.
- **Why the front-most sample.** At silhouettes the foreground's velocity and depth must win, so the
  foreground trail is found and the background cannot claim the edge pixel.
  - This differs on purpose from note 07's `velocity1x`, which averages the two samples for
    reprojection.
- **Fox-style alternative (A/B).** Pack at half res into RGBA8, with a bilinear neighbor max in BA.
  That is a scalability option. The video itself says the half-res *scene color* is not something to
  adopt unless it is wanted for scalability.

---

## 6. One-sided neighbor max

A trail starts at an object and extends *backwards* along -v. A neighbor tile at offset `o` (tile
units) can only reach the center tile if its motion points away from the center, `dot(v, o) > 0`.
That rule removes half of the usual dilation, and it generalizes Guertin's and CryEngine 5's
diagonal check to all 8 neighbors.

```hlsl
// NeighborMaxPS.hlsl  ps_5_0, viewport = TileCount (60x34)
Texture2D<float4> TileMax : register(t0);
cbuffer NeighborCB : register(b0) { int2 TileCount; };

float4 NeighborMaxPS(float4 svpos : SV_Position) : SV_Target
{
    int2   c    = int2(svpos.xy);
    float2 best = TileMax.Load(int3(c, 0)).xy;
    float  bestL2 = dot(best, best);
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 o = int2(x, y);
        if (x == 0 && y == 0) continue;
        int2 t = clamp(c + o, int2(0, 0), TileCount - 1);
        float2 v = TileMax.Load(int3(t, 0)).xy;
        if (dot(v, float2(o)) <= 0.0) continue;   // its trail points away from us
        float l2 = dot(v, v);
        if (l2 > bestL2) { best = v; bestL2 = l2; }
    }
    return float4(best, 0, 0);
}
```

This costs 2040 pixels x 9 loads, which is negligible.

---

## 7. Motion blur pass 1: trailing-only, depth-aware gather (ours)

### 7.1 Derivation

**Scatter model.** A point Y with trail vector v_Y, which already includes the trail time, deposits
its color along the segment from its current position y back to y - v_Y. The trail weight along the
segment is `k(s/|v_Y|)`. For a box, k = 1 (a physical shutter that *ends* at the current frame). For a
"comet", k = 1 - γ·s/|v_Y|, which gives the "clearer point of origin" of NFS 2015 and the video.

**Gather.**

- Pixel p receives Y if p lies on Y's trail, so the candidates lie *ahead* of p: `y = p + v_Y·t`,
  with t in (0, 1].
- p therefore samples **along +v** (in the direction of motion). Each sample is validated
  geometrically: is p within the sample's trail segment, both along it and within about 1 px of the
  line?
- Using *each sample's own* trail removes the "dominant velocity" over-blur. The direction used for
  sampling is only a search path.

**Occlusion.**

- A trail may pass over p only if Y is *in front of* p, or Y moves like p (the same surface, e.g. a
  slanted road or a pure camera rotation).
- A background trail can never cover a foreground. That fixes the NFS 2015 barrel: the road behind the
  barrel is farther and moves differently, so it is rejected.

**No inner blur.**

- A trailing exposure physically makes the leading edge semi-transparent: the object arrived there
  during the exposure.
- The video wants the object at its current position with a trail behind, and the task brief says the
  leading edge stays sharp.
- So p's own color fills any weight that no valid sample claimed. This is Jimenez's "center as
  background" correction, made one-sided. With it, we never need the hidden background, and the
  hardest part of McGuire's and Jimenez's filters (background reconstruction, mirroring) disappears.

**Two layers.**

- **Layer A** is p's own surface. It samples along p's own vector v_P and accepts samples that are in
  front or on the same motion. This gives the texture blur inside moving objects, the camera blur of
  backgrounds, and a sharp leading edge.
- **Layer B** is foreground trails from *other* closer objects. It samples along the neighbor-max
  vector v_N and accepts only samples that are strictly in front and moving differently. It runs only
  when v_N differs from v_P.
- **Final** = B over A. This is Guertin's two-direction idea, with an alpha composite instead of a
  split sample budget.
  - With a single direction (v_N only), backgrounds next to a moving character, under camera motion,
    would lose their own blur. That is a blocky "sharp halo".
  - With v_P only (no tiles, NFS-like), static backgrounds never receive object trails, which gives
    sharp silhouettes.

**Normalization.**

- With uniform motion, every sample of a solid object covers p with weight `1 - γt_i`.
- The expected sum is `norm = N·(1 - γ/2)` for t_i = (i + 0.5)/N, so `alpha = Σw/norm` is exactly 1
  right behind a solid object and falls off over |v|.
- For γ = 0 that falloff is the linear gradient of a box shutter. For γ > 0 the trail fades faster:
  comet-like, with the origin emphasized.

### 7.2 HLSL

```hlsl
// MotionBlurTrailPS.hlsl  ps_5_0, fullscreen triangle, RT = MbTemp (RGBA16F)
Texture2D<float4> SceneColor  : register(t0);   // aaOut (sRGB SRV -> linear values) or HDR resolve
Texture2D<float4> PackedVel   : register(t1);
Texture2D<float4> NeighborMax : register(t2);
SamplerState PointClamp  : register(s0);
SamplerState LinearClamp : register(s1);

cbuffer MotionBlurCB : register(b0) {
    float2 InvScreenSize;
    int    TileSize;        // 32 (= MaxTrailPx)
    float  RelSoftZ;        // 0.05: 5% of depth counts as "same depth"
    float  SameMotionTol;   // 0.05: 5% (+0.5 px) velocity difference counts as "same motion"
    float  TrailFalloff;    // gamma: 0 = box shutter, 0.5 default, 1 = linear comet
    float  Pass2Span;       // 1/N (pass 2)
    float  pad;
};

#define N 8   // uniform taps per layer (A/B: 3, 4, 6, 8, 12)

// Does the trail of a sample at offset 'off' ahead of p (trail vector vY) cover p?
float TrailCover(float2 off, float2 vY, out float along)
{
    along = 0;
    float len = length(vY);
    if (len < 0.5) return 0;
    float2 back = -vY / len;                      // trail runs backwards from the sample
    float2 r    = -off;                           // p relative to the sample
    float  s    = dot(r, back);                   // distance along the trail
    float  lat  = abs(r.x * back.y - r.y * back.x);
    along = saturate(s / len);
    return saturate(len - s + 0.5) * saturate(s + 0.5) * saturate(1.5 - lat);
}
float InFront(float zP, float zY)         { return saturate(1.0 + (zP - zY) / (RelSoftZ * zP)); }
float StrictlyInFront(float zP, float zY) { return saturate((zP - zY) / (RelSoftZ * zP)); }
float SameMotion(float2 a, float2 b)
{
    float tol = SameMotionTol * max(length(a), length(b)) + 0.5;
    return saturate(1.0 - length(a - b) / tol);
}

float4 GatherLayer(float2 uv, float2 dir, float2 vP, float zP, bool foregroundOnly)
{
    float4 sum = 0;
    [unroll] for (int i = 0; i < N; ++i)
    {
        float  t   = (i + 0.5) / N;                       // uniform: no noise, no dither
        float2 off = dir * t;                             // pixels ahead of p
        float2 uvY = uv + off * InvScreenSize;
        float4 pv  = PackedVel.SampleLevel(PointClamp, uvY, 0);
        float  along;
        float  cover = TrailCover(off, pv.xy, along);
        float  layer = foregroundOnly
                     ? StrictlyInFront(zP, pv.z) * (1.0 - SameMotion(vP, pv.xy))
                     : max(InFront(zP, pv.z), SameMotion(vP, pv.xy));
        float  w = cover * layer * (1.0 - TrailFalloff * along);
        sum += float4(SceneColor.SampleLevel(LinearClamp, uvY, 0).rgb, 1.0) * w;
    }
    return sum;   // rgb = weighted color, w = total weight
}

float4 MotionBlurTrailPS(float4 svpos : SV_Position) : SV_Target
{
    float2 uv  = svpos.xy * InvScreenSize;
    float3 cP  = SceneColor.SampleLevel(PointClamp, uv, 0).rgb;
    float4 pvP = PackedVel.SampleLevel(PointClamp, uv, 0);
    float2 vP  = pvP.xy;
    float  zP  = pvP.z;
    float2 vN  = NeighborMax.Load(int3(int2(svpos.xy) / TileSize, 0)).xy;

    if (dot(vN, vN) < 0.25 && dot(vP, vP) < 0.25)
        return float4(cP, 0);                                  // nothing moves near p

    const float norm = N * (1.0 - 0.5 * TrailFalloff);         // weight of a fully covered pixel

    // Layer A: p's own surface, one-sided along its own trail. p's color fills the unclaimed weight,
    // so the leading edge stays opaque and sharp.
    float4 a    = GatherLayer(uv, vP, vP, zP, false);
    float3 colA = (a.rgb + max(norm - a.w, 0.0) * cP) / max(norm, a.w);

    // Layer B: closer objects' trails crossing p (found through the one-sided neighbor max).
    float  alphaB = 0;
    float3 premB  = 0;
    if (length(vN - vP) > 0.5 + SameMotionTol * length(vN))
    {
        float4 b = GatherLayer(uv, vN, vP, zP, true);
        alphaB = saturate(b.w / norm);
        premB  = b.rgb / max(norm, b.w);
    }
    return float4(premB + (1.0 - alphaB) * colA, alphaB);     // alpha = pass-2 direction hint
}
```

**Samplers.** Color is sampled bilinearly, to avoid nearest-texel aliasing (Sousa 2013). Velocity and
depth are point sampled: bilinear would invent velocities and depths between layers (Jimenez).

**What each case does:**

| case | result |
|---|---|
| white sphere moving right, static camera | Layer B paints a trail on the background to its *left* only. The sphere's interior texture is blurred along +v (Layer A). Its right (leading) edge stays sharp. Pixels ahead of it are untouched. |
| camera pan, near pillar (w = 1) over far wall (w = 0) | The wall stays sharp. The pillar's trail crosses the wall (B). The pillar's own leading edge does not pull in wall color (the wall is farther and moves differently). |
| static barrel on a moving road (driving) | Road trails are rejected over the barrel (farther, different motion). The barrel gets its own w(z)-scaled camera blur. |
| pure camera rotation in the near zone | Everything has the same motion (SameMotion = 1), so the result is a uniform directional blur across depth edges, which is physically right for rotation. |

### 7.3 Sample count, tile size and trail length

- **Default: N = 8 per layer, `MaxTrailPx` = tile = 32 px.** The pass-1 stride is at most 4 px, which
  pass 2 closes.
- McGuire's k/S of about constant (20/15 at 720p) suggests about 15 taps for a 30 px blur *without* a
  second pass. With pass 2, 8 x 8 = 64 virtual taps.
- **Worst-case taps per pixel:** 8 (A) + 8 (B), with 2 fetches each. B runs only near differently
  moving objects.

### 7.4 Pitfalls

- **Slanted surfaces vs the barrel.** Both `RelSoftZ` and `SameMotionTol` trade under-blurring
  slanted receding surfaces against background-over-foreground leaks between nearly coplanar objects.
  - Forward motion is benign: the sources of a pixel's trail lie outward from the focus of expansion,
    which is nearer, so `InFront` accepts them.
  - Tune with the stress scene in section 12.
- **Off-screen sources** (objects entering from the border) have no trail sources. McGuire notes a
  width-k guard band is technically required. We accept the loss.
- **Particles and transparents** have no velocity. They sit in the color buffer at the background's
  velocity and depth, so they smear with the background. That is mostly fine for soft smoke, but
  wrong for sharp sprites in front of fast backgrounds. McGuire's "velocity particles" (draws that
  write only velocity) are the extension.
- **Thin features** (poles, wires) work. Layer A always keeps p's own color for unclaimed weight, and
  that center-weight problem is exactly what Guertin fixes in McGuire's filter.
- **Tile-border seams in B.** These happen when two different objects dominate adjacent tiles. The
  A/B options are point vs bilinear neighbor-max lookup (Fox) and no jitter vs a CryEngine-style
  border jitter. Jitter is noise, so it is off by default.
- **Input range.** In our placement the input is the tonemapped AA output (note 07 `aaOut`, an sRGB
  SRV, so the math is in linear light but values are at most 1). Bright small highlights streak
  dimmer than in HDR (Crysis 2008: "LDR: no bright streaks"). See Q1.

---

## 8. Motion blur pass 2: the "separable" pass

### 8.1 What "separable" means in this lineage

It is **not** horizontal-plus-vertical. It is the same directional filter **iterated** on its own
output (Crysis 2008: "ping-pong results ... 8 x 8 samples (virtually 64)"). Each tap of pass 2
contains pass 1's N taps, so the effective kernel is the convolution of the two.

| variant | pass-2 span | combined kernel | used by |
|---|---|---|---|
| **Stride-filling** | one pass-1 stride (`L/N`), centered | N^2 *uniform* taps over the same length L. This is exact for uniform motion. | UE `r.MotionBlurSeparable` (scale 1/N), **ours by default** |
| Full-length re-blur | L | a tent of length 2L (box ⊗ box) with N^2 taps. Smoother, but the trail doubles and the weight moves away from the origin. | Crysis 2008 camera MB (iterate 2-4x), probably Crytek 2013 and Fox (the same tap count in both passes) |

For a *trailing* blur, the full-length variant shifts the trail's weight away from the object's
origin. The tent peaks at L behind it, which undoes the "clear point of origin". So the default is
stride-filling, and the "aggressive" knob is `Pass2Span` between 1/N and 2/N (overlapping strides,
which also smooths residual banding left by the weights). The math, with pass-1 offsets
`(i + 0.5)/N` and pass-2 offsets `((j + 0.5)/M - 0.5)/N`:

```
(i + 0.5)/N + ((j + 0.5)/M - 0.5)/N = (M*i + j + 0.5)/(M*N),   i < N, j < M
```

These are exactly the M·N uniform midpoints over [0, 1]·L.

### 8.2 HLSL

```hlsl
// MotionBlurSmoothPS.hlsl  ps_5_0, fullscreen, input MbTemp, output MbOut
Texture2D<float4> Pass1Color  : register(t0);   // rgb, a = foreground-trail alpha from pass 1
Texture2D<float4> PackedVel   : register(t1);
Texture2D<float4> NeighborMax : register(t2);
// samplers and MotionBlurCB / InFront / SameMotion as in pass 1
#define M 8

float4 MotionBlurSmoothPS(float4 svpos : SV_Position) : SV_Target
{
    float2 uv  = svpos.xy * InvScreenSize;
    float4 c1  = Pass1Color.SampleLevel(PointClamp, uv, 0);
    float4 pvP = PackedVel.SampleLevel(PointClamp, uv, 0);
    float2 vN  = NeighborMax.Load(int3(int2(svpos.xy) / TileSize, 0)).xy;
    float2 dir = (c1.a > 0.01) ? vN : pvP.xy;          // the direction pass 1 stepped along at p
    if (dot(dir, dir) < 0.25) return float4(c1.rgb, 1);

    float2 span = dir * Pass2Span;                     // 1/N = exactly one pass-1 stride
    float3 sum  = c1.rgb;
    float  wsum = 1.0;
    [unroll] for (int j = 0; j < M; ++j)
    {
        float2 off = span * ((j + 0.5) / M - 0.5);     // centered on p
        float2 uvY = uv + off * InvScreenSize;
        float4 pv  = PackedVel.SampleLevel(PointClamp, uvY, 0);
        float  w   = max(InFront(pvP.z, pv.z), SameMotion(pvP.xy, pv.xy));  // stay on p's layer or accept foreground
        sum  += Pass1Color.SampleLevel(LinearClamp, uvY, 0).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}
```

- **Cheap mode (A/B).** Drop the velocity fetch and the weights, leaving a pure color box over
  `L/N`. It leaks at most `L/(2N)` = 2 px across depth edges and halves the fetches.
- **Leading edge.** Pass 2 cannot soften the sharp leading edge. Samples ahead of an object's front
  are farther and move differently, so they get weight 0.

### 8.3 Published costs for comparison

- Crytek 2013: 6 + 6 taps cost 0.236 + 0.236 ms at 1080p on an HD 7970.
- Fox: per the video, the separable pass costs about the same as the first pass, and about 40% more
  than UE's second pass.
- Ours: **estimate** 0.1-0.2 ms for M = 8 at 1080p on the RTX 5060 (16 fetches per pixel).

---

## 9. Sampling pattern policy

- **Default: uniform taps at stratum midpoints, no noise**, plus pass 2. This is the video's
  preferred direction ("uniform sampling to prevent any chance of noise") and Fox's approach.
- Jimenez's take-away supports it for small gaps: dither is a "finisher", and noise is for heavy
  undersampling. Pass 2 is a better finisher than noise because it fills the gaps instead of hiding
  them.
- **A/B patterns** add a per-pixel offset `j` to `t = (i + 0.5 + j)/N`:
  - checkerboard `±0.25` (Jimenez, CryEngine 5, early UE4);
  - IGN `frac(52.9829189*frac(dot(pos, float2(0.06711056, 0.00583715))))-0.5` (UE4.15+);
  - white-noise hash (NFS-like).
  - Each is tested with and without pass 2.

---

## 10. Placement, bloom interaction, ordering

- The video's chart order is: transparents/VFX, then tonemap, then AA (SMAA S2x per MSAA sample layer,
  plus two-frame temporal), then **motion blur**, then **bloom**, then composite effects and UI.
  - Note 07 has `aaOut` "consumed by DOF/motion blur/UI".
  - SMAA S2x needs the per-sample tonemapped layers, so motion blur cannot run before AA without
    breaking S2x.
  - **So motion blur runs on `aaOut`.** This is also the Fox placement (MB in LDR after DOF and before
    grading).
- **Bloom reacts to motion blur:** the bloom downsample chain reads `MbOut`, not the pre-blur image.
  - If bloom uses a Fox-style "HDR luminance in alpha" signal, pass 1 and pass 2 must blur that
    channel too. It is one more channel in the same fetches. Today our pass 1 uses alpha as the
    direction hint, so move the hint to a second RT if needed.
- **DOF before or after MB** is not settled. Sousa 2013 prefers DOF before MB. Guertin 2014 found
  DOF after MB slightly better. The video (1:04) wants DOF after AA. Keep MB as one block that can be
  moved.
- **The motion-blur output is never cleared**, because every pass writes all pixels (video, 1:44).

---

## 11. Half-resolution min/max-depth VFX (The Phantom Pain style)

### 11.1 Sources and positioning

- **Cantlay (GPU Gems 3, ch. 23, 2007), "High-Speed, Off-Screen Particles".**
  - Render particles into a low-res off-screen target cleared to black. Accumulate (1 - α)
    multiplicatively in alpha with separate alpha blending (SrcBlendAlpha = ZERO, DestBlendAlpha =
    INV_SRC_ALPHA). Compose back.
  - Point-sampled depth gives halos. **Max depth** shrinks silhouettes and reduces them ("absolutely
    no physical or theoretical basis").
  - Mixed resolution: a Sobel edge detection of the off-screen result plus stencil, with edges
    re-rendered at full res.
  - Numbers at 1600x1200 on a GeForce 8800 GTX (73-instruction shader):

    | mode | FPS | particle pixels |
    |---|---|---|
    | full res | 25 | 46.9 M |
    | 2x2 low res, max-Z | 46 | 11.7 M |
    | 2x2 mixed res | 44 | |
    | 4x4 low res | 61 | 2.9 M |

    The benefit shrinks with cheaper shaders and smaller coverage: at 3.5 M pixels it was 64.4 vs
    63.3 FPS.
- **Jansen and Bavoil (NVIDIA, 2011), "Fast rendering of opacity-mapped particles using DirectX 11
  tessellation and mixed resolutions".**
  - Downsample the hardware depth in a PS with explicit `Load` ("fine control over the choice of
    sample" with MSAA), or soft-particle tests against a low-res depth texture. "Plain hardware point
    filtering works well with our upsampling filter."
  - Low-res alpha = Π(1 - α_i).
  - **Nearest-depth upsampling.** Gather the 2x2 low-res depths (one `GatherRed`) and take the color
    of the texel whose depth is nearest to the full-res depth. If all four are within a threshold,
    use bilinear instead.
  - Clamp the low-res depths to the particle system's depth range to avoid false edges.
  - Shipped in Batman: Arkham Asylum (quarter or half res per particle system).
- **Checkerboard min/max downsample (UE4.26+ `CBMINMAX`, used for its half/quarter-res volumetric
  clouds).**
  - Each low-res texel takes the min or max of its 2x2 block, alternating in a checkerboard.
  - Every 2x2 footprint used by the upsampler therefore contains *both* a foreground and a background
    depth, so nearest-depth finds a matching texel on both sides of a silhouette.
  - This is our reading of the video's "half-resolution min-max depth ... without common low-resolution
    depth discontinuities". The Fox shader is not public, so the exact Fox method is unverified (Q5).

### 11.2 Depth downsample (fullscreen PS at 960x540)

```hlsl
// HalfDepthDownsamplePS.hlsl  ps_5_0; RT0 = HalfLinZ (R32F), DSV = HalfDepth (D32)
// Depth state: DepthEnable TRUE, DepthWriteMask ALL, DepthFunc ALWAYS. No color clear needed:
// every texel is written, and writing SV_Depth is fine for a fullscreen pass.
Texture2DMS<float, 2> DepthMS : register(t0);
#define REVERSED_Z 0

struct DSOut { float linZ : SV_Target0; float depth : SV_Depth; };

DSOut HalfDepthDownsamplePS(float4 svpos : SV_Position)
{
    int2  h = int2(svpos.xy);
    int2  f = h * 2;
    float dLo = 1e30, dHi = -1e30;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    [unroll] for (int s = 0; s < 2; ++s)
    {
        float d = DepthMS.Load(f + int2(x, y), s);
        dLo = min(dLo, d);
        dHi = max(dHi, d);
    }
    float closest  = REVERSED_Z ? dHi : dLo;
    float farthest = REVERSED_Z ? dLo : dHi;
    bool  odd = ((h.x + h.y) & 1) != 0;          // checkerboard: half the texels keep the background
    DSOut o;
    o.depth = odd ? farthest : closest;
    o.linZ  = LinearDepth(o.depth);
    return o;
}
```

It reads 8 samples per output texel (the whole 16 MiB MSAA depth once): **estimate** about 0.03 ms.

### 11.3 Rendering the half-res VFX

- **Target.** Viewport 960x540. `HalfVfx` (RGBA16F) is cleared to **(0,0,0,1)**: no radiance, and
  transmittance 1. That clear and the depth rewrite are the "two half-resolution buffers" the video
  says must be refreshed between the native and half-res VFX.
- **Depth.** `HalfDepth` is bound as the DSV with `DepthFunc = LESS_EQUAL` (GREATER_EQUAL for reversed
  Z) and `DepthWriteMask = ZERO`. Soft particles read `HalfLinZ` (a separate texture, so there is no
  read-only DSV binding trick). Cantlay's fade is
  `zFade = saturate(scale * (sceneZ - particleZ))`.
- **Premultiplied alpha in one buffer.** The particle PS outputs `(rgb·α, α)` for blended particles
  and `(rgb·intensity, 0)` for additive ones. This answers Cantlay's doubt about mixing additive and
  blended particles in one buffer.

  ```cpp
  // half-res VFX accumulation blend (RT0)
  rt.BlendEnable = TRUE;
  rt.SrcBlend      = D3D11_BLEND_ONE;   rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp      = D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D11_BLEND_ZERO;  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  // dst.rgb = premultiplied radiance over everything behind (in this buffer); dst.a = transmittance T = Π(1 - α)
  ```

- **Which effects go here.** An artist flag per emitter or material, `HalfRes` (default on for smoke,
  dust, fog sprites, fire, large soft glows). Fine particles stay native: sparks, debris, rain
  streaks, thin trails, anything with sub-2-px detail.

### 11.4 Composite (nearest-depth upsample) into the 2x MSAA HDR target

```hlsl
// HalfVfxCompositePS.hlsl  ps_5_0, fullscreen at 1920x1080 into the MSAA HDR RT (pixel frequency)
Texture2D<float4> HalfVfx  : register(t0);    // rgb premultiplied, a = transmittance
Texture2D<float>  HalfLinZ : register(t1);
Texture2DMS<float, 2> DepthMS : register(t2);
SamplerState PointClamp : register(s0);
SamplerState LinearClamp : register(s1);
cbuffer CompositeCB : register(b0) {
    float2 InvFullSize; int2 HalfSize;
    float EdgeRelThreshold;   // 0.1 = 10% of depth
    float Responsiveness;     // responsive-mask strength for half-res VFX (note 07: responsiveness * coverage)
};

float4 HalfVfxCompositePS(float4 svpos : SV_Position) : SV_Target
{
    int2   p     = int2(svpos.xy);
    float  zFull = LinearDepth(DepthMS.Load(p, 0));        // A/B: per-sample at sample frequency
    float2 uv    = svpos.xy * InvFullSize;                 // the same uv addresses the half-res texture

    // 2x2 half-res footprint. Gather order (D3D): x=(0,1) y=(1,1) z=(1,0) w=(0,0) relative to the
    // top-left texel. Verify once with a test texture.
    float4 zH = HalfLinZ.GatherRed(PointClamp, uv);
    float4 dz = abs(zH - zFull);
    float4 vfx;
    [branch] if (all(dz < EdgeRelThreshold * zFull))
    {
        vfx = HalfVfx.SampleLevel(LinearClamp, uv, 0);    // smooth region: bilinear
    }
    else                                                  // depth edge: nearest-depth texel
    {
        int2 base = int2(floor(uv * HalfSize - 0.5));
        int2 o = int2(0, 1); float best = dz.x;
        if (dz.y < best) { best = dz.y; o = int2(1, 1); }
        if (dz.z < best) { best = dz.z; o = int2(1, 0); }
        if (dz.w < best) { best = dz.w; o = int2(0, 0); }
        vfx = HalfVfx.Load(int3(clamp(base + o, int2(0, 0), HalfSize - 1), 0));
    }
    float coverage = 1.0 - vfx.a;
    if (coverage < 1.0 / 1024.0 && all(vfx.rgb < 1e-4)) discard;  // skip the blend RMW where nothing was drawn
    return float4(vfx.rgb, Responsiveness * coverage);
}
```

```cpp
// composite blend: dst.rgb = vfx.rgb + dst.rgb * T  (shader alpha = 1 - T); dst.a = max(mask, dst.a)
rt.BlendEnable = TRUE;
rt.SrcBlend      = D3D11_BLEND_ONE; rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp      = D3D11_BLEND_OP_ADD;
rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ONE;           rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
// depth test off
```

- **The alpha problem.** The color blend needs src.a = coverage, but the color equation wants T. The
  `INV_SRC_ALPHA` form solves both at once, because `dst·(1 - coverage) = dst·T`.
- **Responsive mask.** The alpha `MAX` writes the responsive mask (note 07, section 9) by recycling
  the VFX transmittance. This is the video's "just recycle the blend mask in their alpha channels".
- **Composite frequency.** It runs at pixel frequency into the 2x MSAA target, so both samples get the
  same VFX color, blended against their own destinations.
  - A/B: sample frequency (`SV_SampleIndex` input, using that sample's depth) doubles the invocations
    of a cheap shader.
- **Ordering.** Half-res VFX composite *after* native transparents (video order). Effects that must
  sort against native glass need the native path.

### 11.5 Costs and expectations

- **PS invocations scale by 1/4.** A full-screen sprite at 1080p is 2.07 M invocations; at half res it
  is 0.52 M. Ten layers of smoke overdraw is 20.7 M vs 5.2 M.
- **Fixed overhead:** the downsample (0.52 M cheap invocations), the clear, and the composite
  (2.07 M cheap invocations).
  - The blend read-modify-write is up to 33 MiB on 2x MSAA RGBA16F. The `discard` skips empty areas.
  - **Estimate** 0.05-0.15 ms in total.
- **Break-even.** Cantlay's tables show the gain vanishes for small, cheap effects. At 3.5 M pixels
  with a 41-instruction shader, mixed resolution was *slower* (63.3 vs 64.4 FPS). So keep the per-
  emitter flag, and skip the whole half-res path when no half-res emitter is visible.

### 11.6 Pitfalls

- **Odd sizes.** 1080/2 and 1920/2 are exact. For other resolutions, use `ceil` and clamp the loads.
- **Depth conventions.** Min and max swap under reversed Z, and the linearization differs. Use one
  helper shared with section 5.
- **Halos remain** where a 2x2 block contains three or more depth layers (fences, foliage). Measure
  them against max-only and point downsampling. Edge re-rendering at full res (Cantlay's stencil
  approach) is the escalation path.
- **Soft-particle fade** at half res uses the checkerboard depth, which alternates near edges. Do the
  fade with `HalfLinZ` *point* sampled at the particle's own texel. That is consistent with what the
  depth test used.
- **Reuse.** The same half-res depth and an RGBA16F are what a half-res SSS path would need (video).
  Allocate them once.

---

## 12. Composite effects (half res)

- **Flares and light-source glows.** `HalfFlare` RGBA16F is cleared to 0. It uses additive blending
  (`ONE, ONE`).
  - Glow sprites at light positions depth-test against `HalfDepth` (read-only, LESS_EQUAL).
  - Screen-space ghosts get an occlusion factor from a few `HalfLinZ` taps around the light's
    projected position (computed in the VS, so it costs nothing per pixel).
- **Rain and lens drops.** `HalfDrops` RGBA16F: rg = refraction offset, b = unused or tint, a = drop
  mask. In the final composite pass:
  `color = lerp(color, BloomTex.SampleLevel(LinearClamp, uv + drops.rg, 0).rgb * DropTint, drops.a)`.
  The drops then "absorb the local light" from the blurred bloom image, as the video describes.
- **Final composite.** One fullscreen pass does bloom, flares, drops, grain, grading and CA, reading
  the half-res buffers bilinearly. Following the video, the composite buffers are never full res
  (Crysis 3 "chokes").
- **Measure.** PS invocations and ms for the same flare set rendered into a full-res vs a half-res
  buffer.

---

## 13. Cost summary (1080p, RTX 5060)

All of these are **estimates** from bandwidth and fetch counts (448 GB/s, about 150-300 GTexel/s);
section 14 measures them.

| pass | traffic | estimate |
|---|---|---|
| base-pass MV + bits export | about 20 MiB writes | about 0.05 ms |
| velocity pack + tile max CS | reads 16 + 8 + 4 MiB, writes 16 MiB | 0.10-0.2 ms |
| neighbor max | 2040 px | < 0.01 ms |
| MB pass 1 | 16-32 fetches per pixel (B only near differently moving objects) | 0.15-0.45 ms |
| MB pass 2 | 16 fetches per pixel | 0.10-0.2 ms |
| half-res depth downsample | 16 MiB read | about 0.03 ms |
| half-res VFX composite | about 4 MiB read, at most 33 MiB RMW | 0.05-0.15 ms |

Published reference points:

- Jimenez: 0.52 ms (Xbox One).
- Sousa 2013: 0.47 ms (2 x 6 taps, 1080p, HD 7970).
- Guertin: 1.96 ms (N = 19, 1080p, GTX 780).
- McGuire: 3 ms (720p, GTX 480).
- Crysis 2008: about 1 ms camera MB (1280x1024, G80).

---

## 14. Measurement tools and the A/B test plan

### 14.1 Tools

- **GPU time.** Wrap each pass in a `D3D11_QUERY_TIMESTAMP_DISJOINT` with `D3D11_QUERY_TIMESTAMP`
  begin and end pairs. Read back 2-3 frames later and average over 120 frames.
- **PS invocations.** Wrap the draws in `D3D11_QUERY_PIPELINE_STATISTICS` and read
  `D3D11_QUERY_DATA_PIPELINE_STATISTICS::PSInvocations`, also read back later. This is the video's
  metric for half-res VFX and composite buffers.
- **Ground-truth reference (debug mode).** Accumulation buffer, as in the Jimenez and McGuire ground
  truth.
  - Render the opaque scene K = 64 times with camera and rigid-object transforms interpolated at
    times `t - TrailTime * (k + 0.5)/K`, and average in HDR. This is the *trailing* exposure.
  - Do the same at `t + TrailTime * (0.5 - (k + 0.5)/K)` for the *centered* exposure (using the next
    simulated state). Exercise only these in the stress scenes.
  - Compare the post-process result with PSNR inside the moving regions, and with an "outline width"
    measure: pixels outside the reference's blurred support whose difference exceeds 2/255.
  - This is camera ground truth, not eye perception. It validates occlusion and coverage logic, not
    the aesthetic choice.
- **Stress scenes** (scripted camera and objects, deterministic):
  - S1 white sphere and capsule "character" moving horizontally over a textured wall, static camera.
  - S2 the same with a vertical camera pan (dual-motion tiles).
  - S3 a thin bright vertical line and 1-px point lights moving at 10/30/60 px per frame over black.
  - S4 a driving camera over a textured road with a static barrel and a pillar (NFS barrel case,
    slanted surfaces).
  - S5 fast camera yaw (120-400°/s) with near, mid and far content (U-shape).
  - S6 a camera-tracked car at 6 m (tracked-flag pitfall).
  - S7 smoke wall with 8x overdraw behind and in front of a fence and a character (half-res VFX).
  - S8 a flare-heavy night street.

### 14.2 A/B experiments

| id | toggle | scenes | metric | expected (claim being tested) |
|---|---|---|---|---|
| MB1 | neighbor max: off (own velocity only, NFS-like) / one-sided 3x3 (ours) / symmetric 3x3 (McGuire) / UE-like ±3-tile dilation + fast path | S1, S2 | outline width vs reference, PSNR, ms | Ours removes the thick outline (video 1:46). "Off" gives sharp silhouettes over static backgrounds. The UE-like option reproduces the artifact. |
| MB2 | layer B on / off | S2 | blocky sharp halos around the character | B is required when camera and object motion differ. |
| MB3 | trailing (ours) vs centered (`t` in (-0.5, 0.5), symmetric cover test) | S1, S3 | position of the streak's sharp origin vs the object's true position; side-by-side video for viewers | The video's 1:49-1:51 claim (trailing reads like eye perception). Use a small blind-preference test with 5+ viewers. |
| MB4 | `TrailFalloff` 0 / 0.5 / 1 | S3 | streak luminance profile; preference | "Clearer point of origin" at 0.5-1 |
| MB5 | sampling: uniform / checkerboard / IGN / white noise, each × pass 2 on/off | S3, S5 | ripple metric (total variation of the 1-D luminance profile along the streak minus that of the ideal ramp), temporal flicker (mean abs frame-to-frame difference in steadily moving regions), ms | Uniform + pass 2 has the lowest flicker and comparable ripple, at no noise (video 1:53-1:56). |
| MB6 | N in {3, 4, 6, 8, 12} × M in {0, 4, 8}; `Pass2Span` 1/N, 1.5/N, 2/N, 1 | S3, S5 | ripple, ms | The smallest N·M with no visible stepping. Fox's "3 per direction + separable" is the floor. |
| MB7 | depth-limited camera blur: w = 1 (classic) / near-only cutoff / U-shape; tile-mask variant | S5 | preference; visual cutoff seams | The video's 1:47-1:49 claim |
| MB8 | `CAMERA_TRACKED` on / off | S6 | car sharpness | The pitfall in 5.3 is real |
| MB9 | `RelSoftZ` 0.02/0.05/0.1 × `SameMotionTol` 0.02/0.05/0.15 | S4 | barrel leak (difference on barrel pixels vs reference), road under-blur | Find the stable middle |
| MB10 | packed format: RGBA16F (default) vs R11G11B10 polar (UE) vs half-res RGBA8 (Fox) | S1-S5 | ms, visual difference | Bandwidth vs precision |
| MB11 | MB input: `aaOut` (LDR) vs an HDR resolve before tonemap (breaks S2x, for screenshots only) | S3 bright lights | streak brightness, bloom reaction | Quantifies Q1 |
| VFX1 | VFX resolution: full / half | S7 | PSInvocations of VFX draws, ms of VFX + downsample + composite, image difference in non-edge areas | About 75% fewer PS invocations; net ms gain grows with overdraw |
| VFX2 | depth downsample: point / max / min / checkerboard min/max; upsample bilinear / nearest-depth / nearest+bilinear | S7 | halo pixel count at fence and character edges vs full-res | Checkerboard + nearest+bilinear gives the fewest halos (the video's "without low-res depth discontinuities") |
| VFX3 | composite at pixel vs sample frequency | S7 | edge quality, ms | Pixel frequency is probably enough |
| CMP1 | flare/glow buffer full vs half res | S8 | PSInvocations, ms | The video's "cut by the millions" (1:58) |
| MV1 | base pass with MV + bits RTs bound vs not; skinned characters with previous-palette skinning on/off | any | base-pass ms | "Near free" (1:35) |

---

## 15. Open questions

- **Q1. Motion blur input range.**
  - Per-sample SMAA S2x (note 07) forces MB after tonemap and AA, so bright highlights streak in LDR.
  - Options:
    - (a) accept it, since bloom after MB restores some glow;
    - (b) carry an HDR luminance or "highlight" channel through AA and blur it alongside;
    - (c) move MB before tonemap and give up S2x in moving regions.
  - Decide together with the tonemap and bloom notes, using MB11.
- **Q2. Pass-2 span.** Is stride-filling (1/N) "aggressive" enough, or does the video's "competent,
  aggressive separable pass" mean the full-length Crytek tent (which lengthens and shifts the trail)?
  MB6 decides.
- **Q3. Leading edge.** Should a moving object's leading edge be fully sharp (our design, eye model),
  or slightly transparent (the physical trailing shutter)? A small inner blur could be reintroduced
  using the neighboring background ahead of the object, which is visible, unlike in the centered
  case.
- **Q4. `TrailTime` calibration** (velocity compensation). Run the mocap-glove experiment from the
  video. Also check whether a single constant works across 60/120/240 Hz displays. The physics
  suggests trail time = display persistence minus the eye's own integration, which is unknown.
- **Q5. Fox Engine details** are unverified, because only the video's decrypted-shader analysis exists:
  - the velocity encoding ("closer objects higher precision": possibly a gamma or sqrt length
    encoding like CryEngine's, rather than depth-dependent);
  - the exact min/max scheme for the half-res VFX depth;
  - whether the separable pass uses the same span.
- **Q6. U-shape.** Is analytic w(z) enough, or does the video's "low-resolution mask" add something,
  such as spatial smoothing of the cutoff or including the eye-fixation region? The constants
  (1.5/3/20/60 m, 2-6 rad/s) are first guesses.
- **Q7. Particles in motion blur.** Should VFX write a velocity, or a "don't blur me" bit through the
  half-res composite? (They are currently smeared with the background's motion.)
- **Q8. Half-res VFX vs native transparent sorting**, and whether a per-emitter automatic half-res
  decision (by screen coverage) beats an artist flag.

---

## Sources

- Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering" (2026-08-01),
  https://www.youtube.com/watch?v=SnNm7rSSvlg. Local transcript (minutes 0:08-0:09, 0:28, 1:02-1:04,
  1:34-1:39, 1:42-1:59). Provides all the positioning, the Days Gone/UE4.11, Fox Engine and NFS 2015
  observations, the red-coded tile max, the depth-limited camera blur, trailing, uniform + separable,
  bloom after MB, half-res min/max VFX, the responsive mask and the half-res composite buffers.
- M. McGuire, P. Hennessy, M. Bukowski, B. Osman, "A Reconstruction Filter for Plausible Motion Blur",
  I3D 2012, https://casual-effects.com/research/McGuire2012Blur/McGuire12Blur.pdf. Provides TileMax and
  NeighborMax, Listing 1 (cone/cylinder/softDepthCompare), the V encoding, k = 20 and S = 15, the
  timings (GTX 480 and Xbox 360), the Valve 15%/4% recommendation, the camera-motion damping advice,
  and the CryENGINE 3 summary (four 1-D dilation passes, 9/24 taps).
- J.-P. Guertin, M. McGuire, D. Nowrouzezahrai, "A Fast and Stable Feature-Aware Motion Blur Filter",
  HPG 2014, https://casual-effects.com/research/Guertin2014MotionBlur/Guertin2014MotionBlur-small.pdf
  (NVIDIA tech report NVR-2013-003:
  https://research.nvidia.com/publication/2013-11_fast-and-stable-feature-aware-motion-blur-filter).
  Provides the two-direction sampling, direction-aware weights, the diagonal neighbor test, the center
  weight, the relative z compare, the encoding fix, the GTX 780 timing table, the DOF/MB order, and
  the "re-blur in the direction of motion (as in [Sou08])" remark.
- J. Jimenez, "Next-Generation Post-Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014
  Advances, https://www.iryoku.com/downloads/Next-Generation-Post-Processing-in-Call-of-Duty-Advanced-Warfare-v18.pptx
  (slides and notes). Provides the scatter-as-gather problem statement, 20x20 separable tile max plus
  3x3 neighborhood, DepthCmp/SpreadCmp/SampleWeight, the center-as-background correction, mirrored
  reconstruction, the uniform/random/checkerboard patterns and take-away, IGN (for DOF), min/max tile
  fast paths, R11G11B10 velocity+depth packing, and 0.52 ms on Xbox One.
- T. Sousa, "Crysis Next-Gen Effects", GDC 2008,
  http://web.archive.org/web/20140721041406/http://www.crytek.com/download/GDC08_SousaT_CrysisEffects.ppt.
  Provides the camera MB with near-depth velocity mask, the **iterative ping-pong 8 → 64 → 512
  samples**, about 1 ms at 1280x1024 on G80, and the DX10 object MB (FP16 velocity/depth/ID, 8x-smaller
  mask, 4-pass dilation, velocity clamping, the bone-constant limit).
- T. Sousa, "CryENGINE 3 Rendering Techniques", Microsoft Gamefest 2011,
  http://web.archive.org/web/20140721044344/http://www.crytek.com/download/Graphics_CryENGINE_3_Rendering_Techniques_final.ppt.
  Provides Crysis 2's reprojection + velocity buffer, half-res MB (full res at least 3 ms), on-the-fly
  dilation, HDR before tonemap, and 9 (console) / 24 (PC) taps.
- T. Sousa, "Graphics Gems from CryENGINE 3", SIGGRAPH 2013 Advances,
  https://advances.realtimerendering.com/s2013/Sousa_Graphics_Gems_CryENGINE3.pptx. Provides the 2-pass
  separable MB (6 + 6 taps, 36 accumulated, 0.236 + 0.236 ms at 1080p on an HD 7970), the vectorized
  inner loop, the R8G8 gamma-2 velocity, velocity only when required, and the DOF-before-MB argument.
- CryEngine 5 source (`Engine/Shaders/HWScripts/CryFX/MotionBlur.cfx`,
  `Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/MotionBlur.cpp`), read via the GitHub mirrors
  https://github.com/Pterosoft/-CRYENGINE-Community-Edition- and https://github.com/MergHQ/CRYENGINE.
  Provides PackVelocities (depth reprojection, object override, exposure/frame-time scale, gamma-2
  length, RGBA8), separable tile generation, the diagonal-aware neighborhood, 6/14/24-tap checkerboard
  centered gather, border-randomized tile lookup, and the `ONE, INV_SRC_ALPHA` composite.
- Unreal Engine 4 (4.15-era) shaders and C++ (`PostProcessVelocityFlatten.usf`,
  `PostProcessMotionBlur.usf`, `PostProcessMotionBlur.cpp`, `PostProcessing.cpp`), read via the mirror
  https://github.com/JimmieKJ/unrealTournament (Epic EULA source; described, not reproduced). Provides
  the 11:11:10 polar velocity+depth flatten, 16x16 min/max tile CS (120x68 at 1080p), gather/scatter
  dilation, quality step counts, skip/fast paths, IGN vs the disabled checkerboard, and the
  `r.MotionBlurSeparable` second pass at 1/N scale.
- Unreal Engine `DownsampleDepthPixelShader.usf` (CBMINMAX), read via the mirror
  https://github.com/OpenHUTB/engine, and the `EDownsampleDepthFilter` docs
  (https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/EDownsampleDepthFilter).
  Provides the checkerboard min/max depth downsample.
- J. Jansen, L. Bavoil, "Fast rendering of opacity-mapped particles using DirectX 11 tessellation and
  mixed resolutions", NVIDIA white paper, 2011,
  https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/OpacityMappingSDKWhitePaper.pdf.
  Provides the low-res depth via explicit Load (MSAA sample choice), point-filtered low-res depth,
  transmittance in alpha, and the nearest-depth upsampling with the bilinear fallback and depth
  threshold (code), plus Batman: Arkham Asylum.
- I. Cantlay, "High-Speed, Off-Screen Particles", GPU Gems 3, ch. 23, 2007,
  https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles.
  Provides the off-screen accumulation blend states (separate alpha), max-depth downsampling, soft
  particles, mixed-resolution edge fix-up, and the 8800 GTX performance tables.
- G. Rosado, "Motion Blur as a Post-Processing Effect", GPU Gems 3, ch. 27, 2007,
  https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect.
  Provides camera velocity from depth and the previous view-projection (and notes bugs in its listing).
- A. Courrèges, "Metal Gear Solid V - Graphics Study", 2017,
  https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/. Provides MGSV's dynamic plus
  static velocity composite, characters treated as static, the 8-bit velocity, MB based on MHBO 2012
  after DOF and before grading, transparents in LDR after tonemap, the half-res buffers, and dirt and
  flares composited additively from half res.
- Microsoft Learn, "Format support for Direct3D feature level 11.0 hardware",
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware.
  Provides that R16G16B16A16_FLOAT typed UAV, multisample load and gather4 are required on FL 11_0.
- Microsoft Learn, HLSL `Gather`,
  https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-gather. Provides the
  Gather API (SM 4.1+). The texel order used above follows the D3D gather4 definition (counter-clockwise
  from lower-left); verify with a test texture.
- Note 07 in this repo (`docs/research/07-antialiasing-smaa.md`). Provides the MV convention (RG16F, UV,
  cur - prev, jitter-free), the responsive-mask blend (alpha MAX), `aaOut` feeding motion blur, and the
  per-sample S2x constraint.
