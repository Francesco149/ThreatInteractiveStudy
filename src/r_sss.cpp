// Screen-space subsurface scattering for skin (separable, Jimenez 2015). Runs between the
// lighting passes and the composition, on the albedo-free diffuse lighting buffer, only where the
// stencil says "skin". See shaders/sss.hlsl and docs/research/05 section 3.
#include "renderer_internal.h"
#include <algorithm>
#include <vector>

struct SssCB {
    float4 kernel[17];
    float axis[2];
    float scatter_scale;
    float inv_depth_tolerance;
    float jitter_amount;
    float proj_y;
    uint32_t num_taps;
    float pad;
};

static Texture g_temp;   // MSAA RGBA16F ping-pong target (only skin samples are ever written/read)
static Shader* g_ps_blur;
static float4 g_kernel[17];
static const int TAPS = 17;

static float gaussian(float variance, float r) {
    return expf(-(r * r) / (2.0f * variance)) / (2.0f * PI * variance);
}

// d'Eon & Luebke's sum-of-Gaussians skin profile (the narrowest lobe is left out: "strength"
// keeps that part unblurred). r in millimetres, each channel stretched by its falloff.
static float skin_profile(float r, float falloff) {
    float rr = r / (0.001f + falloff);
    return 0.100f * gaussian(0.0484f, rr) + 0.118f * gaussian(0.187f, rr) + 0.113f * gaussian(0.567f, rr) +
           0.358f * gaussian(1.99f, rr) + 0.078f * gaussian(7.41f, rr);
}

// Same math as Jimenez's SeparableSSS reference kernel generator: quadratically spaced taps
// (dense near the centre), weight = profile x covered width, normalized, then blended with the
// unblurred image by a per-channel strength. Centre tap first.
static void build_kernel(float3 strength, float3 falloff) {
    const float range = 2.0f;  // mm
    struct Tap { float r, g, b, mm; };
    std::vector<Tap> k(TAPS);
    for (int i = 0; i < TAPS; i++) {
        float t = -range + 2.0f * range * i / (TAPS - 1);
        k[i].mm = (t < 0 ? -1.0f : 1.0f) * t * t / range;
    }
    for (int i = 0; i < TAPS; i++) {
        float left = i > 0 ? fabsf(k[i].mm - k[i - 1].mm) : 0.0f;
        float right = i < TAPS - 1 ? fabsf(k[i].mm - k[i + 1].mm) : 0.0f;
        float area = 0.5f * (left + right);
        k[i].r = area * skin_profile(k[i].mm, falloff.x);
        k[i].g = area * skin_profile(k[i].mm, falloff.y);
        k[i].b = area * skin_profile(k[i].mm, falloff.z);
    }
    std::rotate(k.begin(), k.begin() + TAPS / 2, k.begin() + TAPS / 2 + 1);
    float sr = 0, sg = 0, sb = 0;
    for (const Tap& t : k) { sr += t.r; sg += t.g; sb += t.b; }
    for (Tap& t : k) { t.r /= sr; t.g /= sg; t.b /= sb; }
    k[0].r = (1 - strength.x) + strength.x * k[0].r;
    k[0].g = (1 - strength.y) + strength.y * k[0].g;
    k[0].b = (1 - strength.z) + strength.z * k[0].b;
    for (int i = 1; i < TAPS; i++) { k[i].r *= strength.x; k[i].g *= strength.y; k[i].b *= strength.z; }
    for (int i = 0; i < TAPS; i++) g_kernel[i] = {k[i].r, k[i].g, k[i].b, k[i].mm};
}

void sss_init() {
    g_ps_blur = load_ps("sss.hlsl", "ps_blur");
    build_kernel({0.48f, 0.41f, 0.28f}, {1.0f, 0.37f, 0.3f});
    log_msg("SSS kernel centre weight %.4f %.4f %.4f (reference 17-tap: 0.5363 0.6246 0.7489)", g_kernel[0].x,
            g_kernel[0].y, g_kernel[0].z);
}

void pass_sss() {
    const Settings& s = R.cfg;
    if (!s.sss || !R.shading_model_used[SM_SKIN]) return;
    if (!g_temp.tex || g_temp.w != R.w || g_temp.h != R.h || g_temp.samples != R.samples) {
        release_texture(g_temp);
        g_temp = create_texture("sss_temp", R.w, R.h, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RTV | TEX_MS_SRV, R.samples);
    }
    SssCB cb = {};
    for (int i = 0; i < TAPS; i++) cb.kernel[i] = g_kernel[i];
    cb.scatter_scale = s.sss_scale;
    cb.inv_depth_tolerance = 1.0f / 0.015f;
    cb.jitter_amount = 0.2f;
    cb.proj_y = R.frame_constants.proj.m[1][1];
    cb.num_taps = TAPS;

    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    // Stencil EQUAL "skin" (shading model 1 + 1): non-skin pixels never run the shader.
    gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_SM_BITS, false), SM_SKIN + 1);
    gpu.ctx->PSSetShader(g_ps_blur->ps, nullptr, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    for (int pass = 0; pass < 2; pass++) {
        cb.axis[0] = pass == 0 ? 1.0f : 0.0f;
        cb.axis[1] = pass == 0 ? 0.0f : 1.0f;
        update_cb(R.cb_pass, cb);
        Texture& src = pass == 0 ? R.light_diffuse : g_temp;
        Texture& dst = pass == 0 ? g_temp : R.light_diffuse;
        gpu.ctx->OMSetRenderTargets(1, &dst.rtv, R.depth.dsv_readonly);
        ID3D11ShaderResourceView* srvs[3] = {src.srv, R.depth.srv, R.depth.srv_stencil};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 3, nulls);
    }
}
