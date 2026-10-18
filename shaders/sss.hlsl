// Screen-space subsurface scattering, separable (Jimenez et al. 2015, "Separable Subsurface
// Scattering"; kernel from the d'Eon & Luebke skin profile, generated in src/r_sss.cpp).
// Blurs the albedo-free DIFFUSE lighting of skin pixels only (stencil), at full resolution, in two
// draws (horizontal, vertical) that ping-pong through a temp buffer - a pixel-shader
// implementation like Crysis 3's, so stencil testing rejects every non-skin pixel up front.
// MSAA: runs at pixel frequency; the centre comes from the first skin sample, taps read sample 0
// interpolated by hand (MSAA can't be filtered), writes land only on skin samples (stencil).
#include "common.hlsli"

Texture2DMS<float4> t_src : register(t0);     // H: diffuse lighting, V: result of H
Texture2DMS<float> t_depth : register(t1);
Texture2DMS<uint2> t_stencil : register(t2);  // .g = stencil

cbuffer SssCB : register(b1) {
    float4 kernel[17];          // rgb = weight, w = offset in mm; centre first
    float2 axis;                // (1,0) then (0,1)
    float scatter_scale;        // 1 = physically sized profile
    float inv_depth_tolerance;  // 1 / 1.5 cm
    float jitter_amount;
    float proj_y;               // P._22
    uint num_taps;
    float sss_pad;
};

static const uint STENCIL_SKIN = 2;  // shading model SKIN (1) + 1

bool is_skin(int2 p, uint s) { return (t_stencil.Load(p, s).g & STENCIL_SM_MASK) == STENCIL_SKIN; }

// Sample 0 of the two pixels around a sub-pixel offset along the axis, interpolated; non-skin
// texels are rejected so eyebrows, collars and stale data don't bleed in.
bool fetch_tap(float2 centre, float offset_px, out float3 color, out float view_z) {
    float fl = floor(offset_px);
    float a = offset_px - fl;
    int2 p0 = int2(centre + axis * fl);
    int2 p1 = p0 + int2(axis);
    float w0 = (1.0 - a) * (is_skin(p0, 0) ? 1.0 : 0.0);
    float w1 = a * (is_skin(p1, 0) ? 1.0 : 0.0);
    float ws = w0 + w1;
    color = (w0 * t_src.Load(p0, 0).rgb + w1 * t_src.Load(p1, 0).rgb) / max(ws, 1e-5);
    view_z = linear_depth(t_depth.Load(w0 >= w1 ? p0 : p1, 0));
    return ws > 0.0;
}

float4 ps_blur(FullscreenVSOut i) : SV_Target {
    int2 pc = int2(i.pos.xy);
    uint sc = is_skin(pc, 0) ? 0 : 1;   // first skin sample (MSAA edge pixels)
    float3 centre = t_src.Load(pc, sc).rgb;
    float zc = linear_depth(t_depth.Load(pc, sc));

    // Physical footprint: millimetres on the surface -> pixels at this depth.
    float px_per_mm = 0.001 * scatter_scale * 0.5 * proj_y * screen_size.y / zc;
    px_per_mm *= 1.0 + jitter_amount * (bayer2x2_dither(i.pos.xy, frame_index) - 0.5);
    px_per_mm = min(px_per_mm, 20.0);
    if (abs(kernel[num_taps - 1].w) * px_per_mm < 0.5) return float4(centre, 0);  // sub-pixel: skip

    float3 sum = centre * kernel[0].rgb;
    [loop] for (uint k = 1; k < num_taps; ++k) {
        float3 c;
        float z;
        bool ok = fetch_tap(i.pos.xy, kernel[k].w * px_per_mm, c, z);
        float s = ok ? saturate(abs(z - zc) * inv_depth_tolerance) : 1.0;  // follow the surface
        sum += kernel[k].rgb * lerp(c, centre, s);
    }
    return float4(sum, 0);
}
