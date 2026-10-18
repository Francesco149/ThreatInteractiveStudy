// Global illumination: a baked irradiance probe grid of ambient cubes, noiseless by construction
// and cheap at runtime (8 probes, point loads, in the sun/ambient draw), lighting static and dynamic
// objects the same way. See docs/techniques/gi-ao.md and docs/research/05 (section 6).
//
// A bake has two halves:
//  1. Capture (scene load, or "Rebake"): every probe rasterizes a 6 x 8 x 8 cube of "sensors"
//     (albedo, normal and distance of what it sees, or sky) into an atlas. gi_capture.hlsl.
//  2. Relight (after a capture, and whenever the sun changes): render a whole-scene sun shadow
//     map, then one compute pass per bounce lights every sensor (sun + sky + previous bounce) and
//     projects the result onto the probes' ambient cubes. gi_relight.hlsl. The geometry is not
//     re-rasterized, so moving the sun costs a few milliseconds instead of a full re-bake: the
//     "shadow maps first" relighting from TI's GI diagram, done on demand instead of per frame.
#include "renderer_internal.h"
#include <algorithm>
#include <chrono>

static const int FACE_RES = 8;            // sensor texels per cube-face edge (GI_FACE_RES in the shaders)
static const int PROBES_PER_ROW = 128;    // atlas layout: probes per atlas row, 6 face tiles each
static const int BLOCK = 8;               // capture granularity: blocks of 8x8x8 probes (culling, draw size)
// GPU pacing: the capture waits for the GPU after about this many submitted triangles (~10-20 ms
// of work), so the queue never holds seconds of work (a long submission on a busy GPU can hit the
// 2 s TDR timeout or worse).
static const double CAPTURE_TRIANGLES_PER_WAIT = 100e6;
static const float MIN_ANGULAR_RADIUS = 0.06f;   // the capture skips objects smaller than this (radius / distance)
static const float FAR_ANGULAR_RADIUS = 0.5f;    // ... and uses the coarsest LOD below this
static const double MAX_DRAW_TRIANGLES = 25e6;   // larger batches are split into several draws
static const float3 VOLUME_MARGIN = {12, 6, 12}; // meters around the scene's camera positions
static const int CAPTURE_LOD = 2;         // a coarse LOD is plenty for 8x8-texel faces (and 4x fewer triangles)
static const float CAPTURE_NEAR = 0.02f;
static const int SHADOW_SIZE = 2048;      // whole-scene sun shadow map for the sensors
static const float BACKFACE_LIMIT = 0.25f;

struct GIConstants {   // GICB in shaders/gi.hlsli
    float4 grid_origin;   // xyz = probe (0,0,0), w = spacing
    float4 grid_count;    // xyz = probes per axis, w = GI enabled
    float4 bias;          // x = normal bias, y = view bias, z = GTAO enabled, w = probe visibility test
    uint32_t probes_per_row, pad[3];
};

struct CaptureConstants {   // CaptureCB in shaders/gi_capture.hlsl
    uint32_t first_item, item_count, face, two_sided;
    uint32_t block_x, block_y, block_z, probes_per_row;
    uint32_t block_w, block_h, pad0, pad1;   // probes per block along x and y (z: the rest)
    float4 atlas_size;
    float near_z, pad[3];
};

struct RelightConstants {   // RelightCB in shaders/gi_relight.hlsl
    float4x4 shadow_view_proj;
    float shadow_texel;
    uint32_t bounce;
    float projection_scale;
    float total_solid_angle;
    uint32_t probe_count, probes_per_row;
    float backface_limit, pad;
};

static struct {
    int nx = 0, ny = 0, nz = 0, count = 0;
    float3 origin = {0, 0, 0};
    float spacing = 1;
    Texture sensors;        // R32G32_UINT atlas, persistent (relighting needs it)
    Texture grid[2];        // RGBA16F 3D (nx, ny, 6 nz): ping-pong between bounces
    Texture emissive_grid;  // static emissive irradiance, added by every relight
    int current = 0;        // grid[] holding the final result
    Texture shadow;         // whole-scene sun shadow map
    DrawList shadow_list;

    Shader *vs_capture = nullptr, *ps_capture = nullptr, *cs_relight = nullptr, *cs_emissive = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11RasterizerState* rs_capture = nullptr;
    ID3D11Buffer* cb = nullptr;

    bool capture_pending = false, relight_pending = false;
    float4 lit_sun_dir = {}, lit_sun_color = {};
    int lit_bounces = 0;
    float4 lit_bias = {};    // lookup biases + visibility test the bounces were computed with
    float projection_scale = 1, total_solid_angle = 4 * PI;
    GIInfo info;
} G;

const GIInfo& renderer_gi_info() { return G.info; }
void renderer_request_gi_bake() { G.capture_pending = true; }
void gi_on_scene_loaded(Scene&) {
    G.capture_pending = true;
    G.info.baked = false;
}

void gi_init() {
    G.vs_capture = load_vs("gi_capture.hlsl", "vs_capture");
    G.ps_capture = load_ps("gi_capture.hlsl", "ps_capture");
    G.cs_relight = load_cs("gi_relight.hlsl", "cs_relight");
    G.cs_emissive = load_cs("gi_relight.hlsl", "cs_relight", {{"EMISSIVE_ONLY", "1"}});
    G.cb = create_constant_buffer(sizeof(GIConstants));

    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    gpu.dev->CreateInputLayout(el, 3, G.vs_capture->bytecode.data(), G.vs_capture->bytecode.size(), &G.layout);

    // Two-sided (back faces are detected, not culled). The cube-face cameras are left-handed
    // (right x up = forward, the D3D cube map convention) while the main camera is right-handed,
    // so the image is mirrored and front faces come out clockwise.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &G.rs_capture);

    // Projection normalization: sum, exactly as the relight shader does, the cosine-weighted
    // solid angle of every cube texel around one axis (+X). Analytically it is pi; scaling by
    // pi / sum removes the discretization error, so a uniform sky of radiance 1 gives E = 1 * pi.
    const float3 fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float3 right[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const float3 up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    double cos_sum = 0, omega_sum = 0;
    for (int f = 0; f < 6; f++)
        for (int y = 0; y < FACE_RES; y++)
            for (int x = 0; x < FACE_RES; x++) {
                float u = (x + 0.5f) / FACE_RES * 2 - 1, v = (y + 0.5f) / FACE_RES * 2 - 1;
                float dw = (2.0f / FACE_RES) * (2.0f / FACE_RES) / powf(1 + u * u + v * v, 1.5f);
                float3 dir = normalize(fwd[f] + right[f] * u - up[f] * v);
                omega_sum += dw;
                cos_sum += dw * fmaxf(dir.x, 0.0f);
            }
    G.projection_scale = (float)(PI / cos_sum);
    G.total_solid_angle = (float)omega_sum;
}

// ---------------------------------------------------------------------------------------------

static void release_bake() {
    release_texture(G.sensors);
    for (Texture& t : G.grid) release_texture(t);
    release_texture(G.emissive_grid);
}

// Static objects only (animated ones are dynamic: lit by the grid, not part of it), at a coarse LOD.
static bool bake_static(const Instance& in) { return in.anim == ANIM_NONE; }
static int bake_lod(const Primitive& p) { return CAPTURE_LOD < p.lod_count ? CAPTURE_LOD : p.lod_count - 1; }

// The grid covers the static geometry, cropped to where the camera goes when the scene declares
// it (viewpoints and benchmark path, plus a margin): probes only pay off where shading is seen,
// and a 120 m ground plane or a 40 m tower would otherwise spread the probe budget thin. Lookups
// outside the grid clamp to its outer probes. The spacing grows until the probe budget fits.
static void place_grid(const Scene& scene, const Settings& s) {
    AABB b;
    for (const Instance& in : scene.instances)
        if (bake_static(in)) { b.add(in.world_bounds.lo); b.add(in.world_bounds.hi); }
    if (b.lo.x > b.hi.x) b = scene.bounds;
    AABB cam;
    for (const Viewpoint& v : scene.viewpoints) cam.add(v.pos);
    for (const CameraKey& k : scene.bench_path) cam.add(k.pos);
    if (cam.lo.x <= cam.hi.x) {
        AABB crop = {max3(b.lo, cam.lo - VOLUME_MARGIN), min3(b.hi, cam.hi + VOLUME_MARGIN)};
        if (crop.lo.x < crop.hi.x && crop.lo.y < crop.hi.y && crop.lo.z < crop.hi.z) b = crop;
    }
    float3 ext = b.hi - b.lo;
    float sp = fmaxf(s.gi_probe_spacing, 0.25f);
    for (;;) {   // at least 2 probes per axis: the lookup interpolates a 2x2x2 cell
        G.nx = (int)fmaxf(ceilf(ext.x / sp) + 1, 2);
        G.ny = (int)fmaxf(ceilf(ext.y / sp) + 1, 2);
        G.nz = (int)fmaxf(ceilf(ext.z / sp) + 1, 2);
        if ((int64_t)G.nx * G.ny * G.nz <= s.gi_max_probes) break;
        sp *= 1.05f;
    }
    G.count = G.nx * G.ny * G.nz;
    G.spacing = sp;
    // Centered on the bounds, overhanging them by up to half a cell on each side.
    G.origin = b.center() - float3{(float)(G.nx - 1), (float)(G.ny - 1), (float)(G.nz - 1)} * (sp * 0.5f);
}

static void update_constants(bool enabled) {
    const Settings& s = R.cfg;
    GIConstants c;
    c.grid_origin = {G.origin.x, G.origin.y, G.origin.z, G.spacing};
    c.grid_count = {(float)G.nx, (float)G.ny, (float)G.nz, enabled ? 1.0f : 0.0f};
    c.bias = {s.gi_normal_bias, s.gi_view_bias, s.ao != AO_OFF ? 1.0f : 0.0f, s.gi_visibility ? 1.0f : 0.0f};
    c.probes_per_row = PROBES_PER_ROW;
    update_cb(G.cb, c);
    gpu.ctx->VSSetConstantBuffers(4, 1, &G.cb);
    gpu.ctx->PSSetConstantBuffers(4, 1, &G.cb);
    gpu.ctx->CSSetConstantBuffers(4, 1, &G.cb);
}

// One primitive of one static instance; its DrawData is uploaded once for the whole capture.
struct CaptureObject {
    const Primitive* prim;
    uint32_t prim_order;   // index of the first object using this primitive: a run-independent sort key
    int material;
    float3 center;
    float radius;
};

static float distance_to_box(float3 p, float3 lo, float3 hi) {
    return length(max3(max3(lo - p, p - hi), {0, 0, 0}));
}

// Blocks until the GPU has finished everything submitted so far (flushes the queue too).
static void gpu_wait() {
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query* q = nullptr;
    gpu.dev->CreateQuery(&qd, &q);
    gpu.ctx->End(q);
    BOOL done = FALSE;
    while (gpu.ctx->GetData(q, &done, sizeof(done), 0) != S_OK) Sleep(0);
    q->Release();
}

static void capture(Scene& scene) {
    const Settings& s = R.cfg;
    release_bake();
    place_grid(scene, s);
    int rows = (G.count + PROBES_PER_ROW - 1) / PROBES_PER_ROW;
    int aw = PROBES_PER_ROW * 6 * FACE_RES, ah = rows * FACE_RES;
    G.sensors = create_texture("gi_sensors", aw, ah, DXGI_FORMAT_R32G32_UINT, TEX_RTV);
    Texture emissive = create_texture("gi_capture_emissive", aw, ah, DXGI_FORMAT_R11G11B10_FLOAT, TEX_RTV);
    Texture depth = create_texture("gi_capture_depth", aw, ah, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH);
    for (Texture& t : G.grid) t = create_texture_3d("gi_grid", G.nx, G.ny, G.nz * 6, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    G.emissive_grid = create_texture_3d("gi_emissive_grid", G.nx, G.ny, G.nz * 6, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);

    // Objects and their DrawData (bound as `draws`, t8, during the capture only).
    std::vector<CaptureObject> objects;
    std::vector<GpuDrawData> draw_data;
    for (const Instance& in : scene.instances) {
        if (!bake_static(in)) continue;
        for (const Primitive& p : scene.meshes[in.mesh].prims) {
            if (scene.materials[p.material].alpha_mode == ALPHA_BLEND) continue;
            AABB b = transform_aabb(in.world, p.bounds);
            uint32_t order = (uint32_t)objects.size();
            for (const CaptureObject& o : objects)
                if (o.prim == &p) { order = o.prim_order; break; }
            objects.push_back({&p, order, p.material, b.center(), length(b.extent())});
            GpuDrawData g = {};
            for (int r = 0; r < 3; r++) g.world[r] = {in.world.m[r][0], in.world.m[r][1], in.world.m[r][2], in.world.m[r][3]};
            g.material = p.material;
            draw_data.push_back(g);
        }
    }
    ID3D11ShaderResourceView *draws_srv = nullptr, *items_srv = nullptr;
    ID3D11Buffer* draws_buf = create_structured_buffer(sizeof(GpuDrawData), (uint32_t)draw_data.size() + 1, nullptr,
                                                       false, true, &draws_srv, nullptr);
    if (!draw_data.empty()) update_buffer(draws_buf, draw_data.data(), (uint32_t)(draw_data.size() * sizeof(GpuDrawData)));
    ID3D11Buffer* items_buf = create_structured_buffer(sizeof(uint32_t), (uint32_t)objects.size() * 6 + 1, nullptr, false,
                                                       true, &items_srv, nullptr);

    bind_frame_resources(scene);
    gpu.ctx->VSSetShaderResources(8, 1, &draws_srv);
    gpu.ctx->VSSetShaderResources(1, 1, &items_srv);
    update_constants(false);

    float zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearRenderTargetView(G.sensors.rtv, zero);   // 0 = sky
    gpu.ctx->ClearRenderTargetView(emissive.rtv, zero);
    gpu.ctx->ClearDepthStencilView(depth.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    ID3D11RenderTargetView* rtvs[2] = {G.sensors.rtv, emissive.rtv};
    gpu.ctx->OMSetRenderTargets(2, rtvs, depth.dsv);
    set_viewport(aw, ah);
    gpu.ctx->RSSetState(G.rs_capture);
    gpu.ctx->OMSetDepthStencilState(R.ds_shadow, 0);   // reversed Z: GREATER, write
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    ID3D11Buffer* vbs[2] = {scene.vb_position, scene.vb_attribs};
    UINT strides[2] = {sizeof(float3), sizeof(VertexAttribs)}, offsets[2] = {0, 0};
    gpu.ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    gpu.ctx->IASetIndexBuffer(scene.ib, DXGI_FORMAT_R32_UINT, 0);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.ctx->IASetInputLayout(G.layout);
    gpu.ctx->VSSetShader(G.vs_capture->vs, nullptr, 0);
    gpu.ctx->PSSetShader(G.ps_capture->ps, nullptr, 0);

    // Blocks of 8x8x8 probes, one cube face at a time. Only the objects that can matter are drawn:
    //  - inside the face's 90-degree frustum for at least one probe of the block: the frustum is
    //    4 planes through the probe, (F -/+ R).(q - p) >= 0 and (F -/+ U).(q - p) >= 0, and a sphere
    //    is outside the union over the block's probes if it is behind one plane for all of them;
    //  - not so small and far that it would cover a fraction of a texel (radius / distance below
    //    MIN_ANGULAR_RADIUS); small-looking ones (below FAR_ANGULAR_RADIUS) use their coarsest LOD.
    // Objects sharing a primitive and LOD become one instanced draw: instances = objects x probes.
    static const float3 face_fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float3 face_right[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    static const float3 face_up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    struct Item { uint32_t object; int lod; };
    std::vector<Item> selected;
    std::vector<uint32_t> items;
    std::vector<DrawBatch> batches[6];
    G.info.capture_draws = 0;
    G.info.capture_triangles = 0;
    G.info.capture_waits = 0;
    double pending_triangles = 0;
    for (int bz = 0; bz < G.nz; bz += BLOCK)
        for (int by = 0; by < G.ny; by += BLOCK)
            for (int bx = 0; bx < G.nx; bx += BLOCK) {
                int ex = bx + BLOCK < G.nx ? bx + BLOCK : G.nx, ey = by + BLOCK < G.ny ? by + BLOCK : G.ny,
                    ez = bz + BLOCK < G.nz ? bz + BLOCK : G.nz;
                float3 lo = G.origin + float3{(float)bx, (float)by, (float)bz} * G.spacing;
                float3 hi = G.origin + float3{(float)(ex - 1), (float)(ey - 1), (float)(ez - 1)} * G.spacing;
                float3 center = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
                items.clear();
                for (int f = 0; f < 6; f++) {
                    batches[f].clear();
                    float3 n[4] = {face_fwd[f] - face_right[f], face_fwd[f] + face_right[f], face_fwd[f] - face_up[f],
                                   face_fwd[f] + face_up[f]};
                    float plane[4];   // the plane offsets of the block's most permissive probe
                    for (int i = 0; i < 4; i++)
                        plane[i] = dot(n[i], center) - (fabsf(n[i].x) * half.x + fabsf(n[i].y) * half.y + fabsf(n[i].z) * half.z);
                    selected.clear();
                    for (uint32_t i = 0; i < (uint32_t)objects.size(); i++) {
                        const CaptureObject& o = objects[i];
                        bool outside = false;
                        for (int k = 0; k < 4; k++) outside |= dot(n[k], o.center) + o.radius * 1.4142136f < plane[k];
                        if (outside) continue;
                        float d = distance_to_box(o.center, lo, hi) - o.radius;
                        if (d > 0 && o.radius < d * MIN_ANGULAR_RADIUS) continue;
                        bool distant = d > 0 && o.radius < d * FAR_ANGULAR_RADIUS;
                        selected.push_back({i, distant ? o.prim->lod_count - 1 : bake_lod(*o.prim)});
                    }
                    // Deterministic order (not by pointer): coplanar surfaces z-fight the same way every bake.
                    std::sort(selected.begin(), selected.end(), [&](const Item& a, const Item& b) {
                        const CaptureObject &oa = objects[a.object], &ob = objects[b.object];
                        if (oa.prim_order != ob.prim_order) return oa.prim_order < ob.prim_order;
                        return a.lod != b.lod ? a.lod < b.lod : a.object < b.object;
                    });
                    for (size_t k = 0; k < selected.size(); k++) {
                        const CaptureObject& o = objects[selected[k].object];
                        bool same = k > 0 && objects[selected[k - 1].object].prim == o.prim && selected[k - 1].lod == selected[k].lod;
                        if (same) batches[f].back().count++;
                        else batches[f].push_back({o.prim, selected[k].lod, o.material, (uint32_t)items.size(), 1, 0});
                        items.push_back(selected[k].object);
                    }
                }
                if (items.empty()) continue;
                update_buffer(items_buf, items.data(), (uint32_t)(items.size() * sizeof(uint32_t)));
                uint32_t probes = (uint32_t)((ex - bx) * (ey - by) * (ez - bz));
                for (int f = 0; f < 6; f++)
                    for (const DrawBatch& b : batches[f]) {
                        const Material& m = scene.materials[b.material];
                        ID3D11ShaderResourceView* srvs[5] = {m.tex_albedo >= 0 ? scene.textures[m.tex_albedo].srv : nullptr,
                                                             nullptr, nullptr, nullptr,
                                                             m.tex_emissive >= 0 ? scene.textures[m.tex_emissive].srv : nullptr};
                        gpu.ctx->PSSetShaderResources(0, 5, srvs);
                        const MeshLod& lod = b.prim->lods[b.lod];
                        double object_triangles = (double)probes * (lod.index_count / 3);
                        uint32_t per_draw = (uint32_t)fmax(1.0, MAX_DRAW_TRIANGLES / object_triangles);
                        for (uint32_t k = 0; k < b.count; k += per_draw) {
                            uint32_t count = b.count - k < per_draw ? b.count - k : per_draw;
                            CaptureConstants cc = {b.first_draw + k, count, (uint32_t)f, m.double_sided ? 1u : 0u,
                                                   (uint32_t)bx, (uint32_t)by, (uint32_t)bz, PROBES_PER_ROW,
                                                   (uint32_t)(ex - bx), (uint32_t)(ey - by), 0, 0,
                                                   {(float)aw, (float)ah, 1.0f / aw, 1.0f / ah}, CAPTURE_NEAR};
                            update_cb(R.cb_pass, cc);
                            gpu.ctx->DrawIndexedInstanced(lod.index_count, count * probes, lod.first_index, b.prim->base_vertex, 0);
                            G.info.capture_draws++;
                            G.info.capture_triangles += count * object_triangles;
                            pending_triangles += count * object_triangles;
                            if (pending_triangles > CAPTURE_TRIANGLES_PER_WAIT) {   // pacing, see the constant
                                gpu_wait();
                                pending_triangles = 0;
                                G.info.capture_waits++;
                            }
                        }
                    }
            }
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->VSSetShaderResources(1, 1, &null_srv);
    draws_srv->Release();
    draws_buf->Release();
    items_srv->Release();
    items_buf->Release();
    bind_frame_resources(scene);   // restore the frame's `draws` buffer

    // Emissive surfaces: projected once into a static grid (same projection as the relight).
    RelightConstants rc = {};
    rc.projection_scale = G.projection_scale;
    rc.total_solid_angle = G.total_solid_angle;
    rc.probe_count = G.count;
    rc.probes_per_row = PROBES_PER_ROW;
    update_cb(R.cb_pass, rc);
    ID3D11ShaderResourceView* srvs[5] = {G.sensors.srv, nullptr, nullptr, nullptr, emissive.srv};
    gpu.ctx->CSSetShaderResources(0, 5, srvs);
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &G.emissive_grid.uav, nullptr);
    gpu.ctx->CSSetShader(G.cs_emissive->cs, nullptr, 0);
    gpu.ctx->Dispatch(PROBES_PER_ROW, rows, 1);
    ID3D11ShaderResourceView* null_srvs[5] = {};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    gpu.ctx->CSSetShaderResources(0, 5, null_srvs);
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

    release_texture(emissive);
    release_texture(depth);
    G.info.nx = G.nx;
    G.info.ny = G.ny;
    G.info.nz = G.nz;
    G.info.probes = G.count;
    G.info.spacing = G.spacing;
    G.info.memory_mb = ((double)aw * ah * 8 + 3.0 * G.count * 6 * 8 + (double)SHADOW_SIZE * SHADOW_SIZE * 4) / (1024 * 1024);
    G.info.capture_memory_mb = (double)aw * ah * (4 + 4) / (1024 * 1024);
}

// The relight's shadow map: static shadow casters at the capture LOD, in the frame's draw buffer.
static void build_shadow_list(Scene& scene) {
    R.draw_data.clear();
    G.shadow_list.clear();
    for (int i = 0; i < (int)scene.instances.size(); i++) {
        const Instance& in = scene.instances[i];
        if (!bake_static(in) || (in.flags & INST_NO_SHADOW)) continue;
        for (const Primitive& p : scene.meshes[in.mesh].prims)
            if (scene.materials[p.material].alpha_mode != ALPHA_BLEND)
                G.shadow_list.items.push_back({&p, bake_lod(p), p.material, i, 0, 0, 0});
    }
    finalize_shadow_list(scene, G.shadow_list);
    upload_draw_data();   // the frame builds (and uploads) its own lists after this
    bind_frame_resources(scene);
}

// `timed`: wait for the GPU after each half and record the wall times (full bakes only).
static void relight(Scene& scene, bool timed) {
    const Settings& s = R.cfg;
    const FrameConstants& fc = R.frame_constants;
    auto t0 = std::chrono::high_resolution_clock::now();
    build_shadow_list(scene);
    update_constants(false);

    // 1) Whole-scene sun shadow map: orthographic, fitted to the grid in light space; the depth
    //    range spans the whole scene so casters outside the grid still shadow it.
    if (!G.shadow.tex) G.shadow = create_texture("gi_sun_shadow", SHADOW_SIZE, SHADOW_SIZE, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH);
    float3 L = normalize(float3{fc.sun_dir.x, fc.sun_dir.y, fc.sun_dir.z});
    float3 up = fabsf(L.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
    float4x4 light_view = look_to({0, 0, 0}, L, up);
    float3 margin = {G.spacing, G.spacing, G.spacing};
    AABB box = {G.origin - margin, G.origin + float3{(float)G.nx, (float)G.ny, (float)G.nz} * G.spacing};
    AABB box_ls = transform_aabb(light_view, box);
    AABB scene_ls = transform_aabb(light_view, scene.bounds);
    float4x4 proj = ortho_reverse_z(box_ls.lo.x, box_ls.hi.x, box_ls.lo.y, box_ls.hi.y, -scene_ls.hi.z - 1.0f,
                                    -scene_ls.lo.z + 1.0f);
    float4x4 shadow_vp = proj * light_view;
    float texel = fmaxf(box_ls.hi.x - box_ls.lo.x, box_ls.hi.y - box_ls.lo.y) / SHADOW_SIZE;

    {
        GPU_SCOPE("Sun shadow map");
        gpu.ctx->ClearDepthStencilView(G.shadow.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        gpu.ctx->OMSetRenderTargets(0, nullptr, G.shadow.dsv);
        set_viewport(SHADOW_SIZE, SHADOW_SIZE);
        gpu.ctx->OMSetDepthStencilState(R.ds_shadow, 0);
        gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
        update_cb(R.cb_pass, shadow_vp);   // PassCB in depth.hlsl
        draw_shadow_list(scene, G.shadow_list);
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (timed) gpu_wait();
    auto t1 = std::chrono::high_resolution_clock::now();

    // 2) One relight pass per bounce, ping-ponging the grids: bounce b reads bounce b-1.
    int rows = (G.count + PROBES_PER_ROW - 1) / PROBES_PER_ROW;
    int bounces = s.gi_bounces < 1 ? 1 : s.gi_bounces;
    GPU_SCOPE("Bounces");
    gpu.ctx->CSSetShader(G.cs_relight->cs, nullptr, 0);
    ID3D11UnorderedAccessView* null_uav = nullptr;
    for (int b = 0; b < bounces; b++) {
        int dst = b & 1;
        RelightConstants rc = {shadow_vp, texel, (uint32_t)b, G.projection_scale, G.total_solid_angle,
                               (uint32_t)G.count, PROBES_PER_ROW, BACKFACE_LIMIT, 0};
        update_cb(R.cb_pass, rc);
        gpu.ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ID3D11ShaderResourceView* srvs[4] = {G.sensors.srv, b > 0 ? G.grid[dst ^ 1].srv : nullptr, G.shadow.srv,
                                             G.emissive_grid.srv};
        gpu.ctx->CSSetShaderResources(0, 4, srvs);
        gpu.ctx->CSSetUnorderedAccessViews(0, 1, &G.grid[dst].uav, nullptr);
        gpu.ctx->Dispatch(PROBES_PER_ROW, rows, 1);
        G.current = dst;
    }
    ID3D11ShaderResourceView* null_srvs[4] = {};
    gpu.ctx->CSSetShaderResources(0, 4, null_srvs);
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

    G.lit_sun_dir = fc.sun_dir;
    G.lit_sun_color = fc.sun_color;
    G.lit_bounces = s.gi_bounces;
    G.lit_bias = {s.gi_normal_bias, s.gi_view_bias, 0, s.gi_visibility ? 1.0f : 0.0f};
    G.relight_pending = false;
    if (timed) {
        gpu_wait();
        auto t2 = std::chrono::high_resolution_clock::now();
        G.info.shadow_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        G.info.bounces_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
}

// Counts the probes classified as inside geometry (validity 0): a one-off readback after a bake.
static int count_invalid_probes() {
    ID3D11Resource* res = nullptr;
    G.grid[G.current].srv->GetResource(&res);
    ID3D11Texture3D* tex = (ID3D11Texture3D*)res;
    D3D11_TEXTURE3D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture3D* staging = nullptr;
    gpu.dev->CreateTexture3D(&d, nullptr, &staging);
    gpu.ctx->CopyResource(staging, tex);
    int invalid = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(gpu.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int z = 0; z < G.nz; z++)   // stack 0 (+X); validity is the same in every stack
            for (int y = 0; y < G.ny; y++) {
                const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + z * m.DepthPitch + y * m.RowPitch);
                for (int x = 0; x < G.nx; x++) invalid += row[x * 4 + 3] == 0;   // half 0.0
            }
        gpu.ctx->Unmap(staging, 0);
    }
    staging->Release();
    res->Release();
    return invalid;
}

void gi_update(Scene& scene) {
    const Settings& s = R.cfg;
    const FrameConstants& fc = R.frame_constants;
    ID3D11ShaderResourceView* null_srv = nullptr;
    gpu.ctx->PSSetShaderResources(24, 1, &null_srv);   // the grid may be written below
    gpu.ctx->PSSetShaderResources(26, 1, &null_srv);

    if (s.gi && !scene.instances.empty()) {
        // Not sun_color.rgb: that is the illuminance at the camera (atmosphere transmittance at its
        // altitude) and changes whenever the camera moves up or down.
        auto changed = [](float4 a, float4 b) { return fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z) > 1e-5f; };
        if (s.gi_relight_on_sun_change &&
            (changed(fc.sun_dir, G.lit_sun_dir) || fabsf(fc.sun_color.w - G.lit_sun_color.w) > 1e-5f))
            G.relight_pending = true;
        if (s.gi_bounces != G.lit_bounces) G.relight_pending = true;
        // Each bounce samples the previous grid with these, so the stored bounces depend on them.
        float4 bias = {s.gi_normal_bias, s.gi_view_bias, 0, s.gi_visibility ? 1.0f : 0.0f};
        if (changed(bias, G.lit_bias) || bias.w != G.lit_bias.w) G.relight_pending = true;

        if (G.capture_pending) {
            // Full bake: timed with GPU synchronization (a one-off hitch, reported in the UI).
            gpu_wait();
            auto t0 = std::chrono::high_resolution_clock::now();
            {
                GPU_SCOPE("GI capture");
                capture(scene);
            }
            gpu_wait();
            auto t1 = std::chrono::high_resolution_clock::now();
            {
                GPU_SCOPE("GI relight");
                relight(scene, true);
            }
            auto t2 = std::chrono::high_resolution_clock::now();
            G.capture_pending = false;
            G.info.baked = true;
            G.info.capture_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            G.info.relight_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            G.info.invalid_probes = count_invalid_probes();
            log_msg("GI bake: %dx%dx%d = %d probes at %.2f m (%d inside geometry), %.1f MB (+%.1f MB transient)",
                    G.nx, G.ny, G.nz, G.count, G.spacing, G.info.invalid_probes, G.info.memory_mb, G.info.capture_memory_mb);
            log_msg("GI bake time: capture %.1f ms (%d draws, %.0fM triangles, %d GPU waits) + relight %.1f ms "
                    "(sun shadow map %.1f ms, %d bounces %.1f ms)",
                    G.info.capture_ms, G.info.capture_draws, G.info.capture_triangles / 1e6, G.info.capture_waits,
                    G.info.relight_ms, G.info.shadow_ms, s.gi_bounces, G.info.bounces_ms);
        } else if ((G.relight_pending || s.gi_relight_every_frame) && G.info.baked) {
            GPU_SCOPE("GI relight");
            relight(scene, false);
        }
    }

    bool enabled = s.gi && G.info.baked;
    update_constants(enabled);
    if (enabled) {
        gpu.ctx->PSSetShaderResources(24, 1, &G.grid[G.current].srv);
        gpu.ctx->PSSetShaderResources(26, 1, &G.sensors.srv);   // probe visibility (hit distances)
    }
}
