// Debug visualizations (see DebugView in renderer.h). Output is display-referred (sRGB target).
#include "common.hlsli"
#include "gi.hlsli"   // probe grid (b4, t24) and GTAO (t25)
#include "shadow.hlsli"

Texture2DMS<float> t_depth : register(t0);
Texture2DMS<float4> t_albedo : register(t1);
Texture2DMS<float4> t_normal : register(t2);
Texture2DMS<float4> t_material : register(t3);
Texture2DMS<float2> t_motion : register(t4);
Texture2DMS<uint2> t_stencil : register(t5);
Texture2D<float> t_edges : register(t6);
Texture2DMS<float4> t_diffuse : register(t7);
Texture2DMS<float4> t_specular : register(t11);
Texture2D<uint> t_overdraw : register(t12);
StructuredBuffer<float> t_exposure : register(t13);

float3 heat(float t) {  // 0..1 -> blue, cyan, green, yellow, red
    t = saturate(t);
    return saturate(float3(1.5 - abs(4 * t - 3), 1.5 - abs(4 * t - 2), 1.5 - abs(4 * t - 1)));
}

// Shadow texels per 1080p-reference pixel -> colour. Target ("1080p + 8%"): >= 1.08.
float3 density_color(float d) {
    if (d < 1.0) return lerp(float3(0.3, 0.0, 0.0), float3(1.0, 0.1, 0.05), saturate(d));       // undersampled
    if (d < 1.08) return float3(1.0, 0.85, 0.1);                                                  // within 8%
    if (d < 2.0) return lerp(float3(0.2, 0.9, 0.25), float3(0.1, 0.55, 0.2), (d - 1.08) / 0.92); // rule met
    return lerp(float3(0.25, 0.5, 1.0), float3(0.6, 0.15, 0.9), saturate((d - 2.0) / 6.0));      // oversampled (waste)
}

// Density of the shadow map that shades this point, measured like the sizing rule
// (docs/research/03-shadows.md 2.4): a 1080p-reference pixel covers `pixel * zc` metres at view
// depth zc; a local-light texel covers `delta * zL` at light depth zL, a cascade texel a constant
// size. Local lights (worst projector covering the point) are shown at full brightness, the sun
// cascades dimmed.
float3 shadow_density_view(float3 P, float zc, float3 n) {
    float pixel = 2.0 / (proj[1][1] * 1080.0);
    float best = 1e9;
    uint count = (uint)shadow_filter.w;
    [loop] for (uint k = 0; k < count; k++) {
        ShadowProjector sp = shadow_projectors[k];
        if (sp.info.y < 0.5) continue;
        float3 dv = P - sp.pos_texel.xyz;
        if (dot(dv, dv) > sp.info.x * sp.info.x) continue;
        float zl = dot(dv, sp.axis_cone.xyz);
        float limit = sp.axis_cone.w > 0 ? sp.axis_cone.w * length(dv) : max(max(abs(dv.x), abs(dv.y)), abs(dv.z)) * 0.9999;
        if (zl <= 0 || zl < limit) continue;
        best = min(best, pixel * zc / (sp.pos_texel.w * zl));
    }
    float shade = 0.65 + 0.35 * saturate(dot(n, normalize(float3(0.3, 0.8, 0.5))));
    if (best < 1e9) return density_color(best) * shade;
    if (shadow_params.w == 0) return 0.1 * shade;
    int cas = 0;
    for (int c = 0; c < 3; c++)
        if (c < (int)shadow_params.x - 1 && zc > cascade_splits[c]) cas = c + 1;
    if (zc > cascade_splits[(int)shadow_params.x - 1]) return 0.1 * shade;
    return density_color(pixel * zc / cascade_texel[cas]) * shade * 0.4;
}

float3 tonemap_debug(float3 x) {
    float e = auto_exposure > 0 ? t_exposure[0] * exposure : exposure;
    x *= e;
    return x / (1 + x);
}

float4 ps_main(FullscreenVSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float d = t_depth.Load(p, 0);
    uint stencil = t_stencil.Load(p, 0).g;
    float3 c = 0;
    switch (debug_view) {
    case 1: c = t_albedo.Load(p, 0).rgb; break;
    case 2: c = decode_normal_oct(t_normal.Load(p, 0).rg) * 0.5 + 0.5; break;
    case 3: c = t_albedo.Load(p, 0).aaa; break;
    case 4: c = float3(t_material.Load(p, 0).g, t_material.Load(p, 0).b * 0.5, 0); break;
    case 5: {
        float2 m = t_motion.Load(p, 0) * screen_size.xy;  // pixels per frame
        c = float3(saturate(abs(m) / 8.0), 0) + (d == 0 ? 0.1 : 0);
        break;
    }
    case 6: c = d > 0 ? frac(log2(linear_depth(d)) * 0.5).xxx : float3(0.2, 0.3, 0.5); break;
    case 7: {
        static const float3 cols[5] = {float3(0.1, 0.15, 0.3), float3(0.6, 0.6, 0.6), float3(0.9, 0.5, 0.4),
                                       float3(0.3, 0.8, 0.3), float3(0.3, 0.5, 1.0)};
        c = cols[min(stencil & 7, 4)];
        break;
    }
    case 8: {
        c = t_albedo.Load(p, 0).rgb * 0.3;
        if (stencil & 8) c = float3(1, 0.2, 0.1);
        break;
    }
    case 9: c = tonemap_debug(t_diffuse.Load(p, 0).rgb); break;
    case 10: c = tonemap_debug(t_specular.Load(p, 0).rgb); break;
    case 11: {
        float z = linear_depth(d);
        int cas = 0;
        for (int k = 0; k < 3; k++)
            if (k < (int)shadow_params.x - 1 && z > cascade_splits[k]) cas = k + 1;
        static const float3 cc[4] = {float3(1, 0.3, 0.3), float3(0.3, 1, 0.3), float3(0.3, 0.3, 1), float3(1, 1, 0.3)};
        c = d > 0 ? lerp(t_albedo.Load(p, 0).rgb, cc[cas], 0.5) : 0;
        if (z > cascade_splits[(int)shadow_params.x - 1]) c *= 0.3;
        break;
    }
    case 12: {
        // quad invocations at this 2x2 quad (12 = one quad), shown as a heat map: 1 = blue .. 8+ = red
        float quads = t_overdraw.Load(int3(p / 2, 0)) / 12.0;
        c = quads > 0 ? heat((quads - 1) / 7.0) : 0;
        break;
    }
    case 13: c = d > 0 ? float3(0.2, 0.8, 0.3) * (0.4 + 0.6 * frac(linear_depth(d) * 0.1)) : 0; break;
    case 14: {
        // Indirect irradiance alone, shown as the radiance of a white diffuse surface (like the
        // "Diffuse light" view): probes when baked, else the sky ambient cube.
        if (d == 0) break;
        float3 pos = world_from_depth(i.pos.xy * screen_size.zw, d);
        float3 n = decode_normal_oct(t_normal.Load(p, 0).rg);
        c = tonemap_debug(indirect_light(pos, n, normalize(camera_pos.xyz - pos), 1.0).irradiance / PI);
        break;
    }
    case 15: c = d > 0 ? gtao_visibility(p).xxx : 0; break;
    case 16: {
        float3 P = world_from_depth(i.pos.xy * screen_size.zw, d);
        c = d > 0 ? shadow_density_view(P, linear_depth(d), decode_normal_oct(t_normal.Load(p, 0).rg)) : 0.05;
        break;
    }
    }
    return float4(c, 1);
}
