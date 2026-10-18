// Deferred decals written straight into the G-buffer (no separate decal buffer): TI recommends
// Fox Engine's approach, decals write G-buffer channels but never the shading model (stencil) or
// the material ID, so a decal takes on the shading of the surface it lies on (a stain on stone
// still shades like stone). Here: albedo only, blended by the decal's opacity (normals are stored
// octahedrally, which doesn't blend linearly).
// Each decal is a unit box; its back faces are rasterized (works with the camera inside the box)
// and the pixel shader rejects pixels whose G-buffer position lies outside the box.
#include "common.hlsli"

Texture2DMS<float> t_depth : register(t0);
Texture2D t_decal : register(t1);

cbuffer DecalCB : register(b1) {
    float4x4 decal_world;       // unit box [-0.5, 0.5]^3 -> world
    float4x4 decal_inv_world;   // world -> unit box
    float4 decal_tint;          // rgb tint, a = opacity
};

FullscreenVSOut vs_decal(float3 pos : POSITION) {
    FullscreenVSOut o;
    float3 wp = mul(decal_world, float4(pos * 0.5, 1)).xyz;  // box mesh is [-1,1]: scale to unit box
    o.pos = mul(view_proj, float4(wp, 1));
    o.uv = 0;
    return o;
}

float4 ps_decal(FullscreenVSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float d = t_depth.Load(p, 0);
    if (d == 0) discard;
    float3 wp = world_from_depth(i.pos.xy * screen_size.zw, d);
    float3 local = mul(decal_inv_world, float4(wp, 1)).xyz;
    if (any(abs(local) > 0.5)) discard;
    // Projected along the box's local Y axis; fade out towards the top/bottom of the box so the
    // decal doesn't stretch over steep surfaces.
    float2 uv = local.xz + 0.5;
    float4 c = t_decal.Sample(s_aniso, uv);
    float fade = saturate((0.5 - abs(local.y)) * 6.0);
    return float4(c.rgb * decal_tint.rgb, c.a * decal_tint.a * fade);
}
