// GTAO, "ground-truth" ambient occlusion (Jimenez, Wu, Pesce, Jarabo 2016, "Practical Real-Time
// Strategies for Accurate Indirect Occlusion", Algorithm 1), noiseless without temporal history:
// each pixel of a 4x4 tile takes a different slice rotation from a 4x4 Bayer matrix (an ALU
// pattern, as TI recommends for non-temporal effects), and the depth-aware 4x4 spatial denoise
// averages exactly those 16 rotations back together (the spatial half of the paper's 4x4 x 6
// scheme; XeGTAO's denoiser likewise works without TAA).
//
// Passes (pixel shaders, 1x, pixel frequency, from MSAA sample 0):
//   ps_prepare : linear view depth at AO resolution (R32F)
//   ps_gtao    : horizon search, slices x 2 sides x steps, analytic cosine-weighted visibility
//   ps_denoise : 4x4 bilateral (plane distance) average
//   ps_upsample: half-res mode only, depth-aware bilinear back to full resolution
#include "common.hlsli"

cbuffer GtaoCB : register(b1) {
    float4 ao_target_size;   // AO resolution: w, h, 1/w, 1/h
    float ao_radius;         // world-space radius (m)
    float ao_falloff;        // occluders fade out over the last `falloff` meters of the radius
    float ao_max_radius_px;  // screen-space cap, in full-res pixels
    uint ao_slices;
    uint ao_steps;           // per side
    uint ao_scale;           // 1 = full res, 2 = half res
    float2 ao_pad;
};

Texture2DMS<float> t_depth_ms : register(t0);
Texture2DMS<float4> t_normal_ms : register(t1);
Texture2D<float> t_viewz : register(t2);   // linear view depth at AO resolution
Texture2D<float> t_ao : register(t3);      // AO to denoise / upsample

static const float SKY_Z = 1e6;

// View space for AO is left-handed: x right, y up, z = distance along the view direction (so
// the equations read as in the paper). The (jittered) projection is undone exactly.
float3 view_position(float2 full_px, float z) {
    float2 ndc = float2(full_px.x * screen_size.z * 2 - 1, 1 - full_px.y * screen_size.w * 2);
    return float3((ndc.x + proj[0][2]) * z / proj[0][0], (ndc.y + proj[1][2]) * z / proj[1][1], z);
}

// Full-res pixel centre an AO texel stands for (half res: its top-left pixel, where its depth came from).
float2 full_pixel(int2 p) { return p * ao_scale + 0.5; }

float3 view_normal(int2 p) {
    float3 n = mul((float3x3)view, decode_normal_oct(t_normal_ms.Load(p * ao_scale, 0).rg));
    return float3(n.x, n.y, -n.z);
}

float ps_prepare(FullscreenVSOut i) : SV_Target {
    float d = t_depth_ms.Load(int2(i.pos.xy) * ao_scale, 0);
    return d > 0 ? linear_depth(d) : SKY_Z;
}

static const float BAYER4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float ps_gtao(FullscreenVSOut i) : SV_Target {
    int2 pix = int2(i.pos.xy);
    float z = t_viewz.Load(int3(pix, 0));
    if (z >= SKY_Z) return 1;
    float3 P = view_position(full_pixel(pix), z);
    float3 N = view_normal(pix);
    float3 V = normalize(-P);

    // World radius projected at this depth, in AO-resolution pixels.
    float px_per_meter = proj[1][1] * 0.5 * screen_size.y / z;
    float radius_px = min(ao_radius * px_per_meter, ao_max_radius_px) / ao_scale;
    if (radius_px < 1.0) return 1;

    float noise = BAYER4[(pix.x & 3) + 4 * (pix.y & 3)] / 16.0;   // slice rotation
    float step_offset = frac(noise * 7.0 + 0.5);                  // a decorrelated permutation of it
    float visibility = 0;
    [loop] for (uint s = 0; s < ao_slices; s++) {
        float phi = (s + noise) * PI / ao_slices;
        float2 omega = float2(cos(phi), sin(phi));   // screen direction (y down)
        float3 dir = float3(omega.x, -omega.y, 0);   // the same direction in view space
        float3 axis = normalize(cross(dir, V));
        float3 ortho_dir = dir - dot(dir, V) * V;
        // Normal projected into the slice plane, and its angle n to the view vector.
        float3 proj_n = N - axis * dot(N, axis);
        float proj_len = max(length(proj_n), 1e-4);
        float cos_n = saturate(dot(proj_n, V) / proj_len);
        float n = (dot(ortho_dir, proj_n) < 0 ? -1.0 : 1.0) * acos(cos_n);

        // Horizon search on both sides: the highest (largest cosine to V) occluder.
        float horizon_cos[2] = {-1, -1};
        [unroll] for (uint side = 0; side < 2; side++) {
            float sgn = side == 0 ? -1.0 : 1.0;
            [loop] for (uint k = 0; k < ao_steps; k++) {
                float t = (k + step_offset) / ao_steps;
                t *= t;   // denser near the centre, where occluders matter most
                int2 sp = int2(floor(pix + 0.5 + omega * (sgn * max(t * radius_px, k + 1.0))));
                if (any(sp < 0) || any(sp >= int2(ao_target_size.xy))) break;
                float3 d = view_position(full_pixel(sp), t_viewz.Load(int3(sp, 0))) - P;
                float len = length(d);
                float c = dot(d, V) / len;
                c = lerp(-1.0, c, saturate((ao_radius - len) / ao_falloff));   // distant samples don't occlude
                horizon_cos[side] = max(horizon_cos[side], c);
            }
        }
        // Horizon angles, clamped to the hemisphere around the normal, then the analytic
        // cosine-weighted integral of the visible arc (paper, Algorithm 1), weighted by |proj_n|.
        float h0 = n + clamp(-acos(clamp(horizon_cos[0], -1, 1)) - n, -0.5 * PI, 0.5 * PI);
        float h1 = n + clamp(acos(clamp(horizon_cos[1], -1, 1)) - n, -0.5 * PI, 0.5 * PI);
        float sin_n = sin(n);
        visibility += proj_len * 0.25 * ((cos_n + 2 * h0 * sin_n - cos(2 * h0 - n)) + (cos_n + 2 * h1 * sin_n - cos(2 * h1 - n)));
    }
    return saturate(visibility / ao_slices);
}

// 4x4 window (offsets -2..+1): any 4x4 window holds each of the 16 Bayer rotations exactly once.
// Neighbours count by how close they are to the centre pixel's tangent plane, so AO never bleeds
// across depth discontinuities (no halos, which TI singles out as CIAO's flaw).
float ps_denoise(FullscreenVSOut i) : SV_Target {
    int2 pix = int2(i.pos.xy);
    float zc = t_viewz.Load(int3(pix, 0));
    if (zc >= SKY_Z) return 1;
    float3 Pc = view_position(full_pixel(pix), zc);
    float3 Nc = view_normal(pix);
    float tolerance = 0.02 * zc * ao_scale + 0.01;   // a pixel covers more world space further away
    float sum = 0, weight = 0;
    [unroll] for (int y = -2; y <= 1; y++)
        [unroll] for (int x = -2; x <= 1; x++) {
            int2 q = clamp(pix + int2(x, y), 0, int2(ao_target_size.xy) - 1);
            float3 Pq = view_position(full_pixel(q), t_viewz.Load(int3(q, 0)));
            float w = saturate(1.0 - abs(dot(Nc, Pq - Pc)) / tolerance);
            sum += t_ao.Load(int3(q, 0)) * w;
            weight += w;
        }
    return sum / max(weight, 1e-4);
}

// Half-res mode: AO texel h stands for full-res pixel 2h, so even pixels take one texel and odd
// ones the average of two (bilinear), each tap weighted down if its depth differs.
float ps_upsample(FullscreenVSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float d = t_depth_ms.Load(p, 0);
    if (d == 0) return 1;
    float z = linear_depth(d);
    int2 h = p >> 1;
    float2 f = float2(p & 1) * 0.5;
    float sum = 0, weight = 0;
    [unroll] for (int y = 0; y < 2; y++)
        [unroll] for (int x = 0; x < 2; x++) {
            int2 q = min(h + int2(x, y), int2(ao_target_size.xy) - 1);
            float bilinear = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
            float zq = t_viewz.Load(int3(q, 0));
            float w = bilinear * (1e-3 + saturate(1.0 - abs(zq - z) / (0.05 * z)));
            sum += t_ao.Load(int3(q, 0)) * w;
            weight += w;
        }
    return sum / max(weight, 1e-6);
}
