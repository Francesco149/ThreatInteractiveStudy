// Geometry stage: culling, LOD selection, draw sorting/batching, clears, the partial depth
// pre-pass, the G-buffer base pass, and the MSAA complex-pixel stencil.
#include "renderer_internal.h"
#include <algorithm>

// Base pass buckets, executed in the order given by Settings::order.
enum Bucket { B_CLOSE_ALPHA = 0, B_OPAQUE = 1, B_FAR_ALPHA = 2, B_PREPASSED = 3 };

void geometry_init() {
    R.vs_gbuffer = load_vs("gbuffer.hlsl", "vs_main");
    for (int i = 0; i < 4; i++) {
        const char* a = (i & 1) ? "1" : "0";
        const char* d = (i & 2) ? "1" : "0";
        R.ps_gbuffer[i] = load_ps("gbuffer.hlsl", "ps_main", {{"ALPHA_TEST", a}, {"DITHER", d}});
    }
    R.vs_depth = load_vs("depth.hlsl", "vs_depth");
    R.vs_depth_alpha = load_vs("depth.hlsl", "vs_depth_alpha");
    R.vs_shadow = load_vs("depth.hlsl", "vs_depth", {{"SHADOW", "1"}});
    R.vs_shadow_alpha = load_vs("depth.hlsl", "vs_depth_alpha", {{"SHADOW", "1"}});
    R.ps_depth_alpha = load_ps("depth.hlsl", "ps_depth_alpha");
    R.ps_shadow_alpha = load_ps("depth.hlsl", "ps_shadow_alpha");
    R.ps_depth_dither = load_ps("depth.hlsl", "ps_depth_dither");
    R.ps_overdraw = load_ps("overdraw.hlsl", "ps_overdraw", {{"ALPHA_TEST", "0"}});
    R.ps_overdraw_alpha = load_ps("overdraw.hlsl", "ps_overdraw", {{"ALPHA_TEST", "1"}});
    R.ps_edges = load_ps("msaa_edges.hlsl", "ps_main");

    // Stream 0: position (12 B). Stream 1: packed normal/tangent/uv (12 B). Stream 2: draw id (per instance).
    D3D11_INPUT_ELEMENT_DESC full[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 1, 4, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"DRAWID", 0, DXGI_FORMAT_R32_UINT, 2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    gpu.dev->CreateInputLayout(full, 5, R.vs_gbuffer->bytecode.data(), R.vs_gbuffer->bytecode.size(), &R.layout_full);
    D3D11_INPUT_ELEMENT_DESC pos[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"DRAWID", 0, DXGI_FORMAT_R32_UINT, 2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    gpu.dev->CreateInputLayout(pos, 2, R.vs_depth->bytecode.data(), R.vs_depth->bytecode.size(), &R.layout_position);

    R.draw_buffer = create_structured_buffer(sizeof(GpuDrawData), R.draw_capacity, nullptr, false, true, &R.draw_srv, nullptr);
    std::vector<uint32_t> ids(R.draw_capacity);
    for (uint32_t i = 0; i < R.draw_capacity; i++) ids[i] = i;
    R.draw_id_vb = create_buffer(ids.data(), R.draw_capacity * sizeof(uint32_t), D3D11_BIND_VERTEX_BUFFER);
}

// ---------------------------------------------------------------------------------------------
// Culling, LOD selection, sorting and batching

// Projected size of a bounding sphere as a fraction of the screen height.
static float screen_size(float radius, float distance, float fov_y) {
    return radius / (fmaxf(distance, 0.01f) * tanf(fov_y * 0.5f));
}

// Picks the LOD for a projected size, and a dithered cross-fade near the switch point: within the
// band [threshold, threshold * (1 + BAND)] both LODs are drawn with complementary dither masks.
static const float LOD_FADE_BAND = 0.15f;

int select_lod(const Primitive& p, float size, float bias, float* fade) {
    *fade = 0;
    int lod = 0;
    for (int l = 1; l < p.lod_count; l++)
        if (size < p.lods[l].screen_size * bias) lod = l;
    // Fade from `lod` towards the finer `lod - 1` when we're just below its threshold.
    if (lod > 0) {
        float t = p.lods[lod].screen_size * bias;
        float f = (size - t * (1 - LOD_FADE_BAND)) / (t * LOD_FADE_BAND);  // 0 at band start, 1 at threshold
        if (f > 0) *fade = fminf(f, 1.0f);
    }
    return lod;
}

struct SortItem {
    DrawItem item;
    uint64_t key;  // group key (prim/lod/material/flags)
};

static uint64_t group_key(const DrawItem& d, bool use_material) {
    return ((uint64_t)(uintptr_t)d.prim << 16) ^ ((uint64_t)d.lod << 8) ^ (use_material ? (uint64_t)d.material << 40 : 0) ^
           (uint64_t)(d.flags & (DRAW_FADE_IN | DRAW_FADE_OUT)) << 4;
}

static void write_draw(Scene& scene, const DrawItem& d) {
    const Instance& in = scene.instances[d.instance];
    GpuDrawData g;
    for (int r = 0; r < 3; r++) {
        g.world[r] = {in.world.m[r][0], in.world.m[r][1], in.world.m[r][2], in.world.m[r][3]};
        g.prev[r] = {in.prev_world.m[r][0], in.prev_world.m[r][1], in.prev_world.m[r][2], in.prev_world.m[r][3]};
    }
    g.material = d.material;
    g.flags = d.flags;
    g.lod_fade = d.fade;
    g.wind_bend = in.wind_bend;
    R.draw_data.push_back(g);
}

// Sorts a list and turns it into instanced batches. `by_distance`: groups are ordered by their
// closest item (front to back) and items inside a group too; otherwise groups are ordered by key
// (material-sorted). Identical consecutive items become one DrawIndexedInstanced.
static void finalize_list(Scene& scene, DrawList& list, bool by_distance, bool use_material) {
    std::vector<SortItem> items;
    items.reserve(list.items.size());
    for (const DrawItem& d : list.items) items.push_back({d, group_key(d, use_material)});
    if (by_distance) {
        // Group: key -> closest distance in the group.
        std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) {
            return a.key != b.key ? a.key < b.key : a.item.distance < b.item.distance;
        });
        struct Group { size_t begin, end; float dist; };
        std::vector<Group> groups;
        for (size_t i = 0; i < items.size();) {
            size_t j = i;
            while (j < items.size() && items[j].key == items[i].key) j++;
            groups.push_back({i, j, items[i].item.distance});
            i = j;
        }
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.dist < b.dist; });
        std::vector<SortItem> sorted;
        sorted.reserve(items.size());
        for (const Group& g : groups) sorted.insert(sorted.end(), items.begin() + g.begin, items.begin() + g.end);
        items.swap(sorted);
    } else {
        std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) {
            return a.item.material != b.item.material ? a.item.material < b.item.material : a.key < b.key;
        });
    }
    list.batches.clear();
    for (size_t i = 0; i < items.size(); i++) {
        const DrawItem& d = items[i].item;
        if (R.draw_data.size() >= R.draw_capacity) break;
        bool same = !list.batches.empty() && i > 0 && items[i - 1].key == items[i].key &&
                    (!use_material || items[i - 1].item.material == d.material);
        if (same) {
            list.batches.back().count++;
        } else {
            list.batches.push_back({d.prim, d.lod, d.material, (uint32_t)R.draw_data.size(), 1, d.flags});
        }
        write_draw(scene, d);
    }
}

void build_draw_lists(Scene& scene, const Camera& cam) {
    const Settings& s = R.cfg;
    R.draw_data.clear();
    R.list_prepass.clear();
    for (DrawList& l : R.list_base) l.clear();
    for (int k = 0; k < SM_COUNT; k++) R.shading_model_used[k] = false;

    Frustum fr = frustum_from_matrix(R.frame_constants.view_proj_nojitter);
    for (int i = 0; i < (int)scene.instances.size(); i++) {
        const Instance& in = scene.instances[i];
        if (s.frustum_culling && !frustum_test_aabb(fr, in.world_bounds)) continue;
        float3 c = in.world_bounds.center();
        float dist = length(c - cam.pos);
        float size = screen_size(in.radius, dist - in.radius * 0.0f, cam.fov_y);
        const Mesh& mesh = scene.meshes[in.mesh];
        for (const Primitive& p : mesh.prims) {
            const Material& mat = scene.materials[p.material];
            if (mat.alpha_mode == ALPHA_BLEND) continue;  // transparent pass (TODO)
            float fade = 0;
            int lod = s.lod_enabled ? select_lod(p, size, s.lod_bias, &fade) : 0;
            if (!s.lod_dither) fade = 0;
            uint32_t wind = (in.flags & INST_WIND) ? DRAW_WIND : 0;
            bool alpha = mat.alpha_mode == ALPHA_MASK;
            R.shading_model_used[mat.shading_model] = true;

            // An object in an LOD transition is two draws: the coarse LOD fading out and the finer
            // LOD fading in, with complementary 2x2 Bayer dither masks.
            DrawItem items[2];
            int n = 0;
            items[n++] = {&p, lod, p.material, i, dist, fade, wind | (fade > 0 ? DRAW_FADE_OUT : 0u)};
            if (fade > 0) items[n++] = {&p, lod - 1, p.material, i, dist, fade, wind | DRAW_FADE_IN};

            for (int k = 0; k < n; k++) {
                DrawItem& d = items[k];
                bool fading = d.flags & (DRAW_FADE_IN | DRAW_FADE_OUT);
                bool prepass = false;
                switch (s.prepass) {
                case PREPASS_NONE: break;
                case PREPASS_FULL_OPAQUE: prepass = !alpha && !fading; break;
                case PREPASS_FULL_WITH_ALPHA: prepass = !fading; break;
                case PREPASS_PARTIAL: prepass = !alpha && !fading && size >= s.prepass_min_size; break;
                }
                if (prepass) R.list_prepass.items.push_back(d);
                Bucket b;
                if (prepass) b = B_PREPASSED;
                else if (alpha && s.order == ORDER_TI && dist < s.close_alpha_distance) b = B_CLOSE_ALPHA;
                else if (alpha) b = B_FAR_ALPHA;
                else b = B_OPAQUE;
                R.list_base[b].items.push_back(d);
            }
        }
    }

    // Pre-pass: strictly front to back; batched only when consecutive (material doesn't matter
    // for depth-only, except alpha-tested ones which need their opacity texture).
    finalize_list(scene, R.list_prepass, true, s.prepass == PREPASS_FULL_WITH_ALPHA);
    bool by_distance = s.order != ORDER_BY_MATERIAL;
    for (int b = 0; b < 4; b++) finalize_list(scene, R.list_base[b], by_distance, true);

    R.stats.draws_prepass = (int)R.list_prepass.items.size();
    R.stats.batches_prepass = (int)R.list_prepass.batches.size();
    for (int b = 0; b < 4; b++) {
        R.stats.draws_base += (int)R.list_base[b].items.size();
        R.stats.batches_base += (int)R.list_base[b].batches.size();
        for (const DrawItem& d : R.list_base[b].items) R.stats.tris_base += d.prim->lods[d.lod].index_count / 3;
    }
}

// Shadow lists are built by shadows_setup() through this helper (same batching rules).
void finalize_shadow_list(Scene& scene, DrawList& list) { finalize_list(scene, list, true, false); }

void upload_draw_data() {
    if (R.draw_data.empty()) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(gpu.ctx->Map(R.draw_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, R.draw_data.data(), std::min<size_t>(R.draw_data.size(), R.draw_capacity) * sizeof(GpuDrawData));
        gpu.ctx->Unmap(R.draw_buffer, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing a list

enum DrawMode { MODE_DEPTH, MODE_GBUFFER, MODE_SHADOW, MODE_OVERDRAW };

static void bind_material_textures(Scene& scene, int material) {
    const Material& m = scene.materials[material];
    int ids[5] = {m.tex_albedo, m.tex_normal, m.tex_mr, m.tex_ao, m.tex_emissive};
    ID3D11ShaderResourceView* srvs[5];
    for (int i = 0; i < 5; i++) srvs[i] = ids[i] >= 0 ? scene.textures[ids[i]].srv : nullptr;
    gpu.ctx->PSSetShaderResources(0, 5, srvs);
}

// `prepassed`: the base-pass draws of pre-passed objects run with depth EQUAL, so even alpha-tested
// ones can use the cheap no-discard pixel shader: texels that failed the alpha test in the
// pre-pass left no depth to be equal to.
static void draw_batches(Scene& scene, const DrawList& list, DrawMode mode, ID3D11DepthStencilState* ds,
                         bool prepassed = false) {
    if (list.batches.empty()) return;
    ID3D11Buffer* vbs[3] = {scene.vb_position, scene.vb_attribs, R.draw_id_vb};
    UINT strides[3] = {sizeof(float3), sizeof(VertexAttribs), sizeof(uint32_t)};
    UINT offsets[3] = {0, 0, 0};
    gpu.ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);
    gpu.ctx->IASetIndexBuffer(scene.ib, DXGI_FORMAT_R32_UINT, 0);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    int bound_material = -1;
    ID3D11RasterizerState* bound_rs = nullptr;
    ID3D11PixelShader* bound_ps = (ID3D11PixelShader*)-1;
    ID3D11VertexShader* bound_vs = nullptr;
    int bound_ref = -1;
    ID3D11BlendState* bound_bs = nullptr;
    for (const DrawBatch& b : list.batches) {
        const Material& mat = scene.materials[b.material];
        bool alpha = mat.alpha_mode == ALPHA_MASK && !prepassed;
        bool dither = (b.flags & (DRAW_FADE_IN | DRAW_FADE_OUT)) != 0;
        ID3D11VertexShader* vs;
        ID3D11PixelShader* ps;
        ID3D11InputLayout* layout;
        bool needs_textures = mode == MODE_GBUFFER || mode == MODE_OVERDRAW || alpha;
        if (mode == MODE_GBUFFER) {
            vs = R.vs_gbuffer->vs;
            ps = R.ps_gbuffer[(alpha ? 1 : 0) | (dither ? 2 : 0)]->ps;
            layout = R.layout_full;
        } else if (mode == MODE_OVERDRAW) {
            vs = R.vs_gbuffer->vs;
            ps = (alpha ? R.ps_overdraw_alpha : R.ps_overdraw)->ps;
            layout = R.layout_full;
        } else if (alpha) {
            vs = (mode == MODE_SHADOW ? R.vs_shadow_alpha : R.vs_depth_alpha)->vs;
            ps = (mode == MODE_SHADOW ? R.ps_shadow_alpha : R.ps_depth_alpha)->ps;
            layout = R.layout_full;
        } else {
            vs = (mode == MODE_SHADOW ? R.vs_shadow : R.vs_depth)->vs;
            ps = dither ? R.ps_depth_dither->ps : nullptr;
            layout = R.layout_position;
        }
        if (vs != bound_vs) {
            gpu.ctx->VSSetShader(vs, nullptr, 0);
            gpu.ctx->IASetInputLayout(layout);
            bound_vs = vs;
        }
        if (ps != bound_ps) { gpu.ctx->PSSetShader(ps, nullptr, 0); bound_ps = ps; }
        if (needs_textures && b.material != bound_material) {
            bind_material_textures(scene, b.material);
            bound_material = b.material;
        }
        ID3D11RasterizerState* rs;
        if (mode == MODE_SHADOW) rs = mat.double_sided ? R.rs_shadow_none : R.rs_shadow;
        else rs = mat.double_sided ? R.rs_none : R.rs_back;
        if (rs != bound_rs) { gpu.ctx->RSSetState(rs); bound_rs = rs; }
        if (mode == MODE_GBUFFER) {
            int ref = (int)mat.shading_model + 1;  // stencil: shading model + 1 (0 = sky)
            if (ref != bound_ref) { gpu.ctx->OMSetDepthStencilState(ds, ref); bound_ref = ref; }
            bool emissive = mat.tex_emissive >= 0 || mat.emissive.x + mat.emissive.y + mat.emissive.z > 0;
            ID3D11BlendState* bs = emissive ? R.bs_gbuffer_emissive : R.bs_gbuffer;
            if (bs != bound_bs) { gpu.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF); bound_bs = bs; }
        }
        const MeshLod& lod = b.prim->lods[b.lod];
        gpu.ctx->DrawIndexedInstanced(lod.index_count, b.count, lod.first_index, b.prim->base_vertex, b.first_draw);
    }
}

// alpha_test = false draws alpha-tested casters as solid cards, depth-only (the no-discard path the
// pre-passed base-pass draws use): the foliage proxy A/B for distant shadow views.
void draw_shadow_list(Scene& scene, const DrawList& list, bool alpha_test) {
    draw_batches(scene, list, MODE_SHADOW, nullptr, !alpha_test);
}

// ---------------------------------------------------------------------------------------------
// Passes

void pass_clear() {
    const Settings& s = R.cfg;
    // Reversed Z: clear depth to 0 (= infinitely far). Stencil 0 = sky / nothing.
    gpu.ctx->ClearDepthStencilView(R.depth.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    float zero[4] = {0, 0, 0, 0};
    // Only partially written targets need a clear: emissive (only emissive materials write it).
    // Everything else is fully overwritten by the pass that produces it, wherever a later pass will
    // read it (readers mask empty/sky samples with stencil).
    gpu.ctx->ClearRenderTargetView(R.gb_emissive.rtv, zero);
    Texture* others[] = {&R.gb_albedo, &R.gb_normal, &R.gb_material, &R.gb_motion, &R.light_diffuse, &R.light_specular};
    if (s.clear_mode == CLEAR_ALL_HARDWARE) {
        for (Texture* t : others) gpu.ctx->ClearRenderTargetView(t->rtv, zero);
    } else if (s.clear_mode == CLEAR_ALL_FULLSCREEN) {
        // The anti-pattern: "clearing" with a full-screen triangle. Same result, more cost.
        ID3D11RenderTargetView* rtvs[6];
        for (int i = 0; i < 6; i++) rtvs[i] = others[i]->rtv;
        gpu.ctx->OMSetRenderTargets(6, rtvs, nullptr);
        set_viewport(R.w, R.h);
        gpu.ctx->RSSetState(R.rs_none);
        gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
        gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
        gpu.ctx->PSSetShader(R.ps_clear_fs->ps, nullptr, 0);
        draw_fullscreen();
    }
}

void pass_prepass(Scene& scene) {
    if (R.list_prepass.batches.empty()) return;
    gpu.ctx->OMSetRenderTargets(0, nullptr, R.depth.dsv);
    set_viewport(R.w, R.h);
    gpu.ctx->OMSetDepthStencilState(R.ds_prepass, 0);
    gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
    draw_batches(scene, R.list_prepass, MODE_DEPTH, nullptr);
}

void pass_base(Scene& scene) {
    ID3D11RenderTargetView* rtvs[5] = {R.gb_albedo.rtv, R.gb_normal.rtv, R.gb_material.rtv, R.gb_motion.rtv, R.gb_emissive.rtv};
    gpu.ctx->OMSetRenderTargets(5, rtvs, R.depth.dsv);
    set_viewport(R.w, R.h);
    // Execution order of the buckets.
    static const Bucket ti[] = {B_CLOSE_ALPHA, B_OPAQUE, B_FAR_ALPHA, B_PREPASSED};
    static const Bucket classic[] = {B_PREPASSED, B_OPAQUE, B_CLOSE_ALPHA, B_FAR_ALPHA};
    const Bucket* order = R.cfg.order == ORDER_TI ? ti : classic;
    static const char* names[] = {"close alpha-tested", "opaque", "far alpha-tested", "pre-passed (EQUAL)"};
    for (int i = 0; i < 4; i++) {
        Bucket b = order[i];
        if (R.list_base[b].batches.empty()) continue;
        GPU_SCOPE(names[b]);
        draw_batches(scene, R.list_base[b], MODE_GBUFFER, b == B_PREPASSED ? R.ds_base_equal : R.ds_base, b == B_PREPASSED);
    }
}

void pass_msaa_edges() {
    if (R.samples == 1 || !R.cfg.msaa_edge_detect) return;
    // Read depth + normals + material of every sample; pixels whose samples differ get the stencil
    // EDGE bit. Stencil-only: no render target, just a pixel shader that discards simple pixels.
    gpu.ctx->OMSetRenderTargets(0, nullptr, R.depth.dsv_readonly_depth);
    set_viewport(R.w, R.h);
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_edge_mark, STENCIL_EDGE_BIT);
    ID3D11ShaderResourceView* srvs[3] = {R.depth.srv, R.gb_normal.srv, R.gb_material.srv};
    gpu.ctx->PSSetShaderResources(0, 3, srvs);
    gpu.ctx->PSSetShader(R.ps_edges->ps, nullptr, 0);
    draw_fullscreen();
    ID3D11ShaderResourceView* nulls[3] = {};
    gpu.ctx->PSSetShaderResources(0, 3, nulls);
}

void pass_quad_overdraw(Scene& scene) {
    // Re-render the pre-pass and base pass with a pixel shader that counts quad invocations into
    // a UAV (see shaders/overdraw.hlsl), using a scratch copy of the depth/stencil state.
    UINT zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearUnorderedAccessViewUint(R.overdraw.uav, zero);
    gpu.ctx->ClearDepthStencilView(R.depth.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    set_viewport(R.w, R.h);
    gpu.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, R.depth.dsv, 0, 1, &R.overdraw.uav, nullptr);
    gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
    // The pre-pass runs no pixel shader, so it shades no quads: render it depth-only and count
    // only the base pass's shading quads (which the pre-pass depth then rejects).
    gpu.ctx->OMSetDepthStencilState(R.ds_prepass, 0);
    draw_batches(scene, R.list_prepass, MODE_DEPTH, nullptr);
    static const Bucket ti[] = {B_CLOSE_ALPHA, B_OPAQUE, B_FAR_ALPHA, B_PREPASSED};
    static const Bucket classic[] = {B_PREPASSED, B_OPAQUE, B_CLOSE_ALPHA, B_FAR_ALPHA};
    const Bucket* order = R.cfg.order == ORDER_TI ? ti : classic;
    for (int i = 0; i < 4; i++) {
        Bucket b = order[i];
        gpu.ctx->OMSetDepthStencilState(b == B_PREPASSED ? R.ds_base_equal : R.ds_base, 0);
        draw_batches(scene, R.list_base[b], MODE_OVERDRAW, nullptr, b == B_PREPASSED);
    }
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
