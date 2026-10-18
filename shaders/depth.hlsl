// Depth-only rendering: the depth pre-pass (SHADOW=0, main camera matrix) and shadow maps
// (SHADOW=1, matrix from the pass constant buffer). Opaque draws use only the 12-byte position
// stream and no pixel shader at all.
#include "geometry.hlsli"

#ifndef SHADOW
#define SHADOW 0
#endif
// FAR_CLIP=1 (local light shadow views): clip casters deeper than pass_clip.x with a clip
// distance. Shadow views render with depth clip off, so casters in front of the near plane are
// pancaked to it; that also clamps everything beyond the far plane to depth 0, where it still gets
// rasterized (and alpha-tested) only to fail the depth test against the cleared map. Clipping it
// before rasterization gives the same map for less work, and clipping at the deepest visible
// receiver instead of the far plane leaves out casters that can't shadow anything in view
// (docs/techniques/shadows.md 2.5).
#ifndef FAR_CLIP
#define FAR_CLIP 0
#endif
#if FAR_CLIP
#define FAR_CLIP_OUTPUT float far_clip : SV_ClipDistance0;
#define SET_FAR_CLIP(o) o.far_clip = pass_clip.x - o.pos.w   // clip w = depth along the view axis
#else
#define FAR_CLIP_OUTPUT
#define SET_FAR_CLIP(o)
#endif

cbuffer PassCB : register(b1) {
    float4x4 pass_view_proj;
    float4 pass_clip;   // FAR_CLIP: x = depth along the view axis beyond which casters are clipped
};

Texture2D t_albedo : register(t0);

float4 to_clip(float3 wp) {
#if SHADOW
    return mul(pass_view_proj, float4(wp, 1));
#else
    return mul(view_proj, float4(wp, 1));  // must match gbuffer.hlsl exactly (depth EQUAL)
#endif
}

struct VSOutDepth {
    float4 pos : SV_Position;
    nointerpolation uint flags : FLAGS;
    nointerpolation float fade : FADE;
    FAR_CLIP_OUTPUT   // last, so pixel shaders compiled without it still match the layout
};

VSOutDepth vs_depth(float3 pos : POSITION, uint draw_id : DRAWID) {
    DrawData d = draws[draw_id];
    VSOutDepth o;
    o.pos = to_clip(world_position(d, pos, time));
    o.flags = d.flags;
    o.fade = d.lod_fade;
    SET_FAR_CLIP(o);
    return o;
}

struct VSOutAlpha {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
    FAR_CLIP_OUTPUT
};

VSOutAlpha vs_depth_alpha(float3 pos : POSITION, float4 normal : NORMAL, float4 tangent : TANGENT, float2 uv : TEXCOORD0,
                          uint draw_id : DRAWID) {
    DrawData d = draws[draw_id];
    VSOutAlpha o;
    o.pos = to_clip(world_position(d, pos, time));
    o.uv = uv;
    o.material = d.material;
    SET_FAR_CLIP(o);
    return o;
}

// Alpha-tested depth: the texture fetch per pixel is what makes alpha-tested pre-pass draws
// almost as expensive as their base-pass counterparts.
void alpha_clip(VSOutAlpha i, SamplerState s) {
    MaterialData m = materials[i.material];
    float a = m.base_color.a * t_albedo.Sample(s, i.uv).a;
    if (a < m.alpha_cutoff) discard;
}

// Pre-pass: the G-buffer's anisotropic filtering, so both passes agree on coverage.
void ps_depth_alpha(VSOutAlpha i) { alpha_clip(i, s_aniso); }

// Shadow maps: plain trilinear. Leaves seen edge-on from a lamp are where anisotropic filtering
// takes the most taps, for coverage detail nobody sees from there (-20% on a lamp face inside a
// tree canopy, docs/techniques/shadows.md).
void ps_shadow_alpha(VSOutAlpha i) { alpha_clip(i, s_linear_wrap); }

void ps_depth_dither(VSOutDepth i) {
    float dither = bayer2x2_dither(i.pos.xy, frame_index);
    bool fade_in = (i.flags & 2) != 0;
    if (fade_in ? (dither >= i.fade) : (dither < i.fade)) discard;
}
