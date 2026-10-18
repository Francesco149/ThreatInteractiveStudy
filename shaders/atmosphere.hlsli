// Physically based sky: Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique" (EGSR 2020; reference code MIT, github.com/sebh/UnrealEngineSkyAtmosphere).
// Shared by the LUT bakes (atmosphere.hlsl) and by every shader that needs the sky at run time:
// the sky pass, the height fog in the compose pass, the GI probes' sky texels, and the sky ambient
// (the indirect-light fallback where no probe is valid, particles, glass).
//
// Units: kilometres inside the atmosphere model. The planet centre is straight below the camera,
// so a point is (0, ATM_BOTTOM + altitude, 0) with Y up, as in the engine. All LUTs store values
// for a sun illuminance of 1 at the top of the atmosphere; FrameCB.sun_color.w holds the actual
// top-of-atmosphere illuminance, so the sun intensity slider never needs a re-bake.
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI
#include "common.hlsli"

// Earth (Hillaire 2020 table 1, the values of Bruneton's 2017 demo). Coefficients in 1/km.
static const float ATM_BOTTOM = 6360.0;                                   // ground radius
static const float ATM_TOP = 6460.0;                                      // top of the atmosphere
static const float3 RAYLEIGH_SCATTERING = float3(5.802, 13.558, 33.1) * 1e-3;  // no absorption
static const float RAYLEIGH_SCALE_HEIGHT = 8.0;
static const float MIE_SCATTERING = 3.996e-3;
static const float MIE_EXTINCTION = 4.440e-3;                             // single-scattering albedo 0.9
static const float MIE_SCALE_HEIGHT = 1.2;
static const float MIE_G = 0.8;
static const float3 OZONE_ABSORPTION = float3(0.650, 1.881, 0.085) * 1e-3;    // tent profile, 10..40 km
static const float GROUND_ALBEDO = 0.3;

// LUT sizes (must match r_sky.cpp).
static const float2 TRANSMITTANCE_LUT_SIZE = float2(256, 64);
static const float MULTISCATTER_LUT_SIZE = 32;
static const float2 SKY_VIEW_LUT_SIZE = float2(192, 108);

// Bound for every pixel and compute shader once per frame (r_sky.cpp, pass_sky_luts).
Texture2D<float4> t_sky_view : register(t20);          // sky radiance around the camera
Texture2D<float4> t_transmittance : register(t21);     // transmittance along a ray (see transmittance_lut)
StructuredBuffer<float4> t_sky_ambient : register(t22); // ambient cube +X -X +Y -Y +Z -Z (see sky_ambient)

// ---------------------------------------------------------------------------------------------
// Geometry. Written in terms of altitudes rather than radii: with r ~ 6360 km, r*r - R*R loses
// all precision in fp32 for a camera a few metres above the ground.

float camera_altitude_km() { return clamp(camera_pos.y * 0.001, 0.001, 90.0); }

// Distance from a point at altitude h along a direction with zenith cosine mu to the sphere at
// altitude sphere_h, or -1 if there is no intersection in front.
float dist_to_sphere(float h, float mu, float sphere_h) {
    float r = ATM_BOTTOM + h, R = ATM_BOTTOM + sphere_h;
    float b = r * mu;
    float c = (h - sphere_h) * (r + R);     // r^2 - R^2
    float disc = b * b - c;
    if (disc < 0) return -1;
    float s = sqrt(disc);
    if (c > 0) return b < 0 ? c / (s - b) : -1;   // outside: nearest hit (= -b - s, without cancellation)
    return b > 0 ? -c / (b + s) : s - b;           // inside: the exit point
}

// Zenith cosine of the horizon (the tangent to the ground) seen from altitude h.
float horizon_mu(float h) { return -sqrt(h * (2.0 * ATM_BOTTOM + h)) / (ATM_BOTTOM + h); }
bool hits_ground(float h, float mu) { return mu < horizon_mu(h); }

// ---------------------------------------------------------------------------------------------
// LUT parameterisations (Hillaire's reference code, RenderSkyCommon.hlsl).

float from_unit_to_sub_uv(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float from_sub_uv_to_unit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

// Transmittance LUT, Bruneton 2017: x = distance to the top boundary remapped between its minimum
// and maximum for this altitude, y = sqrt-like altitude (rho / H).
float2 transmittance_uv(float h, float mu) {
    float H = sqrt(ATM_TOP * ATM_TOP - ATM_BOTTOM * ATM_BOTTOM);
    float rho = sqrt(max(h * (2.0 * ATM_BOTTOM + h), 0.0));
    float d = max(dist_to_sphere(h, mu, ATM_TOP - ATM_BOTTOM), 0.0);
    float d_min = ATM_TOP - (ATM_BOTTOM + h), d_max = rho + H;
    return float2((d - d_min) / (d_max - d_min), rho / H);
}

// Transmittance from altitude h along mu to the top of the atmosphere, or to the ground for rays
// that hit it (as in the reference code; the sun's visibility is tested separately).
float3 transmittance_lut(float h, float mu) {
    return t_transmittance.SampleLevel(s_linear_clamp, transmittance_uv(h, mu), 0).rgb;
}

// Sky-view LUT: longitude/latitude around the camera, relative to the sun's azimuth. u packs more
// texels towards the sun, v is split at the horizon with a squared mapping on each side so the
// horizon (where the colour changes fastest) gets the most texels.
float2 sky_view_uv(float h, float view_mu, float light_view_cos, bool ground) {
    float r = ATM_BOTTOM + h;
    float beta = acos(sqrt(h * (2.0 * ATM_BOTTOM + h)) / r);   // nadir to horizon
    float zenith_horizon = PI - beta;
    float2 uv;
    float view_angle = acos(clamp(view_mu, -1.0, 1.0));
    if (!ground) uv.y = (1.0 - sqrt(max(1.0 - view_angle / zenith_horizon, 0.0))) * 0.5;
    else uv.y = sqrt(max((view_angle - zenith_horizon) / beta, 0.0)) * 0.5 + 0.5;
    uv.x = sqrt(saturate(-light_view_cos * 0.5 + 0.5));
    uv = float2(from_unit_to_sub_uv(uv.x, SKY_VIEW_LUT_SIZE.x), from_unit_to_sub_uv(uv.y, SKY_VIEW_LUT_SIZE.y));
    // The LUT has the (dark) ground below the horizon: keep the bilinear filter on one side of it,
    // or the sky gets a dark line just above the horizon.
    float half_rows = SKY_VIEW_LUT_SIZE.y * 0.5;
    uv.y = ground ? max(uv.y, (half_rows + 0.5) / SKY_VIEW_LUT_SIZE.y) : min(uv.y, (half_rows - 0.5) / SKY_VIEW_LUT_SIZE.y);
    return uv;
}

// Cosine of the azimuth between a view direction and the sun (both projected on the ground plane).
float light_view_cos(float3 dir, float3 sun) {
    float len2 = dot(dir.xz, dir.xz) * dot(sun.xz, sun.xz);
    return len2 > 1e-12 ? clamp(dot(dir.xz, sun.xz) * rsqrt(len2), -1.0, 1.0) : 0.0;
}

// ---------------------------------------------------------------------------------------------
// Run-time lookups.

// Sky radiance seen from the camera along dir, for a unit sun illuminance (the sun disk is added by
// the sky pass only). Below the horizon the LUT holds the haze in front of a sun-lit ground of
// GROUND_ALBEDO; the ground also reflects the skylight, which the LUT leaves out: `ambient_up` is
// the sky irradiance / pi on an upward-facing surface (the +Y entry of the ambient cube), attenuated
// by the transmittance between the camera and the ground.
float3 sky_radiance_unit(float3 dir, float3 ambient_up) {
    float h = camera_altitude_km();
    bool ground = hits_ground(h, dir.y);
    float2 uv = sky_view_uv(h, dir.y, light_view_cos(dir, -sun_dir.xyz), ground);
    float3 L = t_sky_view.SampleLevel(s_linear_clamp, uv, 0).rgb;
    if (ground) L += GROUND_ALBEDO * ambient_up * transmittance_lut(h, dir.y);
    return L;
}

// The same in lighting units.
float3 sky_radiance(float3 dir) { return sky_radiance_unit(dir, t_sky_ambient[2].rgb) * sun_color.w; }

// The sky just above the horizon in the azimuth of dir: what distant fog converges to (Wenzel 2006:
// match the fog colour to the sky horizon so fogged geometry blends into the sky).
float3 sky_horizon_radiance(float3 dir) {
    float h = camera_altitude_km();
    float2 uv = sky_view_uv(h, horizon_mu(h), light_view_cos(dir, -sun_dir.xyz), false);  // last sky row
    return t_sky_view.SampleLevel(s_linear_clamp, uv, 0).rgb * sun_color.w;
}

// Exponential height fog (analytic integral along the view ray, as in UE's ExponentialHeightFog),
// for a ray from the camera (metres). Returns in-scattered light (rgb) and transmittance (a). The
// in-scattered colour is the sky just above the horizon in the ray's azimuth (Wenzel 2006), so
// distant geometry fades into the sky instead of into a grey wall, and the haze on the sun's side
// glows warm at golden hour. Used by the compose pass, and by the sky pass for the virtual ground
// below the horizon so the two meet without a seam.
// Density: 0.001/m at the ground (~4 km visibility, a light haze). The old 0.0035 was tuned for a
// dim ambient fog colour; with the real horizon radiance, whose Mie aureole towards a low sun is
// very bright, it turned backlit views into a white veil.
float4 height_fog(float3 ray) {
    const float density = 0.001, falloff = 0.12, base_height = 0.0;
    float dist = length(ray);
    float h0 = camera_pos.y - base_height;
    float k = max(falloff * ray.y, -80.0);   // keeps exp(-k) finite for rays > ~650 m downwards
    float integral = density * exp(-falloff * h0) * (abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0) * dist;
    float transmittance = exp(-integral);
    return float4(sky_horizon_radiance(ray) * (1.0 - transmittance), transmittance);
}

// Sky irradiance / pi for a surface with normal n: the radiance a white Lambertian surface would
// reflect, which is what the diffuse lighting buffer holds. Stored as an "ambient cube" (six
// cosine-weighted irradiances along the axes, McTaggart 2004 / Valve), evaluated with n^2 weights.
// The probe grid falls back to it where no probe is valid (indirect_light in gi.hlsli).
float3 sky_ambient(float3 n) {
    float3 n2 = n * n;
    float3 c = n2.x * t_sky_ambient[n.x < 0 ? 1 : 0].rgb + n2.y * t_sky_ambient[n.y < 0 ? 3 : 2].rgb +
               n2.z * t_sky_ambient[n.z < 0 ? 5 : 4].rgb;
    return c * sun_color.w;
}

#endif
