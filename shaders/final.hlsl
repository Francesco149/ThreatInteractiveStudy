// Final output: one draw that scales the image to the window, adds the camera effects
// (chromatic aberration, vignette, film grain), applies the display calibration (brightness curve
// + gamma encoding) and composites the UI on top. TI (Days Gone): render the UI into its own
// target and overlay it in the same draw that does gamma correction; the brightness slider is a
// curve with fixed black and white points (Callisto Protocol: the slider scales the image instead
// of crushing or clipping it).
#include "common.hlsli"

Texture2D<float4> t_image : register(t0);   // linear (sRGB view), render resolution
Texture2D<float4> t_ui : register(t1);      // UI over transparent black: premultiplied, gamma space

cbuffer FinalCB : register(b1) {
    float4 src_size;      // w, h, 1/w, 1/h of t_image
    float grain;
    float vignette;
    float brightness;     // -1..1, 0 = neutral (mid-tone exponent 2^-brightness)
    float chromatic;      // lateral chromatic aberration strength (pixels at the corners)
    float ui_enabled;
    float3 final_pad;
};

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 linear_to_srgb(float3 c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 ps_final(FullscreenVSOut i) : SV_Target {
    float2 d = i.uv - 0.5;
    float3 c;
    if (chromatic > 0) {
        // Lateral CA grows towards the corners: red scaled out, blue scaled in.
        float2 off = d * chromatic * src_size.zw * 2.0;
        c.r = t_image.SampleLevel(s_linear_clamp, i.uv + off, 0).r;
        c.g = t_image.SampleLevel(s_linear_clamp, i.uv, 0).g;
        c.b = t_image.SampleLevel(s_linear_clamp, i.uv - off, 0).b;
    } else {
        c = t_image.SampleLevel(s_linear_clamp, i.uv, 0).rgb;
    }
    c *= lerp(1.0, saturate(1.0 - dot(d, d) * 1.2), vignette);
    float n = hash12(i.pos.xy + frac(time * 13.37) * 512.0) - 0.5;
    c = max(c + n * grain * sqrt(max(c, 1e-4)) * 0.04, 0);

    // Display calibration in display space: black (0) and white (1) stay fixed.
    float3 g = linear_to_srgb(saturate(c));
    g = pow(g, exp2(-brightness));
    // 8-bit dither against banding in dark gradients.
    g += (hash12(i.pos.xy * 1.37 + 17.0) - 0.5) / 255.0;

    if (ui_enabled > 0) {
        float4 ui = t_ui.Load(int3(i.pos.xy, 0));
        g = g * (1.0 - ui.a) + ui.rgb;
    }
    return float4(g, 1);
}
