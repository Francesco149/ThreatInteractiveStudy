// Post stage: auto exposure + white balance, tone mapping (+ MSAA sample separation), SMAA 1x/S2x/T2x/4x with the
// two-frame temporal resolve, final output and debug views. See docs/research/07 for the SMAA
// details (subsample indices, jitter conventions).
#include "renderer_internal.h"

static Texture g_aa_mask[2];      // r = MSAA edge, g = responsive, b = SMAA touched (ping-pong)
static Texture g_smaa_depth;      // 1x stencil for the SMAA edge -> weights optimization
static ID3D11BlendState* g_bs_smaa_first = nullptr;
static ID3D11BlendState* g_bs_smaa_second = nullptr;
static ID3D11Buffer* g_exposure_buf = nullptr;
static ID3D11ShaderResourceView* g_exposure_srv = nullptr;
static ID3D11UnorderedAccessView* g_exposure_uav = nullptr;
static ID3D11Buffer* g_white_balance = nullptr;      // [0..2] white balance matrix rows, [3] adaptation state
static ID3D11ShaderResourceView* g_white_balance_srv = nullptr;
static ID3D11UnorderedAccessView* g_white_balance_uav = nullptr;
static ID3D11Buffer* g_group_sums = nullptr;         // white balance partial sums, one per histogram group
static ID3D11UnorderedAccessView* g_group_sums_uav = nullptr;
static Shader* g_ps_tonemap[2][2];                   // [LUT][layers - 1]
static Shader *g_ps_temporal, *g_ps_copy, *g_ps_final, *g_ps_debug, *g_cs_hist, *g_cs_adapt;
static bool g_history_valid = false;
static int g_last_aa = -1;

struct TonemapCB { uint32_t tonemapper; float bloom_intensity, bloom_normalize, pad; };
struct ExposureCB { float awb_strength, awb_min_mired, awb_max_mired, wb_warmth_mired; };
struct SmaaCB { float4 rt_metrics; float4 subsample_indices; };
struct TemporalCB { float4 rt_metrics; float weight_scale, history_valid, edge_only, clamp_speed_px; };
struct FinalCB { float4 src_size; float grain, vignette, brightness, chromatic, ui_enabled, pad[3]; };

void post_init() {
    for (int lut = 0; lut < 2; lut++) {
        const char* l = lut ? "1" : "0";
        g_ps_tonemap[lut][0] = load_ps("tonemap.hlsl", "ps_main", {{"LAYERS", "1"}, {"TONEMAP_LUT", l}});
        g_ps_tonemap[lut][1] = load_ps("tonemap.hlsl", "ps_main", {{"LAYERS", "2"}, {"TONEMAP_LUT", l}});
    }
    R.vs_smaa_edge = load_vs("smaa.hlsl", "vs_edge");
    R.ps_smaa_edge = load_ps("smaa.hlsl", "ps_edge");
    R.vs_smaa_weight = load_vs("smaa.hlsl", "vs_weight");
    R.ps_smaa_weight = load_ps("smaa.hlsl", "ps_weight");
    R.vs_smaa_blend = load_vs("smaa.hlsl", "vs_blend");
    R.ps_smaa_blend = load_ps("smaa.hlsl", "ps_blend");
    g_ps_temporal = load_ps("temporal.hlsl", "ps_main");
    g_ps_copy = load_ps("temporal.hlsl", "ps_copy");
    g_ps_final = load_ps("final.hlsl", "ps_final");
    g_ps_debug = load_ps("debug.hlsl", "ps_main");
    g_cs_hist = load_cs("exposure.hlsl", "cs_histogram");
    g_cs_adapt = load_cs("exposure.hlsl", "cs_adapt");

    // SMAA neighborhood blending, layer 0: RT0 overwrite, RT1 (mask) blue channel only.
    // Layer 1: RT0 averaged in with blend factor 0.5 (in the sRGB target: a linear average),
    // RT1 blue with MAX.
    D3D11_BLEND_DESC d = {};
    d.IndependentBlendEnable = TRUE;
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_BLUE;
    gpu.dev->CreateBlendState(&d, &g_bs_smaa_first);
    d.RenderTarget[0].BlendEnable = TRUE;
    d.RenderTarget[0].SrcBlend = d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
    d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[1].BlendEnable = TRUE;
    d.RenderTarget[1].SrcBlend = d.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[1].DestBlend = d.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[1].BlendOp = d.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    gpu.dev->CreateBlendState(&d, &g_bs_smaa_second);

    float init = 0;
    g_exposure_buf = create_structured_buffer(sizeof(float), 1, &init, true, false, &g_exposure_srv, &g_exposure_uav);
    float4 wb_init[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 0}};   // identity, not yet adapted
    g_white_balance = create_structured_buffer(sizeof(float4), 4, wb_init, true, false, &g_white_balance_srv, &g_white_balance_uav);
    g_group_sums = create_structured_buffer(sizeof(float4), 8192, nullptr, true, false, nullptr, &g_group_sums_uav);  // up to 8K
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 256 * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    std::vector<uint32_t> zeros(256, 0);
    D3D11_SUBRESOURCE_DATA init_hist = {zeros.data()};
    gpu.dev->CreateBuffer(&bd, &init_hist, &R.histogram);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 256;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    gpu.dev->CreateUnorderedAccessView(R.histogram, &ud, &R.histogram_uav);
}

static void ensure_post_targets() {
    if (g_smaa_depth.tex && g_smaa_depth.w == R.w && g_smaa_depth.h == R.h) return;
    for (Texture& t : g_aa_mask) release_texture(t);
    release_texture(g_smaa_depth);
    for (int i = 0; i < 2; i++) g_aa_mask[i] = create_texture("aa_mask", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RTV);
    g_smaa_depth = create_texture("smaa_stencil", R.w, R.h, DXGI_FORMAT_D24_UNORM_S8_UINT, TEX_DEPTH);
    g_history_valid = false;
}

void pass_velocity() {}  // folded into the tone mapping pass (velocity1x output)

// Metering runs even with auto exposure off: the same histogram pass feeds the white balance.
void pass_exposure() {
    const Settings& s = R.cfg;
    ExposureCB cb = {s.auto_white_balance ? s.awb_strength : 0.0f, 1e6f / fmaxf(s.awb_max_kelvin, 1000.0f),
                     1e6f / fmaxf(s.awb_min_kelvin, 1000.0f), s.wb_warmth};
    update_cb(R.cb_pass, cb);
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* srvs[2] = {R.light_diffuse.srv, R.depth.srv};
    gpu.ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[4] = {R.histogram_uav, g_exposure_uav, g_group_sums_uav, g_white_balance_uav};
    gpu.ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    gpu.ctx->CSSetShader(g_cs_hist->cs, nullptr, 0);
    gpu.ctx->Dispatch((R.w / 4 + 15) / 16, (R.h / 4 + 15) / 16, 1);
    gpu.ctx->CSSetShader(g_cs_adapt->cs, nullptr, 0);
    gpu.ctx->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView* nsrv[2] = {};
    ID3D11UnorderedAccessView* nuav[4] = {};
    gpu.ctx->CSSetShaderResources(0, 2, nsrv);
    gpu.ctx->CSSetUnorderedAccessViews(0, 4, nuav, nullptr);
}

// Effective AA mode given the MSAA count (S2x needs exactly 2 samples).
static AAMode effective_aa() {
    AAMode m = R.cfg.aa;
    if (R.samples != 2) {
        if (m == AA_SMAA_S2X) m = AA_SMAA_1X;
        if (m == AA_SMAA_4X) m = AA_SMAA_T2X;
    }
    return m;
}

static int layer_count() {
    AAMode m = effective_aa();
    return (m == AA_SMAA_S2X || m == AA_SMAA_4X) ? 2 : 1;
}

void pass_tonemap() {
    ensure_post_targets();
    int cur = R.frame & 1;
    int layers = layer_count();
    ID3D11ShaderResourceView* bloom = bloom_srv();
    TonemapCB cb = {(uint32_t)R.cfg.tonemapper, bloom ? R.cfg.bloom_intensity : 0.0f, 1.0f / bloom_level_count()};
    update_cb(R.cb_pass, cb);
    ID3D11RenderTargetView* rtvs[4];
    int n = 0;
    // Two layers (S2x / 4x) are written sRGB-encoded through the UNORM views, as the LUT stores
    // them; a single layer averages the samples in linear and goes through the SRGB view.
    for (int l = 0; l < layers; l++) rtvs[n++] = layers == 2 ? R.ldr[l].rtv : R.ldr_srgb_rtv[l];
    rtvs[n++] = R.velocity.rtv;
    rtvs[n++] = g_aa_mask[cur].rtv;
    gpu.ctx->OMSetRenderTargets(n, rtvs, nullptr);
    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    ID3D11ShaderResourceView* srvs[7] = {R.light_specular.srv, R.gb_motion.srv, R.depth.srv, g_exposure_srv,
                                         g_white_balance_srv, tonemap_lut_srv(), bloom};
    gpu.ctx->PSSetShaderResources(0, 7, srvs);
    gpu.ctx->PSSetShader(g_ps_tonemap[R.cfg.tonemap_lut ? 1 : 0][layers - 1]->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* nulls[7] = {};
    gpu.ctx->PSSetShaderResources(0, 7, nulls);
}

static void copy_to(ID3D11RenderTargetView* dst, ID3D11ShaderResourceView* src) {
    gpu.ctx->OMSetRenderTargets(1, &dst, nullptr);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    gpu.ctx->PSSetShaderResources(0, 1, &src);
    gpu.ctx->PSSetShader(g_ps_copy->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->PSSetShaderResources(0, 1, &null_srv);
}

// SMAA 1x on one layer, writing (or averaging into) smaa_out[cur].
static void smaa_layer(int layer, float4 subsample, bool average_in) {
    int cur = R.frame & 1;
    float zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearRenderTargetView(R.smaa_edges.rtv, zero);
    gpu.ctx->ClearRenderTargetView(R.smaa_weights.rtv, zero);
    gpu.ctx->ClearDepthStencilView(g_smaa_depth.dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
    SmaaCB cb = {{1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h}, subsample};
    update_cb(R.cb_pass, cb);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->IASetInputLayout(nullptr);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        GPU_SCOPE("SMAA edges");
        gpu.ctx->OMSetRenderTargets(1, &R.smaa_edges.rtv, g_smaa_depth.dsv);
        gpu.ctx->OMSetDepthStencilState(R.ds_smaa_mark, 1);
        gpu.ctx->PSSetShaderResources(0, 1, &R.ldr[layer].srv);  // gamma (UNORM) view
        gpu.ctx->VSSetShader(R.vs_smaa_edge->vs, nullptr, 0);
        gpu.ctx->PSSetShader(R.ps_smaa_edge->ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);
    }
    {
        GPU_SCOPE("SMAA weights");
        gpu.ctx->OMSetRenderTargets(1, &R.smaa_weights.rtv, g_smaa_depth.dsv);
        gpu.ctx->OMSetDepthStencilState(R.ds_smaa_test, 1);
        ID3D11ShaderResourceView* srvs[3] = {R.smaa_edges.srv, R.smaa_area.srv, R.smaa_search.srv};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        gpu.ctx->VSSetShader(R.vs_smaa_weight->vs, nullptr, 0);
        gpu.ctx->PSSetShader(R.ps_smaa_weight->ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);
    }
    {
        GPU_SCOPE("SMAA blend");
        ID3D11RenderTargetView* rtvs[2] = {R.smaa_out_srgb_rtv[cur], g_aa_mask[cur].rtv};
        gpu.ctx->OMSetRenderTargets(2, rtvs, nullptr);
        gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
        float factor[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        gpu.ctx->OMSetBlendState(average_in ? g_bs_smaa_second : g_bs_smaa_first, factor, 0xFFFFFFFF);
        ID3D11ShaderResourceView* srvs[3] = {R.ldr_srgb_srv[layer], R.smaa_weights.srv, R.velocity.srv};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        gpu.ctx->VSSetShader(R.vs_smaa_blend->vs, nullptr, 0);
        gpu.ctx->PSSetShader(R.ps_smaa_blend->ps, nullptr, 0);
        gpu.ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* nulls[3] = {};
    gpu.ctx->PSSetShaderResources(0, 3, nulls);
}

void pass_antialiasing() {
    AAMode m = effective_aa();
    int cur = R.frame & 1, prev = cur ^ 1;
    if ((int)m != g_last_aa) g_history_valid = false;
    g_last_aa = m;
    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);

    if (m == AA_NONE || m == AA_MSAA_RESOLVE) {
        copy_to(R.aa_out.rtv, R.ldr_srgb_srv[0]);
        g_history_valid = false;
        return;
    }

    // Subsample indices: which sub-pixel offsets the area texture assumes (SMAA.hlsl table; per
    // frame parity p and MSAA layer, docs/research/07 section 2).
    int p = R.frame & 1;
    if (m == AA_SMAA_1X) {
        smaa_layer(0, {0, 0, 0, 0}, false);
    } else if (m == AA_SMAA_S2X) {
        smaa_layer(0, {1, 1, 1, 0}, false);
        smaa_layer(1, {2, 2, 2, 0}, true);
    } else if (m == AA_SMAA_T2X) {
        smaa_layer(0, p == 0 ? float4{1, 1, 1, 0} : float4{2, 2, 2, 0}, false);
    } else {  // 4x
        static const float4 idx[4] = {{5, 3, 1, 3}, {4, 6, 2, 3}, {3, 5, 1, 4}, {6, 4, 2, 4}};
        smaa_layer(0, idx[2 * p + 0], false);
        smaa_layer(1, idx[2 * p + 1], true);
    }

    if (m == AA_SMAA_T2X || m == AA_SMAA_4X) {
        GPU_SCOPE("Temporal resolve (2 frames)");
        TemporalCB cb = {{1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h}, 30.0f, g_history_valid ? 1.0f : 0.0f, 1.0f, 0.5f};
        update_cb(R.cb_pass, cb);
        gpu.ctx->OMSetRenderTargets(1, &R.aa_out.rtv, nullptr);
        gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
        gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
        ID3D11ShaderResourceView* srvs[5] = {R.smaa_out_srgb_srv[cur], R.smaa_out_srgb_srv[prev], R.velocity.srv,
                                             g_aa_mask[cur].srv, g_aa_mask[prev].srv};
        gpu.ctx->PSSetShaderResources(0, 5, srvs);
        gpu.ctx->PSSetShader(g_ps_temporal->ps, nullptr, 0);
        draw_fullscreen();
        ID3D11ShaderResourceView* nulls[5] = {};
        gpu.ctx->PSSetShaderResources(0, 5, nulls);
        g_history_valid = true;
    } else {
        copy_to(R.aa_out.rtv, R.smaa_out_srgb_srv[cur]);
        g_history_valid = false;
    }
}

void pass_debug_view() {
    gpu.ctx->OMSetRenderTargets(1, &R.post_out.rtv, nullptr);
    set_viewport(R.w, R.h);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    gpu.ctx->RSSetState(R.rs_none);
    ID3D11ShaderResourceView* srvs[14] = {R.depth.srv, R.gb_albedo.srv, R.gb_normal.srv, R.gb_material.srv,
                                          R.gb_motion.srv, R.depth.srv_stencil, nullptr, R.light_diffuse.srv,
                                          nullptr, nullptr, nullptr, R.light_specular.srv, R.overdraw.srv, g_exposure_srv};
    // t8-t10 are the frame's structured buffers; keep them bound.
    gpu.ctx->PSSetShaderResources(0, 8, srvs);
    gpu.ctx->PSSetShaderResources(11, 3, srvs + 11);
    gpu.ctx->PSSetShader(g_ps_debug->ps, nullptr, 0);
    local_shadows_bind(true);  // VIEW_SHADOW_DENSITY reads the projectors
    draw_fullscreen();
    local_shadows_bind(false);
    ID3D11ShaderResourceView* nulls[8] = {};
    gpu.ctx->PSSetShaderResources(0, 8, nulls);
    gpu.ctx->PSSetShaderResources(11, 3, nulls);
}

// The UI gets its own target at window resolution, cleared every frame; the final pass overlays
// it in the same draw that applies the display calibration (TI, Days Gone).
static Texture g_ui;

ID3D11RenderTargetView* renderer_ui_target() {
    if (!g_ui.tex || g_ui.w != gpu.backbuffer_w || g_ui.h != gpu.backbuffer_h) {
        release_texture(g_ui);
        g_ui = create_texture("ui", gpu.backbuffer_w, gpu.backbuffer_h, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RTV);
    }
    float zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearRenderTargetView(g_ui.rtv, zero);
    return g_ui.rtv;
}

void renderer_final_to_backbuffer(bool ui) {
    GPU_SCOPE("Final output + UI composite");
    const Settings& s = R.cfg;
    bool debug = s.view != VIEW_FINAL;
    ID3D11ShaderResourceView* srvs[2] = {debug ? R.post_out.srv : (R.final_srv ? R.final_srv : R.aa_out.srv),
                                         ui && g_ui.srv ? g_ui.srv : nullptr};
    gpu.ctx->OMSetRenderTargets(1, &gpu.backbuffer_rtv_unorm, nullptr);
    set_viewport(gpu.backbuffer_w, gpu.backbuffer_h);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    gpu.ctx->RSSetState(R.rs_none);
    FinalCB cb = {{(float)R.w, (float)R.h, 1.0f / R.w, 1.0f / R.h},
                  debug ? 0.0f : s.film_grain,
                  debug ? 0.0f : s.vignette,
                  debug ? 0.0f : s.brightness,
                  debug ? 0.0f : s.chromatic_aberration,
                  srvs[1] ? 1.0f : 0.0f};
    update_cb(R.cb_pass, cb);
    gpu.ctx->PSSetShaderResources(0, 2, srvs);
    gpu.ctx->PSSetShader(g_ps_final->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* nulls[2] = {};
    gpu.ctx->PSSetShaderResources(0, 2, nulls);
}
