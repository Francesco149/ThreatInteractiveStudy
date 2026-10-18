// Auto exposure and auto white balance from the DIFFUSE-ONLY lighting buffer (no albedo, no
// emissive, no sky): what Fox Engine meters (GDC 2013), so dark materials or a bright sign don't
// push the exposure around. Diffuse lighting D is the radiance of a white Lambertian surface, so an
// 18% grey surface reads 0.18*D: exposure = 1 / average(D) maps it to 0.18.
// The same buffer is the illuminant for white balance: with albedo removed, its mean colour IS the
// light colour (direct + bounce), so no grey-world assumption is needed (research note 06 section 6).
#include "common.hlsli"

Texture2DMS<float4> t_diffuse : register(t0);
Texture2DMS<float> t_depth : register(t1);
RWByteAddressBuffer u_histogram : register(u0);         // 256 uints
RWStructuredBuffer<float> u_exposure : register(u1);    // [0] = current exposure
RWStructuredBuffer<float4> u_group_sums : register(u2); // per histogram group: sum(w * rgb), sum(w)
RWStructuredBuffer<float4> u_white_balance : register(u3); // [0..2] matrix rows, [3] state (mired, tint.xy, initialised)

cbuffer ExposureCB : register(b1) {
    float awb_strength;        // 0 = off .. 1 = full correction towards D65
    float awb_min_mired;       // measured light temperature is clamped to [max K, min K] in mireds
    float awb_max_mired;
    float wb_warmth_mired;     // artist bias, applied in full even with AWB off (+ = warmer picture)
};

static const float MIN_LOG2 = -10.0, RANGE_LOG2 = 24.0;
groupshared uint g_hist[256];
groupshared float4 g_sum[256];

uint groups_x() { return ((uint)screen_size.x / 4 + 15) / 16; }
uint groups_y() { return ((uint)screen_size.y / 4 + 15) / 16; }

[numthreads(16, 16, 1)]
void cs_histogram(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    g_hist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    float4 sum = 0;
    uint2 p = id.xy * 4 + 2;   // 1/16 of the pixels is plenty for metering
    if (p.x < (uint)screen_size.x && p.y < (uint)screen_size.y) {
        float d = t_depth.Load(int2(p), 0);
        float3 light = t_diffuse.Load(int2(p), 0).rgb;
        float lum = luminance(light);
        if (d > 0 && lum > 1e-6) {
            // Centre-weighted: pixels near the middle of the screen count more.
            float2 c = (float2(p) * screen_size.zw - 0.5) * 2.0;
            uint bin = (uint)(saturate((log2(lum) - MIN_LOG2) / RANGE_LOG2) * 254.0) + 1;
            uint weight = (uint)(lerp(4.0, 1.0, saturate(dot(c, c))) + 0.5);
            InterlockedAdd(g_hist[bin], weight);
            sum = float4(light * weight, weight);
        }
    }
    // White balance: sum the light colour over the group (no float atomics in SM 5.0).
    g_sum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) g_sum[gi] += g_sum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) u_group_sums[gid.y * groups_x() + gid.x] = g_sum[0];
    if (g_hist[gi] > 0) u_histogram.InterlockedAdd(gi * 4, g_hist[gi]);
}

// ---------------------------------------------------------------------------------------------
// White balance (research note 06 section 6): light colour -> CCT (McCamy 1992) + tint off the
// Planckian locus, clamped to a camera-like temperature range, adapted slowly in mireds, then a
// partial von Kries adaptation to D65 in Bradford cone space, as one 3x3 matrix in Rec.709.

static const float3x3 M_709_TO_XYZ = float3x3(0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750,
                                              0.0193339, 0.1191920, 0.9503041);
static const float3x3 M_XYZ_TO_709 = float3x3(3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560,
                                              0.0556434, -0.2040259, 1.0572252);
static const float3x3 BRADFORD = float3x3(0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296);
static const float3x3 BRADFORD_INV = float3x3(0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603, 0.0492912,
                                              -0.0085287, 0.0400428, 0.9684867);
static const float2 D65_XY = float2(0.3127, 0.3290);
static const float D65_MIRED = 1e6 / 6504.0;
static const float MAX_TINT = 0.01;       // strongly green/magenta light is only partly corrected
static const float AWB_SPEED = 1.0;       // 1/s: slow, like an eye or a camera's AWB

// Planckian locus, Kim et al. 2002 cubic fit (1667..25000 K).
float2 planckian_xy(float T) {
    float t = 1000.0 / T, t2 = t * t, t3 = t2 * t;
    float x = (T <= 4000.0) ? (-0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t + 0.179910)
                            : (-3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t + 0.240390);
    float x2 = x * x, x3 = x2 * x;
    float y = (T <= 2222.0) ? (-1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683)
            : (T <= 4000.0) ? (-0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867)
                            : (3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483);
    return float2(x, y);
}

float3 lms_of_xy(float2 xy) { return mul(BRADFORD, float3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y)); }

groupshared float g_count[256];

[numthreads(256, 1, 1)]
void cs_adapt(uint gi : SV_GroupIndex) {
    uint count = u_histogram.Load(gi * 4);
    u_histogram.Store(gi * 4, 0);  // reset for next frame
    g_count[gi] = gi == 0 ? 0 : (float)count;
    float4 s = 0;
    uint num_groups = groups_x() * groups_y();
    for (uint g = gi; g < num_groups; g += 256) s += u_group_sums[g];
    g_sum[gi] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 128; k > 0; k >>= 1) {
        if (gi < k) g_sum[gi] += g_sum[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi != 0) return;

    // --- Exposure: average log2 luminance between the 40th and 95th percentiles (ignores deep
    // shadows and small highlights).
    float total = 0;
    for (uint b = 1; b < 256; b++) total += g_count[b];
    float lo = total * 0.40, hi = total * 0.95, acc = 0, sum = 0, n = 0;
    for (uint b2 = 1; b2 < 256; b2++) {
        float c = g_count[b2];
        float take = max(0.0, min(acc + c, hi) - max(acc, lo));
        float log_lum = MIN_LOG2 + ((b2 - 1) + 0.5) / 254.0 * RANGE_LOG2;
        sum += take * log_lum;
        n += take;
        acc += c;
    }
    float avg = n > 0 ? exp2(sum / n) : 1.0;
    float target = 1.0 / max(avg, 1e-4);
    float cur = u_exposure[0];
    if (!(cur > 0) || cur > 1e6) cur = target;
    if (n == 0) target = cur;   // nothing metered (only sky on screen): hold, like the white balance
    // Adapt in log space; brightening is slower than darkening (eyes adapt to light faster).
    float speed = target > cur ? 1.2 : 2.5;
    float l = lerp(log2(cur), log2(target), 1.0 - exp(-delta_time * speed));
    u_exposure[0] = exp2(l);

    // --- White balance. Measured light: luminance-weighted mean colour, plus a faint grey
    // "virtual light" that only matters in near darkness (GT7 slide 222).
    float4 state = u_white_balance[3];
    float3 light = g_sum[0].rgb / max(g_sum[0].w, 1e-6) + 1e-4;
    float3 XYZ = mul(M_709_TO_XYZ, light);
    float2 xy = XYZ.xy / max(XYZ.x + XYZ.y + XYZ.z, 1e-6);
    float mc = (xy.x - 0.3320) / (0.1858 - xy.y);                                   // McCamy 1992
    float cct = clamp(((449.0 * mc + 3525.0) * mc + 6823.3) * mc + 5520.33, 1667.0, 25000.0);
    float2 tint = xy - planckian_xy(cct);
    tint *= min(1.0, MAX_TINT / max(length(tint), 1e-6));
    float target_mired = clamp(1e6 / cct, awb_min_mired, awb_max_mired);
    if (g_sum[0].w <= 0 && state.w > 0) {   // nothing metered (only sky / emissive on screen): hold
        target_mired = state.x;
        tint = state.yz;
    }
    if (state.w == 0) state = float4(target_mired, tint, 1);   // first frame: snap
    float blend = 1.0 - exp(-delta_time * AWB_SPEED);
    state.x += (target_mired - state.x) * blend;
    state.yz += (tint - state.yz) * blend;

    // Von Kries in Bradford LMS: gains that map the assumed illuminant to D65, scaled by strength,
    // times the artist bias (the shift from D65 along the locus by wb_warmth_mired: assuming a
    // bluer light than measured makes the picture warmer).
    float3 d65 = lms_of_xy(D65_XY);
    float3 gain = lerp(1.0, d65 / lms_of_xy(planckian_xy(1e6 / state.x) + state.yz), awb_strength);
    float2 biased = D65_XY + planckian_xy(1e6 / max(D65_MIRED - wb_warmth_mired, 10.0)) - planckian_xy(6504.0);
    gain *= d65 / lms_of_xy(biased);
    float3x3 D = float3x3(gain.x, 0, 0, 0, gain.y, 0, 0, 0, gain.z);
    float3x3 wb = mul(M_XYZ_TO_709, mul(BRADFORD_INV, mul(D, mul(BRADFORD, M_709_TO_XYZ))));
    u_white_balance[0] = float4(wb[0], 0);
    u_white_balance[1] = float4(wb[1], 0);
    u_white_balance[2] = float4(wb[2], 0);
    u_white_balance[3] = state;
}
