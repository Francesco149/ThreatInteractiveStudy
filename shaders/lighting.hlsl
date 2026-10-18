// Deferred lighting. One full-screen (or scissored) draw per shading model, selected with stencil,
// so each permutation only contains the code its pixels need (Days Gone approach).
//   SHADING_MODEL: 0 default, 1 skin, 2 foliage, 3 eye
//   PER_SAMPLE:    1 = runs at sample frequency (MSAA complex pixels), 0 = once per pixel (sample 0)
//   ps_sun:   sun + shadows + ambient. Writes (overwrites) both lighting buffers: no clear needed.
//   ps_light: one point/spot light, additively blended, inside its stencil-marked volume.
#include "brdf.hlsli"
#include "atmosphere.hlsli"
#include "gi.hlsli"
#include "shadow.hlsli"

#ifndef SHADING_MODEL
#define SHADING_MODEL 0
#endif
#ifndef PER_SAMPLE
#define PER_SAMPLE 0
#endif

cbuffer LightCB : register(b2) {
    float4x4 light_volume_world;
    float4 light_pos_range;   // xyz position, w range
    float4 light_color;       // rgb * intensity
    float4 light_dir_type;    // xyz direction the light travels, w = type (1 point, 2 spot)
    float4 light_cone;        // x = cos(outer), y = 1 / (cos(inner) - cos(outer))
    float4 light_shadow;      // x = first shadow projector (< 0: no shadow), y = projector count (6 = cube), z = shadow strength (distance fade)
};

Texture2DArray<float> t_shadow : register(t5);

struct LightOut {
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
};

// Small kernel on purpose (shadow.hlsli): TI argues for barely softened shadows and perfect
// biasing over blurry soft shadows.
float sample_cascade(int c, float3 pos) {
    float4 sp = mul(cascade_view_proj[c], float4(pos, 1));
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    return pcf_array(t_shadow, uv, c, sp.z, shadow_params.y);
}

float sun_shadow(Surface s, float3 L) {
    if (shadow_params.w == 0) return 1;
    int count = (int)shadow_params.x;
    int c = 0;
    [unroll] for (int i = 0; i < 3; i++)
        if (i < count - 1 && s.view_depth > cascade_splits[i]) c = i + 1;
    if (s.view_depth > cascade_splits[count - 1]) return 1;
    // Normal offset (Holbert 2011): push the lookup along the normal by ~a texel, more at grazing angles.
    float NoL = saturate(dot(s.n, L));
    float3 offset = s.n * (cascade_texel[c] * 1.5 * (1.0 - NoL * 0.5)) + L * (cascade_texel[c] * 0.5);
    return sample_cascade(c, s.pos + offset);
}

#if PER_SAMPLE
#define SAMPLE_INPUT , uint sample_index : SV_SampleIndex
#define SAMPLE sample_index
#else
#define SAMPLE_INPUT
#define SAMPLE 0
#endif

void foliage_extra(Surface s, float3 L, float3 radiance, inout LightResult r) {
    // Foliage: light transmitted through thin leaves, seen from the back side.
    float back = saturate(-dot(s.n, L));
    float VoL = saturate(dot(-s.v, L));
    r.diffuse += radiance * (back * 0.25 + pow(VoL, 8.0) * 0.6) * 0.5 / PI;
}

void skin_extra(Surface s, float3 L, float3 radiance, inout LightResult r) {
    // Skin: wrapped diffuse approximates light bleeding past the terminator (the screen-space
    // SSS pass blurs the diffuse buffer for the rest).
    float wrap = saturate((dot(s.n, L) + 0.3) / 1.69) - saturate(dot(s.n, L));
    r.diffuse += radiance * max(wrap, 0) * float3(1.0, 0.35, 0.25) / PI;
}

// Per-shading-model code is selected at compile time (one draw per shading model, TI/Days Gone).
// SHADING_MODEL 99 is the A/B "uber shader": one draw for every model, branching per pixel on the
// shading model stored in the G-buffer, the way UE handles its shading models.
void shading_model_extra(Surface s, float3 L, float3 radiance, inout LightResult r) {
#if SHADING_MODEL == 2
    foliage_extra(s, L, radiance, r);
#elif SHADING_MODEL == 1
    skin_extra(s, L, radiance, r);
#elif SHADING_MODEL == 99
    [branch] if (s.shading_model == 2) foliage_extra(s, L, radiance, r);
    else if (s.shading_model == 1) skin_extra(s, L, radiance, r);
#endif
}

LightOut ps_sun(FullscreenVSOut i SAMPLE_INPUT) {
    int2 p = int2(i.pos.xy);
    float2 uv = i.pos.xy * screen_size.zw;
    Surface s = load_surface(p, SAMPLE, uv);
    float3 L = -sun_dir.xyz;
    float shadow = sun_shadow(s, L);
    LightResult r = eval_surface(s, L, sun_color.rgb * shadow);
    shading_model_extra(s, L, sun_color.rgb * shadow, r);

    // Indirect light (gi.hlsli): baked probe-grid irradiance (the sky ambient cube where GI is
    // off or no probe is valid) and a probe-based specular fallback, occluded by min(GTAO, material AO) with the
    // multi-bounce fit and specular occlusion. AO never touches the direct light above.
    ProfileData pa = profiles[s.pair * 2];
    float metal = lerp(pa.metal, profiles[s.pair * 2 + 1].metal, s.blend);
    float3 f0 = lerp(pa.f0 * s.specular * 2.0, s.albedo, metal);
    float NoV = saturate(dot(s.n, s.v));
    float ao = min(s.ao, gtao_visibility(p));
    IndirectLight indirect = indirect_light(s.pos, s.n, s.v, s.roughness);
    r.diffuse += indirect.irradiance / PI * gtao_multibounce(ao, s.albedo) * (1.0 - metal);
    r.specular += indirect.specular * env_brdf_approx(f0, s.roughness, NoV) * specular_occlusion(NoV, ao, s.roughness);

    LightOut o;
    o.diffuse = float4(r.diffuse, 0);
    o.specular = float4(r.specular, 0);
    return o;
}

LightOut ps_light(FullscreenVSOut i SAMPLE_INPUT) {
    int2 p = int2(i.pos.xy);
    float2 uv = i.pos.xy * screen_size.zw;
    Surface s = load_surface(p, SAMPLE, uv);
    float3 to_light = light_pos_range.xyz - s.pos;
    float d2 = dot(to_light, to_light);
    float3 L = to_light * rsqrt(d2);
    // Inverse-square falloff windowed to reach exactly 0 at the range (Karis 2013, UE4).
    float r = light_pos_range.w;
    float x = d2 / (r * r);
    float window = saturate(1.0 - x * x);
    float atten = window * window / max(d2, 0.01);
    if (light_dir_type.w == 2) {
        float cd = dot(-L, light_dir_type.xyz);
        float cone = saturate((cd - light_cone.x) * light_cone.y);
        atten *= cone * cone;
    }
    [branch] if (light_shadow.x >= 0 && atten > 0)
        atten *= local_light_shadow(light_shadow, light_pos_range.xyz, s.pos, s.n, L);
    float3 radiance = light_color.rgb * atten;
    LightResult lr = eval_surface(s, L, radiance);
    shading_model_extra(s, L, radiance, lr);
    LightOut o;
    o.diffuse = float4(lr.diffuse, 0);
    o.specular = float4(lr.specular, 0);
    return o;
}

// Light volume mesh: rasterizes into stencil (no pixel shader), or, for the Fox Engine A/B variant,
// runs the lighting pixel shader itself (which only uses SV_Position).
FullscreenVSOut vs_volume(float3 pos : POSITION) {
    float3 wp = mul(light_volume_world, float4(pos, 1)).xyz;
    FullscreenVSOut o;
    o.pos = mul(view_proj, float4(wp, 1));
    o.uv = 0;
    return o;
}
