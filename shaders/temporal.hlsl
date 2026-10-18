// SMAA T2x-style two-frame temporal resolve (docs/research/07 section 10.5): blends this frame's
// SMAA output with LAST FRAME'S SMAA OUTPUT (not with an accumulated history), point-sampled at
// the reprojected position (narrow resolve). Velocity weighting from SMAA; responsive mask and an
// edge-only mask so interior texture detail is never blended (TSCMAA idea, with our MSAA edges).
#include "common.hlsli"

Texture2D<float4> t_cur : register(t0);        // smaa_out[cur] (sRGB view -> linear)
Texture2D<float4> t_prev : register(t1);       // smaa_out[prev]
Texture2D<float2> t_vel : register(t2);        // velocity1x, uv units, current - previous
Texture2D<float4> t_mask_cur : register(t3);   // r msaa edge, g responsive, b smaa touched
Texture2D<float4> t_mask_prev : register(t4);

cbuffer TemporalCB : register(b1) {
    float4 rt_metrics;      // 1/w, 1/h, w, h
    float weight_scale;     // 30 (SMAA_REPROJECTION_WEIGHT_SCALE)
    float history_valid;    // 0 on the first frame / mode switch
    float edge_only;        // 1 = blend only on MSAA or SMAA edges
    float clamp_speed_px;   // motion-gated neighbourhood clamp; <= 0 disables
};

float4 ps_main(FullscreenVSOut i) : SV_Target {
    int3 p = int3(i.pos.xy, 0);
    float4 cur = t_cur.Load(p);
    float2 v = t_vel.Load(p);
    float2 puv = i.uv - v;
    float4 prev = t_prev.SampleLevel(s_point_clamp, puv, 0);

    float delta = abs(cur.a * cur.a - prev.a * prev.a) / 5.0;
    float w = 0.5 * saturate(1.0 - sqrt(delta) * weight_scale);

    float4 mc = t_mask_cur.Load(p);
    float4 mp = t_mask_prev.SampleLevel(s_point_clamp, puv, 0);
    w *= 1.0 - max(mc.g, mp.g);
    float edge = max(max(mc.r, mc.b), max(mp.r, mp.b));
    w *= lerp(1.0, edge, edge_only);
    w *= history_valid * (all(puv == saturate(puv)) ? 1.0 : 0.0);

    [branch] if (w > 0.0 && clamp_speed_px > 0.0) {
        float3 mn = cur.rgb, mx = cur.rgb;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x) {
                float3 c = t_cur.Load(p, int2(x, y)).rgb;
                mn = min(mn, c);
                mx = max(mx, c);
            }
        float speed = length(v * rt_metrics.zw);
        prev.rgb = lerp(prev.rgb, clamp(prev.rgb, mn, mx), saturate(speed / clamp_speed_px));
    }
    return float4(lerp(cur.rgb, prev.rgb, w), 1.0);
}

// Plain copy (AA off / SMAA without temporal): sRGB view in, sRGB view out.
float4 ps_copy(FullscreenVSOut i) : SV_Target { return t_cur.Load(int3(i.pos.xy, 0)); }
