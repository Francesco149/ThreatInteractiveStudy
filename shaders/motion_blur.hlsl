// Motion blur ("velocity compensation"), docs/research/08 sections 5-8:
//   cs_pack:        velocity packing at full res: object motion + depth-limited camera motion
//                   (U-shaped weight: near content blurs with the camera, the focus distance stays
//                   sharp, far content only under fast camera rotation), trail length from a fixed
//                   trail time (frame-rate independent), and the 32x32 tile max in the same dispatch.
//   ps_neighbor:    one-sided 3x3 neighbour max: a neighbour tile only counts if its trail points
//                   towards us (trails extend backwards from moving objects).
//   ps_trail:       pass 1, TRAILING-ONLY gather: blur extends behind the motion, the leading edge
//                   stays sharp, background trails never cover the foreground. Uniform taps, no noise.
//   ps_smooth:      pass 2, the "separable" pass (Crysis 2008 lineage): the same directional filter
//                   iterated over one pass-1 stride, 8 x 8 = 64 virtual taps.
//   ps_centered:    A/B reference: classic camera-footage blur centred on the pixel (both directions).
#include "common.hlsli"

cbuffer MotionBlurCB : register(b1) {
    float4x4 cur_clip_to_prev_clip;
    float2 inv_screen;
    float trail_scale;       // trail time / frame time
    float max_trail_px;      // = tile size
    float near_start, near_end, far_start, far_end;
    float far_amount;        // 0..1 from camera angular speed (CPU)
    float camera_blur_scale;
    float rel_soft_z;        // 0.05
    float same_motion_tol;   // 0.05
    float trail_falloff;     // 0 = box shutter, 0.5 = comet ("clear point of origin")
    float pass2_span;        // 1/N = exactly one pass-1 stride
    float2 mb_pad;
};

#define TILE 32
#define TPG 16
#define N 8
#define M 8

// ---------------------------------------------------------------------------------------------
// Velocity pack + tile max (compute)

Texture2DMS<float2> t_motion_ms : register(t0);
Texture2DMS<float> t_depth_ms : register(t1);
RWTexture2D<float4> u_packed : register(u0);   // xy = trail vector in pixels, z = view depth
RWTexture2D<float4> u_tile_max : register(u1);

float camera_blur_weight(float z) {
    float w_near = 1.0 - smoothstep(near_start, near_end, z);
    float w_far = smoothstep(far_start, far_end, z) * far_amount;
    return camera_blur_scale * max(w_near, w_far);
}

groupshared uint gs_max_l2;      // asuint() of a non-negative float sorts like the float
groupshared float2 gs_max_v;

float2 pack_pixel(int2 pix) {
    // Front-most sample wins at MSAA edges (foreground velocity must be found).
    uint s = 0;
    float d = t_depth_ms.Load(pix, 0);
    for (uint k = 1; k < msaa_samples; k++) {
        float dk = t_depth_ms.Load(pix, k);
        if (dk > d) { d = dk; s = k; }   // reversed Z: larger = closer
    }
    float z = d > 0 ? linear_depth(d) : 1e6;
    float2 uv = (pix + 0.5) * inv_screen;
    float4 prev_clip = mul(cur_clip_to_prev_clip, float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1));
    float2 prev_uv = float2(prev_clip.x, -prev_clip.y) / prev_clip.w * 0.5 + 0.5;
    float2 v_cam = uv - prev_uv;                                   // what a static point would do
    float2 v_tot = d > 0 ? t_motion_ms.Load(pix, s) : v_cam;       // sky: camera motion only
    float2 v_obj = v_tot - v_cam;                                  // the object's own motion
    if (dot(v_obj * screen_size.xy, v_obj * screen_size.xy) < 0.0625) v_obj = 0;
    float2 v = (v_obj + camera_blur_weight(z) * v_cam) * screen_size.xy * trail_scale;
    float len = length(v);
    if (len > max_trail_px) v *= max_trail_px / len;
    if (len < 0.5) v = 0;
    u_packed[pix] = float4(v, z, 0);
    return v;
}

[numthreads(TPG, TPG, 1)]
void cs_pack(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) { gs_max_l2 = 0; gs_max_v = 0; }
    GroupMemoryBarrierWithGroupSync();
    // Each thread packs 4 pixels 16 apart, so a warp writes contiguous row segments (coalesced);
    // a 2x2 quad per thread made every warp store every other pixel and cost 2x more.
    int2 base = int2(gid.xy) * TILE + int2(gtid.xy);
    float3 best = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        int2 pix = base + int2(i & 1, i >> 1) * TPG;
        if (any(pix >= int2(screen_size.xy))) continue;
        float2 v = pack_pixel(pix);
        float l2 = dot(v, v);
        if (l2 > best.z) best = float3(v, l2);
    }
    // Tile max: one atomic per thread, then the thread holding the maximum writes its vector
    // (ties write the same length; any of them is a valid answer).
    InterlockedMax(gs_max_l2, asuint(best.z));
    GroupMemoryBarrierWithGroupSync();
    if (asuint(best.z) == gs_max_l2) gs_max_v = best.xy;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) u_tile_max[gid.xy] = float4(gs_max_v, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// One-sided neighbour max (pixel shader over the tile grid)

Texture2D<float4> t_tile_max : register(t0);

float4 ps_neighbor(FullscreenVSOut i) : SV_Target {
    int2 c = int2(i.pos.xy);
    int2 count = int2(ceil(screen_size.xy / TILE));
    float2 best = t_tile_max.Load(int3(c, 0)).xy;
    float best_l2 = dot(best, best);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 o = int2(x, y);
            if (x == 0 && y == 0) continue;
            int2 t = clamp(c + o, int2(0, 0), count - 1);
            float2 v = t_tile_max.Load(int3(t, 0)).xy;
            if (dot(v, float2(o)) <= 0.0) continue;  // its trail points away from us
            float l2 = dot(v, v);
            if (l2 > best_l2) { best = v; best_l2 = l2; }
        }
    return float4(best, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Pass 1: trailing-only gather

Texture2D<float4> t_scene : register(t0);       // AA output (sRGB view: linear values)
Texture2D<float4> t_packed : register(t1);
Texture2D<float4> t_neighbor : register(t2);

// Does the trail of a sample at offset `off` ahead of p (with trail vector v_y) cover p?
float trail_cover(float2 off, float2 v_y, out float along) {
    along = 0;
    float len = length(v_y);
    if (len < 0.5) return 0;
    float2 back = -v_y / len;           // the trail runs backwards from the sample
    float2 r = -off;                    // p relative to the sample
    float s = dot(r, back);             // distance along the trail
    float lat = abs(r.x * back.y - r.y * back.x);
    along = saturate(s / len);
    return saturate(len - s + 0.5) * saturate(s + 0.5) * saturate(1.5 - lat);
}
float in_front(float zp, float zy) { return saturate(1.0 + (zp - zy) / (rel_soft_z * zp)); }
float strictly_in_front(float zp, float zy) { return saturate((zp - zy) / (rel_soft_z * zp)); }
float same_motion(float2 a, float2 b) {
    float tol = same_motion_tol * max(length(a), length(b)) + 0.5;
    return saturate(1.0 - length(a - b) / tol);
}

float4 gather_layer(float2 uv, float2 dir, float2 vp, float zp, bool foreground_only) {
    float4 sum = 0;
    [unroll] for (int i = 0; i < N; ++i) {
        float t = (i + 0.5) / N;               // uniform: no noise, no dither
        float2 off = dir * t;                  // pixels ahead of p
        float2 uvy = uv + off * inv_screen;
        float4 pv = t_packed.SampleLevel(s_point_clamp, uvy, 0);
        float along;
        float cover = trail_cover(off, pv.xy, along);
        float layer = foreground_only ? strictly_in_front(zp, pv.z) * (1.0 - same_motion(vp, pv.xy))
                                      : max(in_front(zp, pv.z), same_motion(vp, pv.xy));
        float w = cover * layer * (1.0 - trail_falloff * along);
        sum += float4(t_scene.SampleLevel(s_linear_clamp, uvy, 0).rgb, 1.0) * w;
    }
    return sum;
}

float4 ps_trail(FullscreenVSOut i) : SV_Target {
    float2 uv = i.pos.xy * inv_screen;
    float3 cp = t_scene.SampleLevel(s_point_clamp, uv, 0).rgb;
    float4 pvp = t_packed.SampleLevel(s_point_clamp, uv, 0);
    float2 vp = pvp.xy;
    float zp = pvp.z;
    float2 vn = t_neighbor.Load(int3(int2(i.pos.xy) / TILE, 0)).xy;
    if (dot(vn, vn) < 0.25 && dot(vp, vp) < 0.25) return float4(cp, 0);  // nothing moves near p

    const float norm = N * (1.0 - 0.5 * trail_falloff);
    // Layer A: p's own surface along its own trail; p's colour fills the unclaimed weight, so the
    // leading edge stays opaque and sharp.
    float4 a = gather_layer(uv, vp, vp, zp, false);
    float3 col_a = (a.rgb + max(norm - a.w, 0.0) * cp) / max(norm, a.w);
    // Layer B: trails of closer objects crossing p (found through the neighbour max).
    float alpha_b = 0;
    float3 prem_b = 0;
    if (length(vn - vp) > 0.5 + same_motion_tol * length(vn)) {
        float4 b = gather_layer(uv, vn, vp, zp, true);
        alpha_b = saturate(b.w / norm);
        prem_b = b.rgb / max(norm, b.w);
    }
    return float4(prem_b + (1.0 - alpha_b) * col_a, alpha_b);
}

// ---------------------------------------------------------------------------------------------
// Pass 2: the "separable" pass: iterate over one pass-1 stride (stride-filling, N x M taps).

Texture2D<float4> t_pass1 : register(t0);

float4 ps_smooth(FullscreenVSOut i) : SV_Target {
    float2 uv = i.pos.xy * inv_screen;
    float4 c1 = t_pass1.SampleLevel(s_point_clamp, uv, 0);
    float4 pvp = t_packed.SampleLevel(s_point_clamp, uv, 0);
    float2 vn = t_neighbor.Load(int3(int2(i.pos.xy) / TILE, 0)).xy;
    float2 dir = (c1.a > 0.01) ? vn : pvp.xy;
    if (dot(dir, dir) < 0.25) return float4(c1.rgb, 1);
    float2 span = dir * pass2_span;
    float3 sum = c1.rgb;
    float wsum = 1.0;
    [unroll] for (int j = 0; j < M; ++j) {
        float2 off = span * ((j + 0.5) / M - 0.5);
        float2 uvy = uv + off * inv_screen;
        float4 pv = t_packed.SampleLevel(s_point_clamp, uvy, 0);
        float w = max(in_front(pvp.z, pv.z), same_motion(pvp.xy, pv.xy));
        sum += t_pass1.SampleLevel(s_linear_clamp, uvy, 0).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}

// ---------------------------------------------------------------------------------------------
// A/B reference: classic centred per-pixel blur along the pixel's own velocity (camera-footage
// look, what most engines ship), same tap count, no depth tests.

float4 ps_centered(FullscreenVSOut i) : SV_Target {
    float2 uv = i.pos.xy * inv_screen;
    float2 v = t_packed.SampleLevel(s_point_clamp, uv, 0).xy;
    if (dot(v, v) < 0.25) return t_scene.SampleLevel(s_point_clamp, uv, 0);
    float3 sum = 0;
    [unroll] for (int k = 0; k < N * 2; ++k) {
        float t = (k + 0.5) / (N * 2) - 0.5;
        sum += t_scene.SampleLevel(s_linear_clamp, uv + v * t * inv_screen, 0).rgb;
    }
    return float4(sum / (N * 2), 1);
}
