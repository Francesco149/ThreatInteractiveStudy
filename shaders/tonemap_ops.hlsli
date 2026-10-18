// Tone curves for the A/B switch and the 3D LUT that bakes them (docs/research/06 sections 3-4).
// Every operator takes "exposed" linear Rec.709 (after exposure x white balance: 0.18 = mid grey,
// 1.0 = diffuse white under the average light) and returns display-linear Rec.709 in [0, 1].
// Operators that do not map 0.18 to ~0.18 are pre-scaled so the A/B compares curves, not exposures
// (as GT7 slide 154 does for ACES).
#ifndef TONEMAP_OPS_HLSLI
#define TONEMAP_OPS_HLSLI

// Must match the Tonemapper enum in renderer.h.
#define TM_GT7 0
#define TM_GT 1
#define TM_FROSTBITE 2
#define TM_ACES 3
#define TM_REINHARD 4
#define TM_LINEAR 5
#define TM_AGX 6

static const float3x3 M_709_TO_2020 = float3x3(   // BT.2087
    0.627403896, 0.329283038, 0.043313066,
    0.069097289, 0.919540395, 0.011362316,
    0.016391439, 0.088013308, 0.895595253);
static const float3x3 M_2020_TO_709 = float3x3(
     1.660491, -0.587641, -0.072850,
    -0.124550,  1.132900, -0.008349,
    -0.018151, -0.100579,  1.118730);

// IEC 61966-2-1 piecewise sRGB.
float3 linear_to_srgb(float3 c) {
    c = saturate(c);
    return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 srgb_to_linear(float3 c) {
    c = max(c, 0.0);
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// ---------------------------------------------------------------------------------------------
// SMPTE ST 2084 (PQ) and ICtCp, in GT frame-buffer units (1.0 = 100 cd/m2). fp32 only: the
// pow(x, 78.84) does not survive half precision.

static const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;

float3 pq_encode(float3 fb) {
    float3 ym = pow(max(fb, 0.0) * 0.01, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2);
}
float3 pq_decode(float3 n) {
    float3 np = pow(saturate(n), 1.0 / PQ_M2);
    float3 l = max(np - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * np);
    return pow(l, 1.0 / PQ_M1) * 100.0;
}

// BT.2100 ICtCp from/to linear Rec.2020 (matrices exactly as in the GT7 sample code).
float3 rec2020_to_ictcp(float3 rgb) {
    float3 lms = float3(dot(rgb, float3(1688.0, 2146.0, 262.0)),
                        dot(rgb, float3(683.0, 2951.0, 462.0)),
                        dot(rgb, float3(99.0, 309.0, 3688.0))) / 4096.0;
    float3 p = pq_encode(lms);
    return float3(dot(p, float3(2048.0, 2048.0, 0.0)),
                  dot(p, float3(6610.0, -13613.0, 7003.0)),
                  dot(p, float3(17933.0, -17390.0, -543.0))) / 4096.0;
}
float3 ictcp_to_rec2020(float3 ictcp) {
    float3 p = float3(dot(ictcp, float3(1.0, 0.00860904, 0.11103)),
                      dot(ictcp, float3(1.0, -0.00860904, -0.11103)),
                      dot(ictcp, float3(1.0, 0.560031, -0.320627)));
    float3 lms = pq_decode(p);
    return max(float3(dot(lms, float3(3.43661, -2.50645, 0.0698454)),
                      dot(lms, float3(-0.79133, 1.9836, -0.192271)),
                      dot(lms, float3(-0.0259499, -0.0989137, 1.12486))), 0.0);
}

// ---------------------------------------------------------------------------------------------
// GT7 tone mapping, SDR path. HLSL port of Polyphony Digital's gt7_tone_mapping.cpp v1.0
// (SIGGRAPH 2025 PBS course, Suzuki, Yasutomi, Uchimura), which carries this notice:
//
//   MIT License
//
//   Copyright (c) 2025 Polyphony Digital Inc.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// 1. "Skewed" colour: the GT V2 curve (toe + linear section + convergent exponential shoulder,
//    hard-clipped at the peak) per Rec.2020 channel. Per-channel curves twist hues in highlights.
// 2. "Untwisted" colour: in ICtCp, the intensity of the skewed colour with the chroma of the
//    original, faded to white as the original intensity reaches the peak (0.98..1.16 x peak).
// 3. out = lerp(skewed, untwisted, 0.6): mostly hue-preserving, with a camera-like hint of twist,
//    so bright orange goes to yellow, then white (GT7 slides 139-150).
// SDR: peak = 250 nits = 2.5 GT units (GT's SDR paper white), output scaled by 1/2.5.
static const float GT7_PEAK = 2.5, GT7_MID = 0.538, GT7_LIN = 0.444, GT7_TOE = 1.28;
static const float GT7_KA = 2.963333333, GT7_KB = -3.373351238, GT7_KC = -0.539568345;  // shoulder, alpha = 0.25
static const float GT7_BLEND = 0.6, GT7_FADE_START = 0.98, GT7_FADE_END = 1.16;
static const float GT7_PEAK_I = 0.602559155;   // ICtCp intensity of (2.5, 2.5, 2.5)

float gt7_curve(float x) {   // GTToneMappingCurveV2::evaluateCurve
    x = max(x, 0.0);
    float w_lin = smoothstep(0.0, GT7_MID, x);
    float toe = GT7_MID * pow(x / GT7_MID, GT7_TOE);
    float shoulder = GT7_KA + GT7_KB * exp(x * GT7_KC);
    return (x < GT7_LIN * GT7_PEAK) ? lerp(toe, x, w_lin) : shoulder;
}

float3 tonemap_gt7(float3 rgb709) {
    float3 rgb = mul(M_709_TO_2020, rgb709) * GT7_PEAK;   // GT units, 250-nit paper white
    float3 ucs = rec2020_to_ictcp(rgb);
    float3 skewed = float3(gt7_curve(rgb.r), gt7_curve(rgb.g), gt7_curve(rgb.b));
    float3 skewed_ucs = rec2020_to_ictcp(skewed);
    float chroma = 1.0 - smoothstep(GT7_FADE_START, GT7_FADE_END, ucs.x / GT7_PEAK_I);
    float3 scaled = ictcp_to_rec2020(float3(skewed_ucs.x, ucs.yz * chroma));
    float3 out_fb = min(lerp(skewed, scaled, GT7_BLEND), GT7_PEAK);
    return saturate(mul(M_2020_TO_709, out_fb * (1.0 / GT7_PEAK)));   // SDR correction, simple gamut clip
}

// ---------------------------------------------------------------------------------------------
// GT (Uchimura 2017, Gran Turismo Sport): the per-channel curve GT7 builds on. Toe [0, m], linear
// section from m, exponential shoulder towards P.
float3 tonemap_gt(float3 x) {
    const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0, S1 = m + a * l0;
    float C2 = (a * P) / (P - S1), CP = -C2 / P;
    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(S0, x);
    float3 w1 = 1.0 - w0 - w2;
    float3 T = m * pow(max(x, 0.0) / m, c) + b;
    float3 L = m + a * (x - m);
    float3 S = P - (P - S1) * exp(CP * (x - S0));
    return saturate(T * w0 + L * w1 + S * w2);
}

// ---------------------------------------------------------------------------------------------
// Frostbite 2017 display mapping (Fry, GDC 2017, "High Dynamic Range Color Grading and Display in
// Frostbite"): luma-only shoulder in ICtCp with luminance-adaptive desaturation, blended 60/40 with
// a per-channel shoulder, then part of the pre-compression chroma restored. No toe.
static const float3x3 FB_709_TO_LMS = float3x3(0.2958228, 0.6230644, 0.0811415,
                                               0.1562108, 0.7272264, 0.1164892,
                                               0.0351226, 0.1565945, 0.8081554);
static const float3x3 FB_LMS_TO_709 = float3x3(6.1720123, -5.3196584, 0.1470959,
                                               -1.3238950, 2.5602005, -0.2361092,
                                               -0.0117088, -0.2648908, 1.2767432);
float3 fb_rgb_to_ictcp(float3 c) {
    float3 p = pq_encode(mul(FB_709_TO_LMS, max(c, 0.0)));
    return float3(dot(p, float3(0.5, 0.5, 0.0)), dot(p, float3(1.6137, -3.3234, 1.7097)), dot(p, float3(4.3780, -4.2455, -0.1325)));
}
float3 fb_ictcp_to_rgb(float3 i) {
    float3 p = float3(dot(i, float3(1.0, 0.00860514569398152, 0.11103560447547328)),
                      dot(i, float3(1.0, -0.00860514569398152, -0.11103560447547328)),
                      dot(i, float3(1.0, 0.56004885956263900, -0.32063747023212210)));
    return mul(FB_LMS_TO_709, pq_decode(p));
}
float3 range_compress(float3 x, float t) {
    return (x < t) ? x : t + (1.0 - t) * (1.0 - exp(-(x - t) / (1.0 - t)));
}
float3 tonemap_frostbite(float3 col) {
    float3 ictcp = fb_rgb_to_ictcp(col);
    float sat = pow(smoothstep(1.0, 0.3, ictcp.x), 1.3);
    col = fb_ictcp_to_rgb(ictcp * float3(1.0, sat, sat));
    float max_col = max(col.r, max(col.g, col.b));
    float3 hue_preserving = col * (range_compress(max_col.xxx, 0.25).x / max(max_col, 1e-6));
    col = lerp(range_compress(col, 0.25), hue_preserving, 0.6);
    float3 mapped = fb_rgb_to_ictcp(col);
    float boost = 0.3 * smoothstep(1.0, 0.5, ictcp.x);
    mapped.yz = lerp(mapped.yz, ictcp.yz * mapped.x / max(1e-3, ictcp.x), boost);
    return saturate(fb_ictcp_to_rgb(mapped));
}

// ---------------------------------------------------------------------------------------------
// ACES: Stephen Hill's fit of the RRT + sRGB ODT (BakingLab ACES.hlsl). Pre-scaled x1.462.
float3 tonemap_aces(float3 c) {
    const float3x3 in_m = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 out_m = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    float3 v = mul(in_m, c * 1.462);
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return saturate(mul(out_m, v));
}

// Reinhard x / (1 + x), per channel. Pre-scaled x1.22.
float3 tonemap_reinhard(float3 c) { c *= 1.22; return c / (1.0 + c); }

// AgX, minimal version (Wrensch; as in three.js / Filament), Rec.2020 working space. Pre-scaled x0.808.
float3 tonemap_agx(float3 c) {
    const float3x3 inset = float3x3(0.856627153315983, 0.137318972929847, 0.11189821299995,
                                    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
                                    0.0482516061458583, 0.101439036467562, 0.811302368396859);
    const float3x3 outset = float3x3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
                                     -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
                                     -0.016493938717834573, -0.016493938717834257, 1.2519364065950405);
    const float min_ev = -12.47393, max_ev = 4.026069;
    c = mul(transpose(inset), mul(M_709_TO_2020, c * 0.808));   // source matrices are GLSL column-major
    c = saturate((clamp(log2(max(c, 1e-10)), min_ev, max_ev) - min_ev) / (max_ev - min_ev));
    float3 x2 = c * c, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    c = pow(max(mul(transpose(outset), c), 0.0), 2.2);
    return saturate(mul(M_2020_TO_709, c));
}

float3 tonemap_operator(uint op, float3 x) {
    x = max(x, 0.0);
    switch (op) {
    case TM_GT: return tonemap_gt(x);
    case TM_FROSTBITE: return tonemap_frostbite(x);
    case TM_ACES: return tonemap_aces(x);
    case TM_REINHARD: return tonemap_reinhard(x);
    case TM_LINEAR: return saturate(x);
    case TM_AGX: return tonemap_agx(x);
    default: return tonemap_gt7(x);
    }
}

// ---------------------------------------------------------------------------------------------
// 3D LUT addressing (research note 06 section 4). An HDR input spans ~20 stops, so uniform indexing
// would spend almost every texel on highlights. GT7's pow(4) shaper (slide 173): x = X_MAX * u^4.
// 64^3 RGBA16F, X_MAX = 64 exposed (~8.5 EV above mid grey), storing sRGB-encoded output.
static const float LUT_SIZE = 64.0;
static const float LUT_MAX = 64.0;
float3 lut_shaper_encode(float3 x) { return sqrt(sqrt(saturate(x * (1.0 / LUT_MAX)))); }
float3 lut_shaper_decode(float3 u) { float3 u2 = u * u; return LUT_MAX * u2 * u2; }
float3 lut_uvw(float3 u) { return u * ((LUT_SIZE - 1.0) / LUT_SIZE) + 0.5 / LUT_SIZE; }   // texel centres

#endif
