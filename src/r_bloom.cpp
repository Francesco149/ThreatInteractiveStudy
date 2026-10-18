// Bloom, Call of Duty: Advanced Warfare style (Jimenez 2014): 6 levels from half resolution down
// in one R11G11B10F texture with mips, 13-tap downsamples (Karis-weighted on the first one, which
// is a compute shader reading the MSAA scene through a groupshared tile) and 3x3 tent upsamples
// added onto the next larger level. Built from the HDR scene colour before exposure, mixed in by
// the tone-map pass (pass_tonemap). Shaders: bloom.hlsl.
#include "renderer_internal.h"

static const int BLOOM_LEVELS = 6;

static ID3D11Texture2D* g_tex = nullptr;
static ID3D11RenderTargetView* g_rtv[BLOOM_LEVELS] = {};
static ID3D11ShaderResourceView* g_srv[BLOOM_LEVELS] = {};   // one mip each: a pass never reads the level it writes
static ID3D11UnorderedAccessView* g_uav0 = nullptr;          // level 0, written by the compute downsample
static int g_w[BLOOM_LEVELS], g_h[BLOOM_LEVELS];
static Shader *g_cs_first, *g_ps_down, *g_ps_up;

struct BloomCB { float src_texel[2]; float pad[2]; };

void bloom_init() {
    g_cs_first = load_cs("bloom.hlsl", "cs_downsample_first");
    g_ps_down = load_ps("bloom.hlsl", "ps_downsample");
    g_ps_up = load_ps("bloom.hlsl", "ps_upsample");
}

static void ensure_targets() {
    int w = (R.w + 1) / 2, h = (R.h + 1) / 2;
    if (g_tex && g_w[0] == w && g_h[0] == h) return;
    for (int i = 0; i < BLOOM_LEVELS; i++) { SAFE_RELEASE(g_rtv[i]); SAFE_RELEASE(g_srv[i]); }
    SAFE_RELEASE(g_uav0);
    SAFE_RELEASE(g_tex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = BLOOM_LEVELS;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    gpu.dev->CreateTexture2D(&d, nullptr, &g_tex);
    set_debug_name(g_tex, "bloom");
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = d.Format;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    gpu.dev->CreateUnorderedAccessView(g_tex, &ud, &g_uav0);
    for (int i = 0; i < BLOOM_LEVELS; i++) {
        g_w[i] = w > 1 ? w : 1;
        g_h[i] = h > 1 ? h : 1;
        w /= 2;
        h /= 2;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = d.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = i;
        gpu.dev->CreateRenderTargetView(g_tex, &rd, &g_rtv[i]);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = d.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = i;
        sd.Texture2D.MipLevels = 1;
        gpu.dev->CreateShaderResourceView(g_tex, &sd, &g_srv[i]);
    }
}

static void draw_level(int dst, int src, Shader* ps) {
    BloomCB cb = {{1.0f / g_w[src], 1.0f / g_h[src]}};
    update_cb(R.cb_pass, cb);
    gpu.ctx->OMSetRenderTargets(1, &g_rtv[dst], nullptr);
    gpu.ctx->PSSetShaderResources(1, 1, &g_srv[src]);
    set_viewport(g_w[dst], g_h[dst]);
    gpu.ctx->PSSetShader(ps->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->PSSetShaderResources(1, 1, &null_srv);
}

void pass_bloom() {
    if (!R.cfg.bloom || R.cfg.bloom_intensity <= 0) return;
    ensure_targets();
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    {
        GPU_SCOPE("Bloom downsample");
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.ctx->CSSetShaderResources(0, 1, &R.light_specular.srv);   // the HDR scene colour after compose + sky
        gpu.ctx->CSSetUnorderedAccessViews(0, 1, &g_uav0, nullptr);
        gpu.ctx->CSSetShader(g_cs_first->cs, nullptr, 0);
        gpu.ctx->Dispatch((g_w[0] + 7) / 8, (g_h[0] + 7) / 8, 1);
        ID3D11ShaderResourceView* null_srv = nullptr;
        ID3D11UnorderedAccessView* null_uav = nullptr;
        gpu.ctx->CSSetShaderResources(0, 1, &null_srv);
        gpu.ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        for (int i = 1; i < BLOOM_LEVELS; i++) draw_level(i, i - 1, g_ps_down);
    }
    {
        GPU_SCOPE("Bloom upsample");
        gpu.ctx->OMSetBlendState(R.bs_additive, nullptr, 0xFFFFFFFF);
        for (int i = BLOOM_LEVELS - 1; i > 0; i--) draw_level(i - 1, i, g_ps_up);
        gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    }
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// Level 0 holds the sum of all levels; the tone-map pass divides by the count.
ID3D11ShaderResourceView* bloom_srv() { return R.cfg.bloom && R.cfg.bloom_intensity > 0 ? g_srv[0] : nullptr; }
int bloom_level_count() { return BLOOM_LEVELS; }
