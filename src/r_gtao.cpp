// Ambient occlusion: GTAO (Jimenez et al. 2016) at full or half resolution, with a depth-aware
// 4x4 spatial denoise and no temporal history (shaders/gtao.hlsl). The lighting pass applies it to
// indirect light only, with the multi-bounce fit and specular occlusion (shaders/gi.hlsli).
// Inputs are MSAA sample 0 of the G-buffer depth and normals; the result is a 1x R8 texture.
#include "renderer_internal.h"

struct GtaoConstants {   // GtaoCB in shaders/gtao.hlsl
    float4 target_size;
    float radius, falloff, max_radius_px;
    uint32_t slices, steps, scale;
    float pad[2];
};

static Texture g_viewz;     // R32F linear depth, AO resolution
static Texture g_raw;       // R8 noisy AO (one slice rotation per pixel)
static Texture g_denoised;  // R8 after the 4x4 denoise
static Texture g_full;      // R8 full-res result of the half-res mode
static int g_scale = 0, g_w = 0, g_h = 0;
static Shader *g_ps_prepare, *g_ps_main, *g_ps_denoise, *g_ps_upsample;

void gtao_init() {
    g_ps_prepare = load_ps("gtao.hlsl", "ps_prepare");
    g_ps_main = load_ps("gtao.hlsl", "ps_gtao");
    g_ps_denoise = load_ps("gtao.hlsl", "ps_denoise");
    g_ps_upsample = load_ps("gtao.hlsl", "ps_upsample");
}

static void ensure_targets(int scale) {
    if (g_viewz.tex && g_scale == scale && g_w == R.w && g_h == R.h) return;
    for (Texture* t : {&g_viewz, &g_raw, &g_denoised, &g_full}) release_texture(*t);
    g_scale = scale;
    g_w = R.w;
    g_h = R.h;
    int w = (R.w + scale - 1) / scale, h = (R.h + scale - 1) / scale;
    g_viewz = create_texture("gtao_viewz", w, h, DXGI_FORMAT_R32_FLOAT, TEX_RTV);
    g_raw = create_texture("gtao_raw", w, h, DXGI_FORMAT_R8_UNORM, TEX_RTV);
    g_denoised = create_texture("gtao_denoised", w, h, DXGI_FORMAT_R8_UNORM, TEX_RTV);
    if (scale > 1) g_full = create_texture("gtao_full", R.w, R.h, DXGI_FORMAT_R8_UNORM, TEX_RTV);
}

static void ao_pass(const char* name, Shader* ps, Texture& target, ID3D11ShaderResourceView* t2, ID3D11ShaderResourceView* t3) {
    GPU_SCOPE(name);
    gpu.ctx->OMSetRenderTargets(1, &target.rtv, nullptr);
    set_viewport(target.w, target.h);
    ID3D11ShaderResourceView* srvs[4] = {R.depth.srv, R.gb_normal.srv, t2, t3};
    gpu.ctx->PSSetShaderResources(0, 4, srvs);
    gpu.ctx->PSSetShader(ps->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* nulls[4] = {};
    gpu.ctx->PSSetShaderResources(0, 4, nulls);
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void pass_gtao() {
    const Settings& s = R.cfg;
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->PSSetShaderResources(25, 1, &null_srv);   // last frame's result, about to be rewritten
    if (s.ao == AO_OFF) return;

    int scale = s.ao == AO_GTAO_HALF ? 2 : 1;
    ensure_targets(scale);
    GtaoConstants c = {};
    c.target_size = {(float)g_raw.w, (float)g_raw.h, 1.0f / g_raw.w, 1.0f / g_raw.h};
    c.radius = s.ao_radius;
    c.falloff = s.ao_radius * 0.33f;
    c.max_radius_px = 64.0f;
    c.slices = (uint32_t)s.ao_slices;
    c.steps = (uint32_t)s.ao_steps;
    c.scale = (uint32_t)scale;
    update_cb(R.cb_pass, c);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);

    ao_pass("Linear depth", g_ps_prepare, g_viewz, nullptr, nullptr);
    ao_pass("Horizon search", g_ps_main, g_raw, g_viewz.srv, nullptr);
    Texture* result = &g_raw;
    if (s.ao_denoise) {
        ao_pass("Denoise 4x4", g_ps_denoise, g_denoised, g_viewz.srv, g_raw.srv);
        result = &g_denoised;
    }
    if (scale > 1) {
        ao_pass("Upsample", g_ps_upsample, g_full, g_viewz.srv, result->srv);
        result = &g_full;
    }
    gpu.ctx->PSSetShaderResources(25, 1, &result->srv);   // read by the lighting pass and debug views
}
