// Shared declarations for all shaders. Must match the C++ structs in renderer.h / scene.cpp.
#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#pragma pack_matrix(row_major)

static const float PI = 3.14159265358979;

// ---------------------------------------------------------------------------------------------
// Constant buffers. b0 = per frame, b1 = per pass, b2 = per light.

cbuffer FrameCB : register(b0) {
    float4x4 view;
    float4x4 proj;               // jittered
    float4x4 view_proj;          // jittered
    float4x4 inv_view_proj;      // jittered
    float4x4 view_proj_nojitter;
    float4x4 prev_view_proj_nojitter;
    float4x4 inv_proj;
    float4 camera_pos;           // xyz, w = near plane
    float4 screen_size;          // w, h, 1/w, 1/h
    float4 jitter;               // xy = this frame's jitter in NDC, zw = last frame's
    float4 sun_dir;              // direction the light travels, w = unused
    float4 sun_color;            // rgb: sun illuminance at the camera (atmosphere transmittance applied),
                                 // w: sun illuminance at the top of the atmosphere (scales the sky LUTs)
    float time;
    float prev_time;
    float exposure;              // linear multiplier applied before tone mapping
    uint frame_index;
    uint msaa_samples;
    uint debug_view;
    float wind_strength;
    float lod_dither_enabled;
    float delta_time;
    float auto_exposure;         // 1 = use the metered exposure buffer, `exposure` is then compensation
    float texture_unjitter;      // 1 = sample material textures at the unjittered pixel centre
    float frame_pad;
};

// ---------------------------------------------------------------------------------------------
// Per-draw data lives in one structured buffer per frame (no per-draw constant buffer updates).
// The vertex shader gets the index through a per-instance vertex stream (see renderer.cpp).

struct DrawData {
    float4 world0, world1, world2;   // 3x4 world matrix rows
    float4 prev0, prev1, prev2;      // last frame's
    uint material;
    uint flags;                      // 1 = wind, 2 = dither fade in, 4 = dither fade out
    float lod_fade;                  // 0..1 dither threshold for LOD cross-fades
    float wind_bend;                 // wind_offset() sway coefficient (scene.cpp, wind_bend())
};

struct MaterialData {
    float4 base_color;
    float3 emissive;
    float roughness;
    float metallic;
    float alpha_cutoff;
    uint shading_model;
    uint profile_pair;
    uint flags;                      // 1 albedo, 2 normal, 4 mr, 8 ao, 16 emissive, 32 alpha mask
    uint3 pad;
};

struct ProfileData {
    float3 f0;
    float metal;
    float clearcoat;
    float clearcoat_rough;
    float sheen;
    float diffuse_wrap;
};

StructuredBuffer<DrawData> draws : register(t8);
StructuredBuffer<MaterialData> materials : register(t9);
StructuredBuffer<ProfileData> profiles : register(t10);

SamplerState s_aniso : register(s0);       // anisotropic wrap (materials)
SamplerState s_linear_clamp : register(s1);
SamplerState s_point_clamp : register(s2);
SamplerComparisonState s_shadow : register(s3);  // GREATER_EQUAL comparison (reversed Z), clamp
SamplerState s_linear_wrap : register(s4);

// ---------------------------------------------------------------------------------------------
// Stencil layout (8 bits), see docs/TECHNIQUES.md:
//   bits 0-2  shading model + 1 (0 = sky / empty sample)
//   bit  3    MSAA complex pixel (needs per-sample shading)
//   bit  7    inside the current light volume (toggled by the stencil volume pass, cleared by lighting)
#define STENCIL_SM_MASK 0x07
#define STENCIL_EDGE 0x08
#define STENCIL_LIGHT 0x80

// ---------------------------------------------------------------------------------------------
// Helpers

float3x4 rows3x4(float4 a, float4 b, float4 c) { return float3x4(a, b, c); }

float3 transform_point(float4 r0, float4 r1, float4 r2, float3 p) {
    return float3(dot(r0, float4(p, 1)), dot(r1, float4(p, 1)), dot(r2, float4(p, 1)));
}

float3 transform_dir(float4 r0, float4 r1, float4 r2, float3 d) {
    return float3(dot(r0.xyz, d), dot(r1.xyz, d), dot(r2.xyz, d));
}

// Normals transform with the inverse transpose, which for M = [a b c] (columns) is the cofactor
// matrix [b x c, c x a, a x b] / det(M): correct under non-uniform scale, no inverse needed (only
// the sign of the determinant matters, the result is normalized later).
float3 transform_normal(float4 r0, float4 r1, float4 r2, float3 n) {
    float3 a = float3(r0.x, r1.x, r2.x), b = float3(r0.y, r1.y, r2.y), c = float3(r0.z, r1.z, r2.z);
    float3 bc = cross(b, c);
    return (bc * n.x + cross(c, a) * n.y + cross(a, b) * n.z) * (dot(a, bc) < 0 ? -1.0 : 1.0);
}

// Reversed infinite projection: view distance = near / device_depth.
float linear_depth(float device_z) { return camera_pos.w / max(device_z, 1e-7); }

float3 world_from_depth(float2 uv, float device_z) {
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, device_z, 1);
    float4 w = mul(inv_view_proj, ndc);
    return w.xyz / w.w;
}

// Octahedral normal encoding (Cigolle et al. 2014, "A Survey of Efficient Representations for
// Independent Unit Vectors"): 2 x 10 bits in the G-buffer give ~0.1 degree error.
float2 oct_wrap(float2 v) { return (1.0 - abs(v.yx)) * ((v.xy >= 0.0) ? 1.0 : -1.0); }

float2 encode_normal_oct(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : oct_wrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

float3 decode_normal_oct(float2 e) {
    e = e * 2.0 - 1.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += (n.xy >= 0.0) ? -t : t;
    return normalize(n);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 2x2 Bayer dither (values 0, .5, .75, .25) shifted every frame between two positions, so a
// 50% LOD cross-fade alternates its pattern and the 2-frame temporal AA averages it out.
float bayer2x2_dither(float2 pixel, uint frame) {
    uint2 p = uint2(pixel) + uint2(frame & 1, 0);
    uint i = (p.x & 1) + (p.y & 1) * 2;
    static const float bayer[4] = {0.125, 0.625, 0.875, 0.375};
    return bayer[i];
}

// Full-screen triangle: draw 3 vertices, no vertex buffer.
struct FullscreenVSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenVSOut fullscreen_vs(uint id : SV_VertexID) {
    FullscreenVSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

#endif
