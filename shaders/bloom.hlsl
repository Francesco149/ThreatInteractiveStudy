// Bloom, Call of Duty: Advanced Warfare style (Jimenez 2014, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare", SIGGRAPH 2014 Advances). Non-thresholded: the whole HDR scene
// blooms by its own intensity, and the tone-map pass mixes it in with a small lerp weight.
//   cs_downsample_first  full res (MSAA) -> half res: 13-tap filter with a partial Karis average
//   ps_downsample        13-tap filter, half res -> ... -> 1/64 res (6 levels)
//   ps_upsample          3x3 tent of the smaller level, added (blend ONE/ONE) onto the next larger
//                        level, which still holds its own downsample: E' = E, D' = D + blur(E'), ...
// Filtering on the way down *and* up is what keeps it stable: a plain bilinear chain aliases and
// pulses under motion (Jimenez, citing Mittring 2012).
#include "common.hlsli"

Texture2DMS<float4> t_scene : register(t0);   // HDR scene colour (MSAA), first downsample only
Texture2D<float4> t_src : register(t1);       // the previous level

cbuffer BloomCB : register(b1) {
    float2 src_texel;   // 1 / size of the source level
    float2 bloom_pad;
};

float karis_weight(float3 c) { return 1.0 / (1.0 + luminance(c)); }

// One full-resolution texel, its MSAA samples averaged (a box resolve, fine for a blur input).
// Clamped to the fp16 range: one overflowed specular sample (inf) or NaN would otherwise spread
// over the whole screen. D3D11 min/max return the non-NaN operand, so NaN becomes 0.
float3 load_scene(int2 p) {
    p = clamp(p, int2(0, 0), int2(screen_size.xy) - 1);
    float3 c = 0;
    for (uint s = 0; s < msaa_samples; s++) c += min(max(t_scene.Load(p, s).rgb, 0.0), 65504.0);
    return c / msaa_samples;
}

// The 13 bilinear taps of the CoD:AW downsample sit on texel corners, so each one is the average of
// a 2x2 block of source texels: with an MSAA source (no hardware filtering) the filter is evaluated
// from the 6x6 texels under its footprint. The taps form five overlapping 2x2 groups: the centre
// group (weight 0.5) and four corner groups (0.125 each). The Karis average (1 / (1 + luma)) is
// applied per group, only on this first downsample: it removes the fireflies a single very bright
// sample would otherwise turn into a pulsing blob, without darkening large bright areas.
// Compute shader: an 8x8 group of outputs needs a 20x20 tile of full-res texels, loaded (and
// MSAA-resolved) once into groupshared memory: ~12 sample loads per output instead of 72 for a pixel
// shader that loads its whole 6x6 footprint (see docs/techniques/tonemap-sky-bloom.md for timings).
RWTexture2D<float4> u_level0 : register(u0);
groupshared float3 g_tile[20][20];

// Bilinear tap at an offset of (ox, oy) source texels from the output's centre = the average of the
// 2x2 texels around that corner: local [2 + o, 3 + o] from `base` (the output's 6x6 footprint).
float3 tap(int2 base, int ox, int oy) {
    int x = base.x + 2 + ox, y = base.y + 2 + oy;
    return 0.25 * (g_tile[y][x] + g_tile[y][x + 1] + g_tile[y + 1][x] + g_tile[y + 1][x + 1]);
}

[numthreads(8, 8, 1)]
void cs_downsample_first(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    int2 tile_origin = int2(gid.xy) * 16 - 2;
    for (uint n = gi; n < 400; n += 64) g_tile[n / 20][n % 20] = load_scene(tile_origin + int2(n % 20, n / 20));
    GroupMemoryBarrierWithGroupSync();
    uint2 out_pos = gid.xy * 8 + gtid.xy;
    uint2 out_size;
    u_level0.GetDimensions(out_size.x, out_size.y);
    if (any(out_pos >= out_size)) return;
    int2 base = int2(gtid.xy) * 2;   // the output's centre is the corner between local texels base+2 and base+3
    float3 a = tap(base, -2, -2), b = tap(base, 0, -2), c = tap(base, 2, -2);
    float3 d = tap(base, -2, 0), e = tap(base, 0, 0), f = tap(base, 2, 0);
    float3 g = tap(base, -2, 2), h = tap(base, 0, 2), k = tap(base, 2, 2);
    float3 j0 = tap(base, -1, -1), j1 = tap(base, 1, -1), j2 = tap(base, -1, 1), j3 = tap(base, 1, 1);
    float3 groups[5] = {(j0 + j1 + j2 + j3) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25,
                        (d + e + g + h) * 0.25, (e + f + h + k) * 0.25};
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int q = 0; q < 5; q++) {
        float w = (q == 0 ? 0.5 : 0.125) * karis_weight(groups[q]);
        sum += groups[q] * w;
        wsum += w;
    }
    u_level0[out_pos] = float4(sum / wsum, 1);
}

float3 src(float2 uv) { return t_src.SampleLevel(s_linear_clamp, uv, 0).rgb; }

float4 ps_downsample(FullscreenVSOut i) : SV_Target {
    float2 uv = i.uv, t = src_texel;
    float3 a = src(uv + t * float2(-2, -2)), b = src(uv + t * float2(0, -2)), c = src(uv + t * float2(2, -2));
    float3 d = src(uv + t * float2(-2, 0)), e = src(uv), f = src(uv + t * float2(2, 0));
    float3 g = src(uv + t * float2(-2, 2)), h = src(uv + t * float2(0, 2)), k = src(uv + t * float2(2, 2));
    float3 j = src(uv + t * float2(-1, -1)), l = src(uv + t * float2(1, -1));
    float3 m = src(uv + t * float2(-1, 1)), n = src(uv + t * float2(1, 1));
    float3 o = (j + l + m + n) * (0.5 / 4.0) +
               ((a + b + d + e) + (b + c + e + f) + (d + e + g + h) + (e + f + h + k)) * (0.125 / 4.0);
    return float4(o, 1);
}

// 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16, radius one source texel. Repeated tents converge to a
// Gaussian, and the sum over the levels to a wide, smooth falloff.
float4 ps_upsample(FullscreenVSOut i) : SV_Target {
    float2 uv = i.uv, r = src_texel;
    float3 s = src(uv) * 4.0;
    s += (src(uv + r * float2(-1, 0)) + src(uv + r * float2(1, 0)) + src(uv + r * float2(0, -1)) + src(uv + r * float2(0, 1))) * 2.0;
    s += src(uv + r * float2(-1, -1)) + src(uv + r * float2(1, -1)) + src(uv + r * float2(-1, 1)) + src(uv + r * float2(1, 1));
    return float4(s / 16.0, 1);
}
