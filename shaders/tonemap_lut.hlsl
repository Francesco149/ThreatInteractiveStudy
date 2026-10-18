// Bakes the selected tone curve into the 64^3 RGBA16F 3D LUT the tone-map pass samples (research
// note 06 section 4; Callisto bakes a 64^3 RGBA16 LUT the same way, GT7 uses the pow(4) indexing).
// Exposure and white balance are applied before the lookup, so this only runs when the curve
// changes (r_tonemap_lut.cpp). Dispatch(16, 16, 16).
#include "common.hlsli"
#include "tonemap_ops.hlsli"

RWTexture3D<float4> u_lut : register(u0);

cbuffer LutCB : register(b1) {
    uint lut_operator;
    float3 lut_pad;
};

[numthreads(4, 4, 4)]
void cs_bake(uint3 id : SV_DispatchThreadID) {
    float3 x = lut_shaper_decode(id / (LUT_SIZE - 1.0));   // exposed linear Rec.709 at this texel centre
    u_lut[id] = float4(linear_to_srgb(tonemap_operator(lut_operator, x)), 1);   // sRGB-encoded: half the error of linear
}
