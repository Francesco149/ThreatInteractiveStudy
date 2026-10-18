// Forward transparents (glass, sorted back to front) and particles rendered at half resolution
// with a checkerboard min/max depth (The Phantom Pain), composited with a nearest-depth upsample.
// All of them write the responsive mask (alpha, MAX blend) for the temporal AA. See
// shaders/vfx.hlsl and docs/research/08 section 11.
//
// Particles are a pure function of time (each slot's age = (time + offset) mod lifetime), so the
// benchmark sees identical particles for every configuration without any simulation state.
#include "renderer_internal.h"
#include <algorithm>

struct GpuParticle {  // must match Particle in shaders/vfx.hlsl
    float3 pos;
    float size;
    float4 color;
    float rotation;
    float emissive;
    float seed;
    float pad;
};

struct VfxCB {
    float4 half_size;
    float soft_distance;
    float full_res;
    float pad[2];
};

static const uint32_t MAX_PARTICLES = 8192;
static ID3D11Buffer* g_particle_buf = nullptr;
static ID3D11ShaderResourceView* g_particle_srv = nullptr;
static std::vector<GpuParticle> g_particles;
static DrawList g_glass;
static Texture g_half_color, g_half_depth;
static ID3D11BlendState* g_bs_premul_max = nullptr;
static ID3D11DepthStencilState* g_ds_write_always = nullptr;
static Shader *g_vs_particle, *g_ps_particle, *g_ps_downsample, *g_ps_composite, *g_vs_glass, *g_ps_glass;

void vfx_init() {
    g_vs_particle = load_vs("vfx.hlsl", "vs_particle");
    g_ps_particle = load_ps("vfx.hlsl", "ps_particle");
    g_ps_downsample = load_ps("vfx.hlsl", "ps_depth_downsample");
    g_ps_composite = load_ps("vfx.hlsl", "ps_composite");
    g_vs_glass = load_vs("vfx.hlsl", "vs_glass");
    g_ps_glass = load_ps("vfx.hlsl", "ps_glass");
    g_particle_buf = create_structured_buffer(sizeof(GpuParticle), MAX_PARTICLES, nullptr, false, true, &g_particle_srv, nullptr);

    // Premultiplied alpha for colour; MAX for alpha, which accumulates the responsive mask.
    D3D11_BLEND_DESC d = {};
    d.RenderTarget[0].BlendEnable = TRUE;
    d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    gpu.dev->CreateBlendState(&d, &g_bs_premul_max);
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    gpu.dev->CreateDepthStencilState(&ds, &g_ds_write_always);
}

static float frand(uint32_t seed) { return (hash_u32(seed) & 0xFFFFFF) / 16777216.0f; }

// Deterministic particles for one emitter at `time`.
static void emit(const VfxEmitter& e, int emitter_index, float time) {
    struct Kind { int count; float life; float3 vel; float spread; float size0, size1; float4 color; float emissive; };
    Kind k;
    if (e.type == "fire")
        k = {40, 1.1f, {0, 1.4f, 0}, 0.25f, 0.35f, 0.08f, {1.0f, 0.45f, 0.12f, 0.9f}, 1};
    else if (e.type == "spray")
        k = {90, 1.4f, {0, 3.2f, 0}, 1.0f, 0.10f, 0.35f, {0.85f, 0.9f, 1.0f, 0.35f}, 0};
    else  // smoke
        k = {110, 8.0f, {0.45f, 0.5f, 0.12f}, 0.25f, 0.25f, 1.9f, {0.62f, 0.62f, 0.65f, 0.32f}, 0};
    for (int i = 0; i < k.count && g_particles.size() < MAX_PARTICLES; i++) {
        uint32_t seed = (uint32_t)(emitter_index * 1000 + i);
        float offset = frand(seed) * k.life;
        float age = fmodf(time + offset, k.life);
        float t = age / k.life;
        float3 dir = {frand(seed * 3 + 1) - 0.5f, 0, frand(seed * 7 + 2) - 0.5f};
        float3 p = e.position + dir * (k.spread * 0.5f) + k.vel * age;
        if (e.type == "spray") {  // ballistic
            float3 v = {dir.x * 1.6f, k.vel.y * (0.7f + 0.3f * frand(seed * 11)), dir.z * 1.6f};
            p = e.position + v * age + float3{0, -4.9f * age * age, 0};
            if (p.y < e.position.y - 0.3f) continue;
        } else {
            p += dir * (k.spread * t * 2.0f);  // smoke spreads as it rises
        }
        float fade = saturate(t * 6.0f) * saturate((1.0f - t) * 2.5f);
        GpuParticle g;
        g.pos = p;
        g.size = lerp(k.size0, k.size1, sqrtf(t)) * (0.8f + 0.4f * frand(seed * 13));
        g.color = {k.color.x, k.color.y, k.color.z, k.color.w * fade};
        g.rotation = frand(seed * 17) * 6.28f + age * (frand(seed * 19) - 0.5f);
        g.emissive = k.emissive;
        g.seed = frand(seed * 23);
        g.pad = 0;
        g_particles.push_back(g);
    }
}

// CPU side, before the frame's draw data is uploaded: glass draw list + particle buffer.
void vfx_setup(Scene& scene, const Camera& cam, float time) {
    g_glass.clear();
    Frustum fr = frustum_from_matrix(R.frame_constants.view_proj_nojitter);
    for (int i = 0; i < (int)scene.instances.size(); i++) {
        const Instance& in = scene.instances[i];
        if (R.cfg.frustum_culling && !frustum_test_aabb(fr, in.world_bounds)) continue;
        float dist = length(in.world_bounds.center() - cam.pos);
        for (const Primitive& p : scene.meshes[in.mesh].prims)
            if (scene.materials[p.material].alpha_mode == ALPHA_BLEND)
                g_glass.items.push_back({&p, 0, p.material, i, -dist, 0, 0});  // negative: back to front
    }
    // Back to front, no instancing merges across distances (order matters for blending).
    std::sort(g_glass.items.begin(), g_glass.items.end(), [](const DrawItem& a, const DrawItem& b) { return a.distance < b.distance; });
    finalize_shadow_list(scene, g_glass);  // batching helper (consecutive identical items only)

    g_particles.clear();
    if (R.cfg.particles)
        for (int e = 0; e < (int)scene.emitters.size(); e++) emit(scene.emitters[e], e, time);
    std::sort(g_particles.begin(), g_particles.end(), [&](const GpuParticle& a, const GpuParticle& b) {
        return length(a.pos - cam.pos) > length(b.pos - cam.pos);
    });
    if (!g_particles.empty()) update_buffer(g_particle_buf, g_particles.data(), (uint32_t)(g_particles.size() * sizeof(GpuParticle)));
}

static void draw_particles(bool full_res) {
    VfxCB cb = {{(float)g_half_color.w, (float)g_half_color.h, 1.0f / g_half_color.w, 1.0f / g_half_color.h}, 0.6f,
                full_res ? 1.0f : 0.0f};
    if (full_res) cb.half_size = {(float)R.w, (float)R.h, 1.0f / R.w, 1.0f / R.h};
    update_cb(R.cb_pass, cb);
    gpu.ctx->IASetInputLayout(nullptr);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.ctx->VSSetShader(g_vs_particle->vs, nullptr, 0);
    gpu.ctx->PSSetShader(g_ps_particle->ps, nullptr, 0);
    gpu.ctx->VSSetShaderResources(11, 1, &g_particle_srv);
    gpu.ctx->DrawInstanced(6, (UINT)g_particles.size(), 0, 0);
}

void pass_transparent(Scene& scene) {
    ID3D11ShaderResourceView* nulls[6] = {};
    set_viewport(R.w, R.h);
    if (!g_glass.batches.empty()) {
        GPU_SCOPE("Glass (forward)");
        gpu.ctx->OMSetRenderTargets(1, &R.light_specular.rtv, R.depth.dsv_readonly);
        gpu.ctx->OMSetBlendState(g_bs_premul_max, nullptr, 0xFFFFFFFF);
        gpu.ctx->OMSetDepthStencilState(R.ds_transparent, 0);
        gpu.ctx->RSSetState(R.rs_none);
        ID3D11Buffer* vbs[3] = {scene.vb_position, scene.vb_attribs, R.draw_id_vb};
        UINT strides[3] = {sizeof(float3), sizeof(VertexAttribs), sizeof(uint32_t)}, offsets[3] = {};
        gpu.ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);
        gpu.ctx->IASetIndexBuffer(scene.ib, DXGI_FORMAT_R32_UINT, 0);
        gpu.ctx->IASetInputLayout(R.layout_full);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.ctx->VSSetShader(g_vs_glass->vs, nullptr, 0);
        gpu.ctx->PSSetShader(g_ps_glass->ps, nullptr, 0);
        for (const DrawBatch& b : g_glass.batches) {
            const MeshLod& l = b.prim->lods[0];
            gpu.ctx->DrawIndexedInstanced(l.index_count, b.count, l.first_index, b.prim->base_vertex, b.first_draw);
        }
    }
    if (g_particles.empty()) return;

    if (!R.cfg.vfx_half_res) {
        GPU_SCOPE("Particles (full res)");
        gpu.ctx->OMSetRenderTargets(1, &R.light_specular.rtv, R.depth.dsv_readonly);
        gpu.ctx->OMSetBlendState(g_bs_premul_max, nullptr, 0xFFFFFFFF);
        gpu.ctx->OMSetDepthStencilState(R.ds_transparent, 0);
        gpu.ctx->RSSetState(R.rs_none);
        gpu.ctx->PSSetShaderResources(0, 1, &R.depth.srv);
        draw_particles(true);
        gpu.ctx->PSSetShaderResources(0, 1, nulls);
        return;
    }

    int hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    if (!g_half_color.tex || g_half_color.w != hw || g_half_color.h != hh) {
        release_texture(g_half_color);
        release_texture(g_half_depth);
        g_half_color = create_texture("vfx_half_color", hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RTV);
        g_half_depth = create_texture("vfx_half_depth", hw, hh, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH);
    }
    gpu.ctx->RSSetState(R.rs_none);
    {
        GPU_SCOPE("Half-res depth (min/max)");
        // With the hijack clear on, the colour target rides along and gets zeroed by this draw.
        bool hijack = R.cfg.hijack_clear;
        gpu.ctx->OMSetRenderTargets(1, &g_half_color.rtv, g_half_depth.dsv);
        set_viewport(hw, hh);
        gpu.ctx->OMSetDepthStencilState(g_ds_write_always, 0);
        gpu.ctx->OMSetBlendState(hijack ? R.bs_opaque : R.bs_no_color, nullptr, 0xFFFFFFFF);
        gpu.ctx->PSSetShaderResources(0, 1, &R.depth.srv);
        gpu.ctx->PSSetShader(g_ps_downsample->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 1, nulls);
    }
    {
        GPU_SCOPE("Particles (half res)");
        // The half-res buffer must start at zero (TI notes this clear as the price of half-res
        // shading). With the hijack clear, the depth downsample already did it.
        if (!R.cfg.hijack_clear) {
            float zero[4] = {0, 0, 0, 0};
            gpu.ctx->ClearRenderTargetView(g_half_color.rtv, zero);
        }
        gpu.ctx->OMSetRenderTargets(1, &g_half_color.rtv, g_half_depth.dsv_readonly);
        gpu.ctx->OMSetDepthStencilState(R.ds_transparent, 0);
        gpu.ctx->OMSetBlendState(g_bs_premul_max, nullptr, 0xFFFFFFFF);
        gpu.ctx->PSSetShaderResources(4, 1, &g_half_depth.srv);
        draw_particles(false);
        gpu.ctx->PSSetShaderResources(4, 1, nulls);
    }
    {
        GPU_SCOPE("Particle composite (upsample)");
        gpu.ctx->OMSetRenderTargets(1, &R.light_specular.rtv, nullptr);
        set_viewport(R.w, R.h);
        gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
        gpu.ctx->OMSetBlendState(g_bs_premul_max, nullptr, 0xFFFFFFFF);
        ID3D11ShaderResourceView* srvs[6] = {R.depth.srv, nullptr, nullptr, nullptr, g_half_depth.srv, g_half_color.srv};
        gpu.ctx->PSSetShaderResources(0, 6, srvs);
        VfxCB cb = {{(float)hw, (float)hh, 1.0f / hw, 1.0f / hh}, 0.6f, 0};
        update_cb(R.cb_pass, cb);
        gpu.ctx->PSSetShader(g_ps_composite->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 6, nulls);
    }
}
