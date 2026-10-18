// Tone mapping + MSAA sample separation + velocity resolve (pass P0 in docs/research/07, pass T in
// docs/research/06 section 7). Per sample: mix in the bloom (un-exposed HDR, energy-conserving
// lerp), apply the camera matrix (exposure x white balance, one 3x3, so the LUT never depends on
// exposure), then the tone curve: one trilinear fetch of the baked 64^3 LUT (TONEMAP_LUT = 1), or
// the curve evaluated per sample (TONEMAP_LUT = 0, kept for the ALU vs LUT cost A/B).
// LAYERS = 2: each of the two MSAA samples is tone mapped into its own LDR layer, so SMAA S2x can
//             run on each sample layer separately (resolve happens after tone mapping, per sample).
//             Written sRGB-encoded through the UNORM view (the LUT already stores encoded values).
// LAYERS = 1: all samples are tone mapped and averaged into one layer (1x, 4x MSAA, plain resolve),
//             averaged in linear and written through the SRGB view.
#include "common.hlsli"
#include "tonemap_ops.hlsli"

#ifndef LAYERS
#define LAYERS 1
#endif
#ifndef TONEMAP_LUT
#define TONEMAP_LUT 1
#endif

Texture2DMS<float4> t_scene : register(t0);     // HDR scene colour (MSAA), a = responsive mask
Texture2DMS<float2> t_motion : register(t1);    // G-buffer motion (MSAA)
Texture2DMS<float> t_depth : register(t2);
StructuredBuffer<float> t_exposure : register(t3);         // metered exposure (auto exposure)
StructuredBuffer<float4> t_white_balance : register(t4);   // rows 0-2: white balance matrix (exposure.hlsl)
Texture3D<float4> t_tone_lut : register(t5);               // baked tone curve, sRGB-encoded (tonemap_lut.hlsl)
Texture2D<float4> t_bloom : register(t6);                  // half-res bloom, sum of all levels (bloom.hlsl)

cbuffer TonemapCB : register(b1) {
    uint tonemapper;
    float bloom_intensity;   // lerp weight of the bloom (0 = off)
    float bloom_normalize;   // 1 / number of bloom levels: the bloom texture holds their sum
    float tm_pad;
};

float exposure_value() {
    return auto_exposure > 0 ? t_exposure[0] * exposure : exposure;
}

// Exposed linear Rec.709 -> sRGB-encoded display value.
float3 tonemap_encoded(float3 x) {
#if TONEMAP_LUT
    return t_tone_lut.SampleLevel(s_linear_clamp, lut_uvw(lut_shaper_encode(x)), 0).rgb;
#else
    return linear_to_srgb(tonemap_operator(tonemapper, x));
#endif
}

// --- Velocity: G-buffer motion for geometry, camera-only reprojection for sky samples ----------

float2 sample_velocity(int2 p, uint s, float2 uv) {
    float d = t_depth.Load(p, s);
    if (d > 0) return t_motion.Load(p, s);
    // Sky: a direction at infinity. Reproject the view direction with the unjittered matrices.
    float4 ndc = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    float4 w = mul(inv_view_proj, float4(ndc.xy, 1e-7, 1));
    float3 dir = normalize(w.xyz / w.w - camera_pos.xyz);
    float4 cur = mul(view_proj_nojitter, float4(camera_pos.xyz + dir * 1e5, 1));
    float4 prev = mul(prev_view_proj_nojitter, float4(camera_pos.xyz + dir * 1e5, 1));
    return (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5, -0.5);
}

struct TonemapOut {
    float4 layer0 : SV_Target0;
#if LAYERS == 2
    float4 layer1 : SV_Target1;
    float2 velocity : SV_Target2;
    float4 mask : SV_Target3;      // r = MSAA edge (samples differ), g = responsive
#else
    float2 velocity : SV_Target1;
    float4 mask : SV_Target2;
#endif
};

TonemapOut ps_main(FullscreenVSOut i) {
    int2 p = int2(i.pos.xy);
    float3x3 camera = float3x3(t_white_balance[0].xyz, t_white_balance[1].xyz, t_white_balance[2].xyz) * exposure_value();
    float3 bloom = 0;
    if (bloom_intensity > 0) bloom = t_bloom.SampleLevel(s_linear_clamp, i.uv, 0).rgb * bloom_normalize;
    TonemapOut o;
#if LAYERS == 2
    float4 h0 = t_scene.Load(p, 0), h1 = t_scene.Load(p, 1);
    float3 c0 = tonemap_encoded(mul(camera, lerp(h0.rgb, bloom, bloom_intensity)));
    float3 c1 = tonemap_encoded(mul(camera, lerp(h1.rgb, bloom, bloom_intensity)));
    o.layer0 = float4(c0, 0);
    o.layer1 = float4(c1, 0);
    o.velocity = 0.5 * (sample_velocity(p, 0, i.uv) + sample_velocity(p, 1, i.uv));
    // Edge test on the gamma-encoded values: the two samples differ visibly -> geometric edge.
    o.mask = float4(any(abs(c0 - c1) > 2.0 / 255.0) ? 1 : 0, max(h0.a, h1.a), 0, 0);
#else
    float3 sum = 0;
    float2 vel = 0;
    float responsive = 0;
    for (uint s = 0; s < msaa_samples; s++) {
        float4 h = t_scene.Load(p, s);
        // Resolve after tone mapping (no fireflies across edges), in linear.
        sum += srgb_to_linear(tonemap_encoded(mul(camera, lerp(h.rgb, bloom, bloom_intensity))));
        vel += sample_velocity(p, s, i.uv);
        responsive = max(responsive, h.a);
    }
    o.layer0 = float4(sum / msaa_samples, 0);
    o.velocity = vel / msaa_samples;
    o.mask = float4(0, responsive, 0, 0);
#endif
    return o;
}
