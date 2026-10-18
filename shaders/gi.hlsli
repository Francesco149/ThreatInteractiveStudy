// Indirect lighting shared by the lighting pass, the debug views and the probe relight shader:
// the baked irradiance probe grid (docs/techniques/gi-ao.md) and the GTAO helpers.
#ifndef GI_HLSLI
#define GI_HLSLI
#include "common.hlsli"
#include "atmosphere.hlsli"

#define GI_FACE_RES 8   // sensor texels per probe cube-face edge; must match FACE_RES in src/r_gi.cpp

// Must match GIConstants in src/r_gi.cpp. Registers b4 / t24-t26 are reserved for GI + AO so they
// never collide with the per-pass slots of the shaders that include this file (t20-t23: the sky).
cbuffer GICB : register(b4) {
    float4 gi_grid_origin;   // xyz = world position of probe (0,0,0), w = probe spacing (m)
    float4 gi_grid_count;    // xyz = probes per axis, w = 1 if the grid is baked and enabled
    float4 gi_bias;          // x = normal bias, y = view bias (fractions of the spacing), z = GTAO on, w = visibility test on
    uint4 gi_atlas;          // x = probes per row of the sensor atlas
};

// Ambient-cube grid (Valve, McTaggart 2004; as an irradiance volume: Hooker 2016). One Texture3D
// holds the six axis directions as six stacks along z: +X, -X, +Y, -Y, +Z, -Z. Each texel is
// rgb = E_axis * validity, a = validity, where E_axis is the irradiance a surface facing that axis
// receives and validity = 0 for probes buried inside geometry (premultiplied, so invalid probes
// drop out of any weighted sum).
Texture3D<float4> t_gi_grid : register(t24);
Texture2D<float> t_gtao : register(t25);         // full-res GTAO visibility (1 = unoccluded)
Texture2D<uint2> t_gi_sensors : register(t26);   // the probes' captures (hit distances), gi_capture.hlsl

uint gi_probe_index(uint3 c) { return c.x + (uint)gi_grid_count.x * (c.y + (uint)gi_grid_count.y * c.z); }

// How far the probe saw along `dir`: the hit distance stored in its sensor for that direction
// (cube-face texel, D3D face convention as in the capture). Sky = unbounded.
float gi_probe_distance(Texture2D<uint2> sensors, uint probe, float3 dir) {
    float3 a = abs(dir);
    uint face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z) {
        face = dir.x > 0 ? 0 : 1;
        uv = float2(dir.x > 0 ? -dir.z : dir.z, -dir.y) / a.x;
    } else if (a.y >= a.z) {
        face = dir.y > 0 ? 2 : 3;
        uv = float2(dir.x, dir.y > 0 ? dir.z : -dir.z) / a.y;
    } else {
        face = dir.z > 0 ? 4 : 5;
        uv = float2(dir.z > 0 ? dir.x : -dir.x, -dir.y) / a.z;
    }
    uint2 t = min((uint2)((uv * 0.5 + 0.5) * GI_FACE_RES), GI_FACE_RES - 1);
    uint2 tile = uint2(((probe % gi_atlas.x) * 6 + face) * GI_FACE_RES, (probe / gi_atlas.x) * GI_FACE_RES);
    uint2 raw = sensors.Load(int3(tile + t, 0));
    return (raw.x >> 24) == 0 ? 1e30 : f16tof32(raw.y >> 16);
}

struct GISample {
    float3 irradiance;   // E(N)
    float3 specular;     // E(spec_dir), if requested
    float validity;      // trilinear-weighted probe validity, 0 = no valid probe around P
};

// Irradiance at P from the 8 probes of its grid cell, as in DDGI (Majercik et al. 2019; RTXGI's
// Irradiance.hlsl) but with ambient cubes and without its octahedral depth maps:
//  - the lookup point is pushed off the surface along the normal and towards the viewer
//    (normal offset as in Silvennoinen & Timonen 2015);
//  - each probe's trilinear weight is scaled by DDGI's "smooth backface" term, so probes behind
//    the surface count less;
//  - visibility: a probe whose own capture hit something closer than the lookup point, in that
//    direction, cannot see it (a wall between them): its weight drops to ~0. This is what stops
//    light leaking through walls thinner than the probe spacing;
//  - invalid probes (inside geometry) have zero premultiplied values and validity.
// Each probe then evaluates its ambient cube as sum(n_i^2 * E_i), three loads for the axes N faces.
GISample gi_sample_grid(Texture3D<float4> grid, Texture2D<uint2> sensors, float3 P, float3 N, float3 V,
                        float3 spec_dir, bool with_specular) {
    float spacing = gi_grid_origin.w;
    uint3 n = (uint3)gi_grid_count.xyz;
    float3 Pb = P + (N * gi_bias.x + V * gi_bias.y) * spacing;
    float3 g = clamp((Pb - gi_grid_origin.xyz) / spacing, 0.0, float3(n - 1));
    uint3 base = min((uint3)g, n - 2);
    float3 f = g - float3(base);
    uint3 axis_n = uint3(N.x >= 0 ? 0 : 1, N.y >= 0 ? 2 : 3, N.z >= 0 ? 4 : 5) * n.z;
    uint3 axis_s = uint3(spec_dir.x >= 0 ? 0 : 1, spec_dir.y >= 0 ? 2 : 3, spec_dir.z >= 0 ? 4 : 5) * n.z;
    float3 wn = N * N, ws = spec_dir * spec_dir;

    GISample r = (GISample)0;
    float weight = 0;
    [unroll] for (uint i = 0; i < 8; i++) {
        uint3 o = uint3(i & 1, (i >> 1) & 1, i >> 2);
        uint3 c = base + o;
        float3 t = lerp(1.0 - f, f, float3(o));
        float trilinear = t.x * t.y * t.z;
        float3 probe_pos = gi_grid_origin.xyz + float3(c) * spacing;
        float wrap = dot(normalize(probe_pos - P), N) * 0.5 + 0.5;
        float w = trilinear * (wrap * wrap + 0.2);
        if (gi_bias.w > 0) {
            float3 to_point = Pb - probe_pos;
            float dist = length(to_point);
            float seen = gi_probe_distance(sensors, gi_probe_index(c), to_point / max(dist, 1e-4));
            w *= max(saturate(1.0 + (seen - dist) / (0.25 * spacing)), 0.02);   // soft edge: 1/4 cell
        }
        float4 ex = grid.Load(int4(c.xy, c.z + axis_n.x, 0));
        float4 ey = grid.Load(int4(c.xy, c.z + axis_n.y, 0));
        float4 ez = grid.Load(int4(c.xy, c.z + axis_n.z, 0));
        r.irradiance += w * (wn.x * ex.rgb + wn.y * ey.rgb + wn.z * ez.rgb);
        if (with_specular) {
            float4 sx = grid.Load(int4(c.xy, c.z + axis_s.x, 0));
            float4 sy = grid.Load(int4(c.xy, c.z + axis_s.y, 0));
            float4 sz = grid.Load(int4(c.xy, c.z + axis_s.z, 0));
            r.specular += w * (ws.x * sx.rgb + ws.y * sy.rgb + ws.z * sz.rgb);
        }
        weight += w * ex.a;
        r.validity += trilinear * ex.a;
    }
    float inv = weight > 1e-6 ? 1.0 / weight : 0.0;
    r.irradiance *= inv;
    r.specular *= inv;
    return r;
}

struct IndirectLight {
    float3 irradiance;   // diffuse: irradiance arriving at the surface
    float3 specular;     // radiance for the split-sum specular fallback
};

// Indirect light for the lighting pass: probes when baked, else the sky ambient cube (atmosphere.hlsli).
// The specular part is a cheap fallback for rough surfaces: probe irradiance around the
// reflection direction / pi is the cosine-weighted average radiance over that hemisphere, what a
// roughness ~1 lobe reflects. No high-frequency detail (that is the reflection pass's job), but
// occluded and coloured by the scene like the diffuse GI. The direction is the mirror direction
// bent towards the normal with roughness (Frostbite's dominant direction, Lagarde & de Rousiers 2014).
IndirectLight indirect_light(float3 P, float3 N, float3 V, float roughness) {
    float a = roughness * roughness;
    float3 dir = normalize(lerp(N, reflect(-V, N), (1.0 - a) * (sqrt(1.0 - a) + a)));
    IndirectLight l;
    l.irradiance = PI * sky_ambient(N);   // sky_ambient is irradiance / pi
    l.specular = sky_ambient(dir);
    if (gi_grid_count.w > 0) {
        GISample s = gi_sample_grid(t_gi_grid, t_gi_sensors, P, N, V, dir, true);
        float k = saturate(s.validity * 20.0);
        l.irradiance = lerp(l.irradiance, s.irradiance, k);
        l.specular = lerp(l.specular, s.specular / PI, k);
    }
    return l;
}

// ---------------------------------------------------------------------------------------------
// Ambient occlusion application (GTAO paper, Jimenez et al. 2016). AO is applied to indirect
// light only, never to direct light (TI; Frostbite's occlusion table agrees).

float gtao_visibility(int2 pixel) { return gi_bias.z > 0 ? t_gtao.Load(int3(pixel, 0)) : 1.0; }

// Multi-bounce fit (GTAO paper, "approximated multi-bounce": a cubic in AO and albedo): brightens
// occlusion on light albedos, where interreflections inside the crease would have lit it,
// instead of a uniform grey darkening.
float3 gtao_multibounce(float ao, float3 albedo) {
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    return max(ao, ((ao * a + b) * ao + c) * ao);
}

// Specular occlusion from AO (Lagarde & de Rousiers 2014, "Moving Frostbite to PBR", section 4.10).
float specular_occlusion(float NoV, float ao, float roughness) {
    return saturate(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

#endif
