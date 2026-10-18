// Sky: drawn only on sky samples (stencil shading model == 0), overwriting whatever the compose
// pass left there. The atmosphere is one fetch of the sky-view LUT (Hillaire 2020, see
// atmosphere.hlsli); the sun disk is added analytically, because the LUT is far too coarse for a
// 0.53 degree disk (paper section 5.3).
#include "atmosphere.hlsli"

static const float SUN_ANGULAR_RADIUS = 0.004651;   // 0.2665 degrees
static const float SUN_LIMB_DARKENING = 0.6;        // linear limb-darkening law, u ~ 0.6 in the visible

// The sun's radiance is ~1e5 times the sky's: far beyond RGBA16F (max 65504) and far more than the
// bloom should spread. The disk is capped relative to the sun's illuminance (so the look does not
// depend on the scene's light units: ~300x the sky instead of ~1e5x), and absolutely for fp16.
static const float SUN_DISK_MAX_RELATIVE = 16.0;   // x top-of-atmosphere illuminance
static const float SUN_DISK_MAX = 60000.0;

float3 sun_disk(float3 d) {
    float3 sun = -sun_dir.xyz;
    float sin2_r = SUN_ANGULAR_RADIUS * SUN_ANGULAR_RADIUS;
    float3 c = cross(d, sun);
    float x2 = dot(c, c) / sin2_r;                   // (angle from the disk centre / disk radius)^2
    if (dot(d, sun) < 0 || x2 >= 1.0 || hits_ground(camera_altitude_km(), d.y)) return 0;
    // sun_color is the illuminance at the camera (transmittance applied on the CPU); spread it over
    // the disk's solid angle, with limb darkening normalised to keep that illuminance.
    float mu = sqrt(1.0 - x2);
    float limb = (1.0 - SUN_LIMB_DARKENING * (1.0 - mu)) / (1.0 - SUN_LIMB_DARKENING / 3.0);
    float solid_angle = PI * sin2_r;
    return min(sun_color.rgb * (limb / solid_angle), min(sun_color.w * SUN_DISK_MAX_RELATIVE, SUN_DISK_MAX));
}

float4 ps_sky(FullscreenVSOut i) : SV_Target {
    float4 ndc = float4(i.uv.x * 2 - 1, 1 - i.uv.y * 2, 0.5, 1);
    float4 w = mul(inv_view_proj, ndc);
    float3 d = normalize(w.xyz / w.w - camera_pos.xyz);
    float3 L = sky_radiance(d) + sun_disk(d);
    // Below the horizon the ray ends on the virtual ground: fog it like the scene's geometry, so it
    // continues where a finite floor ends instead of showing a seam.
    float h = camera_altitude_km();
    if (hits_ground(h, d.y)) {
        float4 fog = height_fog(d * (dist_to_sphere(h, d.y, 0.0) * 1000.0));
        L = L * fog.a + fog.rgb;
    }
    return float4(L, 0);
}
