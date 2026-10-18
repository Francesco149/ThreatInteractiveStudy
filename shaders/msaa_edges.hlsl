// MSAA complex-pixel detection (Lauritzen 2010, "Deferred Rendering for Current and Future
// Rendering Pipelines"; Crysis 3 does the same before its lighting). A pixel needs per-sample
// lighting only if its samples come from surfaces that shade differently. Samples from coplanar
// triangles of one flat surface (same normal, continuous depth, same material) are NOT edges,
// which is what TI's "baked flat-discontinuity" idea aims for, done here from the G-buffer itself.
// Non-edge pixels are discarded; edge pixels get the stencil EDGE bit (stencil REPLACE). No colour
// output: a render target would have to be multisampled like the depth buffer. (The temporal AA's
// edge mask is derived in the tone mapping pass from the per-sample colours instead.)
#include "common.hlsli"

Texture2DMS<float> t_depth : register(t0);
Texture2DMS<float4> t_normal : register(t1);
Texture2DMS<float4> t_material : register(t2);

void ps_main(FullscreenVSOut i) {
    int2 p = int2(i.pos.xy);
    float d0 = t_depth.Load(p, 0);
    float3 n0 = decode_normal_oct(t_normal.Load(p, 0).rg);
    float2 m0 = t_material.Load(p, 0).ra;   // profile pair, shading model
    float z0 = linear_depth(d0);
    bool edge = false;
    [unroll] for (uint s = 1; s < 4; s++) {
        if (s >= msaa_samples) break;
        float d = t_depth.Load(p, s);
        if ((d0 == 0) != (d == 0)) { edge = true; break; }   // geometry vs sky
        if (d0 == 0) continue;                               // both sky
        float z = linear_depth(d);
        float3 n = decode_normal_oct(t_normal.Load(p, s).rg);
        float2 m = t_material.Load(p, s).ra;
        if (abs(z - z0) > 0.01 * z0 + 0.02 || dot(n, n0) < 0.98 || any(m != m0)) { edge = true; break; }
    }
    if (!edge) discard;
}
