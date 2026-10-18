// Composition: scene colour = diffuse lighting x albedo + emissive (+ fog), added onto the
// specular lighting buffer, which from here on is the HDR scene colour. Recycling the specular
// RGBA16F target saves a whole MSAA buffer (TI: "the specular RGBA16 is overwritten/recycled").
#include "brdf.hlsli"
#include "atmosphere.hlsli"

#ifndef PER_SAMPLE
#define PER_SAMPLE 0
#endif

Texture2DMS<float4> t_light_diffuse : register(t6);
Texture2DMS<float4> t_emissive : register(t7);

#if PER_SAMPLE
#define SAMPLE_INPUT , uint sample_index : SV_SampleIndex
#define SAMPLE sample_index
#else
#define SAMPLE_INPUT
#define SAMPLE 0
#endif

// Dual-source blending merges fog into this pass: the blend unit computes
//   scene = color0 + specular_buffer * color1
// so the specular already in the buffer is attenuated by the fog too, in one draw.
struct ComposeOut {
    float4 color : SV_Target0;
    float4 transmittance : SV_Target1;
};

ComposeOut ps_compose(FullscreenVSOut i SAMPLE_INPUT) {
    int2 p = int2(i.pos.xy);
    float3 albedo = t_gb_albedo.Load(p, SAMPLE).rgb;
    float3 diffuse = t_light_diffuse.Load(p, SAMPLE).rgb;
    float3 emissive = t_emissive.Load(p, SAMPLE).rgb;
    float d = t_depth.Load(p, SAMPLE);
    float3 pos = world_from_depth(i.pos.xy * screen_size.zw, d);
    float4 fog = height_fog(pos - camera_pos.xyz);   // atmosphere.hlsli
    ComposeOut o;
    o.color = float4((diffuse * albedo + emissive) * fog.a + fog.rgb, 0);
    o.transmittance = float4(fog.aaa, 1);
    return o;
}
