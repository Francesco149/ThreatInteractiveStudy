// Shadow map lookups shared by the lighting shaders and the debug views: the ShadowCB layout, the
// optimized PCF kernels, and the local-light shadow atlas (see docs/techniques/shadows.md).
//
// Depth is reversed everywhere (near = 1, far = 0, maps cleared to 0), so s_shadow compares with
// GREATER_EQUAL: a receiver is lit when its depth is >= the stored occluder depth.
#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI
#include "common.hlsli"

// Must match ShadowConstants in src/renderer_internal.h.
cbuffer ShadowCB : register(b3) {
    float4x4 cascade_view_proj[4];
    float4 cascade_splits;   // far view distance of each cascade
    float4 cascade_texel;    // world size of one texel per cascade (normal offset)
    float4 shadow_params;    // x = cascade count, y = map size, z = 1/size, w = enabled
    float4 shadow_filter;    // x = PCF kernel (0 = 4-tap tent, 1 = 9-tap), y = atlas size, z = 1/atlas size, w = projector count
};

// One local-light shadow view ("projector"): a spot light, or one cube face of a point light.
// Must match GpuProjector in src/r_local_shadows.cpp.
struct ShadowProjector {
    row_major float4x4 world_to_atlas;  // world -> (atlas u, atlas v, depth) * w; w = depth along the axis
    float4 pos_texel;                    // xyz light position, w = texel size in tan units (texels are square)
    float4 axis_cone;                    // xyz view axis, w = cos(outer cone) for spots, 0 for cube faces
    float4 info;                         // x = light range, y = 1 if the tile holds this view (rendered now or cached), z = atlas slice
    float4 pad;
};

Texture2DArray<float> t_shadow_atlas : register(t14);   // slice 0: this frame's views, slice 1: cached views
StructuredBuffer<ShadowProjector> shadow_projectors : register(t15);

// ---------------------------------------------------------------------------------------------
// Optimized PCF (Castano, "Shadow Mapping Summary - Part 1", 2013, as used in The Witness and in
// MJP's Shadows sample). Each SampleCmp with a LINEAR comparison filter returns a bilinearly
// weighted 2x2 comparison; offsets and weights are solved so the taps sum to the target kernel:
//   4 taps -> 3x3 tent over a 4x4-texel footprint (per-axis weights 3-2s, 1+2s; sum 16)
//   9 taps -> 5x5 near-Gaussian (1,3,4) over 6x6 texels (weights 4-3s, 7, 1+3s; sum 144)
// TI wants "barely softened" shadows: the small kernel is just enough to anti-alias the edge at
// ~1 texel per pixel, and needs the least bias.

struct PcfKernel {
    float2 base;            // uv of the texel corner the offsets are relative to
    float3 u, v;            // tap offsets in texels, per axis
    float3 uw, vw;          // tap weights, per axis
    float norm;
};

PcfKernel pcf_kernel(float2 uv, float size, bool wide) {
    PcfKernel k;
    float2 tc = uv * size;
    float2 b = floor(tc + 0.5);
    float2 s = tc + 0.5 - b;            // sub-texel position in [0, 1)
    k.base = (b - 0.5) / size;
    if (wide) {
        float2 w0 = 4.0 - 3.0 * s, w2 = 1.0 + 3.0 * s;
        float2 o0 = (3.0 - 2.0 * s) / w0 - 2.0, o1 = (3.0 + s) / 7.0, o2 = s / w2 + 2.0;
        k.u = float3(o0.x, o1.x, o2.x);
        k.v = float3(o0.y, o1.y, o2.y);
        k.uw = float3(w0.x, 7.0, w2.x);
        k.vw = float3(w0.y, 7.0, w2.y);
        k.norm = 1.0 / 144.0;
    } else {
        float2 w0 = 3.0 - 2.0 * s, w1 = 1.0 + 2.0 * s;
        float2 o0 = (2.0 - s) / w0 - 1.0, o1 = s / w1 + 1.0;
        k.u = float3(o0.x, o1.x, 0);
        k.v = float3(o0.y, o1.y, 0);
        k.uw = float3(w0.x, w1.x, 0);
        k.vw = float3(w0.y, w1.y, 0);
        k.norm = 1.0 / 16.0;
    }
    return k;
}

float pcf_array(Texture2DArray<float> map, float2 uv, float slice, float z, float size) {
    bool wide = shadow_filter.x > 0.5;
    PcfKernel k = pcf_kernel(uv, size, wide);
    float inv = 1.0 / size;
    float sum = 0;
    [branch] if (wide) {
        [unroll] for (int j = 0; j < 3; j++)
            [unroll] for (int i = 0; i < 3; i++)
                sum += k.uw[i] * k.vw[j] *
                       map.SampleCmpLevelZero(s_shadow, float3(k.base + float2(k.u[i], k.v[j]) * inv, slice), z);
    } else {
        [unroll] for (int j = 0; j < 2; j++)
            [unroll] for (int i = 0; i < 2; i++)
                sum += k.uw[i] * k.vw[j] *
                       map.SampleCmpLevelZero(s_shadow, float3(k.base + float2(k.u[i], k.v[j]) * inv, slice), z);
    }
    return sum * k.norm;
}

// ---------------------------------------------------------------------------------------------
// Local light shadows

// Cube face of a point light for a direction from the light: +X, -X, +Y, -Y, +Z, -Z (the order
// local_shadows_setup() creates the faces in). Every visible lit pixel lies in a face whose
// projector was rendered, because that face's view volume contains the pixel.
uint cube_face(float3 d) {
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0 ? 0 : 1;
    if (a.y >= a.z) return d.y >= 0 ? 2 : 3;
    return d.z >= 0 ? 4 : 5;
}

// Projector index for a light (LightCB.shadow: x = first projector, y = 6 for point lights).
int shadow_projector_index(float4 light_shadow, float3 light_pos, float3 pos) {
    int i = (int)light_shadow.x;
    if (light_shadow.y > 1.5) i += cube_face(pos - light_pos);
    return i;
}

// Inline shadow lookup for one local light (no projection mask pass: TI's lighting chart).
// Bias: the casters get slope-scaled raster bias; the receiver is moved along its normal by a
// distance proportional to the shadow texel size at that point (Holbert 2011 "normal offset
// shadows"), more at grazing angles, plus a fraction of a texel toward the light.
// light_shadow.z fades the shadow out for distant lights (1 = full shadow).
float local_light_shadow(float4 light_shadow, float3 light_pos, float3 pos, float3 n, float3 L) {
    ShadowProjector p = shadow_projectors[shadow_projector_index(light_shadow, light_pos, pos)];
    if (p.info.y < 0.5) return 1;
    float texel = p.pos_texel.w * dot(pos - p.pos_texel.xyz, p.axis_cone.xyz);  // world size of a texel here
    float NoL = saturate(dot(n, L));
    float3 biased = pos + n * (texel * 1.5 * sqrt(1.0 - NoL * NoL)) + L * (texel * 0.5);
    float4 h = mul(p.world_to_atlas, float4(biased, 1));
    float3 uvz = h.xyz / h.w;
    return lerp(1.0, pcf_array(t_shadow_atlas, uvz.xy, p.info.z, uvz.z, shadow_filter.y), light_shadow.z);
}

#endif
