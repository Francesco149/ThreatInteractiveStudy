// G-buffer decoding and the BRDF with dual material profiles.
#ifndef BRDF_HLSLI
#define BRDF_HLSLI
#include "common.hlsli"

Texture2DMS<float> t_depth : register(t0);
Texture2DMS<float4> t_gb_albedo : register(t1);
Texture2DMS<float4> t_gb_normal : register(t2);
Texture2DMS<float4> t_gb_material : register(t3);

struct Surface {
    float3 pos;
    float3 n;
    float3 v;            // towards the camera
    float3 albedo;
    float roughness;     // perceptual
    float specular;      // 0.5 = profile's nominal F0
    uint pair;
    float blend;         // 0 = profile a, 1 = profile b
    float ao;
    float view_depth;
    uint shading_model;
};

Surface load_surface(int2 p, uint s, float2 uv) {
    Surface o;
    float d = t_depth.Load(p, s);
    float4 a = t_gb_albedo.Load(p, s);
    float4 n = t_gb_normal.Load(p, s);
    float4 m = t_gb_material.Load(p, s);
    float2 pos_uv = uv;
    float pos_d = d;
#if !PER_SAMPLE
    // Lighting jitter compensation: the G-buffer holds the surface seen through the *jittered*
    // pixel; light it at the point seen through the unjittered pixel centre instead, so shadow and
    // light boundaries don't move by 1/8 px every frame. Reversed-Z device depth is linear in screen
    // space on planar surfaces, so the depth there is a first-order extrapolation. Only on continuous
    // surfaces (depth edges are MSAA/SMAA edges, which the 2-frame resolve blends anyway), and only
    // for per-pixel shading (derivatives are per pixel).
    if (texture_unjitter > 0) {
        float2 g = float2(jitter.x, -jitter.y) * screen_size.xy * 0.5;  // geometry shift, px, y down
        float dx = ddx_fine(d), dy = ddy_fine(d);
        if (abs(dx) + abs(dy) < 0.02 * d) {
            pos_d = d + dx * g.x + dy * g.y;
            pos_uv = uv + g * screen_size.zw;
        }
    }
#endif
    o.pos = world_from_depth(pos_uv, pos_d);
    o.view_depth = linear_depth(d);
    o.n = decode_normal_oct(n.rg);
    o.v = normalize(camera_pos.xyz - o.pos);
    o.albedo = a.rgb;
    o.roughness = a.a;
    o.specular = n.b;
    o.pair = (uint)(m.r * 255.0 + 0.5);
    o.blend = m.g;
    o.ao = m.b;
    o.shading_model = (uint)(m.a * 3.0 + 0.5);
    return o;
}

// GGX / Trowbridge-Reitz NDF, alpha = roughness^2.
float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * f * f);
}

// Height-correlated Smith visibility (Heitz 2014), includes the 1/(4 NoL NoV) term.
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gl = NoV * sqrt((-NoL * a2 + NoL) * NoL + a2);
    float gv = NoL * sqrt((-NoV * a2 + NoV) * NoV + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 F_Schlick(float3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

// Light contribution split in the two lighting buffers:
//   diffuse  = irradiance x diffuse weight (no albedo: it is applied in the compose pass, and the
//              albedo-free buffer is what auto exposure meters, as in Fox Engine)
//   specular = reflected radiance
struct LightResult {
    float3 diffuse;
    float3 specular;
};

LightResult eval_profile(ProfileData p, Surface s, float3 L, float3 radiance) {
    LightResult r;
    float3 H = normalize(L + s.v);
    float NoL_raw = dot(s.n, L);
    float NoL = saturate(NoL_raw);
    float NoV = max(dot(s.n, s.v), 1e-4);
    float NoH = saturate(dot(s.n, H));
    float VoH = saturate(dot(s.v, H));
    float a = max(s.roughness * s.roughness, 0.002);

    // Metals: glTF convention, the albedo is the specular colour. Dielectrics: F0 from the profile
    // (measured/IOR-derived), scaled by the G-buffer specular channel (0.5 = nominal).
    float3 f0 = p.metal > 0.5 ? s.albedo : p.f0 * (s.specular * 2.0);
    float3 F = F_Schlick(f0, VoH);
    float3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;

    // Wrapped diffuse for foliage/cloth-like profiles (energy-normalized wrap).
    float w = p.diffuse_wrap;
    float wrap = saturate((NoL_raw + w) / ((1 + w) * (1 + w)));
    float3 diff = (p.metal > 0.5 ? 0.0 : 1.0) * (1.0 - F) * wrap / PI;

    // Sheen: a grazing-angle retro-reflective lobe (cloth). Kept in the diffuse buffer: albedo-tinted.
    diff += p.sheen * pow(1.0 - NoV, 5.0) * NoL * 0.5;

    // Clearcoat: a second, smooth GGX lobe with F0 = 0.04 on top (lacquer, paint).
    if (p.clearcoat > 0) {
        float ac = max(p.clearcoat_rough * p.clearcoat_rough, 0.002);
        float Fc = F_Schlick(0.04, VoH).x * p.clearcoat;
        float coat = D_GGX(NoH, ac) * V_SmithGGXCorrelated(NoV, NoL, ac) * Fc;
        spec = spec * (1.0 - Fc) + coat;
        diff *= (1.0 - Fc);
    }
    r.diffuse = diff * radiance;
    r.specular = spec * NoL * radiance;
    return r;
}

// Dual material profiles: evaluate both profiles of the pair and blend the *results* (the blend
// channel replaces the classic metallic channel), so e.g. thinning paint over iron keeps both
// BRDFs intact instead of producing an in-between material that exists in neither.
LightResult eval_surface(Surface s, float3 L, float3 radiance) {
    ProfileData pa = profiles[s.pair * 2];
    LightResult r = eval_profile(pa, s, L, radiance);
    if (s.blend > 0.0) {
        ProfileData pb = profiles[s.pair * 2 + 1];
        LightResult rb = eval_profile(pb, s, L, radiance);
        r.diffuse = lerp(r.diffuse, rb.diffuse, s.blend);
        r.specular = lerp(r.specular, rb.specular, s.blend);
    }
    return r;
}

// Karis' analytic approximation of the split-sum environment BRDF (mobile, 2014).
float3 env_brdf_approx(float3 f0, float roughness, float NoV) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

#endif
