// Deferred G-buffer decals (albedo only), drawn right after the base pass. See shaders/decal.hlsl.
// Decal textures are generated procedurally at startup (stains, moss, cracks), so no assets needed.
#include "renderer_internal.h"

struct DecalCB {
    float4x4 world;
    float4x4 inv_world;
    float4 tint;
};

static ID3D11Buffer* g_box_vb = nullptr;
static ID3D11Buffer* g_box_ib = nullptr;
static ID3D11BlendState* g_bs_decal = nullptr;
static ID3D11RasterizerState* g_rs_back = nullptr;
static Shader *g_vs, *g_ps;
static Texture g_tex[3];   // stain, moss, cracks

static float hashf(int x, int y, int s) { return (hash_u32(x * 73856093u ^ y * 19349663u ^ s * 83492791u) & 0xFFFF) / 65535.0f; }

static float value_noise(float x, float y, int seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hashf(xi, yi, seed), b = hashf(xi + 1, yi, seed), c = hashf(xi, yi + 1, seed), d = hashf(xi + 1, yi + 1, seed);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

static float fbm(float x, float y, int seed) {
    float v = 0, amp = 0.5f;
    for (int o = 0; o < 5; o++) { v += amp * value_noise(x, y, seed + o); x *= 2.03f; y *= 2.03f; amp *= 0.5f; }
    return v;
}

static Texture make_decal_texture(int kind) {
    const int N = 256;
    std::vector<uint8_t> px(N * N * 4);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (x + 0.5f) / N * 2 - 1, v = (y + 0.5f) / N * 2 - 1;
            float r = sqrtf(u * u + v * v);
            float n = fbm(x / 40.0f, y / 40.0f, kind * 17);
            float a = 0;
            float3 c = {0, 0, 0};
            if (kind == 0) {  // dark water/oil stain with a soft irregular border
                a = saturate((0.75f - r + (n - 0.5f) * 0.6f) * 4.0f) * 0.85f;
                c = float3{0.10f, 0.08f, 0.06f} * (0.7f + 0.6f * n);
            } else if (kind == 1) {  // moss patches
                float m = fbm(x / 18.0f, y / 18.0f, 99);
                a = saturate((0.8f - r) * 3.0f) * saturate((m - 0.45f) * 5.0f);
                c = float3{0.16f, 0.24f, 0.07f} * (0.6f + 0.8f * n);
            } else {  // cracks: thin dark lines along noise iso-contours
                float f = fbm(x / 60.0f, y / 60.0f, 7);
                float line = 1.0f - saturate(fabsf(f - 0.5f) * 60.0f);
                a = line * saturate((0.9f - r) * 4.0f) * 0.9f;
                c = {0.05f, 0.05f, 0.05f};
            }
            uint8_t* p = &px[(y * N + x) * 4];
            p[0] = (uint8_t)(saturate(c.x) * 255);
            p[1] = (uint8_t)(saturate(c.y) * 255);
            p[2] = (uint8_t)(saturate(c.z) * 255);
            p[3] = (uint8_t)(saturate(a) * 255);
        }
    Texture t = create_texture("decal", N, N, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RTV | TEX_MIPS);
    gpu.ctx->UpdateSubresource(t.tex, 0, nullptr, px.data(), N * 4, 0);
    gpu.ctx->GenerateMips(t.srv);
    return t;
}

void decals_init() {
    g_vs = load_vs("decal.hlsl", "vs_decal");
    g_ps = load_ps("decal.hlsl", "ps_decal");
    float3 v[8];
    for (int i = 0; i < 8; i++) v[i] = {(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f};
    uint32_t idx[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    g_box_vb = create_buffer(v, sizeof(v), D3D11_BIND_VERTEX_BUFFER);
    g_box_ib = create_buffer(idx, sizeof(idx), D3D11_BIND_INDEX_BUFFER);
    D3D11_BLEND_DESC d = {};
    d.RenderTarget[0].BlendEnable = TRUE;
    d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    // RGB only: the albedo target's alpha holds roughness, which this decal type leaves alone.
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    gpu.dev->CreateBlendState(&d, &g_bs_decal);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // the box winding doesn't matter: draw both, the PS clips to the box
    rd.DepthClipEnable = FALSE;
    rd.MultisampleEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &g_rs_back);
    for (int k = 0; k < 3; k++) g_tex[k] = make_decal_texture(k);
}

void pass_decals(Scene& scene) {
    if (!R.cfg.decals || scene.decals.empty()) return;
    Frustum fr = frustum_from_matrix(R.frame_constants.view_proj_nojitter);
    gpu.ctx->OMSetRenderTargets(1, &R.gb_albedo.rtv, nullptr);
    set_viewport(R.w, R.h);
    gpu.ctx->OMSetBlendState(g_bs_decal, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    gpu.ctx->RSSetState(g_rs_back);
    UINT stride = sizeof(float3), offset = 0;
    gpu.ctx->IASetInputLayout(R.layout_volume);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.ctx->IASetVertexBuffers(0, 1, &g_box_vb, &stride, &offset);
    gpu.ctx->IASetIndexBuffer(g_box_ib, DXGI_FORMAT_R32_UINT, 0);
    gpu.ctx->VSSetShader(g_vs->vs, nullptr, 0);
    gpu.ctx->PSSetShader(g_ps->ps, nullptr, 0);
    gpu.ctx->PSSetShaderResources(0, 1, &R.depth.srv);
    for (const Decal& dc : scene.decals) {
        AABB box = transform_aabb(dc.world, AABB{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}});
        if (!frustum_test_aabb(fr, box)) continue;
        DecalCB cb = {dc.world, inverse(dc.world), dc.tint};
        update_cb(R.cb_pass, cb);
        gpu.ctx->PSSetShaderResources(1, 1, &g_tex[dc.kind % 3].srv);
        gpu.ctx->DrawIndexed(36, 0, 0);
    }
    ID3D11ShaderResourceView* nulls[2] = {};
    gpu.ctx->PSSetShaderResources(0, 2, nulls);
}
