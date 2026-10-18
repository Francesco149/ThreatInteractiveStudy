// GI probe relight: one thread group per probe lights the probe's 6 x 8 x 8 sensors (captured by
// gi_capture.hlsl) and projects the radiance they see onto the six ambient-cube axes:
//
//   L(sensor) = sky radiance                                         (texel saw the sky)
//             = albedo/pi * (E_sun * N.L * shadow + E_prev(P, N))   (texel saw a surface)
//   E_axis   += L * max(0, dir . axis) * d_omega                     (cosine-weighted irradiance)
//
// shadow comes from a whole-scene sun shadow map (the camera cascades only cover the view) and
// E_prev is the previous bounce's grid sampled exactly like the lighting pass does, so running
// this N times gives N bounces (TI: "feed sensors into probes", "propagate light across probes";
// Far Cry 3 / The Division radiance-transfer lineage; research note 05, sections 6.2-6.3).
// Deterministic: no rays, no noise, no temporal accumulation.
//
// EMISSIVE_ONLY: runs once after the capture and projects the captured emissive radiance into a
// static grid that every relight adds (emissive light does not change with the sun).
#include "gi.hlsli"
#include "atmosphere.hlsli"

#ifndef EMISSIVE_ONLY
#define EMISSIVE_ONLY 0
#endif

cbuffer RelightCB : register(b1) {
    float4x4 gi_shadow_view_proj;
    float gi_shadow_texel;       // world size of a GI shadow map texel
    uint gi_bounce;              // 0 = direct sun + sky only (no previous grid)
    float gi_projection_scale;   // pi / sum(max(0, dir.axis) * d_omega): exact pi for a uniform sky
    float gi_total_solid_angle;  // sum of d_omega over the cube (~4 pi)
    uint gi_probe_count;
    uint gi_probes_per_row;
    float gi_backface_limit;     // probes seeing more back faces than this are inside geometry
    float relight_pad;
};

Texture2D<uint2> t_sensors : register(t0);
Texture3D<float4> t_prev_grid : register(t1);
Texture2D<float> t_gi_shadow : register(t2);
Texture3D<float4> t_emissive_grid : register(t3);
Texture2D<float4> t_capture_emissive : register(t4);
RWTexture3D<float4> u_grid : register(u0);

static const float3 FACE_FWD[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
static const float3 FACE_RIGHT[6] = {float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0)};
static const float3 FACE_UP[6] = {float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0)};

float gi_sun_shadow(float3 P, float3 n) {
    float3 L = -sun_dir.xyz;
    // Normal offset (Holbert 2011) + a little along the light, in shadow map texels.
    float3 p = P + n * (gi_shadow_texel * 1.5) + L * (gi_shadow_texel * 1.0);
    float4 sp = mul(gi_shadow_view_proj, float4(p, 1));
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0) || any(uv > 1)) return 1;
    return t_gi_shadow.SampleCmpLevelZero(s_shadow, uv, sp.z);   // bilinear 2x2 PCF
}

// Radiance seen by one sensor; adds its solid angle to `back` if it saw a back face.
float3 sensor_radiance(uint2 raw, float3 probe_pos, float3 dir, float d_omega, inout float back) {
    uint flags = raw.x >> 24;
    if (flags == 0) return sky_radiance(dir);   // no sun disk: the sun arrives as shadowed direct light
    if (flags & 2) { back += d_omega; return 0; }
    float3 albedo = float3(raw.x & 255, (raw.x >> 8) & 255, (raw.x >> 16) & 255) / 255.0;
    albedo *= albedo;
    float3 n = decode_normal_oct(float2(raw.y & 255, (raw.y >> 8) & 255) / 255.0);
    float3 P = probe_pos + dir * f16tof32(raw.y >> 16);
    float3 E = sun_color.rgb * saturate(dot(n, -sun_dir.xyz)) * gi_sun_shadow(P, n);
    if (gi_bounce > 0) E += gi_sample_grid(t_prev_grid, t_sensors, P, n, -dir, n, false).irradiance;
    return albedo / PI * E;
}

groupshared float3 gs_e[6][64];
groupshared float gs_back[64];

[numthreads(64, 1, 1)]
void cs_relight(uint3 group : SV_GroupID, uint ti : SV_GroupIndex) {
    uint probe = group.x + group.y * gi_probes_per_row;   // dispatch layout = atlas layout
    uint3 n = (uint3)gi_grid_count.xyz;
    uint3 cell = uint3(probe % n.x, (probe / n.x) % n.y, probe / (n.x * n.y));
    float3 probe_pos = gi_grid_origin.xyz + float3(cell) * gi_grid_origin.w;

    // This thread's texel (same position on all six faces) and its solid angle.
    const float R = GI_FACE_RES;
    uint2 t = uint2(ti % GI_FACE_RES, ti / GI_FACE_RES);
    float u = (t.x + 0.5) / R * 2 - 1, v = (t.y + 0.5) / R * 2 - 1;
    float d_omega = (2.0 / R) * (2.0 / R) / pow(1 + u * u + v * v, 1.5);
    uint2 tile = uint2((probe % gi_probes_per_row) * 6 * GI_FACE_RES, (probe / gi_probes_per_row) * GI_FACE_RES);

    float3 e[6] = {(float3)0, (float3)0, (float3)0, (float3)0, (float3)0, (float3)0};
    float back = 0;
    [unroll] for (uint f = 0; f < 6; f++) {
        float3 dir = normalize(FACE_FWD[f] + u * FACE_RIGHT[f] - v * FACE_UP[f]);
        uint2 texel = tile + uint2(f * GI_FACE_RES, 0) + t;
#if EMISSIVE_ONLY
        uint2 raw = t_sensors.Load(int3(texel, 0));
        float3 L = (raw.x >> 24) == 1 ? t_capture_emissive.Load(int3(texel, 0)).rgb : 0.0;
#else
        float3 L = sensor_radiance(t_sensors.Load(int3(texel, 0)), probe_pos, dir, d_omega, back);
#endif
        L *= d_omega;
        e[0] += L * max(dir.x, 0); e[1] += L * max(-dir.x, 0);
        e[2] += L * max(dir.y, 0); e[3] += L * max(-dir.y, 0);
        e[4] += L * max(dir.z, 0); e[5] += L * max(-dir.z, 0);
    }

    // Sum the 64 threads' partial results (shared-memory tree reduction).
    [unroll] for (uint a = 0; a < 6; a++) gs_e[a][ti] = e[a];
    gs_back[ti] = back;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1) {
        if (ti < s) {
            [unroll] for (uint a = 0; a < 6; a++) gs_e[a][ti] += gs_e[a][ti + s];
            gs_back[ti] += gs_back[ti + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (ti < 6 && probe < gi_probe_count) {
        uint3 out_texel = uint3(cell.xy, cell.z + ti * n.z);
        float3 E = gs_e[ti][0] * gi_projection_scale;
#if EMISSIVE_ONLY
        u_grid[out_texel] = float4(E, 0);
#else
        // Probes inside geometry see mostly back faces (DDGI's classification; Hooker 2016 skips
        // voxels that see back faces): validity 0 removes them from the interpolation.
        float validity = gs_back[0] / gi_total_solid_angle < gi_backface_limit ? 1.0 : 0.0;
        E += t_emissive_grid.Load(int4(out_texel, 0)).rgb;
        u_grid[out_texel] = float4(E * validity, validity);
#endif
    }
}
