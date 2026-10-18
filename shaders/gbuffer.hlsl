// G-buffer base pass. Permutations: ALPHA_TEST (alpha-tested foliage etc.), DITHER (LOD cross-fade).
// Shaders without discard keep early depth/stencil testing; that's why these are compile-time
// permutations instead of runtime branches.
#include "geometry.hlsli"

#ifndef ALPHA_TEST
#define ALPHA_TEST 0
#endif
#ifndef DITHER
#define DITHER 0
#endif

Texture2D t_albedo : register(t0);
Texture2D t_normal : register(t1);
Texture2D t_mr : register(t2);       // glTF: g = roughness, b = metallic (here: profile blend)
Texture2D t_ao : register(t3);
Texture2D t_emissive : register(t4);

struct VSIn {
    float3 pos : POSITION;
    float4 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    uint draw_id : DRAWID;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float4 clip_cur : TEXCOORD1;    // unjittered, for motion vectors
    float4 clip_prev : TEXCOORD2;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint flags : FLAGS;
    nointerpolation float fade : FADE;
};

VSOut vs_main(VSIn v) {
    DrawData d = draws[v.draw_id];
    VSOut o;
    float3 wp = world_position(d, v.pos, time);
    o.pos = mul(view_proj, float4(wp, 1));
    float3 n = v.normal.xyz * 2 - 1;
    float3 t = v.tangent.xyz * 2 - 1;
    o.normal = transform_normal(d.world0, d.world1, d.world2, n);
    o.tangent = float4(transform_dir(d.world0, d.world1, d.world2, t), v.tangent.w > 0.5 ? 1 : -1);
    o.uv = v.uv;
    o.clip_cur = mul(view_proj_nojitter, float4(wp, 1));
    o.clip_prev = mul(prev_view_proj_nojitter, float4(prev_world_position(d, v.pos, prev_time), 1));
    o.material = d.material;
    o.flags = d.flags;
    o.fade = d.lod_fade;
    return o;
}

struct GBuffer {
    float4 albedo : SV_Target0;     // rgb albedo (sRGB target), a roughness
    float4 normal : SV_Target1;     // rg octahedral normal, b specular, a flags
    float4 material : SV_Target2;   // r profile pair id, g profile blend, b AO, a shading model
    float2 motion : SV_Target3;     // uv(current) - uv(previous)
    float4 emissive : SV_Target4;   // only written by emissive materials (blend state masks it otherwise)
};

// Geometric specular anti-aliasing: widen the NDF by the screen-space variance of the normal
// (Tokuyoshi & Kaplanyan, "Improved Geometric Specular Antialiasing", I3D 2019).
float specular_aa_roughness(float3 n, float roughness) {
    const float SIGMA2 = 0.15915494;  // 1 / (2 pi)
    const float KAPPA = 0.18;
    float3 dndu = ddx(n), dndv = ddy(n);
    float kernel_a2 = min(2.0 * SIGMA2 * (dot(dndu, dndu) + dot(dndv, dndv)), KAPPA);
    float a = roughness * roughness;
    float a2 = saturate(a * a + kernel_a2);
    return sqrt(sqrt(a2));
}

GBuffer ps_main(VSOut i, bool front : SV_IsFrontFace) {
    MaterialData m = materials[i.material];

#if DITHER
    // LOD cross-fade: the outgoing LOD keeps the pixels where dither >= fade, the incoming LOD
    // the others. The pattern alternates every frame (2-position 2x2 Bayer) for the 2-frame TAA.
    float dither = bayer2x2_dither(i.pos.xy, frame_index);
    bool fade_in = (i.flags & 2) != 0;
    if (fade_in ? (dither >= i.fade) : (dither < i.fade)) discard;
#endif

    // Texture jitter compensation: the camera jitter (temporal AA) moves the geometry by up to
    // 1/8 px each frame; evaluating the UV at the *unjittered* pixel centre keeps texture detail
    // perfectly still. That matters here because the 2-frame resolve only blends edge pixels:
    // jittered texels in the interior would crawl at 30 Hz. EvaluateAttributeSnapped offsets are
    // in 1/16 px on a [-8, 7] grid; our jitters (2/16 and 4/16 px) are exact. The geometry shift is
    // (j.x, -j.y) px for a projection jitter j (y up), which is the offset that undoes it.
    // (Filmic SMAA, Jimenez 2016, "unjitter the texture coordinates"; docs/research/07 section 7.)
    float2 uv = i.uv;
    if (texture_unjitter > 0) {
        float2 j = jitter.xy * screen_size.xy * 0.5;
        uv = EvaluateAttributeSnapped(i.uv, int2(round(16.0 * j.x), round(-16.0 * j.y)));
    }

    float4 albedo = m.base_color;
    if (m.flags & 1) albedo *= t_albedo.Sample(s_aniso, uv);
#if ALPHA_TEST
    // The opacity fetch stays jittered: cutout silhouettes are edges, they *should* move with the
    // samples so the temporal resolve supersamples them.
    float alpha = m.base_color.a * ((m.flags & 1) ? t_albedo.Sample(s_aniso, i.uv).a : 1.0);
    if (alpha < m.alpha_cutoff) discard;
#endif

    float roughness = m.roughness;
    float blend = m.metallic;
    if (m.flags & 4) {
        float4 mr = t_mr.Sample(s_aniso, uv);
        roughness *= mr.g;
        blend *= mr.b;
    }
    float ao = (m.flags & 8) ? t_ao.Sample(s_aniso, uv).r : 1.0;

    float3 n = normalize(i.normal);
    if (!front) n = -n;  // two-sided materials
    if (m.flags & 2) {
        float3 t = normalize(i.tangent.xyz - n * dot(n, i.tangent.xyz));
        float3 b = cross(n, t) * i.tangent.w * (front ? 1 : -1);
        float3 tn = t_normal.Sample(s_aniso, uv).xyz * 2 - 1;  // glTF: OpenGL convention (+Y up)
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
    roughness = specular_aa_roughness(n, max(roughness, 0.02));

    GBuffer o;
    o.albedo = float4(albedo.rgb, roughness);
    o.normal = float4(encode_normal_oct(n), 0.5, 0);
    o.material = float4(m.profile_pair / 255.0, blend, ao, m.shading_model / 3.0);
    float2 cur = i.clip_cur.xy / i.clip_cur.w;
    float2 prev = i.clip_prev.xy / i.clip_prev.w;
    o.motion = (cur - prev) * float2(0.5, -0.5);
    float3 e = m.emissive;
    if (m.flags & 16) e *= t_emissive.Sample(s_aniso, uv).rgb;
    o.emissive = float4(e, 0);
    return o;
}
