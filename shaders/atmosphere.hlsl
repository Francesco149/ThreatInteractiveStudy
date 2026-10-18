// Sky LUT bakes (Hillaire 2020), run by r_sky.cpp:
//   cs_transmittance  256x64  once (depends only on the atmosphere constants)
//   cs_multiscatter   32x32   once
//   cs_sky_view       192x108 when the sun moves (or the camera altitude changes)
//   cs_sky_ambient    6 values: the sky's ambient cube, after each sky-view bake
// Ported from the reference code (github.com/sebh/UnrealEngineSkyAtmosphere, MIT), with Y up.
#include "atmosphere.hlsli"

Texture2D<float4> t_multiscatter : register(t23);
RWTexture2D<float4> u_lut : register(u0);
RWStructuredBuffer<float4> u_sky_ambient : register(u1);

// Henyey-Greenstein; cos_theta is the angle between the direction to the sun and the view ray, so
// looking at the sun (cos = 1) is forward scattering.
float hg_phase(float g, float cos_theta) {
    float denom = 1.0 + g * g - 2.0 * g * cos_theta;
    return (1.0 - g * g) / (4.0 * PI * denom * sqrt(denom));
}
float rayleigh_phase(float cos_theta) { return 3.0 / (16.0 * PI) * (1.0 + cos_theta * cos_theta); }

struct Medium {
    float3 rayleigh;     // Rayleigh scattering
    float mie;           // Mie scattering
    float3 scattering;   // total scattering
    float3 extinction;   // total extinction (scattering + Mie and ozone absorption)
};

Medium sample_medium(float h) {
    h = max(h, 0.0);
    float d_rayleigh = exp(-h / RAYLEIGH_SCALE_HEIGHT);
    float d_mie = exp(-h / MIE_SCALE_HEIGHT);
    float d_ozone = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    Medium m;
    m.rayleigh = RAYLEIGH_SCATTERING * d_rayleigh;
    m.mie = MIE_SCATTERING * d_mie;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.rayleigh + MIE_EXTINCTION * d_mie + OZONE_ABSORPTION * d_ozone;
    return m;
}

float3 multiscatter_lut(float h, float mu_s) {
    float2 uv = saturate(float2(mu_s * 0.5 + 0.5, h / (ATM_TOP - ATM_BOTTOM)));
    uv = float2(from_unit_to_sub_uv(uv.x, MULTISCATTER_LUT_SIZE), from_unit_to_sub_uv(uv.y, MULTISCATTER_LUT_SIZE));
    return t_multiscatter.SampleLevel(s_linear_clamp, uv, 0).rgb;
}

// ---------------------------------------------------------------------------------------------
// Transmittance LUT: optical depth along the ray, 40 midpoint steps. Like the reference, the ray
// stops at the ground; the sun's visibility is tested separately (earth shadow).

void transmittance_params(float2 uv, out float h, out float mu) {
    float H = sqrt(ATM_TOP * ATM_TOP - ATM_BOTTOM * ATM_BOTTOM);
    float rho = H * uv.y;
    float r = sqrt(rho * rho + ATM_BOTTOM * ATM_BOTTOM);
    h = rho * rho / (r + ATM_BOTTOM);
    float d_min = ATM_TOP - r, d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

float3 optical_depth(float h, float mu, int steps) {
    float t_ground = dist_to_sphere(h, mu, 0.0);
    float t_max = t_ground >= 0 ? t_ground : dist_to_sphere(h, mu, ATM_TOP - ATM_BOTTOM);
    float r0 = ATM_BOTTOM + h;
    float3 od = 0;
    for (int i = 0; i < steps; i++) {
        float t = t_max * (i + 0.5) / steps;
        float r = sqrt(r0 * r0 + t * t + 2.0 * r0 * t * mu);
        od += sample_medium(r - ATM_BOTTOM).extinction;
    }
    return od * (t_max / steps);
}

[numthreads(8, 8, 1)]
void cs_transmittance(uint3 id : SV_DispatchThreadID) {
    float h, mu;
    transmittance_params((id.xy + 0.5) / TRANSMITTANCE_LUT_SIZE, h, mu);
    u_lut[id.xy] = float4(exp(-optical_depth(h, mu, 40)), 1);
}

// ---------------------------------------------------------------------------------------------
// Single scattering ray march (IntegrateScatteredLuminance in the reference code), for a unit sun
// illuminance. Each step integrates the in-scattering analytically over its segment
// (S - S * exp(-ext * dt)) / ext (Hillaire 2015, "Physically Based and Unified Volumetric Rendering
// in Frostbite", slide 28), so few steps stay energy-conserving.

struct Scattering {
    float3 L;                  // in-scattered radiance towards the origin
    float3 multi_scat_as_1;    // f_ms integrand (Hillaire 2020 eq. 8): scattering with a unit-integral phase
};

Scattering integrate(float h, float3 dir, float3 sun, float min_steps, float max_steps, bool physical_phase,
                     bool ground, bool use_multiscatter) {
    Scattering res = (Scattering)0;
    float t_ground = dist_to_sphere(h, dir.y, 0.0);
    float t_max = t_ground >= 0 ? t_ground : dist_to_sphere(h, dir.y, ATM_TOP - ATM_BOTTOM);
    if (t_max <= 0) return res;

    // Variable step count (more for long paths); samples are spread quadratically so the dense
    // air near the camera gets most of them.
    float steps = lerp(min_steps, max_steps, saturate(t_max * 0.01));
    float steps_floor = floor(steps);
    float t_max_floor = t_max * steps_floor / steps;

    float cos_theta = dot(sun, dir);
    float mie_phase = hg_phase(MIE_G, cos_theta);
    float ray_phase = rayleigh_phase(cos_theta);
    float3 origin = float3(0, ATM_BOTTOM + h, 0);
    float3 throughput = 1;
    for (float s = 0; s < steps; s += 1.0) {
        float t0 = s / steps_floor, t1 = (s + 1.0) / steps_floor;
        t0 = t_max_floor * t0 * t0;
        t1 = t1 * t1 > 1.0 ? t_max : t_max_floor * t1 * t1;
        float dt = t1 - t0;
        float3 P = origin + (t0 + dt * 0.3) * dir;   // 0.3: the reference code's sample position in the segment
        float r = length(P);
        float ph = r - ATM_BOTTOM;
        float mu_s = dot(sun, P / r);
        Medium m = sample_medium(ph);
        float3 sample_t = exp(-m.extinction * dt);

        float3 phase_scattering = physical_phase ? m.mie * mie_phase + m.rayleigh * ray_phase
                                                 : m.scattering * (1.0 / (4.0 * PI));
        float earth_shadow = hits_ground(ph, mu_s) ? 0.0 : 1.0;
        float3 ms = use_multiscatter ? multiscatter_lut(ph, mu_s) : 0.0;
        float3 S = earth_shadow * transmittance_lut(ph, mu_s) * phase_scattering + ms * m.scattering;

        float3 ext = max(m.extinction, 1e-9);
        res.L += throughput * (S - S * sample_t) / ext;
        res.multi_scat_as_1 += throughput * (m.scattering - m.scattering * sample_t) / ext;
        throughput *= sample_t;
    }

    // Sun light bounced by the ground (Lambertian, GROUND_ALBEDO).
    if (ground && t_ground > 0) {
        float3 P = origin + t_ground * dir;
        float mu_s = dot(sun, normalize(P));
        res.L += throughput * transmittance_lut(0.0, mu_s) * saturate(mu_s) * GROUND_ALBEDO / PI;
    }
    return res;
}

// ---------------------------------------------------------------------------------------------
// Multiple scattering LUT (Hillaire 2020 section 5.2): for a point at altitude h with the sun at
// zenith cosine mu_s, integrate second-order scattering L2 and the transfer factor f_ms over 64
// directions (one thread each). Orders >= 2 sum to the geometric series L2 / (1 - f_ms) (eq. 10).

groupshared float3 g_l2[64];
groupshared float3 g_fms[64];

[numthreads(1, 1, 64)]
void cs_multiscatter(uint3 id : SV_DispatchThreadID) {
    float2 uv = (id.xy + 0.5) / MULTISCATTER_LUT_SIZE;
    uv = float2(from_sub_uv_to_unit(uv.x, MULTISCATTER_LUT_SIZE), from_sub_uv_to_unit(uv.y, MULTISCATTER_LUT_SIZE));
    float mu_s = uv.x * 2.0 - 1.0;
    float h = max(saturate(uv.y) * (ATM_TOP - ATM_BOTTOM), 0.001);
    float3 sun = float3(sqrt(saturate(1.0 - mu_s * mu_s)), mu_s, 0.0);

    // 8x8 stratified uniform directions on the sphere.
    uint k = id.z;
    float a = ((k / 8) + 0.5) / 8.0, b = ((k % 8) + 0.5) / 8.0;
    float theta = 2.0 * PI * a, cos_phi = 1.0 - 2.0 * b, sin_phi = sqrt(saturate(1.0 - cos_phi * cos_phi));
    float3 dir = float3(cos(theta) * sin_phi, cos_phi, sin(theta) * sin_phi);
    Scattering s = integrate(h, dir, sun, 20, 20, false, true, false);
    g_l2[k] = s.L / 64.0;
    g_fms[k] = s.multi_scat_as_1 / 64.0;
    GroupMemoryBarrierWithGroupSync();
    for (uint n = 32; n > 0; n >>= 1) {
        if (k < n) {
            g_l2[k] += g_l2[k + n];
            g_fms[k] += g_fms[k + n];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (k == 0) {
        // Averages over the sphere = integrals x isotropic phase 1/(4 pi) (eq. 5 and 7).
        u_lut[id.xy] = float4(g_l2[0] / (1.0 - g_fms[0]), 1);
    }
}

// ---------------------------------------------------------------------------------------------
// Sky-view LUT: single scattering with the real phase functions + multiple scattering from the
// LUT above, for every direction around the camera, sun-relative (the sun is in the XY plane).

void sky_view_params(float2 uv, float h, out float view_mu, out float lvc) {
    uv = float2(from_sub_uv_to_unit(uv.x, SKY_VIEW_LUT_SIZE.x), from_sub_uv_to_unit(uv.y, SKY_VIEW_LUT_SIZE.y));
    float r = ATM_BOTTOM + h;
    float beta = acos(sqrt(h * (2.0 * ATM_BOTTOM + h)) / r);
    float zenith_horizon = PI - beta;
    if (uv.y < 0.5) {
        float c = 1.0 - 2.0 * uv.y;
        view_mu = cos(zenith_horizon * (1.0 - c * c));
    } else {
        float c = uv.y * 2.0 - 1.0;
        view_mu = cos(zenith_horizon + beta * c * c);
    }
    lvc = -(uv.x * uv.x * 2.0 - 1.0);
}

[numthreads(8, 8, 1)]
void cs_sky_view(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)SKY_VIEW_LUT_SIZE)) return;
    float h = camera_altitude_km();
    float view_mu, lvc;
    sky_view_params((id.xy + 0.5) / SKY_VIEW_LUT_SIZE, h, view_mu, lvc);
    float mu_s = -sun_dir.y;
    float3 sun = float3(sqrt(saturate(1.0 - mu_s * mu_s)), mu_s, 0.0);
    float sin_v = sqrt(saturate(1.0 - view_mu * view_mu));
    float3 dir = float3(sin_v * lvc, view_mu, sin_v * sqrt(saturate(1.0 - lvc * lvc)));
    Scattering s = integrate(h, dir, sun, 4, 32, true, true, true);
    u_lut[id.xy] = float4(s.L, 1);
}

// ---------------------------------------------------------------------------------------------
// Sky ambient cube: cosine-weighted integral of the sky over 4096 uniform directions for each of
// the six axes. Stored as irradiance / pi (unit sun illuminance), see sky_ambient().
// Two passes: first the irradiance on an upward-facing surface, which then also lights the virtual
// ground seen in the lower hemisphere (so downward-facing surfaces get ground bounce, not sky blue).

static const uint AMBIENT_DIRS = 4096;
static const uint AMBIENT_THREADS = 256;
groupshared float3 g_ambient[AMBIENT_THREADS][6];

float3 fibonacci_sphere(uint i) {
    float y = 1.0 - (2.0 * i + 1.0) / AMBIENT_DIRS;
    float r = sqrt(saturate(1.0 - y * y));
    float phi = i * 2.39996323;   // golden angle
    return float3(cos(phi) * r, y, sin(phi) * r);
}

// Sums g_ambient[*][0..count) into g_ambient[0][0..count).
void reduce_ambient(uint gi, uint count) {
    GroupMemoryBarrierWithGroupSync();
    for (uint s = AMBIENT_THREADS / 2; s > 0; s >>= 1) {
        if (gi < s)
            for (uint a = 0; a < count; a++) g_ambient[gi][a] += g_ambient[gi + s][a];
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(AMBIENT_THREADS, 1, 1)]
void cs_sky_ambient(uint gi : SV_GroupIndex) {
    float3 up = 0;
    for (uint i = gi; i < AMBIENT_DIRS; i += AMBIENT_THREADS) {
        float3 d = fibonacci_sphere(i);
        if (d.y > 0) up += sky_radiance_unit(d, 0.0) * d.y;
    }
    g_ambient[gi][0] = up;
    reduce_ambient(gi, 1);
    float3 ambient_up = g_ambient[0][0] * (4.0 / AMBIENT_DIRS);   // sum(L cos) * (4 pi / N) / pi
    GroupMemoryBarrierWithGroupSync();

    float3 acc[6] = {(float3)0, (float3)0, (float3)0, (float3)0, (float3)0, (float3)0};
    for (uint i2 = gi; i2 < AMBIENT_DIRS; i2 += AMBIENT_THREADS) {
        float3 d = fibonacci_sphere(i2);
        float3 L = sky_radiance_unit(d, ambient_up);
        acc[0] += L * max(d.x, 0); acc[1] += L * max(-d.x, 0);
        acc[2] += L * max(d.y, 0); acc[3] += L * max(-d.y, 0);
        acc[4] += L * max(d.z, 0); acc[5] += L * max(-d.z, 0);
    }
    for (uint a = 0; a < 6; a++) g_ambient[gi][a] = acc[a];
    reduce_ambient(gi, 6);
    if (gi < 6) u_sky_ambient[gi] = float4(g_ambient[0][gi] * (4.0 / AMBIENT_DIRS), 0);
}
