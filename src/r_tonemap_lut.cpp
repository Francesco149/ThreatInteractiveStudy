// The tone curve baked into a 64^3 RGBA16F 3D LUT with GT7's pow(4) input shaper (research note 06
// section 4). Exposure and white balance are applied before the lookup, so the LUT only depends on
// the curve: it is re-baked when the tone mapper changes (or its shader is reloaded), never per
// frame. Settings::tonemap_lut = false evaluates the curve per sample instead, for the cost A/B.
// Shaders: tonemap_lut.hlsl, tonemap_ops.hlsli.
#include "renderer_internal.h"

static const int LUT_SIZE = 64;   // must match tonemap_ops.hlsli

static Texture g_lut;
static Shader* g_cs_bake = nullptr;
static int g_baked_operator = -1;
static ID3D11ComputeShader* g_baked_with = nullptr;   // a shader reload creates a new object: re-bake

struct LutCB { uint32_t op; float pad[3]; };

void tonemap_lut_init() {
    g_lut = create_texture_3d("tone_lut", LUT_SIZE, LUT_SIZE, LUT_SIZE, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    g_cs_bake = load_cs("tonemap_lut.hlsl", "cs_bake");
}

void pass_tonemap_lut() {
    if (!R.cfg.tonemap_lut) return;
    if (g_baked_operator == (int)R.cfg.tonemapper && g_baked_with == g_cs_bake->cs) return;
    LutCB cb = {(uint32_t)R.cfg.tonemapper};
    update_cb(R.cb_pass, cb);
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->PSSetShaderResources(5, 1, &null_srv);   // tonemap.hlsl reads it at t5
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &g_lut.uav, nullptr);
    gpu.ctx->CSSetShader(g_cs_bake->cs, nullptr, 0);
    gpu.ctx->Dispatch(LUT_SIZE / 4, LUT_SIZE / 4, LUT_SIZE / 4);
    ID3D11UnorderedAccessView* null_uav = nullptr;
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    g_baked_operator = R.cfg.tonemapper;
    g_baked_with = g_cs_bake->cs;
}

ID3D11ShaderResourceView* tonemap_lut_srv() { return g_lut.srv; }
