// Sky and atmosphere (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique"): LUT bakes, the sky's ambient cube, and the sun's colour at the camera.
//
//   transmittance LUT 256x64, multi-scattering LUT 32x32   baked once (atmosphere constants only)
//   sky-view LUT 192x108 + ambient cube                    re-baked when the sun moves or the
//                                                          camera's altitude changes by > 1 m
//   sky pass (sky.hlsl), fog colour (compose.hlsl), the indirect-light fallback (gi.hlsli),
//   particles and glass (vfx.hlsl), GI probe relight (gi_relight.hlsl) read them
//
// All LUTs are for a unit sun illuminance at the top of the atmosphere; FrameCB.sun_color.w carries
// the real one, so the sun intensity slider does not re-bake anything. Shaders: atmosphere.hlsl(i).
#include "renderer_internal.h"

// Must match shaders/atmosphere.hlsli.
static const int TRANSMITTANCE_W = 256, TRANSMITTANCE_H = 64;
static const int MULTISCATTER_SIZE = 32;
static const int SKY_VIEW_W = 192, SKY_VIEW_H = 108;
static const UINT SKY_SRV_SLOT = 20;   // t20 sky view, t21 transmittance, t22 ambient cube, t23 multi-scattering

static Texture g_transmittance, g_multiscatter, g_sky_view;
static ID3D11Buffer* g_ambient = nullptr;
static ID3D11ShaderResourceView* g_ambient_srv = nullptr;
static ID3D11UnorderedAccessView* g_ambient_uav = nullptr;
static Shader *g_cs_transmittance, *g_cs_multiscatter, *g_cs_sky_view, *g_cs_ambient;

// What the current LUTs were baked for. A shader reload (F5) creates new shader objects, so
// comparing the pointers also re-bakes after editing atmosphere.hlsl.
static ID3D11ComputeShader* g_baked_static = nullptr;
static ID3D11ComputeShader* g_baked_view = nullptr;
static float3 g_baked_sun = {0, 0, 0};
static float g_baked_camera_y = -1e9f;

void sky_init() {
    g_transmittance = create_texture("sky_transmittance", TRANSMITTANCE_W, TRANSMITTANCE_H, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    g_multiscatter = create_texture("sky_multiscatter", MULTISCATTER_SIZE, MULTISCATTER_SIZE, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    g_sky_view = create_texture("sky_view", SKY_VIEW_W, SKY_VIEW_H, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    float4 zeros[6] = {};
    g_ambient = create_structured_buffer(sizeof(float4), 6, zeros, true, false, &g_ambient_srv, &g_ambient_uav);
    set_debug_name(g_ambient, "sky_ambient_cube");
    g_cs_transmittance = load_cs("atmosphere.hlsl", "cs_transmittance");
    g_cs_multiscatter = load_cs("atmosphere.hlsl", "cs_multiscatter");
    g_cs_sky_view = load_cs("atmosphere.hlsl", "cs_sky_view");
    g_cs_ambient = load_cs("atmosphere.hlsl", "cs_sky_ambient");
}

// ---------------------------------------------------------------------------------------------
// The sun's colour: the same 40-step optical-depth integral the transmittance LUT stores, evaluated
// on the CPU for the one direction that matters, so the sun colour stays a plain constant in FrameCB
// (no GPU readback) and agrees with the sky by construction.

static const float ATM_BOTTOM = 6360.0f, ATM_TOP = 6460.0f;   // km
static const float SUN_ANGULAR_RADIUS = 0.004651f;            // 0.2665 degrees

static float3 extinction(float h) {
    h = fmaxf(h, 0.0f);
    float d_rayleigh = expf(-h / 8.0f), d_mie = expf(-h / 1.2f);
    float d_ozone = fmaxf(0.0f, 1.0f - fabsf(h - 25.0f) / 15.0f);
    float3 rayleigh = float3{5.802f, 13.558f, 33.1f} * 1e-3f;
    float3 ozone = float3{0.650f, 1.881f, 0.085f} * 1e-3f;
    return rayleigh * d_rayleigh + float3{4.44e-3f, 4.44e-3f, 4.44e-3f} * d_mie + ozone * d_ozone;
}

// Distance from altitude h along zenith cosine mu to the top of the atmosphere (the ray is assumed
// not to hit the ground).
static float dist_to_top(float h, float mu) {
    float r = ATM_BOTTOM + h;
    float b = r * mu, c = (h - (ATM_TOP - ATM_BOTTOM)) * (r + ATM_TOP);   // r^2 - Rt^2 < 0
    float s = sqrtf(fmaxf(b * b - c, 0.0f));
    return b > 0 ? -c / (b + s) : s - b;
}

static float horizon_mu(float h) { return -sqrtf(h * (2.0f * ATM_BOTTOM + h)) / (ATM_BOTTOM + h); }

static float3 transmittance(float h, float mu) {
    const int steps = 40;
    float t_max = dist_to_top(h, mu), r0 = ATM_BOTTOM + h;
    float3 od = {0, 0, 0};
    for (int i = 0; i < steps; i++) {
        float t = t_max * (i + 0.5f) / steps;
        float r = sqrtf(r0 * r0 + t * t + 2.0f * r0 * t * mu);
        od += extinction(r - ATM_BOTTOM);
    }
    od = od * (t_max / steps);
    return {expf(-od.x), expf(-od.y), expf(-od.z)};
}

float3 sky_sun_illuminance(float3 sun_dir, float camera_y, float toa_illuminance) {
    float h = clampf(camera_y * 0.001f, 0.001f, 90.0f);   // camera_altitude_km() in atmosphere.hlsli
    float mu = -sun_dir.y, mu_h = horizon_mu(h);
    // Fraction of the sun's disk above the horizon (linear over the disk's diameter), so the sun
    // fades out over half a degree at sunset instead of switching off.
    float visible = clampf((asinf(mu) - asinf(mu_h)) / (2.0f * SUN_ANGULAR_RADIUS) + 0.5f, 0.0f, 1.0f);
    return transmittance(h, fmaxf(mu, mu_h + 1e-4f)) * (toa_illuminance * visible);
}

// ---------------------------------------------------------------------------------------------

static void run_cs(Shader* cs, ID3D11UnorderedAccessView* uav0, ID3D11UnorderedAccessView* uav1, UINT x, UINT y, UINT z) {
    ID3D11UnorderedAccessView* uavs[2] = {uav0, uav1};
    gpu.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    gpu.ctx->CSSetShader(cs->cs, nullptr, 0);
    gpu.ctx->Dispatch(x, y, z);
    ID3D11UnorderedAccessView* nulls[2] = {};
    gpu.ctx->CSSetUnorderedAccessViews(0, 2, nulls, nullptr);
}

static void set_sky_srvs(bool ps, ID3D11ShaderResourceView* sky_view, ID3D11ShaderResourceView* transmittance,
                         ID3D11ShaderResourceView* ambient, ID3D11ShaderResourceView* multiscatter) {
    ID3D11ShaderResourceView* srvs[4] = {sky_view, transmittance, ambient, multiscatter};
    gpu.ctx->CSSetShaderResources(SKY_SRV_SLOT, 4, srvs);
    if (ps) gpu.ctx->PSSetShaderResources(SKY_SRV_SLOT, 4, srvs);
}

void pass_sky_luts() {
    const FrameConstants& fc = R.frame_constants;
    float3 sun = {fc.sun_dir.x, fc.sun_dir.y, fc.sun_dir.z};
    float camera_y = fc.camera_pos.y;
    bool bake_static = g_baked_static != g_cs_multiscatter->cs;
    bool bake_view = bake_static || g_baked_view != g_cs_sky_view->cs || length(sun - g_baked_sun) > 1e-6f ||
                     fabsf(camera_y - g_baked_camera_y) > 1.0f;   // 1 m: the horizon moves < 0.04 degrees

    if (bake_static || bake_view) {
        set_sky_srvs(true, nullptr, nullptr, nullptr, nullptr);   // they are about to be written
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (bake_static) {
        GPU_SCOPE("Transmittance + multi-scattering LUTs");
        run_cs(g_cs_transmittance, g_transmittance.uav, nullptr, TRANSMITTANCE_W / 8, TRANSMITTANCE_H / 8, 1);
        set_sky_srvs(false, nullptr, g_transmittance.srv, nullptr, nullptr);
        run_cs(g_cs_multiscatter, g_multiscatter.uav, nullptr, MULTISCATTER_SIZE, MULTISCATTER_SIZE, 1);
        g_baked_static = g_cs_multiscatter->cs;
    }
    if (bake_view) {
        {
            GPU_SCOPE("Sky-view LUT");
            set_sky_srvs(false, nullptr, g_transmittance.srv, nullptr, g_multiscatter.srv);
            run_cs(g_cs_sky_view, g_sky_view.uav, nullptr, (SKY_VIEW_W + 7) / 8, (SKY_VIEW_H + 7) / 8, 1);
        }
        {
            GPU_SCOPE("Sky ambient cube");
            set_sky_srvs(false, g_sky_view.srv, g_transmittance.srv, nullptr, nullptr);
            run_cs(g_cs_ambient, nullptr, g_ambient_uav, 1, 1, 1);
        }
        g_baked_view = g_cs_sky_view->cs;
        g_baked_sun = sun;
        g_baked_camera_y = camera_y;
    }
    // Every later pass (and compute shader) can read the sky: rebind each frame.
    set_sky_srvs(true, g_sky_view.srv, g_transmittance.srv, g_ambient_srv, nullptr);
}
