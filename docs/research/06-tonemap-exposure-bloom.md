# R6 — Tone mapping, exposure, white balance, LUTs, bloom, final composite & UI gamma

Research note for the D3D11 educational engine (FL 11_0/11_1, SM 5.0, deferred, 2x MSAA, 1080p60 SDR,
RTX 5060 8 GB + Ryzen 9 5900X). Video: Threat Interactive, *"Tutorial: How To Optimize Almost Every Step
In Modern Game Rendering"* (2026-08-01), chapters 1:04:45 "Tone Mapping & Exposure Mechanics" and
1:57:29 "Motion Effects, Composite Effects, & UI/Gamma".

Every HLSL snippet in this note was compiled with `fxc` (Windows SDK 10.0.26100, `ps_5_0` / `cs_5_0`,
`/O3`) during research; the numerical claims marked *(sim)* come from a NumPy port of the operators
(GT7 port checked against Polyphony's reference C++). Nothing here is engine code yet.

---

## 0. Decisions at a glance

| Topic | Recommendation (v1) | Why / source |
|---|---|---|
| Render space | Linear Rec.709 primaries, RGBA16F lighting (2x MSAA) | Engine baseline. GT7 runs in Rec.2020; we convert in and out inside the tone mapper only |
| "Exposed" convention | After exposure, **0.18 = middle grey, 1.0 = diffuse white** | Lets every curve be compared at the same mid-grey |
| Tone curve | **GT7 tone mapping, SDR path**: exact port of Polyphony's MIT sample, used with input × 2.5 (250-nit paper white) | Video's choice (1:10–1:15). SIGGRAPH 2025 PBS course |
| A/B curves | GT (Uchimura 2017, with the toe on/off), Frostbite 2017 display mapper, MGSV/Fox, ACES (Hill fit), AgX, PBR Neutral, Reinhard, Hable. All pre-scaled so 0.18 → 0.18 | Video comparisons; GT7 slides 154–169 |
| LUT | Bake GT7 (+ optional grade) into a **64³ R16G16B16A16_FLOAT 3D LUT** with a **pow(4) shaper** (`x = 64·u⁴`). Store **sRGB-encoded** output. Bake with a CS only when parameters change. A/B against 32³ and against R10G10B10A2 | Callisto (64³ RGBA16, video capture); GT7 slide 173: pow(4) indexing. *(sim)*: the shaper matters far more than the resolution |
| Exposure + WB | Apply both as **one 3×3 "camera matrix"** per pixel before the LUT, so the LUT never needs a re-bake for exposure | Simplest. Keeps the LUT static |
| Auto exposure | **Incident-style metering**: 256-bin log2 histogram of a ¼-res copy of the **diffuse-only (albedo-free) lighting buffer**, emissive/sky/SSS masked out through alpha. Trimmed mean, exponential adaptation (to brighter 3/s, to darker 1/s), EV clamp, compensation. GPU-only, no readback | Fox Engine (GDC 2013 slide: auto exposure keyed to the lighting, not the materials; MGSV capture in video). GT7 adaptation model |
| Auto white balance | Same buffers. Luminance-weighted mean **light colour** → CCT (McCamy) → clamp [Tmin, Tmax] in mireds + bounded tint → Bradford von Kries to D65, partial strength, slow adaptation, artist bias | Video 1:16–1:18; GT7 slide 222 |
| Bloom | **v1 (B1):** Jimenez 2014 chain from the *un-exposed HDR* scene. Half-res, 6 mips, R11G11B10F, 13-tap downsample, 3×3 tent upsample, Karis weights on the first level. Mixed **energy-conservingly in the tone-map pass**. **v2 (B2):** an experiment with Fox-style post-motion-blur bloom | CoD:AW (SIGGRAPH 2014); the video wants bloom after motion blur, but calls it WIP (1:57) |
| Tone-map pass outputs | **Two non-MSAA R10G10B10A2_UNORM** targets (sample 0, sample 1), holding sRGB-encoded LDR. This is the "MSAA separation" the video's chart puts in this pass, ready for SMAA S2x | Video pipeline chart (box "DOF Blend, Physically Based Tone Mapping & MSAA Separation") |
| Final composite | One full-screen draw: chromatic aberration, half-res composite layer, (bloom B2), vignette, user gamma/brightness, grain + 8-bit TPDF dither, then **UI from its own premultiplied RGBA8 target in sRGB space**. Writes an R8G8B8A8_UNORM back buffer RTV with manual encode | Video 1:58–2:00 (Fox, NFS2015, Days Gone, Callisto) |

---

## 1. What the video proposes (for traceability)

* **SDR first.** Maximise "limited RGB SDR" before HDR/BT.2020 (1:05, 1:19).
* **Curve anatomy** (1:05–1:08): a linear mid-section ("conveys realism"), a **contrast toe**
  (darks fall to black faster; the video credits this to perception, the de Vries–Rose law), and a
  **smooth shoulder**. Reinhard looks washed out; ACES destroys detail and colour (the video's claim).
* **Fox Engine / MGSV** (1:07–1:08): lighting is resolved and tone-mapped early with a linear +
  smooth-shoulder curve that has no toe, then written to 8-bit. A grading LUT later acts like a
  "retrospective contrast toe": it lowers shadows and raises highlights.
* **Callisto Protocol** (1:08–1:10) ships the stand-alone GT curve with console variables. The video's
  capture shows `r.PhxGTTonemapper.LinearBegin` default **.001** and `LinearLength` **0.5**. The video
  changes these to **.022 / 0.4** and claims the cvar is scaled 10× relative to Uchimura's `m = 0.22`.
  So the default effectively removes the toe.
* **Hue shifts** (1:10–1:15): GT7 changes the *direction* of highlight hue shift. It stays more neutral:
  orange → yellow → white, never a washed-out orange. Frostbite's display mapping is more uniform but
  washes out vivid, properly exposed colours. The video wants bright orange to reach yellow by
  "increasing green first".
* **Auto exposure is very important** (1:16). The video cites GT7's interconnected system.
  **Auto white balance** with a min/max range and an artist temperature bias (1:16–1:17). Metering
  should use the **diffuse-only lighting buffer**, as Fox Engine does, so dark albedo and emissives don't
  affect it (1:17–1:18). The video also wants emissive and direct-light SSS excluded, possibly through the
  alpha channel of an RGBA16 lighting buffer plus an ID system.
* **LUT** (1:18–1:19): tone mapping goes through a small LUT. Callisto uses a **64³ RGBA16** 3D LUT
  (video capture: `R16G16B16A16_FLOAT 64x64x64`, one `DrawInstanced` over 64 slices through a GS,
  262k PS invocations, **61 µs**, GPU not stated). The chart notes Days Gone uses a **32³ RGB10A2** LUT
  and that **GT7's "power-of-4 nonlinearity anti-banding" should be implemented regardless**.
* **Pipeline chart, tone-map box:** "DOF Blend, Physically Based Tone Mapping & MSAA Separation". It
  separates the 2xMSAA RGBA16 into **two 10-bit tone-mapped resources (sample 0 and sample 1)**. First draw:
  LUT update. Second draw: merged compositing + tone mapping. Budget: 0.93 ms for The Callisto Protocol's stage
  (bloom cost excluded), with 0.25 ms(?) as the target.
  Crysis 3 is the model: it merges filmic functions, the bloom resolve and the half-res volumetric fog
  upscale into the tone-map pass (0:12). The chart estimates roughly 0.20 ms, plus about 0.05 ms for the
  second tone-mapped image.
* **Anti-aliasing after tone mapping** (SMAA S2x on the two LDR sample images, then temporal SMAA).
  DOF and motion blur also run after AA (1:04, "needs refinement").
* **Bloom** (1:57) is "WIP". It can be done post-tone-map if scene luminance is stored (Fox), but the
  video wants bloom that **reacts to the motion-blurred result**. The chart's "Downscaled Bloom
  Resource" box (CoD:AW method, **0.38 ms**) sits *after* motion blur and also produces the mip chain
  for stochastic SSR.
* **Composite** (1:58–1:59): half-res composite buffers (Fox, NFS2015; Crysis 3's full-res one "chokes").
  One full-res merged pass: film grain, vignetting, chromatic aberration, colour grading, bloom and
  half-res lens/rain upsample (**0.15 ms**). The camera effects sit underneath artist-composited effects.
* **UI** (2:00): the UI is drawn to its own render target and composited **with gamma correction in the
  same draw** (Days Gone). Per the chart, this compositing step is where Fox Engine applies gamma
  correction (**0.1 ms**). Callisto's brightness slider "scales" instead of clipping the test image (Dead Space
  Remake clips it). Alan Wake 2 and Callisto offer several display-gamma choices.

---

## 2. Conventions used below

* **Scene-linear**: lighting buffer values, linear Rec.709 primaries, engine units (ideally physical).
* **Exposed**: `exposed = CameraMatrix · sceneLinear`, where `CameraMatrix = exposure × whiteBalance`.
  Auto-exposure is defined so that **0.18 = middle grey** and **1.0 = diffuse white** under the average light.
* **GT frame-buffer units** (GT7 code): `1.0 = 100 cd/m²`. GT assumes an **SDR paper white of 250 nits**,
  so `gtInput = exposed × 2.5`, and the SDR output is divided by 2.5 again (`sdrCorrectionFactor`).
  *(sim)* With this convention GT7 maps exposed 0.18 → 0.179, almost the same as the original GT curve
  (table in §3.4). That confirms 2.5 is the intended scale.
* **Display-linear**: tone-mapper output, `[0,1]`, Rec.709, `1.0 = SDR white`.
* **sRGB-encoded**: `LinearToSRGB(display-linear)` (IEC 61966-2-1 piecewise). R10G10B10A2 has **no
  `_SRGB` view format**, so every pass that writes or reads it encodes or decodes manually.

Shared helpers (used by every snippet):

```hlsl
static const float3x3 M_709_TO_2020 = float3x3(            // BT.2087
    0.627403896, 0.329283038, 0.043313066,
    0.069097289, 0.919540395, 0.011362316,
    0.016391439, 0.088013308, 0.895595253);
static const float3x3 M_2020_TO_709 = float3x3(
     1.660491, -0.587641, -0.072850,
    -0.124550,  1.132900, -0.008349,
    -0.018151, -0.100579,  1.118730);

float3 LinearToSRGB(float3 c)
{
    c = saturate(c);
    return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 SRGBToLinear(float3 c)
{
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
```

---

## 3. Tone curves

### 3.1 GT tone mapping — Uchimura & Suzuki (CEDEC 2017; SIGGRAPH Asia 2018 course "Practical HDR and Wide Color Techniques in Gran Turismo SPORT")

**What and why.** A per-channel curve made of three sections joined smoothly: a power-function **toe**
(contrast adjustable), a **linear mid-section** that the GT artists considered the most important part
for photorealism, and an exponential **shoulder** that converges to the display peak `P`. Its
parameters are independent, unlike the polynomial filmic curves (Hable, Lottes VDR), so it is easy to
tune. `P` lets the same curve serve SDR and any HDR peak. GT Sport notes (p.118–123) give the design
philosophy; the curve itself is on Desmos.

**Formula** (per channel, x ≥ 0). Defaults: `P = 1` (max display brightness), `a = 1` (contrast),
`m = 0.22` (linear section start), `l = 0.4` (linear section length), `c = 1.33` (black / toe
tightness), `b = 0` (pedestal):

```
l0 = (P - m) * l / a
S0 = m + l0            S1 = m + a * l0
C2 = a * P / (P - S1)  CP = -C2 / P
w0 = 1 - smoothstep(0, m, x)        // toe weight
w2 = step(S0, x)                    // shoulder weight (hard switch, C1-continuous)
w1 = 1 - w0 - w2                    // linear weight
T  = m * pow(x / m, c) + b          // toe
L  = m + a * (x - m)                // linear
S  = P - (P - S1) * exp(CP * (x - S0))   // shoulder
y  = T*w0 + L*w1 + S*w2
```

```hlsl
float3 GTToneMap(float3 x, float P, float a, float m, float l, float c, float b)
{
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;
    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(S0, x);
    float3 w1 = 1.0 - w0 - w2;
    float3 T = m * pow(max(x, 0.0) / m, c) + b;          // toe
    float3 L = m + a * (x - m);                          // linear section
    float3 S = P - (P - S1) * exp(CP * (x - S0));        // shoulder
    return T * w0 + L * w1 + S * w2;
}
// default: GTToneMap(exposed, 1.0, 1.0, 0.22, 0.4, 1.33, 0.0)
```

**The "contrast toe" knob is `m`.** The toe occupies `[0, m]`, blending into the linear part through
`smoothstep(0, m, x)`, with exponent `c`. Setting `m → 0` removes the toe; that is Callisto's default
per the video's capture, `LinearBegin .001`. With the defaults the curve is linear (identity, since
`a = 1`) on `[0.22, 0.532]`. Then the shoulder takes over: *(sim)* 0.99 at x ≈ 2.35, i.e. **~3.7 stops
above 0.18**.

**Pitfalls.**
* The smoothstep blends make the exact inverse awkward. GT's own notes call this a mistake (p.125). Avoid
  any design that needs to invert the curve, such as inverse tone mapping for HDR video or the Fox-style
  "store HDR in alpha" trick.
* Per-channel → hue twist and desaturation in highlights. GT7 slides 136–137 show colours collapsing to
  cyan, magenta and yellow.
* The shoulder never reaches `P` exactly. GT7 fixed this with the "V2" curve (§3.2).

**Performance.** GT Sport on PS4 at 1080p: the analytic curve took **0.40 ms**, a 1D LUT (8192 × 32-bit,
square-root indexing over 0–50,000 nits) took **0.24 ms**, because ALU was the bottleneck (GTS notes p.184–201).

### 3.2 GT7 tone mapping — Suzuki, Yasutomi, Uchimura (SIGGRAPH 2025 PBS course; MIT sample code 2025-08-10)

**What and why.** GT Sport's per-channel curve twisted hues and over-desaturated highlights. With GT7's
real-time time-of-day and weather, sharp ray-traced highlights and vivid night lights, PDI wanted
highlights that do not twist hue but still keep a slight "camera-like" twist. **Answer: compute both
and blend.** Slides 139–150; the code is on slide 243 and in `gt7_tone_mapping.cpp` (MIT, Polyphony
Digital).

**Algorithm (exact, from the sample):**
1. **"Skewed" colour:** apply the *GT Tone Mapping V2* curve to each Rec.2020 channel.
   V2 = the GT curve with a convergent exponential shoulder and a hard clip at the peak (slide 147:
   the shoulder clips sharply, which PDI accepted because TVs tone-map as well).
2. Convert the **original** RGB and the skewed RGB to a **UCS**. The default is **ICtCp** (BT.2100 /
   ITU-T T.302, PQ-based, fast); **Jzazbz** is the alternative, with the PQ exponent scaled by 1.7.
3. **"Untwisted" colour** = UCS(luminance of the *skewed* colour, chroma of the *original* colour ×
   `chromaScale`), where `chromaScale = 1 − smoothstep(0.98, 1.16, I_original / I_peak)`. Chroma fades
   to white as the original intensity approaches and passes the peak.
4. **Blend in RGB:** `out = lerp(skewed, untwisted, 0.6)`, then `min(out, peak)`. PDI tried blending in
   the UCS and preferred RGB (slide 150).
5. **SDR:** peak = 250 nits (GT's SDR paper white). The output is multiplied by `1/2.5` so that 1.0 = SDR white.

**Constants** (`initializeAsSDR()` → `initializeCurve(2.5, 0.25, 0.538, 0.444, 1.28)`):

| Symbol | Value | Meaning |
|---|---|---|
| `peak` | 2.5 | 250 nits in GT units |
| `alpha` | 0.25 | shoulder shape (only enters `k`) |
| `midPoint` | 0.538 | end of the toe blend (smoothstep 0 → 0.538) |
| `linearSection` | 0.444 | shoulder starts at `0.444 × peak = 1.11` |
| `toeStrength` | 1.28 | toe exponent |
| `k` | (0.444−1)/(0.25−1) = 0.741333 | |
| `kA, kB, kC` | 2.963333, −3.373351, −0.539568 | `kA = P·l + P·k`, `kB = −P·k·e^{l/k}`, `kC = −1/(k·P)` |
| `blendRatio` | 0.6 | weight of the untwisted colour |
| `fadeStart, fadeEnd` | 0.98, 1.16 | chroma fade in units of `I_peak` |
| `I_peak` | 0.6025592 | ICtCp `I` of RGB (2.5, 2.5, 2.5) |

The shoulder `kA + kB·e^{kC·x}` is C1-continuous with the linear part at `x = l·P` (value `l·P`,
slope 1). It tends to `1.185·P`, so the `min(…, peak)` clips at **x = 1.47·P**. In exposed units, a
neutral colour reaches SDR white at **exposed ≈ 1.47**, i.e. **~3.0 stops above mid-grey**. That is
less headroom than GT (3.7), MGSV (4.2), Frostbite (4.3), ACES (5.7–6.5) or AgX (6.4) *(sim, §3.4)*.
The video's chart treats this as intentional: GT7 introduces clipping faster, which it argues mimics
naked-eye perception.

**HLSL port** (SDR, Rec.709 in/out; MIT © 2025 Polyphony Digital Inc. Keep the notice in the engine
file that contains this port):

```hlsl
// ---- ST 2084 (PQ) in GT units: 1.0 = 100 cd/m2 ----
static const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;
float3 PQEncode(float3 fb)                       // linear -> PQ signal
{
    float3 ym = pow(max(fb, 0.0) * 0.01, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2);
}
float3 PQDecode(float3 n)                        // PQ signal -> linear
{
    float3 np = pow(saturate(n), 1.0 / PQ_M2);
    float3 l  = max(np - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * np);
    return pow(l, 1.0 / PQ_M1) * 100.0;
}
// ---- ICtCp, input/output linear Rec.2020 (matrices exactly as in the GT7 sample) ----
float3 Rec2020ToICtCp(float3 rgb)
{
    float3 lms = float3(dot(rgb, float3(1688.0, 2146.0,  262.0)),
                        dot(rgb, float3( 683.0, 2951.0,  462.0)),
                        dot(rgb, float3(  99.0,  309.0, 3688.0))) / 4096.0;
    float3 p = PQEncode(lms);
    return float3(dot(p, float3( 2048.0,   2048.0,     0.0)),
                  dot(p, float3( 6610.0, -13613.0,  7003.0)),
                  dot(p, float3(17933.0, -17390.0,  -543.0))) / 4096.0;
}
float3 ICtCpToRec2020(float3 ictcp)
{
    float3 p = float3(dot(ictcp, float3(1.0,  0.00860904,  0.11103)),
                      dot(ictcp, float3(1.0, -0.00860904, -0.11103)),
                      dot(ictcp, float3(1.0,  0.560031,   -0.320627)));
    float3 lms = PQDecode(p);
    return max(float3(dot(lms, float3( 3.43661,   -2.50645,    0.0698454)),
                      dot(lms, float3(-0.79133,    1.9836,    -0.192271)),
                      dot(lms, float3(-0.0259499, -0.0989137,  1.12486))), 0.0);
}
// ---- GT7, SDR ----
static const float GT7_PEAK = 2.5, GT7_MID = 0.538, GT7_LIN = 0.444, GT7_TOE = 1.28;
static const float GT7_KA = 2.963333333, GT7_KB = -3.373351238, GT7_KC = -0.539568345;
static const float GT7_BLEND = 0.6, GT7_FADE_START = 0.98, GT7_FADE_END = 1.16;
static const float GT7_TARGET_I = 0.602559155;            // I of (2.5, 2.5, 2.5)

float GT7Curve(float x)                                   // GTToneMappingCurveV2::evaluateCurve
{
    x = max(x, 0.0);
    float wLin     = smoothstep(0.0, GT7_MID, x);
    float toe      = GT7_MID * pow(x / GT7_MID, GT7_TOE);
    float shoulder = GT7_KA + GT7_KB * exp(x * GT7_KC);
    return (x < GT7_LIN * GT7_PEAK) ? lerp(toe, x, wLin) : shoulder;
}

// in: exposed linear Rec.709 (0.18 = mid grey). out: display-linear Rec.709 [0,1]
float3 GT7ToneMapSDR(float3 rgb709)
{
    float3 rgb       = mul(M_709_TO_2020, rgb709) * GT7_PEAK;        // to GT units, 250-nit paper white
    float3 ucs       = Rec2020ToICtCp(rgb);
    float3 skewed    = float3(GT7Curve(rgb.r), GT7Curve(rgb.g), GT7Curve(rgb.b));
    float3 skewedUcs = Rec2020ToICtCp(skewed);
    float  chroma    = 1.0 - smoothstep(GT7_FADE_START, GT7_FADE_END, ucs.x / GT7_TARGET_I);
    float3 scaled    = ICtCpToRec2020(float3(skewedUcs.x, ucs.yz * chroma));
    float3 outFb     = min(lerp(skewed, scaled, GT7_BLEND), GT7_PEAK);
    return saturate(mul(M_2020_TO_709, outFb * (1.0 / GT7_PEAK)));   // SDR correction 1/2.5, simple gamut clip
}
```

**Behaviour *(sim, our port)*.** Neutral input: exposed 0.01 → 0.0043, 0.05 → 0.036, 0.18 → 0.179,
0.5 → 0.498, 1.0 → 0.835 (sRGB 236), ≥ 1.47 → 1.0. The toe region is exposed < 0.215 (= 0.538/2.5); the
linear region is 0.215–0.444; the shoulder runs 0.444–1.47. The neutral chroma fade runs from exposed ≈ 0.85
to ≈ 2.6. Saturated colours at increasing luminance (sRGB-8 output):

| colour (unit luminance ×) | 0.5 | 1 | 2 | 4 |
|---|---|---|---|---|
| orange (1, .35, .02) | 255,164,32 | 255,213,63 | 255,255,198 | 255,255,241 |
| red (1,0,0) | 255,46,0 | 255,105,30 | 255,208,167 | 255,250,214 |
| green (0,1,0) | 17,214,0 | 79,255,23 | 227,255,183 | 255,255,235 |
| blue (0,0,1) | 125,6,255 | 162,126,255 | 236,175,237 | 255,218,253 |

Orange goes orange → yellow → white, which matches the video's argument that very bright orange should read as yellow (1:15).
**Pitfall:** pure Rec.709 blue passes through lilac/pink before white. Saturated blue lights need
checking in the A/B. Pure colours converge to white only at very high luminance: red ≈ 13×, blue ≈ 9.5×
unit luminance, where blue's channel value ≈ 130. That matters for the LUT domain (§4).

**Pitfalls.**
* **Exposure must be right.** With only ~3 stops above mid-grey in SDR, the curve depends on auto
  exposure and glare. GT7 slide 178 treats glare as "expanding the perceptual output range" of
  saturated pixels.
* The ICtCp matrices expect **Rec.2020** input. Do not feed Rec.709 directly; convert, which is cheap.
  The final 2020 → 709 conversion can go slightly negative or above 1; `saturate` is acceptable for 709 content.
* Precision: `pow(x, 78.84)` in PQEncode is fine in fp32. Do not evaluate this in half precision.
* **Cost:** two RGB→ICtCp conversions, one ICtCp→RGB, three curve evaluations and an sRGB encode. `fxc`
  shows **234 instruction slots** for a pixel shader doing it for both MSAA samples (22 `exp` and 18 `log`
  vector instructions), against **34** for the LUT version. That is why the LUT exists (§4).
* The video reports GT7's **auto-exposure / white-balance / glare system** as part of the "physically based
  tone mapping" (slide 6). The curve alone is not the whole look.

### 3.3 Frostbite 2017 "display mapping" — Alex Fry, GDC 2017 (EA, slides on SlideShare)

**What and why.** Frostbite needed one HDR-mastered grade for SDR, HDR10 and Dolby Vision ("grade once,
output many"). The pipeline: post FP16 → **PQ-encoded LUT space** → **33³ grading LUT (10-bit UNORM
runtime)** → **UI composited offscreen** (premultiplied alpha, which Fry calls essential) → **display mapping and
encode per display, last in the pipeline**. Design goals: no toe, no contrast change, no hue shifts, no
built-in film look. So it is linear up to a threshold, with a **luma-only shoulder in ICtCp** and
progressive chroma desaturation. The SDR mapper is deliberately **under-exposed** because TVs
over-brighten SDR. Later the team **re-introduced partial hue shift**, because pure hue preservation looked
unnatural for bright colours and broke VFX authored for hue shifts. Fire VFX were re-authored with
blackbody ramps.

**Algorithm** (from the slide code; 1.0 = 100 nits, display-relative):
```
ictcp = RGBToICtCp(col)                                  // 709 -> XYZ -> LMS -> PQ(1.0=100nit) -> ICtCp
sat   = pow(smoothstep(1.0, 0.3, I), 1.3)                // 1 below I=0.3, 0 above I=1.0
col   = ICtCpToRGB(ictcp * (1, sat, sat))                // luminance-adaptive desaturation
rc(x,t) = x < t ? x : t + (1-t)*(1 - exp(-(x-t)/(1-t)))  // exponential "photographic" shoulder, t = 0.25
huePres = col * rc(max(col), .25) / max(col)
perChan = rc(col, .25)
col   = lerp(perChan, huePres, 0.6)
m     = RGBToICtCp(col)
boost = 0.3 * smoothstep(1.0, 0.5, I)                    // re-add some pre-compression chroma
m.CtCp = lerp(m.CtCp, ictcp.CtCp * m.I / max(1e-3, I), boost)
out   = ICtCpToRGB(m)
```
HLSL for the A/B is in §3.4. **Numbers *(sim, our port)*:** 0.18 → 0.180, 1.0 → 0.724, reaches 0.99 at
3.5 (4.3 stops). Orange at luminance 2 → (255,211,132), a desaturated peach. GT7 gives (255,255,198),
a yellow-white. This is the washed-out orange the video describes.

**Performance (Xbox One, 1080p):** the legacy final resample cost 0.43 ms plus UI. Naive HDR + UI
composite: ~1.1 ms (UI clear +0.25 ms, composite +0.45 ms). With the UI target aliased as sRGB for free
decode, a 1D LUT replacing PQ→linear ALU, and the UI target in ESRAM: ~0.8 ms. A single-pass CS resample:
~0.7 ms. Skipping unwritten UI tiles through CMASK: ~0.5 ms. HDR10 encode added ~0.2 ms. LUT
decorrelation study: the share of LUT texels touched is RGB 7.9 %, YCgCo 3.3 %, YCbCr 3.3 %, ICtCp 4.1 %.

**How the video positions it:** better than per-channel curves at hue preservation, but no toe by default
and it washes out vivid, properly exposed colours. GT7 is preferred.

### 3.4 Other curves for the A/B switch, and the exposure-matching table

For a fair comparison, pre-scale each operator so that exposed 0.18 maps to display 0.18. GT7 slide 154
did the same, scaling ACES because ACES maps 0.18 to ~10 nits instead of 18. *(sim, neutral inputs; orange =
(1, .35, .02) normalised to unit luminance, then ×0.5 / ×2 / ×8, shown in sRGB-8)*:

| operator | 0.18 → | 1.0 → | pre-scale for 0.18→0.18 | input reaching 0.99 (stops above 0.18) | orange ×0.5 / ×2 / ×8 |
|---|---|---|---|---|---|
| linear clamp | 0.180 | 1.000 | 1.000 | 0.99 (2.5) | 255,165,40 / 255,255,83 / 255,255,159 |
| Reinhard x/(1+x) | 0.153 | 0.500 | 1.220 | 99 (9.1) | 191,143,40 / 233,204,80 / 249,238,139 |
| Hable (Uncharted 2, W = 11.2) | 0.067 | 0.304 | 2.928 | 10.8 (5.9) | 154,102,22 / 223,172,51 / 255,236,98 |
| ACES Narkowicz (incl. ×0.6) | 0.140 | 0.673 | 1.205 | 9.25 (5.7) | 217,158,17 / 248,229,61 / 255,251,151 |
| ACES Hill RRT+ODT fit | 0.106 | 0.619 | 1.462 | 16.1 (6.5) | 216,148,39 / 253,226,139 / 255,250,223 |
| AgX (minimal polynomial) | 0.215 | 0.590 | 0.808 | 15.2 (6.4) | 211,163,104 / 247,217,174 / 255,245,226 |
| Khronos PBR Neutral | 0.140 | 0.869 | 1.222 | 6.32 (5.1) | 243,153,43 / 253,196,156 / 255,232,219 |
| MGSV / Fox (A = 0.6) | 0.180 | 0.812 | 1.000 | 3.41 (4.2) | 235,165,40 / 255,244,83 / 255,255,159 |
| GT (P=1, m=.22, …) | 0.179 | 0.828 | 1.005 | 2.35 (3.7) | 238,165,26 / 255,248,75 / 255,255,159 |
| **GT7 SDR (×2.5)** | 0.179 | 0.835 | 1.003 | 1.43 (3.0) | 255,164,32 / 255,255,198 / 255,255,255 |
| Frostbite 2017 | 0.180 | 0.724 | 1.000 | 3.51 (4.3) | 225,153,54 / 255,211,132 / 255,237,198 |

Reading the table: GT, GT7, MGSV and Frostbite share the same linear mid-tones (they map 0.18 → 0.18).
They differ in the toe (GT/GT7 have one; MGSV/Frostbite don't) and in highlight hue behaviour. ACES and
Hable are much darker at the same exposure.

```hlsl
float3 TonemapMGSV(float3 x)          // Fox Engine (per channel), per Courreges' MGSV study 2017
{
    const float A = 0.6, B = 0.45333;  // linear to 0.6, rational shoulder, hits 1.0 at x = 4.0
    return (x <= A) ? x : min(1.0, A + B - (B * B) / (x - A + B));
}
float3 TonemapAcesHill(float3 c)      // S. Hill, BakingLab ACES.hlsl (sRGB in/out)
{
    const float3x3 inM  = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 outM = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    float3 v = mul(inM, c);
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return saturate(mul(outM, v));
}
float3 TonemapPbrNeutral(float3 c)    // Khronos PBR Neutral (Lalish 2024)
{
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    c -= (x < 0.08) ? x - 6.25 * x * x : 0.04;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, newPeak.xxx, g);
}
float3 TonemapAgX(float3 c)           // minimal AgX (Wrensch / three.js / Filament), Rec.2020 working space
{
    const float3x3 inset  = float3x3(0.856627153315983, 0.137318972929847, 0.11189821299995,
                                     0.0951212405381588, 0.761241990602591, 0.0767994186031903,
                                     0.0482516061458583, 0.101439036467562, 0.811302368396859);
    const float3x3 outset = float3x3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
                                     -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
                                     -0.016493938717834573, -0.016493938717834257, 1.2519364065950405);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(transpose(inset), mul(M_709_TO_2020, c));          // source matrices are GLSL column-major
    c = saturate((clamp(log2(max(c, 1e-10)), minEv, maxEv) - minEv) / (maxEv - minEv));
    float3 x2 = c * c, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    c = pow(max(mul(transpose(outset), c), 0.0), 2.2);
    return saturate(mul(M_2020_TO_709, c));
}
// Frostbite 2017: Rec.709 <-> LMS(ICtCp spec) folded into one matrix each; reuses PQEncode/PQDecode
static const float3x3 FB_709_TO_LMS = float3x3(0.2958228, 0.6230644, 0.0811415,
                                               0.1562108, 0.7272264, 0.1164892,
                                               0.0351226, 0.1565945, 0.8081554);
static const float3x3 FB_LMS_TO_709 = float3x3( 6.1720123, -5.3196584,  0.1470959,
                                               -1.3238950,  2.5602005, -0.2361092,
                                               -0.0117088, -0.2648908,  1.2767432);
float3 FbRgbToICtCp(float3 c)
{
    float3 p = PQEncode(mul(FB_709_TO_LMS, max(c, 0.0)));
    return float3(dot(p, float3(0.5, 0.5, 0.0)), dot(p, float3(1.6137, -3.3234, 1.7097)), dot(p, float3(4.3780, -4.2455, -0.1325)));
}
float3 FbICtCpToRgb(float3 i)
{
    float3 p = float3(dot(i, float3(1.0,  0.00860514569398152,  0.11103560447547328)),
                      dot(i, float3(1.0, -0.00860514569398152, -0.11103560447547328)),
                      dot(i, float3(1.0,  0.56004885956263900, -0.32063747023212210)));
    return mul(FB_LMS_TO_709, PQDecode(p));
}
float  RangeCompress(float x, float t)  { return (x < t) ? x : t + (1.0 - t) * (1.0 - exp(-(x - t) / (1.0 - t))); }
float3 RangeCompress(float3 x, float t) { return float3(RangeCompress(x.r, t), RangeCompress(x.g, t), RangeCompress(x.b, t)); }
float3 TonemapFrostbite(float3 col)
{
    float3 ictcp = FbRgbToICtCp(col);
    float  sat   = pow(smoothstep(1.0, 0.3, ictcp.x), 1.3);
    col = FbICtCpToRgb(ictcp * float3(1.0, sat, sat));
    float  maxCol = max(col.r, max(col.g, col.b));
    float3 huePreserving = col * (RangeCompress(maxCol, 0.25) / max(maxCol, 1e-6));
    col = lerp(RangeCompress(col, 0.25), huePreserving, 0.6);
    float3 mapped = FbRgbToICtCp(col);
    float  boost  = 0.3 * smoothstep(1.0, 0.5, ictcp.x);
    mapped.yz = lerp(mapped.yz, ictcp.yz * mapped.x / max(1e-3, ictcp.x), boost);
    return saturate(FbICtCpToRgb(mapped));
}
```
Hable: `f(x) = ((x(Ax+CB)+DE)/(x(Ax+B)+DF)) − E/F`, `out = f(x)/f(W)`, with A=.15 B=.50 C=.10 D=.20 E=.02
F=.30 W=11.2 (constants as listed on GT7 slide 110). Reinhard: `x/(1+x)`, or extended `x(1+x/w²)/(1+x)`
with w = 4 (slide 109). The A/B UI only needs a `switch` in the LUT bake shader (§4). The per-pixel
cost is identical for all operators once baked.

---

## 4. Baking the tone mapper (and grade) into a 3D LUT

**What and why.** A global, pixel-independent operator maps an RGB input to an RGB output, so it can be
tabulated once and fetched with one trilinear 3D sample (GT7 slide 97: "LUTs work very well" for global
operators). GT Sport used a 1D LUT because ALU was the bottleneck (0.40 → 0.24 ms on PS4). GT7 bakes
its colour-volume operator into a 3D LUT (slide 173). Callisto: 64³ RGBA16F (video capture). Frostbite:
33³ 10-bit, PQ-encoded input. Days Gone: 32³ RGB10A2 (per the video chart). Unreal Engine uses a 32³ LUT
indexed through a log2 shaper (`LinToLog` in the engine source, roughly 14 stops around 0.18).

**Non-linear indexing ("anti-banding").** An HDR input spans ~20 stops, so uniform indexing wastes almost
every texel on highlights. GT Sport used a *square* mapping for its 1D LUT (more texels near zero,
inverse = sqrt). GT7 uses **pow(4)**: `x = X_max · u⁴`, `u = (x/X_max)^{1/4}` = `sqrt(sqrt(x/X_max))`.
Slide 174 shows **32³ with pow(4)** banding less than **64³ linear**.

**Our simulation *(sim)*.** GT7 SDR baked with fp16 storage and trilinear filtering emulated (texel-centre
addressing). The test set: 60k random colours, luminance log-uniform from 2⁻⁹ to 2⁷ (exposed), biased
toward saturated colours. Error = max-channel |LUT − ALU| in **sRGB 8-bit codes**:

| LUT | shaper | X_max | p99 | mean | p99, Y ∈ [0.05,1) | p99, Y ∈ [1,4) |
|---|---|---|---|---|---|---|
| 32³ | linear | 32 | 78.7 | 25.3 | — | — |
| 64³ | linear | 32 | 52.4 | 16.4 | — | — |
| 32³ | pow2 | 128 | 16.5 | 5.4 | — | — |
| 32³ | **pow4** | 128 | 6.7 | 0.90 | 8.5 | 5.5 |
| 32³ | log2 (0.18·2^((u−g)·19)) | 128 | 5.2 | 0.47 | 5.2 | 7.7 |
| 64³ | **pow4** | **64** | — | — | 2.4 | 2.0 |
| 64³ | **pow4** | 128 | 2.2 | 0.22 | 3.2 | 2.4 |
| 64³ | log2 | 128 | 2.2 | 0.13 | 3.1 | 2.8 |

Storing **linear** output instead of sRGB-encoded output roughly doubles the mean error. The worst errors
come from bright, saturated colours where GT7 has kinks (the per-channel clip, the chroma smoothstep and
the 2020→709 clip). Low-saturation colours (saturation < 0.3) reach p99 ≈ 1.4–1.9 codes at 64³ and ≈ 2.7–3.3
at 32³. Inputs
beyond `X_max` are clamped; with X_max = 64 that produced up to ~10 codes of error only for very bright
(Y > 4), very saturated lights, which converge to white anyway.

**Recommended LUT (v1):** 64³, `DXGI_FORMAT_R16G16B16A16_FLOAT` (2 MB), pow(4) shaper, `X_max = 64`
exposed (≈ +8.5 EV above 0.18), storing `LinearToSRGB(ToneMap(x))`. The A/B candidates: 32³
(256 KB), `R10G10B10A2_UNORM` (sRGB-encoded values are fine in 10 bits), X_max = 128, and a log2 shaper.

```hlsl
static const float LUT_SIZE = 64.0;
static const float LUT_MAX  = 64.0;
float3 LutShaperEncode(float3 x) { return sqrt(sqrt(saturate(x * (1.0 / LUT_MAX)))); }   // (x/MAX)^(1/4)
float3 LutShaperDecode(float3 u) { float3 u2 = u * u; return LUT_MAX * u2 * u2; }       // MAX * u^4
float3 LutUVW(float3 u)          { return u * ((LUT_SIZE - 1.0) / LUT_SIZE) + 0.5 / LUT_SIZE; } // texel centres

// Bake: Dispatch(16,16,16) for 64^3. UAV = RWTexture3D on an R16G16B16A16_FLOAT Texture3D
// (BindFlags SRV|UAV; typed UAV stores of RGBA16F and R10G10B10A2 are fine on FL11_0).
RWTexture3D<float4> OutLUT : register(u0);
[numthreads(4, 4, 4)]
void BakeToneLutCS(uint3 id : SV_DispatchThreadID)
{
    float3 u = id / (LUT_SIZE - 1.0);             // 0..1 exactly at texel centres
    float3 x = LutShaperDecode(u);                // exposed linear Rec.709
    float3 y = GT7ToneMapSDR(x);                  // or: switch (operatorIndex) for the A/B
    // optional creative grade here, in display space (Fox-style "retrospective toe" LUT, lift/gamma/gain...)
    OutLUT[id] = float4(LinearToSRGB(y), 1.0);
}

// Apply (in the tone-map pass); sampler: MIN_MAG_MIP_LINEAR, ADDRESS CLAMP
float3 lutOut = ToneLUT.SampleLevel(LinearClamp, LutUVW(LutShaperEncode(exposed)), 0).rgb;  // sRGB-encoded
```

**Pitfalls.**
* **Texel-centre remap** (`LutUVW`). Without it, the ends of the range are interpolated toward the clamp
  border and every value is slightly off.
* **Clamp the domain.** Negative inputs (from 2020→709 or from bloom) go to 0. Values beyond `X_max`
  saturate, which is acceptable as shown above.
* **Exposure and white balance are applied *before* the lookup** (§7). The LUT then depends only on
  curve and grade parameters, so it is baked once, or when an artist changes something. Baking every
  frame is affordable: Callisto took 61 µs for 64³ on an unknown GPU, and our bake CS is 111 instruction
  slots per texel × 262k texels. Still, there is no reason to bake every frame unless grades blend per frame.
* A grading LUT authored in sRGB (Photoshop/Resolve "identity cube") can be applied *inside the bake*.
  Do **not** chain a second 3D LUT at runtime.
* The LUT only approximates GT7 (2–3 codes p99 at 64³ on saturated colours). Keep the ALU path as a
  debug reference and add an error view showing `abs(LUT − ALU) × 32`.

---

## 5. Auto exposure

### 5.1 Sources and what they say

* **Fox Engine (Kojima Productions, GDC 2013, "Photorealism Through the Eyes of a FOX").** The slide shown
  in the video says *"Auto exposure focuses on lighting element"*. The talk showed real-camera footage
  of auto-exposure failing on dark materials (YouTube 46:19). The video's GPU capture of MGSV (~1:18)
  shows a chain from a **1920×1080 RGBA16F lighting buffer (no albedo)** → 480×270 RGBA16F → 128² → 32²,
  with the 480×270 draw taking 58 µs. Courrèges' MGSV study confirms that diffuse lighting is accumulated
  in its own HDR target before the "Lighting Combination and Tonemap" pass multiplies by albedo. This is
  the photographers' **incident-light metering**: measure the light falling on surfaces, not the light
  reflected, so a black car or snow does not move exposure.
* **GT7 (slides 188–222).** A typical real-time pipeline downscales, builds a histogram, rejects outliers,
  computes a target, then applies exponential smoothing that is fast at first and slows down. But
  GT7 argues that real life is not always "properly exposed": night stays dark and snow stays blinding. GT7
  models two stages of eye adaptation:
  * **short-term** (pupil + neural gain): range **3–5 EV**, adapts in a few seconds;
  * **long-term** (receptor sensitivity): range **~12 EV**, adapts over tens of minutes.

  Long-term adaptation bounds the short-term range, which produces the mildly glaring day and the dark
  night. Light adaptation is faster than dark adaptation, so dawn and dusk are not symmetric. Metering
  mixes the **frame-buffer histogram** with **light probes, sky dome, sun and sky visibility (SH AO)**,
  using fixed weights. That stays stable across GT's camera cuts every 4–5 s. One parameter set for the
  whole game; no artist overrides.
* **Exposure math** (Lagarde & de Rousiers 2014, "Moving Frostbite to PBR"; as summarised in bruop's
  histogram post): `EV100 = log2(L_avg · S/K)` with S = 100 and K = 12.5. Saturation-based
  `L_max = 78/(q·S) · 2^EV100` with q = 0.65, giving `exposure = 1/L_max = 1/(9.6 · L_avg)`. The
  histogram: 256 bins, bin 0 reserved for near-black, `bin = 1 + 254·t`, a groupshared histogram per
  16×16 group merged with atomics, and a 256-thread reduction pass.

### 5.2 Our metering (diffuse-only, incident-style)

**Input requirement (from the lighting doc).** A **diffuse-only lighting buffer** `D`: RGB = diffuse
lighting with albedo = 1 (direct + indirect), alpha = **metering weight**. The lighting passes write
0 for sky, emissive-only pixels, and SSS transmission or "opacity-buffer" scattering, which the video
asks to exclude. RGBA16F, MSAA or not. If the engine cannot provide it, the fallback is
`D ≈ L_diffuse / max(albedo, 0.04)`, but then emissive must still be masked.

**Exposure definition.** For a white Lambertian surface, `D` is its radiance. A grey card (albedo 0.18)
under the average light therefore has radiance 0.18·`D_avg`. Mapping that to 0.18 gives
**`exposure = 1 / D_avg`** (× 2^EC). An average-lit white surface lands on 1.0 (display ≈ 0.835 with
GT7) and mid-grey materials on mid-grey, whatever the scene's albedo distribution. EV100 equivalent, if
light units are physical: `EV100 ≈ log2(0.18 · D_avg · 8) = log2(1.44 · D_avg)`.

**Passes:**
* **M0, metering downsample** (PS, full-screen triangle, 480×270 RGBA16F RTV): average a 4×4 block of
  `D` (MSAA sample 0 only), weighted by alpha.
* **M1, histogram** (CS, 16×16 threads, Dispatch(30,17,1)): 256 bins of
  `log2(luminance)` mapped from `[MinLog2, MinLog2 + Log2Range]`. Bin 0 = excluded. Also writes one
  float4 per group with Σ(w·rgb) and Σw for white balance. No float atomics are needed, so it stays SM 5.0.
* **M2, adapt** (CS, 1 group × 256): trimmed mean between `LowPercent` and `HighPercent`, target EV,
  exponential adaptation, white balance (§6), then writes the **camera matrix** to a structured buffer
  that the tone-map pass reads as an SRV. **GPU-only, no CPU readback**, and the result is used in the
  same frame.

```hlsl
// ---- M0: 480x270 RGBA16F ----
Texture2DMS<float4, 2> DiffuseLighting : register(t0);
float4 MeterDownsamplePS(float4 pos : SV_Position) : SV_Target
{
    int2 base = int2(pos.xy) * 4;
    float4 sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        float4 d = DiffuseLighting.Load(base + int2(x, y), 0);
        sum += float4(d.rgb * d.a, d.a);           // excluded pixels don't darken the average
    }
    return float4(sum.rgb / max(sum.a, 1e-6), sum.a / 16.0);
}

// ---- M1: histogram + white-balance partial sums ----
Texture2D<float4>          DiffuseQuarter : register(t0);
RWByteAddressBuffer        Histogram      : register(u0);  // 256 x uint (raw UAV), zeroed by M2
RWStructuredBuffer<float4> GroupSums      : register(u1);  // 510 x float4
cbuffer MeterCB : register(b0) { uint2 InputSize; float MinLog2; float InvLog2Range; uint GroupsX; uint3 Pad; };
groupshared uint   gsBins[256];
groupshared float4 gsSum[256];

[numthreads(16, 16, 1)]
void MeterHistogramCS(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    gsBins[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    float4 sum = 0;
    if (all(id.xy < InputSize))
    {
        float4 d   = DiffuseQuarter.Load(int3(id.xy, 0));
        float  lum = dot(d.rgb, float3(0.2126, 0.7152, 0.0722));
        if (d.a > 0.0 && lum > 0.0)
        {
            float t = saturate((log2(lum) - MinLog2) * InvLog2Range);
            InterlockedAdd(gsBins[1 + uint(t * 254.0)], 1);
            sum = float4(d.rgb * d.a, d.a);
        }
    }
    gsSum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (gi < s) gsSum[gi] += gsSum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) GroupSums[gid.y * GroupsX + gid.x] = gsSum[0];
    if (gsBins[gi] != 0) Histogram.InterlockedAdd(gi * 4, gsBins[gi]);
}
```

M2 is listed in full in §6, because it also computes white balance. Its exposure part:

```hlsl
// trimmed mean of log2(D) over the counted pixels in [LowPercent, HighPercent]
float lo = total * LowPercent, hi = total * HighPercent, cum = 0, wsum = 0, lsum = 0;
for (uint b = 1; b < 256; ++b)
{
    float c = gsBins[b];
    float w = max(0.0, min(cum + c, hi) - max(cum, lo));          // part of this bin inside [lo,hi]
    lsum += w * (MinLog2 + ((b - 1) + 0.5) / 254.0 * Log2Range);
    wsum += w; cum += c;
}
targetEV = clamp(lsum / max(wsum, 1.0), MinEV, MaxEV);            // keep last EV if total == 0
float speed = (targetEV > state.x) ? SpeedToBright : SpeedToDark;  // brighter scene: light adaptation, faster
state.x += (targetEV - state.x) * (1.0 - exp(-DeltaTime * speed)); // frame-rate independent
exposure = exp2(-state.x + ExposureCompensation);
```

**Starting parameters** (tunable; not taken from a single source): `MinLog2 = −10`, `Log2Range = 26`
(≈ 0.1 EV per bin, covering moonlight to sunlit white for physical units), `LowPercent = 0.25`,
`HighPercent = 0.95`, `SpeedToBright = 3 /s`, `SpeedToDark = 1 /s` (light adaptation faster, as in
GT7; similar to common engine defaults), `MinEV/MaxEV` = the scene's plausible lighting range,
`ExposureCompensation = 0`. On camera cuts, set `state.z = 0` from the CPU (one `UpdateSubresource`)
so the next frame snaps instead of animating.

**Optional GT7-style dual adaptation (v2):** keep `EV_long` adapting slowly (τ ≈ 30–60 s) and clamp
the target into `[EV_long − 2, EV_long + 2]` (≈ 4 EV short-term range) before the fast adaptation.
Night stays darker and snow stays brighter than a camera would render them.

**Pitfalls.**
* **Excluded is not black.** Sky and emissive pixels must have weight 0 (bin 0), not luminance 0, or
  outdoor shots under-expose massively.
* **Scenes lit only by emissives** (no lights, no GI from emissives) produce `total = 0`. Keep the last EV
  (handled), or fall back to metering the full lighting buffer.
* Diffuse-only metering ignores albedo **on purpose**. A black room and a white room under the same lamp
  get the same exposure, as an incident meter would. If the art direction wants high-key or low-key, use
  `ExposureCompensation`, not the meter.
* Specular highlights and the sun disk never enter `D`, so no outlier rejection for them is needed. The
  high percentile only cuts small, very bright lit areas.
* Exposure is computed from the **current** frame's lighting and used the same frame. That is valid
  because the tone-map pass comes after lighting, and there is no feedback loop through the tone-mapped image.

**Cost (prediction, RTX 5060 1080p):** M0 reads 2.07 M × 8 B ≈ 17 MB (~0.04 ms); M1 and M2 are
negligible (< 0.02 ms each). Compare MGSV's 58 µs downsample draw in the video's capture.

---

## 6. Auto white balance

**What and why.** Hue-preserving tone mappers make warm light look warmer, and real eyes adapt to light
temperature (video 1:16). GT7 uses the auto-exposure mechanism on **colour temperature and delta
(tint)**: it adapts slowly toward **D65**, measures with the same multiple references plus a faint
**virtual light** to stabilise at night, blends the measurements luminance-weighted in **Yxy**, and
limits correction to **real cameras' AWB specs** (slide 222). The video wants a **min/max auto range**
and an **artist temperature bias**.

**Why the diffuse-only buffer is the right input.** Classic gray-world assumes the *reflected* image
averages to grey, which fails on a green lawn or a red car. The diffuse-only buffer has albedo
removed, so its luminance-weighted mean colour **is the illuminant mixture** (direct + bounced light).
No grey-world assumption is needed.

**Algorithm** (in M2, after exposure):
1. `light = Σ(w·rgb)/Σw + VirtualLight` (VirtualLight = faint white, e.g. 1e-3 × typical daylight
   radiance; it dominates only in near-darkness).
2. `XYZ = M_709→XYZ · light`, `xy = XY/(X+Y+Z)`.
3. **CCT (McCamy 1992):** `n = (x − 0.3320)/(0.1858 − y)`, `CCT = 449n³ + 3525n² + 6823.3n + 5520.33`.
   Clamp it to [1667, 25000] K.
4. **Tint** = `xy − Planck(CCT)`. Clamp its length to `MaxTint` (≈ 0.01) so strongly green or magenta
   lights are only partly corrected.
5. **Range:** `mired = clamp(1e6/CCT, 1e6/MaxKelvin, 1e6/MinKelvin)`. Adapt `mired` and `tint`
   exponentially (`AwbSpeed` ≈ 0.5–1 /s). Mireds are used because equal steps look roughly equally large.
6. **Assumed illuminant** `srcXY = Planck(1e6/(mired + BiasMired)) + tint`. A positive bias assumes a
   warmer light and so gives a cooler picture.
7. **Von Kries in Bradford LMS** toward D65 (0.3127, 0.3290), partial: `gain = lerp(1, LMS_D65/LMS_src, Strength)`.
   `M_wb = M_XYZ→709 · B⁻¹ · diag(gain) · B · M_709→XYZ`.
8. `CameraMatrix = exposure · M_wb`.

Planckian locus (Kim et al. 2002 cubic fit, valid 1667–25000 K), `t = 1000/T`:
* x: T ≤ 4000: `−0.2661239t³ − 0.2343589t² + 0.8776956t + 0.179910`;
  else `−3.0258469t³ + 2.1070379t² + 0.2226347t + 0.240390`
* y: T ≤ 2222: `−1.1063814x³ − 1.34811020x² + 2.18555832x − 0.20219683`;
  T ≤ 4000: `−0.9549476x³ − 1.37418593x² + 2.09137015x − 0.16748867`;
  else `3.0817580x³ − 5.87338670x² + 3.75112997x − 0.37001483`

```hlsl
// ---- M2 (Dispatch(1,1,1)): histogram -> exposure, light colour -> white balance, adaptation ----
RWByteAddressBuffer        Histogram  : register(u0);
StructuredBuffer<float4>   GroupSums  : register(t0);
RWStructuredBuffer<float4> CameraData : register(u1);  // [0..2] matrix rows, [3] (EV, mired, init, exposure), [4] (tint.xy)
cbuffer AdaptCB : register(b0)
{
    float MinLog2, Log2Range, LowPercent, HighPercent;
    float MinEV, MaxEV, ExposureCompensation, SpeedToBright;
    float SpeedToDark, DeltaTime, MinKelvin, MaxKelvin;
    float BiasMired, AwbStrength, AwbSpeed, VirtualLight;
    float MaxTint; uint NumGroups; uint2 Pad;
};
groupshared uint   gsBins[256];
groupshared float4 gsSum[256];

static const float3x3 M_709_TO_XYZ = float3x3(0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041);
static const float3x3 M_XYZ_TO_709 = float3x3(3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252);
static const float3x3 BRADFORD     = float3x3(0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296);
static const float3x3 BRADFORD_INV = float3x3(0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603, 0.0492912, -0.0085287, 0.0400428, 0.9684867);

float2 PlanckianXY(float T)
{
    float t = 1000.0 / T, t2 = t * t, t3 = t2 * t;
    float x = (T <= 4000.0) ? (-0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t + 0.179910)
                            : (-3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t + 0.240390);
    float x2 = x * x, x3 = x2 * x;
    float y = (T <= 2222.0) ? (-1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683)
            : (T <= 4000.0) ? (-0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867)
                            : ( 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483);
    return float2(x, y);
}
float3 XYToXYZ(float2 xy) { return float3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y); }
float3x3 WhiteBalanceMatrix(float2 srcXY, float strength)          // Bradford von Kries: srcXY -> D65
{
    float3 srcLms = mul(BRADFORD, XYToXYZ(srcXY));
    float3 dstLms = mul(BRADFORD, XYToXYZ(float2(0.3127, 0.3290)));
    float3 gain   = lerp(1.0, dstLms / srcLms, strength);
    float3x3 D    = float3x3(gain.x, 0, 0, 0, gain.y, 0, 0, 0, gain.z);
    return mul(M_XYZ_TO_709, mul(BRADFORD_INV, mul(D, mul(BRADFORD, M_709_TO_XYZ))));
}

[numthreads(256, 1, 1)]
void MeterAdaptCS(uint gi : SV_GroupIndex)
{
    gsBins[gi] = Histogram.Load(gi * 4);
    Histogram.Store(gi * 4, 0);                                        // clear for next frame
    float4 s = 0;
    for (uint g = gi; g < NumGroups; g += 256) s += GroupSums[g];
    gsSum[gi] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 128; k > 0; k >>= 1)
    {
        if (gi < k) gsSum[gi] += gsSum[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi != 0) return;

    float4 state = CameraData[3];
    float total = 0;
    for (uint b = 1; b < 256; ++b) total += gsBins[b];
    float targetEV = state.x;
    if (total > 0)
    {
        float lo = total * LowPercent, hi = total * HighPercent, cum = 0, wsum = 0, lsum = 0;
        for (uint b = 1; b < 256; ++b)
        {
            float c = gsBins[b];
            float w = max(0.0, min(cum + c, hi) - max(cum, lo));
            lsum += w * (MinLog2 + ((b - 1) + 0.5) / 254.0 * Log2Range); wsum += w; cum += c;
        }
        targetEV = clamp(lsum / max(wsum, 1.0), MinEV, MaxEV);
    }

    float3 light = gsSum[0].rgb / max(gsSum[0].w, 1e-6) + VirtualLight;
    float3 XYZ   = mul(M_709_TO_XYZ, light);
    float2 xy    = XYZ.xy / max(XYZ.x + XYZ.y + XYZ.z, 1e-6);
    float  n     = (xy.x - 0.3320) / (0.1858 - xy.y);                                   // McCamy
    float  cct   = clamp(((449.0 * n + 3525.0) * n + 6823.3) * n + 5520.33, 1667.0, 25000.0);
    float2 tint  = xy - PlanckianXY(cct);
    tint *= min(1.0, MaxTint / max(length(tint), 1e-6));
    float  targetMired = clamp(1e6 / cct, 1e6 / MaxKelvin, 1e6 / MinKelvin);

    float4 wbState = CameraData[4];
    if (state.z == 0.0) { state.x = targetEV; state.y = targetMired; state.z = 1.0; wbState.xy = tint; }
    float speed = (targetEV > state.x) ? SpeedToBright : SpeedToDark;
    state.x += (targetEV - state.x) * (1.0 - exp(-DeltaTime * speed));
    float awbBlend = 1.0 - exp(-DeltaTime * AwbSpeed);
    state.y    += (targetMired - state.y) * awbBlend;
    wbState.xy += (tint - wbState.xy) * awbBlend;
    state.w = exp2(-state.x + ExposureCompensation);                 // exposure

    float2 srcXY = PlanckianXY(1e6 / (state.y + BiasMired)) + wbState.xy;
    float3x3 cam = state.w * WhiteBalanceMatrix(srcXY, AwbStrength);
    CameraData[0] = float4(cam[0], 0);
    CameraData[1] = float4(cam[1], 0);
    CameraData[2] = float4(cam[2], 0);
    CameraData[3] = state;
    CameraData[4] = wbState;
}
```

**Verified behaviour *(sim)*** with MinKelvin 3000, MaxKelvin 9000, MaxTint 0.01, strength 1:
* A D65 light and a 6500 K blackbody give the identity. White surfaces stay (1, 1, 1).
* 3000 K blackbody (light RGB 1, .48, .15): a white surface becomes (1, 1, 1), fully neutralised.
* 2700 K: (1, .84, .68), clamped at 3000 K, so it stays warm.
* 10000 K: (.92, .95, 1), clamped at 9000 K.
* Red "horror" light (1, .1, .05): stays strongly red at (1, .19, .25).
* Green light (.7, 1, .6): partly corrected to (.69, 1, .68), because the tint is clamped.

**Pitfalls.**
* The red light became slightly *more magenta* (blue 0.05 → 0.25), because of the clamped tint and
  partial CCT correction. For extreme coloured lights, lower `AwbStrength` (an artist volume), or scale
  the strength down with `|tint|`.
* McCamy's formula is valid only near the locus (~2000–12500 K). Out-of-range results are clamped.
* The matrix preserves the luminance of white (Y = 1 in and out), so white balance does not fight exposure.
* GT7 tunes AWB with **multiple light references**, which the video doubts can be done from the frame
  buffer alone (1:17). A frame-buffer-only AWB will drift when the camera points at a coloured wall
  lit by bounce. `AwbSpeed` and `MaxTint` limit this; a probe/sun reference is an open question.

---

## 7. The tone-map pass (merged): camera matrix + bloom + LUT + MSAA separation

**Pass T** (full-screen triangle, pixel frequency, no depth, blend off):
* SRVs: `SceneHDR` (2x MSAA RGBA16F), `CameraData` (structured), `ToneLUT` (3D), `Bloom` (half res);
  sampler: linear clamp.
* RTVs: **two** `R10G10B10A2_UNORM` 1920×1080 non-MSAA targets: SV_Target0 = sample 0,
  SV_Target1 = sample 1. Both hold **sRGB-encoded** LDR. D3D11's standard 2x pattern puts sample 0 at
  (+4,+4)/16 and sample 1 at (−4,−4)/16 of a pixel, which is what the SMAA S2x stage needs to know (see
  the AA doc).
* Video slot for "DOF blend" and other merges (Crysis 3 also upscales half-res volumetric fog here).

```hlsl
Texture2DMS<float4, 2>   SceneHDR    : register(t0);
StructuredBuffer<float4> CameraData  : register(t1);   // rows 0..2 = exposure * white balance
Texture3D<float4>        ToneLUT     : register(t2);
Texture2D<float4>        Bloom       : register(t3);   // B1: built from the un-exposed HDR scene
SamplerState             LinearClamp : register(s0);
cbuffer TonemapCB : register(b0) { float BloomIntensity; float2 InvTargetSize; float Pad0; }
struct PSOut { float4 sample0 : SV_Target0; float4 sample1 : SV_Target1; };

float3 TonemapToDisplay(float3 x)       // -> sRGB-encoded
{
#if USE_LUT
    return ToneLUT.SampleLevel(LinearClamp, LutUVW(LutShaperEncode(x)), 0).rgb;
#else
    return LinearToSRGB(GT7ToneMapSDR(x));
#endif
}

PSOut TonemapPS(float4 pos : SV_Position)
{
    int2 p = int2(pos.xy);
    float3x3 camera = float3x3(CameraData[0].xyz, CameraData[1].xyz, CameraData[2].xyz);
    float3 bloom = Bloom.SampleLevel(LinearClamp, pos.xy * InvTargetSize, 0).rgb;
    float3 s0 = mul(camera, lerp(SceneHDR.Load(p, 0).rgb, bloom, BloomIntensity));   // energy conserving
    float3 s1 = mul(camera, lerp(SceneHDR.Load(p, 1).rgb, bloom, BloomIntensity));
    PSOut o;
    o.sample0 = float4(TonemapToDisplay(s0), 1.0);
    o.sample1 = float4(TonemapToDisplay(s1), 1.0);
    return o;
}
```

**Costs.** `fxc` slots: ALU GT7 **234**, LUT **34**. Prediction for the RTX 5060 (spec: 30 SMs × 128
lanes ≈ 2.5 GHz, 448 GB/s; verify): memory floor ≈ 2.07 M px × (16 B read + 8 B written) ≈ 50 MB ≈ 0.11 ms.
The ALU path issues ~100 transcendental ops per pixel. At 16 SFU lanes/SM that is roughly 0.15–0.2 ms,
so ALU should land around ~0.2–0.25 ms and the LUT path near the bandwidth floor, ~0.12 ms. The video's
target for this whole box is 0.25 ms; the chart lists The Callisto Protocol's equivalent stage at 0.93 ms, excluding bloom.

**Optimisation to test:** for interior pixels both MSAA samples are equal, so
`if (all(h0 == h1)) out1 = out0;`. This halves the ALU path almost everywhere; branches are coherent
in flat regions. It matters little with the LUT.

**Pitfalls.**
* SMAA's edge detection expects gamma-space input, which the RGB10A2 targets hold. SMAA's reference
  *blending* reads through an sRGB view to blend in linear space, which RGB10A2 cannot offer. Either
  decode/encode manually in SMAA's blend pass, or accept gamma-space blending. This is a decision for the AA doc.
* Alpha (2 bits) is unusable for side data. The video stores the temporal responsive mask in a reused
  RG16F SSR target.
* Exposure is applied after the bloom lerp, so the bloom chain is exposure-independent and can be built
  before M2 finishes. The fp16 range is enough for physical units below 65504.

---

## 8. Bloom

### 8.1 Jimenez 2014 (SIGGRAPH 2014 Advances, "Next Generation Post Processing in Call of Duty: Advanced Warfare")

* **Goal:** improve the perception of a wider dynamic range in a non-intrusive way. **Non-thresholded**
  HDR input (as suggested by Ward 1997). Temporal stability is the main concern.
* **Filter while downsampling *and* while upsampling** (citing Mittring 2012). Bilinear-only chains
  alias and pulse; bicubic rings; Gaussian is probably the best option.
* **Downsample = 13 bilinear taps** as 5 overlapping 2×2 boxes: a centre box (taps at ±1 texel,
  weight 0.5) and four corner boxes (taps at 0/±2 texels, 0.125 each). Weights sum to 1.
* **Upsample = progressive** (Mittring): `E' = E`, `D' = D + blur(E')`, …, `A' = A + blur(B')`, with
  a **3×3 tent** scaled by a radius parameter. Repeated tent convolutions converge to a Gaussian (Kraus 2007).
* **Fireflies:** a non-linear intensity mapping before averaging causes pulsing, so they use a
  **partial Karis average** (Karis weight `1/(1+luma)` per 2×2 block) **only on the mip0 → mip1 downsample**.
* **Result:** 6 mips, R11G11B10, 13-tap down, 9-tap up. **Veil: 0.35 ms on Xbox One.**

### 8.2 Our chain (B1: pre-tone-map, HDR)

Half res 960×540 → 30×17: 6 levels in **one** R11G11B10_FLOAT texture with mips. Create one RTV and
one SRV per mip (SRV `MostDetailedMip = i, MipLevels = 1`), so a pass never reads and writes the same
subresource. Passes: B0 (MSAA → half) → B1…B5 (13-tap down) → U5…U1 (tent up, **additive blend**
ONE/ONE onto the next larger mip, which still holds its downsample; that implements `D' = D + blur(E')`).
The tone-map pass then samples mip 0, divided by the level count (6) so the sum is an average. Mixed with
`lerp(scene, bloom, BloomIntensity)`, starting around 0.04 (tunable). Because both inputs are averages of
scene radiance, this mix conserves energy.

```hlsl
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float KarisWeight(float3 c) { return 1.0 / (1.0 + Luma(c)); }

// B0: full-res 2xMSAA -> half res; 2x2 px x 2 samples, Karis weighted (firefly suppression)
float4 BloomFirstDownsamplePS(float4 pos : SV_Position) : SV_Target
{
    int2 base = int2(pos.xy) * 2;
    float3 sum = 0; float wsum = 0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        int2 p = base + int2(i & 1, i >> 1);
        float3 c = 0.5 * (SceneHDR.Load(p, 0).rgb + SceneHDR.Load(p, 1).rgb);
        float w = KarisWeight(c);
        sum += c * w; wsum += w;
    }
    return float4(sum / wsum, 1.0);
}

// B1..B5: 13-tap downsample (SrcTexel = 1 / source mip size)
float4 BloomDownsamplePS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 t = SrcTexel;
    float3 a = Src.SampleLevel(LinearClamp, uv + t * float2(-2, -2), 0).rgb;
    float3 b = Src.SampleLevel(LinearClamp, uv + t * float2( 0, -2), 0).rgb;
    float3 c = Src.SampleLevel(LinearClamp, uv + t * float2( 2, -2), 0).rgb;
    float3 d = Src.SampleLevel(LinearClamp, uv + t * float2(-2,  0), 0).rgb;
    float3 e = Src.SampleLevel(LinearClamp, uv,                      0).rgb;
    float3 f = Src.SampleLevel(LinearClamp, uv + t * float2( 2,  0), 0).rgb;
    float3 g = Src.SampleLevel(LinearClamp, uv + t * float2(-2,  2), 0).rgb;
    float3 h = Src.SampleLevel(LinearClamp, uv + t * float2( 0,  2), 0).rgb;
    float3 i = Src.SampleLevel(LinearClamp, uv + t * float2( 2,  2), 0).rgb;
    float3 j = Src.SampleLevel(LinearClamp, uv + t * float2(-1, -1), 0).rgb;
    float3 k = Src.SampleLevel(LinearClamp, uv + t * float2( 1, -1), 0).rgb;
    float3 l = Src.SampleLevel(LinearClamp, uv + t * float2(-1,  1), 0).rgb;
    float3 m = Src.SampleLevel(LinearClamp, uv + t * float2( 1,  1), 0).rgb;
    float3 o = (j + k + l + m) * (0.5 / 4.0)
             + (a + b + d + e) * (0.125 / 4.0) + (b + c + e + f) * (0.125 / 4.0)
             + (d + e + g + h) * (0.125 / 4.0) + (e + f + h + i) * (0.125 / 4.0);
    return float4(o, 1.0);
}

// U5..U1: 3x3 tent of the smaller level; blend state ONE + ONE onto the larger level
float4 BloomUpsamplePS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 r = SrcTexel * UpsampleRadius;                      // radius ~1.0 texel of the source mip
    float3 s = Src.SampleLevel(LinearClamp, uv, 0).rgb * 4.0;
    s += (Src.SampleLevel(LinearClamp, uv + r * float2(-1, 0), 0).rgb + Src.SampleLevel(LinearClamp, uv + r * float2(1, 0), 0).rgb +
          Src.SampleLevel(LinearClamp, uv + r * float2(0, -1), 0).rgb + Src.SampleLevel(LinearClamp, uv + r * float2(0, 1), 0).rgb) * 2.0;
    s += (Src.SampleLevel(LinearClamp, uv + r * float2(-1, -1), 0).rgb + Src.SampleLevel(LinearClamp, uv + r * float2(1, -1), 0).rgb +
          Src.SampleLevel(LinearClamp, uv + r * float2(-1,  1), 0).rgb + Src.SampleLevel(LinearClamp, uv + r * float2(1,  1), 0).rgb);
    return float4(s / 16.0, 1.0);
}
```

**Deviation from CoD:AW:** our first level is a Karis-weighted 2×2 box over the MSAA samples. MSAA
textures cannot be bilinearly sampled, so it is not the 13-tap. The A/B alternative is `ResolveSubresource`
to a full-res RGBA16F (~0.1 ms of traffic), followed by a 13-tap with partial Karis. Compare temporal
stability on thin bright geometry.

**Cost prediction (RTX 5060):** B0 reads 33 MB (~0.08 ms). The rest of the chain is ~0.7 M px of small
kernels, ~0.03 ms. The video budgets 0.38 ms (including SSR mip generation); CoD:AW measured 0.35 ms on Xbox One.

### 8.3 Placement: the video wants bloom after motion blur

| Variant | Where | Pros | Cons |
|---|---|---|---|
| **B1** (recommended v1) | HDR, before the tone-map pass; mixed in pass T | Correct energy; small bright lights bloom by their true intensity; matches CoD:AW, UE and Crysis 3 (bloom resolved in the tone-map pass) | Does not react to motion blur (the blur happens later, on LDR) |
| **B2** (Fox-style) | Pass T also writes the HDR luminance (MGSV stores the pre-tone-map HDR luminance in the scene's **alpha**). Motion blur blurs it with the colour; bloom bright-pass and chain run on the post-MB image; the composite adds bloom | Bloom streaks with motion blur (video: spinning wheels, metallic weapons, naked-eye match) | RGB10A2 has only 2 alpha bits, so this needs an extra R8/R16F target (log-encoded luminance) written by T and processed by MB. Bloom is then added in display space: an approximation |
| **B3** (chart as drawn) | Downsample the post-MB, tone-mapped RGB10A2 (CoD:AW chain), reused as the SSR mip chain | Free reuse with SSR | LDR only: everything above white is clipped at the GT7 clip (~1.47 exposed), so headlights and specular sparkles lose their glare strength |

Implement B1 first. Then build B2 as an experiment, because the video's stated goal is bloom that reacts
to motion blur (§11).

---

## 9. Final composite + UI + gamma (one draw)

**Pass C** (full-screen triangle). RTV: the back buffer as `DXGI_FORMAT_R8G8B8A8_UNORM` (not `_SRGB`),
because we encode manually and composite the UI in sRGB space. Flip-model swap chains
(`DXGI_SWAP_EFFECT_FLIP_DISCARD`) cannot use `_SRGB` *buffer* formats (`DXGI_ERROR_INVALID_CALL`),
but they can take an `_SRGB` *RTV* on an `R8G8B8A8_UNORM` buffer (Walbourn). Allowed flip buffer formats
include `R16G16B16A16_FLOAT`, `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM` and `R10G10B10A2_UNORM`. A 10-bit back
buffer is an A/B option against 8-bit + dither.

**UI target:** a full-res `R8G8B8A8_UNORM`, cleared to (0,0,0,0) only when UI is drawn (the video's
budget is 0.1 ms). UI shaders output **premultiplied, sRGB-encoded** colour. Blend state for colour and
alpha: `SrcBlend = ONE, DestBlend = INV_SRC_ALPHA, BlendOp = ADD`. Frostbite calls premultiplied alpha
essential and cut UI cost by keeping the UI target fast and skipping untouched tiles. D3D11 on PC
has no CMASK access, so the equivalent is to skip the UI fetch when a CPU "UI present" flag is false.

```hlsl
Texture2D<float4> SceneLDR  : register(t0);  // post SMAA / MB: R10G10B10A2, sRGB-encoded
Texture2D<float4> HalfResFx : register(t1);  // half res RGBA16F, premultiplied linear (lens drops, rain, flares)
Texture2D<float4> UiLayer   : register(t2);  // premultiplied sRGB-encoded RGBA8
cbuffer CompositeCB : register(b0)
{
    float2 InvSize; float Aspect; uint Frame;
    float  ChromaticAberration;  // e.g. 0.004
    float  VignetteStrength;     // 0..1
    float  GrainStrength;        // e.g. 0.03 (sRGB-encoded units)
    float  UserGamma;            // brightness slider, 1.0 = neutral
};
float Hash(uint2 p, uint frame)  // PCG-style integer hash -> [0,1)
{
    uint v = p.x * 1973u + p.y * 9277u + frame * 26699u;
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return float((v >> 22u) ^ v) * (1.0 / 4294967296.0);
}
float4 CompositePS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // 1) chromatic aberration: radial, grows with r^2 (3 taps, R and B pushed apart)
    float2 d  = uv - 0.5;
    float  r2 = dot(d * float2(Aspect, 1.0), d * float2(Aspect, 1.0));
    float2 ca = d * ChromaticAberration * r2;
    float3 c = float3(SceneLDR.SampleLevel(LinearClamp, uv + ca, 0).r,
                      SceneLDR.SampleLevel(LinearClamp, uv,      0).g,
                      SceneLDR.SampleLevel(LinearClamp, uv - ca, 0).b);
    c = SRGBToLinear(c);
    // 2) half-res composite layer (bloom B2/B3 would be added here as well)
    float4 fx = HalfResFx.SampleLevel(LinearClamp, uv, 0);
    c = c * (1.0 - fx.a) + fx.rgb;
    // 3) vignette, cos^4-like falloff
    float v = 1.0 / (1.0 + r2 * 1.5);
    c *= lerp(1.0, v * v, VignetteStrength);
    // 4) user brightness: power curve keeps 0->0 and 1->1 (scales mid-tones, never clips the test image)
    c = pow(saturate(c), UserGamma);
    // 5) encode + film grain weighted to mid-tones; the +1/255 term is the TPDF dither for 8-bit
    float3 e = LinearToSRGB(c);
    float  n = Hash(uint2(pos.xy), Frame) + Hash(uint2(pos.xy) + 7u, Frame) - 1.0;   // triangular [-1,1]
    float  lum = dot(e, float3(0.2126, 0.7152, 0.0722));
    e += n * (GrainStrength * 4.0 * lum * (1.0 - lum) + 1.0 / 255.0);
    // 6) UI over the scene in sRGB space, premultiplied, same draw
    float4 ui = UiLayer.Load(int3(pos.xy, 0));
    e = ui.rgb + e * (1.0 - ui.a);
    return float4(saturate(e), 1.0);
}
```

**Gamma / brightness calibration math.**
* *Brightness slider* (Callisto-like "scaling"): `display = pow(display, γ_u)` in display-linear,
  γ_u ∈ [0.6, 1.6], default 1, applied to the scene only (not the UI). Black stays black and white stays
  white, so all three calibration patterns stay visible while mid-tones move. The bad pattern (the video's
  Dead Space Remake example) is an offset or black-level crush that deletes the dark test image.
* *Display EOTF choice* (Alan Wake 2 / Callisto offer several): encode with the sRGB piecewise OETF,
  pure `x^(1/2.2)`, or `x^(1/2.4)` (BT.1886-like). Many monitors decode with pure 2.2, which shows
  sRGB-encoded shadows slightly lifted. The 2.2 option fixes that. Implement it as a `switch` in
  `LinearToSRGB`.
* *Calibration screen:* three patterns (shadows, mid-tones, highlights), e.g. a logo at encoded 0.02 on
  black, a mid-grey patch, and a logo at 0.98 on white. Instruction: "barely visible". Draw the patterns
  *through* the same composite path.

**Pitfalls.**
* Grain and dither go after the final encode, otherwise they are quantised away. The UI is blended
  after the grain, so the UI is not grainy.
* Chromatic aberration and vignette are camera effects, so they go *under* the half-res artist layer
  and the UI (video chart note).
* The video asks for debug views of the half-res and full-res composite layers (stand-alone, quad
  overdraw) so artists can optimise composite meshes.

**Cost prediction:** reads RGB10A2 (3 cached taps) + UI + half-res FX, writes RGBA8: ≈ 30 MB, ≈ 0.07 ms.
The video budgets 0.15 ms (composite) + 0.1 ms (UI).

---

## 10. Recommended pass list and resources

| # | Pass | Type | Inputs | Output (format, size) | When |
|---|---|---|---|---|---|
| M0 | Metering downsample | PS | diffuse-only lighting (alpha = weight) | Dq: RGBA16F 480×270 (1 MB) | every frame |
| M1 | Histogram + WB sums | CS 16×16, (30,17,1) | Dq | Histogram: raw 256×u32; GroupSums: 510×float4 | every frame |
| M2 | Adapt (exposure + AWB) | CS 256, (1,1,1) | Histogram, GroupSums, CameraData | CameraData: structured 5×float4 (SRV+UAV) | every frame |
| L | LUT bake | CS 4³, (16,16,16) | parameters | ToneLUT: 64³ RGBA16F 3D (2 MB) | on change / A/B toggle |
| B0–B5, U5–U1 | Bloom (B1) | PS chain | SceneHDR 2xMSAA | Bloom: R11G11B10F 960×540 + 5 mips (~2.8 MB) | every frame |
| T | Tone map + MSAA separation | PS (2 MRT) | SceneHDR, CameraData, ToneLUT, Bloom | 2 × R10G10B10A2 1920×1080 (sRGB-encoded) | every frame |
| … | SMAA S2x, temporal SMAA, DOF, motion blur, half-res FX | other docs | | | |
| C | Composite + UI + gamma | PS | post-MB LDR, half-res FX, UI RT | back buffer R8G8B8A8_UNORM | every frame |

Order constraints: M0–M2 need only the lighting buffers, so they can run right after lighting. L is
independent. The bloom chain needs only SceneHDR. T needs all of them. No CPU readback anywhere. For
logging/graphs only, copy CameraData to a staging buffer and read it 2–3 frames later.

D3D11 state summary:
* Every full-screen pass: one triangle generated from `SV_VertexID`, no input layout, depth and stencil
  disabled, culling off.
* Samplers: LinearClamp (`D3D11_FILTER_MIN_MAG_MIP_LINEAR`, CLAMP) for the LUT, bloom, half-res FX and CA.
* Blend states: opaque everywhere, except the bloom upsample (ONE/ONE ADD) and the UI drawing
  (ONE/INV_SRC_ALPHA).
* Raw buffer: `D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS`, UAV `DXGI_FORMAT_R32_TYPELESS` +
  `D3D11_BUFFER_UAV_FLAG_RAW`.
* Structured buffers: `D3D11_RESOURCE_MISC_BUFFER_STRUCTURED`, stride 16.
* Unbind a UAV before binding the same resource as an SRV. The runtime does this for you, with a debug-layer warning.

Expected total (predictions to verify): M* ~0.06 ms, bloom ~0.1 ms, T ~0.12 ms (LUT), C ~0.07 ms.
That is well under the video's budgets (0.25 + 0.38 + 0.15 + 0.1 ms).

---

## 11. A/B test plan (how to measure each claim)

Common setup: D3D11 timestamp queries (`D3D11_QUERY_TIMESTAMP_DISJOINT` + one `TIMESTAMP` per pass
boundary), averaged over 300 frames on a fixed camera path. Present with VSync off and record the
GPU clock. Optionally lock clocks with `nvidia-smi -lgc` (needs admin; the user's call). Run Nsight
Graphics GPU Trace on each pass to see whether it is SFU-, ALU- or memory-bound. Test scenes:
(a) the video's "Cornell box with coloured spheres" (orange/red/blue spheres, white sphere, chrome
sphere, a green emissive window); (b) an outdoor sunlit scene with sky; (c) a dark interior with a
bright doorway; (d) a fast-spinning metallic wheel; (e) a colour-gradient test wall
(hue × luminance ramps, smooth sky).

1. **ALU GT7 vs 3D LUT.** Variants: ALU, ALU + equal-sample early-out, LUT 64³ RGBA16F, 32³ RGBA16F,
   64³ R10G10B10A2, X_max 64 vs 128, pow4 vs log2.
   * Measure the time of pass T and the bake time.
   * Image error vs ALU: max / p99 in 8-bit codes, plus an `abs × 32` heatmap.
   * Banding check on the gradient wall: count 1-code steps per 64 px.
   * Expected *(sim)*: 64³ pow4 p99 ≈ 2–3 codes on saturated colours, under 1 code on neutrals. Linear
     indexing visibly bands even at 64³.
   * Expected time: LUT ≈ 0.1 ms faster than ALU at 1080p 2xMSAA; the ALU path should become
     SFU-bound.
   * Video budget: ≤ 0.25 ms for the whole pass.
2. **Tone mappers side by side.** Split-screen or a toggle via the LUT bake `switch`, every operator
   pre-scaled per the §3.4 table.
   * Exposure sweep −2…+4 EV on the spheres.
   * Metrics: hue drift of the orange albedo vs exposure (the ICtCp hue angle `atan2(Cp, Ct)` of the
     output); chroma at correct exposure; black-level / toe effect (fraction of pixels ≤ sRGB 16 at fixed
     exposure); highlight headroom (the stop at which the white sphere clips).
   * GT with `m = 0.22` vs `m = 0.022` vs `m = 0.001` reproduces the video's Callisto toe experiment.
3. **Exposure metering: diffuse-only vs full lighting vs final colour.**
   * Same room and light, floor albedo swapped from 0.05 to 0.9. Expected ΔEV ≈ 0 for diffuse-only, and
     ≈ log2(0.9/0.05) × (floor share of the frame) for colour metering.
   * Emissive TV filling 30 % of the view: expected ΔEV ≈ 0 for diffuse-only.
   * Tunnel exit (scene c): plot EV(t) for SpeedToBright/SpeedToDark settings.
   * Log CameraData through staging readback.
4. **White balance.**
   * 3000 K interior vs 6500 K exterior: measure the output chromaticity of a white card. Target:
     neutral within MinK..MaxK.
   * Red horror light: must stay red.
   * Camera pointed at a coloured wall lit only by bounce: watch for drift and tune MaxTint/AwbSpeed.
   * Compare against AWB off, as GT7 slide 212 does ("Auto WB: Off / On").
5. **Bloom.**
   * B1 vs B2 (and B3) on the spinning wheel and a moving headlight: does bloom streak with the motion
     blur (the video's perception claim)? Also measure the cost.
   * B0 box+Karis vs resolve+13-tap: temporal stability (frame-to-frame luminance variance of bloom on
     thin bright wires).
6. **Composite.** Merged pass vs separate passes (CA, vignette, grain, UI) for cost. UI target vs drawing
   the UI straight onto the back buffer (cost of clear + full-screen fetch). 8-bit + dither vs a 10-bit
   back buffer on the gradient wall.

---

## 12. Open questions

1. **Diffuse-only buffer.** Does the lighting design (R5) provide an albedo-free diffuse RGBA16F with a
   metering-weight alpha? Who writes weight 0 for SSS transmission and emissive pixels? The video's
   suggested "ID system" is unspecified.
2. **Bloom placement.** B1 (energy-correct, pre-MB) vs B2 (Fox-style, reacts to MB, needs a luminance
   side channel because RGB10A2 has no usable alpha) vs B3 (the chart's LDR chain shared with SSR). The
   video itself says this needs more work.
3. **SMAA blending in gamma vs linear** with RGB10A2 targets (no `_SRGB` view). Decide with the AA doc.
4. **GT7 SDR headroom (~3 stops)** relies on exposure and glare. Do we need GT7's dual
   short/long-term adaptation and its multi-reference metering (probes, sky, sun) for stability on
   camera cuts, or is frame-buffer-only metering enough for our scenes?
5. **Grade LUT.** Does the video's Fox-style "retrospective toe" grade belong in the baked LUT (our
   proposal) or in the composite pass (as the chart says), given GT7 already has a toe?
6. **Rec.2020 rendering** (video 1:19): the GT7 port already works internally in 2020. Moving the
   lighting buffer to 2020 primaries would remove the 709 → 2020 conversion but affects the whole pipeline.
7. **Hunt/HK effects and mesopic (Purkinje) shift** (video 1:15, GT7 future work): out of scope for v1.

---

## Sources

* Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering", 2026-08-01:
  https://www.youtube.com/watch?v=SnNm7rSSvlg. Transcript 1:04–1:20, 1:57–2:01; pipeline-chart frames at
  ~1:04:41 (tone-map box, budgets), ~1:57:38 and ~1:58:10 (bloom / composite / UI boxes), Callisto cvars
  (~1:09), Callisto LUT capture (~1:19), MGSV metering capture and the Fox GDC slide (~1:18).
* Suzuki, Yasutomi, Uchimura, "Driving Toward Reality: Physically Based Tone Mapping and Perceptual
  Fidelity in Gran Turismo 7", SIGGRAPH 2025 PBS course, slides v1.1:
  https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf. GT7
  algorithm (slides 139–150), GT V2 curve (147), LUT pow(4) (173–174), glare (177–187), auto
  exposure / dual adaptation / multi-reference metering (188–221), AWB (222), operator comparison (98–120, 154–172).
* Polyphony Digital, `gt7_tone_mapping.cpp` (MIT, v1.0 2025-08-10):
  https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp.
  Exact constants, ICtCp/Jzazbz/PQ code, SDR path (250-nit paper white).
* Uchimura & Suzuki, "Practical HDR and Wide Color Techniques in Gran Turismo SPORT" (SIGGRAPH Asia 2018
  course notes): http://cdn2.gran-turismo.com/data/www/pdi_publications/PracticalHDRandWCGinGTS_note_20181222.pdf.
  GT curve design, per-channel gamut mapping, 1D LUT square mapping, PS4 0.40 → 0.24 ms, compatible SDR/HDR grading.
* Suzuki & Yasutomi, "Realistic Real-time Sky Dome Rendering in Gran Turismo 7", GDC 2023:
  https://s3.amazonaws.com/gran-turismo.com/pdi_publications/GDC2023_GT7_SKY_RENDERING.pdf. Downloaded;
  background only (GT7 lighting references used by exposure).
* dmnsgn/shaders-tone-map (GLSL reference of Uchimura 2017 with default parameters, Khronos PBR Neutral,
  minimal AgX): https://github.com/dmnsgn/shaders-tone-map
* Alex Fry, "High Dynamic Range Color Grading and Display in Frostbite", GDC 2017. Slides:
  https://www.slideshare.net/slideshow/high-dynamic-range-color-grading-and-display-in-frostbite/72997674
  (read via the agent laptop). Talk page: https://www.ea.com/news/high-dynamic-range-color-grading-and-display-in-frostbite
  (404 at research time). Pipeline, display-mapper code, ICtCp/PQ code, 33³ 10-bit PQ LUT, Xbox One timings.
* Jorge Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014
  Advances: https://www.iryoku.com/downloads/Next-Generation-Post-Processing-in-Call-of-Duty-Advanced-Warfare-v18.pptx.
  Bloom/veil (13-tap, tent, partial Karis, 6 R11G11B10 mips, 0.35 ms on Xbox One).
* Brian Karis, "Tone mapping" (2013): https://graphicrants.blogspot.com/2013/12/tone-mapping.html. Weighted
  average with `1/(1+luma)`.
* Adrian Courrèges, "Metal Gear Solid V – Graphics Study" (2017):
  https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/. MGSV tone curve (A = 0.6,
  B = 0.45333), separate HDR diffuse/specular buffers, HDR luminance in alpha for post-tone-map bloom,
  16³ grading LUT, post chain order.
* Kojima Productions, "Photorealism Through the Eyes of a FOX", GDC 2013:
  https://gdcvault.com/play/1019121/Photorealism-Through-the-Eyes-of (members only). YouTube recording
  timestamp from the video: https://youtu.be/WsmxBE9Gw6A?t=2779.
* Bruno Opsenica, "Automatic Exposure Using a Luminance Histogram": https://bruop.github.io/exposure/.
  256-bin histogram compute design, EV100 / saturation-based exposure (from Lagarde & de Rousiers 2014,
  "Moving Frostbite to PBR").
* Wikipedia, "Planckian locus": https://en.wikipedia.org/wiki/Planckian_locus. Kim et al. cubic fit
  coefficients. McCamy 1992 CCT formula (well known; Color Research & Application 17(2)).
* Microsoft, "DXGI flip model": https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model,
  and Chuck Walbourn, "The Care and Feeding of Modern Swap Chains":
  https://walbourn.github.io/care-and-feeding-of-modern-swapchains/. sRGB RTV on a UNORM flip buffer;
  allowed formats include R10G10B10A2.
* Stephen Hill, ACES fit (BakingLab `ACES.hlsl`, cited by GT7 slide 239), and Krzysztof Narkowicz, "ACES
  Filmic Tone Mapping Curve" (2016). Constants as used in §3.4.
