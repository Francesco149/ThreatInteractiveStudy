// Lighting stage: sun + ambient, local lights with stencil volumes, composition, sky.
#include "renderer_internal.h"

static ID3D11BlendState* g_bs_compose = nullptr;
static ID3D11DepthStencilState* g_ds_unmark = nullptr;
static ID3D11RasterizerState* g_rs_volume_back = nullptr;

struct LightConstants {  // must match LightCB in shaders/lighting.hlsl
    float4x4 volume_world;
    float4 pos_range;
    float4 color;
    float4 dir_type;
    float4 cone;
    float4 shadow;   // local_shadow_constants()
};

void lighting_init() {
    for (int sm = 0; sm < SM_COUNT; sm++)
        for (int ps = 0; ps < 2; ps++) {
            char smd[4], psd[4];
            snprintf(smd, sizeof(smd), "%d", sm);
            snprintf(psd, sizeof(psd), "%d", ps);
            R.ps_sun[sm][ps] = load_ps("lighting.hlsl", "ps_sun", {{"SHADING_MODEL", smd}, {"PER_SAMPLE", psd}});
            R.ps_light[sm][ps] = load_ps("lighting.hlsl", "ps_light", {{"SHADING_MODEL", smd}, {"PER_SAMPLE", psd}});
        }
    R.vs_volume = load_vs("lighting.hlsl", "vs_volume");
    R.ps_light_uber[0] = load_ps("lighting.hlsl", "ps_light", {{"SHADING_MODEL", "99"}, {"PER_SAMPLE", "0"}});
    R.ps_light_uber[1] = load_ps("lighting.hlsl", "ps_light", {{"SHADING_MODEL", "99"}, {"PER_SAMPLE", "1"}});
    R.ps_compose[0] = load_ps("compose.hlsl", "ps_compose", {{"PER_SAMPLE", "0"}});
    R.ps_compose[1] = load_ps("compose.hlsl", "ps_compose", {{"PER_SAMPLE", "1"}});
    R.ps_sky = load_ps("sky.hlsl", "ps_sky");
    D3D11_INPUT_ELEMENT_DESC pos[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    gpu.dev->CreateInputLayout(pos, 1, R.vs_volume->bytecode.data(), R.vs_volume->bytecode.size(), &R.layout_volume);

    // Compose: scene = src0 + dest * src1 (dual-source blending applies fog to the specular
    // already in the destination).
    D3D11_BLEND_DESC d = {};
    d.RenderTarget[0].BlendEnable = TRUE;
    d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    gpu.dev->CreateBlendState(&d, &g_bs_compose);

    // A/B: clear the light-volume stencil bit with its own draw.
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = 0xFF;
    ds.StencilWriteMask = STENCIL_LIGHT_BIT;
    ds.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_ALWAYS};
    ds.BackFace = ds.FrontFace;
    gpu.dev->CreateDepthStencilState(&ds, &g_ds_unmark);
    // Fox Engine variant: light with the back faces of the volume (works with the camera inside it).
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_FRONT;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &g_rs_volume_back);
}

// Screen-space rectangle of a bounding sphere; full screen if the camera is (nearly) inside it.
static D3D11_RECT sphere_scissor(float3 c, float radius, bool* inside) {
    D3D11_RECT full = {0, 0, R.w, R.h};
    float3 cam = {R.frame_constants.camera_pos.x, R.frame_constants.camera_pos.y, R.frame_constants.camera_pos.z};
    *inside = length(c - cam) < radius + R.frame_constants.camera_pos.w * 2.0f;
    if (*inside) return full;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 8; i++) {
        float3 p = c + float3{(i & 1) ? radius : -radius, (i & 2) ? radius : -radius, (i & 4) ? radius : -radius};
        float4 clip = R.frame_constants.view_proj * float4{p.x, p.y, p.z, 1};
        if (clip.w <= R.frame_constants.camera_pos.w) return full;  // a corner is behind the near plane
        float x = clip.x / clip.w, y = clip.y / clip.w;
        minx = fminf(minx, x); maxx = fmaxf(maxx, x);
        miny = fminf(miny, y); maxy = fmaxf(maxy, y);
    }
    D3D11_RECT r;
    r.left = (LONG)floorf((minx * 0.5f + 0.5f) * R.w);
    r.right = (LONG)ceilf((maxx * 0.5f + 0.5f) * R.w);
    r.top = (LONG)floorf((0.5f - maxy * 0.5f) * R.h);
    r.bottom = (LONG)ceilf((0.5f - miny * 0.5f) * R.h);
    r.left = r.left < 0 ? 0 : r.left;
    r.top = r.top < 0 ? 0 : r.top;
    r.right = r.right > R.w ? R.w : r.right;
    r.bottom = r.bottom > R.h ? R.h : r.bottom;
    return r;
}

void lights_setup(Scene& scene, const Camera&) {
    R.local_lights.clear();
    Frustum fr = frustum_from_matrix(R.frame_constants.view_proj_nojitter);
    for (int i = 0; i < (int)scene.lights.size(); i++) {
        const Light& l = scene.lights[i];
        if (l.type == LIGHT_DIRECTIONAL) continue;
        if (!frustum_test_sphere(fr, l.position, l.range)) continue;
        LocalLightDraw d;
        d.light = i;
        if (l.type == LIGHT_POINT) {
            d.volume_world = translation(l.position) * scaling({l.range, l.range, l.range});
        } else {
            // Unit cone along -Z (apex at origin) -> light direction, length = range.
            float3 dir = normalize(l.direction);
            float3 up = fabsf(dir.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
            float3 r = normalize(cross(dir, up));
            float3 u = cross(r, dir);
            float4x4 basis = {{{r.x, u.x, -dir.x, 0}, {r.y, u.y, -dir.y, 0}, {r.z, u.z, -dir.z, 0}, {0, 0, 0, 1}}};
            float rad = tanf(fminf(l.outer_cone, 1.5f)) * l.range;
            d.volume_world = translation(l.position) * basis * scaling({rad, rad, l.range});
        }
        d.scissor = sphere_scissor(l.position, l.range, &d.camera_inside);
        if (d.scissor.right <= d.scissor.left || d.scissor.bottom <= d.scissor.top) continue;
        R.local_lights.push_back(d);
    }
    R.stats.lights_visible = (int)R.local_lights.size();
}

// One lit full-screen draw for one shading model, split by the MSAA edge bit into a per-pixel
// draw (simple pixels) and a per-sample draw (complex pixels). `extra_mask/extra_ref` add the
// light-volume bit for local lights.
// `geometry` draws the covering primitive: a full-screen triangle by default, or the light volume
// mesh for the Fox Engine variant.
static void draw_shading_model(Shader* const ps[2], int sm, UINT8 extra_mask, UINT8 extra_ref, bool clear_light,
                               void (*geometry)() = draw_fullscreen) {
    UINT8 sm_ref = (UINT8)(sm + 1);
    if (R.samples == 1) {
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_SM_BITS | extra_mask, clear_light), sm_ref | extra_ref);
        gpu.ctx->PSSetShader(ps[0]->ps, nullptr, 0);
        geometry();
    } else if (R.cfg.msaa_edge_detect) {
        UINT8 mask = STENCIL_SM_BITS | STENCIL_EDGE_BIT | extra_mask;
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(mask, clear_light), sm_ref | extra_ref);
        gpu.ctx->PSSetShader(ps[0]->ps, nullptr, 0);
        geometry();
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(mask, clear_light), sm_ref | extra_ref | STENCIL_EDGE_BIT);
        gpu.ctx->PSSetShader(ps[1]->ps, nullptr, 0);
        geometry();
    } else {
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_SM_BITS | extra_mask, clear_light), sm_ref | extra_ref);
        gpu.ctx->PSSetShader(ps[1]->ps, nullptr, 0);
        geometry();
    }
}

// A/B: the uber-shader variant of draw_shading_model: the stencil only tests the light bit (+edge).
static void draw_uber(Shader* const ps[2], bool clear_light, void (*geometry)()) {
    if (R.samples > 1 && R.cfg.msaa_edge_detect) {
        UINT8 mask = STENCIL_LIGHT_BIT | STENCIL_EDGE_BIT;
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(mask, clear_light), STENCIL_LIGHT_BIT);
        gpu.ctx->PSSetShader(ps[0]->ps, nullptr, 0);
        geometry();
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(mask, clear_light), STENCIL_LIGHT_BIT | STENCIL_EDGE_BIT);
        gpu.ctx->PSSetShader(ps[1]->ps, nullptr, 0);
        geometry();
    } else {
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_LIGHT_BIT, clear_light), STENCIL_LIGHT_BIT);
        gpu.ctx->PSSetShader(ps[R.samples > 1 ? 1 : 0]->ps, nullptr, 0);
        geometry();
    }
}

// The current light's volume mesh (input assembler state is set by the caller).
static UINT g_volume_index_count = 0;
static void draw_volume_mesh() { gpu.ctx->DrawIndexed(g_volume_index_count, 0, 0); }

static void bind_gbuffer_srvs() {
    ID3D11ShaderResourceView* srvs[6] = {R.depth.srv, R.gb_albedo.srv, R.gb_normal.srv, R.gb_material.srv, nullptr,
                                         R.cfg.shadows ? R.shadow_cascades.srv : nullptr};
    gpu.ctx->PSSetShaderResources(0, 6, srvs);
}

void pass_sun(Scene&) {
    ID3D11RenderTargetView* rtvs[2] = {R.light_diffuse.rtv, R.light_specular.rtv};
    gpu.ctx->OMSetRenderTargets(2, rtvs, R.depth.dsv_readonly);
    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);  // overwrite: no clear needed
    bind_gbuffer_srvs();
    for (int sm = 0; sm < SM_COUNT; sm++)
        if (R.shading_model_used[sm]) draw_shading_model(R.ps_sun[sm], sm, 0, 0, false);
}

void pass_local_lights(Scene& scene) {
    if (R.local_lights.empty()) return;
    const Settings& s = R.cfg;
    ID3D11RenderTargetView* rtvs[2] = {R.light_diffuse.rtv, R.light_specular.rtv};
    set_viewport(R.w, R.h);
    bind_gbuffer_srvs();
    local_shadows_bind(true);
    UINT stride = sizeof(float3), offset = 0;
    for (const LocalLightDraw& d : R.local_lights) {
        const Light& l = scene.lights[d.light];
        LightConstants lc;
        lc.volume_world = d.volume_world;
        lc.pos_range = {l.position.x, l.position.y, l.position.z, l.range};
        float3 c = l.color * (l.intensity * s.light_intensity);
        lc.color = {c.x, c.y, c.z, 0};
        lc.dir_type = {l.direction.x, l.direction.y, l.direction.z, (float)l.type};
        float co = cosf(l.outer_cone), ci = cosf(fminf(l.inner_cone, l.outer_cone - 0.01f));
        lc.cone = {co, 1.0f / fmaxf(ci - co, 1e-4f), 0, 0};
        lc.shadow = local_shadow_constants(d.light);
        update_cb(R.cb_light, lc);
        R.stats.draws_lights++;

        bool volume = s.light_cull == LIGHTS_STENCIL_VOLUME || s.light_cull == LIGHTS_VOLUME_MESH;
        bool cone = l.type == LIGHT_SPOT;
        if (volume) {
            // 1) Mark the samples inside the volume in stencil bit 7 (no colour, no pixel shader).
            gpu.ctx->OMSetRenderTargets(0, nullptr, R.depth.dsv_readonly_depth);
            gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
            gpu.ctx->OMSetDepthStencilState(R.ds_light_volume, 0);
            gpu.ctx->RSSetState(R.rs_volume);
            gpu.ctx->RSSetScissorRects(1, &d.scissor);
            gpu.ctx->IASetInputLayout(R.layout_volume);
            gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            gpu.ctx->IASetVertexBuffers(0, 1, cone ? &R.cone_vb : &R.sphere_vb, &stride, &offset);
            gpu.ctx->IASetIndexBuffer(cone ? R.cone_ib : R.sphere_ib, DXGI_FORMAT_R32_UINT, 0);
            gpu.ctx->VSSetShader(R.vs_volume->vs, nullptr, 0);
            gpu.ctx->PSSetShader(nullptr, nullptr, 0);
            gpu.ctx->DrawIndexed(cone ? R.cone_index_count : R.sphere_index_count, 0, 0);
        }
        // 2) Light it: one full-screen triangle per shading model, clipped to the light's screen
        //    rectangle, stencil-tested against the volume bit, which the draw clears again.
        gpu.ctx->OMSetRenderTargets(2, rtvs, volume ? R.depth.dsv_readonly_depth : R.depth.dsv_readonly);
        gpu.ctx->OMSetBlendState(R.bs_additive, nullptr, 0xFFFFFFFF);
        bool scissor = s.light_cull != LIGHTS_FULLSCREEN && s.light_scissor;
        D3D11_RECT full = {0, 0, R.w, R.h};
        gpu.ctx->RSSetState(R.rs_scissor);
        gpu.ctx->RSSetScissorRects(1, scissor ? &d.scissor : &full);
        bool mesh = s.light_cull == LIGHTS_VOLUME_MESH;
        bool unmark_in_draw = volume && !s.light_separate_unmark;
        if (mesh) {
            gpu.ctx->RSSetState(g_rs_volume_back);
            gpu.ctx->VSSetShader(R.vs_volume->vs, nullptr, 0);
            g_volume_index_count = cone ? R.cone_index_count : R.sphere_index_count;
        }
        if (s.light_uber_shader && volume) {
            // A/B: one draw for all shading models (the volume pass already skipped sky samples, so
            // the stencil test only needs the light bit), branching per pixel in the shader.
            draw_uber(R.ps_light_uber, unmark_in_draw, mesh ? draw_volume_mesh : draw_fullscreen);
        } else {
            for (int sm = 0; sm < SM_COUNT; sm++) {
                if (!R.shading_model_used[sm]) continue;
                UINT8 bit = volume ? STENCIL_LIGHT_BIT : 0;
                draw_shading_model(R.ps_light[sm], sm, bit, bit, unmark_in_draw, mesh ? draw_volume_mesh : draw_fullscreen);
            }
        }
        if (volume && s.light_separate_unmark) {
            // Clear the mark of every sample in the light's rectangle with a colourless draw.
            gpu.ctx->OMSetRenderTargets(0, nullptr, R.depth.dsv_readonly_depth);
            gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
            gpu.ctx->OMSetDepthStencilState(g_ds_unmark, 0);
            gpu.ctx->RSSetState(R.rs_scissor);
            gpu.ctx->RSSetScissorRects(1, &d.scissor);
            gpu.ctx->PSSetShader(nullptr, nullptr, 0);
            draw_fullscreen();
        }
    }
    gpu.ctx->RSSetState(R.rs_none);
    local_shadows_bind(false);
}

void pass_compose() {
    gpu.ctx->OMSetRenderTargets(1, &R.light_specular.rtv, R.depth.dsv_readonly);
    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(g_bs_compose, nullptr, 0xFFFFFFFF);
    ID3D11ShaderResourceView* srvs[8] = {R.depth.srv, R.gb_albedo.srv, nullptr, nullptr, nullptr, nullptr,
                                         R.light_diffuse.srv, R.gb_emissive.srv};
    gpu.ctx->PSSetShaderResources(0, 8, srvs);
    if (R.samples > 1 && R.cfg.msaa_edge_detect) {
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_EDGE_BIT, false), 0);
        gpu.ctx->PSSetShader(R.ps_compose[0]->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_EDGE_BIT, false), STENCIL_EDGE_BIT);
        gpu.ctx->PSSetShader(R.ps_compose[1]->ps, nullptr, 0);
        draw_fullscreen();
    } else {
        gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
        gpu.ctx->PSSetShader(R.ps_compose[R.samples > 1 ? 1 : 0]->ps, nullptr, 0);
        draw_fullscreen();
    }
    ID3D11ShaderResourceView* nulls[8] = {};
    gpu.ctx->PSSetShaderResources(0, 8, nulls);
}

void pass_sky() {
    gpu.ctx->OMSetRenderTargets(1, &R.light_specular.rtv, R.depth.dsv_readonly);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(ds_stencil_test(STENCIL_SM_BITS, false), 0);  // sky samples only
    gpu.ctx->PSSetShader(R.ps_sky->ps, nullptr, 0);
    draw_fullscreen();
}
