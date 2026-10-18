// SMAA passes (Jimenez et al. 2012, reference implementation v2.8, MIT) wrapped for plain D3D11:
// our own sampler/porting macros instead of the Effects-framework path. Each pass is compiled as
// its own shader so the register reuse below is legal.
#include "common.hlsli"

cbuffer SMAACB : register(b1) {
    float4 rt_metrics;        // 1/w, 1/h, w, h
    float4 subsample_indices;
};

#define SMAA_RT_METRICS rt_metrics
#define SMAA_CUSTOM_SL 1
#define LinearSampler s_linear_clamp
#define PointSampler s_point_clamp
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#ifndef SMAA_PRESET_ULTRA
#define SMAA_PRESET_HIGH
#endif
#define SMAA_REPROJECTION 1
#include "../third_party/smaa/SMAA.hlsl"

Texture2D t0_tex : register(t0);
Texture2D t1_tex : register(t1);
Texture2D t2_tex : register(t2);

void fullscreen_tri(uint id, out float4 pos, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

struct V3 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 offset[3] : TEXCOORD1; };
struct VW { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 offset[3] : TEXCOORD2; };
struct V1 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 offset : TEXCOORD1; };

// Pass 1: colour edge detection on the gamma-space (UNORM view) layer. Discards non-edges, so the
// stencil REPLACE marks only edge pixels for pass 2.
V3 vs_edge(uint id : SV_VertexID) { V3 o; fullscreen_tri(id, o.pos, o.uv); SMAAEdgeDetectionVS(o.uv, o.offset); return o; }
float2 ps_edge(V3 i) : SV_Target { return SMAAColorEdgeDetectionPS(i.uv, i.offset, t0_tex); }

// Pass 2: blending weights (stencil-masked to edge pixels).
VW vs_weight(uint id : SV_VertexID) { VW o; fullscreen_tri(id, o.pos, o.uv); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.offset); return o; }
float4 ps_weight(VW i) : SV_Target {
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.offset, t0_tex, t1_tex, t2_tex, subsample_indices);
}

// Pass 3: neighborhood blending of the sRGB view; alpha carries the (antialiased) velocity length
// for the temporal pass. Second output: "SMAA modified this pixel" for the edge-only temporal mask.
V1 vs_blend(uint id : SV_VertexID) { V1 o; fullscreen_tri(id, o.pos, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.offset); return o; }
void ps_blend(V1 i, out float4 color : SV_Target0, out float4 mask : SV_Target1) {
    color = SMAANeighborhoodBlendingPS(i.uv, i.offset, t0_tex, t1_tex, t2_tex);
    float4 a;
    a.x = t1_tex.SampleLevel(LinearSampler, i.offset.xy, 0).a;
    a.y = t1_tex.SampleLevel(LinearSampler, i.offset.zw, 0).g;
    a.wz = t1_tex.SampleLevel(LinearSampler, i.uv, 0).xz;
    mask = float4(0, 0, dot(a, 1.0) >= 1e-5 ? 1.0 : 0.0, 0);
}
