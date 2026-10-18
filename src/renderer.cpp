// Renderer setup and the frame graph (a plain function calling every pass in order).
#include "renderer_internal.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

Renderer R;

const FrameStats& renderer_stats() { return R.stats; }

// ---------------------------------------------------------------------------------------------
// State creation helpers

static ID3D11RasterizerState* make_raster(D3D11_CULL_MODE cull, bool depth_clip = true, bool scissor = false,
                                          int depth_bias = 0, float slope_bias = 0, bool msaa = true) {
    D3D11_RASTERIZER_DESC d = {};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull;
    d.FrontCounterClockwise = TRUE;  // glTF / OpenGL winding
    d.DepthClipEnable = depth_clip;
    d.ScissorEnable = scissor;
    d.MultisampleEnable = msaa;
    d.DepthBias = depth_bias;
    d.SlopeScaledDepthBias = slope_bias;
    ID3D11RasterizerState* s = nullptr;
    gpu.dev->CreateRasterizerState(&d, &s);
    return s;
}

static ID3D11BlendState* make_blend(int rt_count, bool additive, UINT8 mask0 = D3D11_COLOR_WRITE_ENABLE_ALL) {
    D3D11_BLEND_DESC d = {};
    d.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; i++) {
        D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[i];
        rt.BlendEnable = additive;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = i < rt_count ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    }
    d.RenderTarget[0].RenderTargetWriteMask = mask0;
    ID3D11BlendState* s = nullptr;
    gpu.dev->CreateBlendState(&d, &s);
    return s;
}

struct StencilDesc {
    bool enable = false;
    UINT8 read = 0xFF, write = 0xFF;
    D3D11_COMPARISON_FUNC func = D3D11_COMPARISON_ALWAYS;
    D3D11_STENCIL_OP pass = D3D11_STENCIL_OP_KEEP, depth_fail = D3D11_STENCIL_OP_KEEP, fail = D3D11_STENCIL_OP_KEEP;
};

static ID3D11DepthStencilState* make_depth(bool test, bool write, D3D11_COMPARISON_FUNC func, StencilDesc st = {}) {
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = test;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = func;
    d.StencilEnable = st.enable;
    d.StencilReadMask = st.read;
    d.StencilWriteMask = st.write;
    d.FrontFace = {st.fail, st.depth_fail, st.pass, st.func};
    d.BackFace = d.FrontFace;
    ID3D11DepthStencilState* s = nullptr;
    gpu.dev->CreateDepthStencilState(&d, &s);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Light volume meshes: an icosphere for point lights and a cone for spot lights. Both are scaled
// so that the low-poly mesh fully contains the light's range (a sphere *inscribed* in the mesh).

static void make_light_meshes() {
    // Icosphere, subdivided once (80 triangles).
    std::vector<float3> v;
    std::vector<uint32_t> idx;
    float t = (1.0f + sqrtf(5.0f)) / 2.0f;
    float3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                       {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (float3 p : base) v.push_back(normalize(p));
    uint32_t faces[] = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                        3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int f = 0; f < 20; f++) {
        uint32_t a = faces[f * 3], b = faces[f * 3 + 1], c = faces[f * 3 + 2];
        uint32_t ab = (uint32_t)v.size(); v.push_back(normalize(v[a] + v[b]));
        uint32_t bc = (uint32_t)v.size(); v.push_back(normalize(v[b] + v[c]));
        uint32_t ca = (uint32_t)v.size(); v.push_back(normalize(v[c] + v[a]));
        uint32_t tris[] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
        idx.insert(idx.end(), tris, tris + 12);
    }
    // Scale so the faces (not just the vertices) lie outside the unit sphere: the inscribed radius
    // of this mesh is the distance to the closest face plane.
    float inscribed = 1.0f;
    for (size_t i = 0; i < idx.size(); i += 3) {
        float3 n = normalize(cross(v[idx[i + 1]] - v[idx[i]], v[idx[i + 2]] - v[idx[i]]));
        inscribed = fminf(inscribed, fabsf(dot(n, v[idx[i]])));
    }
    for (float3& p : v) p = p / inscribed;
    R.sphere_vb = create_buffer(v.data(), (uint32_t)(v.size() * sizeof(float3)), D3D11_BIND_VERTEX_BUFFER);
    R.sphere_ib = create_buffer(idx.data(), (uint32_t)(idx.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER);
    R.sphere_index_count = (uint32_t)idx.size();

    // Cone along -Z with apex at the origin, length 1 and radius 1 at the base (scaled per light).
    // The circle is approximated by a polygon that circumscribes it.
    const int seg = 16;
    float circ = 1.0f / cosf(PI / seg);
    std::vector<float3> cv = {{0, 0, 0}, {0, 0, -1}};
    std::vector<uint32_t> ci;
    for (int i = 0; i < seg; i++) {
        float a = 2 * PI * i / seg;
        cv.push_back({cosf(a) * circ, sinf(a) * circ, -1});
    }
    for (int i = 0; i < seg; i++) {
        uint32_t a = 2 + i, b = 2 + (i + 1) % seg;
        ci.insert(ci.end(), {0, b, a, 1, a, b});
    }
    R.cone_vb = create_buffer(cv.data(), (uint32_t)(cv.size() * sizeof(float3)), D3D11_BIND_VERTEX_BUFFER);
    R.cone_ib = create_buffer(ci.data(), (uint32_t)(ci.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER);
    R.cone_index_count = (uint32_t)ci.size();
}

// ---------------------------------------------------------------------------------------------
// Render targets (recreated when resolution or MSAA count changes)

static void release_targets() {
    Texture* all[] = {&R.depth, &R.gb_albedo, &R.gb_normal, &R.gb_material, &R.gb_motion, &R.gb_emissive,
                      &R.light_diffuse, &R.light_specular, &R.velocity, &R.smaa_edges,
                      &R.smaa_weights, &R.aa_out, &R.post_out, &R.overdraw, &R.ldr[0], &R.ldr[1], &R.ldr[2],
                      &R.ldr[3], &R.smaa_out[0], &R.smaa_out[1]};
    for (Texture* t : all) release_texture(*t);
    for (int i = 0; i < 4; i++) { SAFE_RELEASE(R.ldr_srgb_srv[i]); SAFE_RELEASE(R.ldr_srgb_rtv[i]); }
    for (int i = 0; i < 2; i++) { SAFE_RELEASE(R.smaa_out_srgb_srv[i]); SAFE_RELEASE(R.smaa_out_srgb_rtv[i]); }
}

// 8-bit colour targets that SMAA reads both as gamma-space values (UNORM view, for edge detection)
// and as linear values (SRGB view, for blending).
static Texture create_ldr(const char* name, ID3D11ShaderResourceView** srgb_srv, ID3D11RenderTargetView** srgb_rtv) {
    Texture t;
    t.w = R.w;
    t.h = R.h;
    t.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = R.w;
    d.Height = R.h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    gpu.dev->CreateTexture2D(&d, nullptr, &t.tex);
    set_debug_name(t.tex, name);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    sd.Format = rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpu.dev->CreateShaderResourceView(t.tex, &sd, &t.srv);
    gpu.dev->CreateRenderTargetView(t.tex, &rd, &t.rtv);
    sd.Format = rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    gpu.dev->CreateShaderResourceView(t.tex, &sd, srgb_srv);
    gpu.dev->CreateRenderTargetView(t.tex, &rd, srgb_rtv);
    return t;
}

static void create_targets(const Settings& s) {
    release_targets();
    R.w = s.render_w;
    R.h = s.render_h;
    R.samples = s.msaa;
    int S = R.samples;
    DXGI_FORMAT depth_fmt = s.depth_24bit ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    R.depth = create_texture("depth", R.w, R.h, depth_fmt, TEX_DEPTH | TEX_MS_SRV, S);
    R.gb_albedo = create_texture("gb_albedo", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RTV | TEX_MS_SRV, S);
    R.gb_normal = create_texture("gb_normal", R.w, R.h, DXGI_FORMAT_R10G10B10A2_UNORM, TEX_RTV | TEX_MS_SRV, S);
    R.gb_material = create_texture("gb_material", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RTV | TEX_MS_SRV, S);
    R.gb_motion = create_texture("gb_motion", R.w, R.h, DXGI_FORMAT_R16G16_FLOAT, TEX_RTV | TEX_MS_SRV, S);
    R.gb_emissive = create_texture("gb_emissive", R.w, R.h, DXGI_FORMAT_R11G11B10_FLOAT, TEX_RTV | TEX_MS_SRV, S);
    R.light_diffuse = create_texture("light_diffuse", R.w, R.h, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RTV | TEX_MS_SRV, S);
    R.light_specular = create_texture("light_specular", R.w, R.h, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RTV | TEX_MS_SRV, S);
    R.velocity = create_texture("velocity", R.w, R.h, DXGI_FORMAT_R16G16_FLOAT, TEX_RTV);
    for (int i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "ldr_sample%d", i);
        R.ldr[i] = create_ldr(name, &R.ldr_srgb_srv[i], &R.ldr_srgb_rtv[i]);
    }
    for (int i = 0; i < 2; i++) R.smaa_out[i] = create_ldr("smaa_out", &R.smaa_out_srgb_srv[i], &R.smaa_out_srgb_rtv[i]);
    R.smaa_edges = create_texture("smaa_edges", R.w, R.h, DXGI_FORMAT_R8G8_UNORM, TEX_RTV);
    R.smaa_weights = create_texture("smaa_weights", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RTV);
    R.aa_out = create_texture("aa_out", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RTV);
    R.post_out = create_texture("post_out", R.w, R.h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RTV);
    R.overdraw = create_texture("quad_overdraw", R.w, R.h, DXGI_FORMAT_R32_UINT, TEX_UAV);
    R.cfg = s;
    R.have_prev = false;
}

// ---------------------------------------------------------------------------------------------

void renderer_init(const Settings& s) {
    R.cb_frame = create_constant_buffer(sizeof(FrameConstants));
    R.cb_pass = create_constant_buffer(1024);
    R.cb_light = create_constant_buffer(256);

    R.samplers[0] = create_sampler(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_COMPARISON_NEVER, 8);
    R.samplers[1] = create_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP);
    R.samplers[2] = create_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    R.samplers[3] = create_sampler(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                   D3D11_COMPARISON_GREATER_EQUAL);
    R.samplers[4] = create_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP);

    R.rs_back = make_raster(D3D11_CULL_BACK);
    R.rs_none = make_raster(D3D11_CULL_NONE);
    R.rs_volume = make_raster(D3D11_CULL_NONE, false, true);
    R.rs_scissor = make_raster(D3D11_CULL_NONE, true, true);
    R.rs_shadow = make_raster(D3D11_CULL_BACK, false, false, -2, -2.0f, false);
    R.rs_shadow_none = make_raster(D3D11_CULL_NONE, false, false, -2, -2.0f, false);

    R.bs_opaque = make_blend(8, false);
    R.bs_gbuffer = make_blend(4, false);          // RT 0-3 written, RT4 (emissive) masked
    R.bs_gbuffer_emissive = make_blend(5, false);
    R.bs_additive = make_blend(8, true);
    R.bs_no_color = make_blend(0, false, 0);
    {
        D3D11_BLEND_DESC d = {};
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
        d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        gpu.dev->CreateBlendState(&d, &R.bs_blend_factor);
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        gpu.dev->CreateBlendState(&d, &R.bs_alpha);
    }

    // Depth is reversed (near = 1), so "closer" is GREATER.
    StencilDesc write_sm;
    write_sm.enable = true;
    write_sm.write = STENCIL_SM_BITS;
    write_sm.pass = D3D11_STENCIL_OP_REPLACE;
    R.ds_prepass = make_depth(true, true, D3D11_COMPARISON_GREATER);
    R.ds_base = make_depth(true, true, D3D11_COMPARISON_GREATER, write_sm);
    R.ds_base_equal = make_depth(true, false, D3D11_COMPARISON_GREATER_EQUAL, write_sm);  // = EQUAL here, keeps the Hi-Z direction
    R.ds_none = make_depth(false, false, D3D11_COMPARISON_ALWAYS);
    R.ds_shadow = make_depth(true, true, D3D11_COMPARISON_GREATER);
    R.ds_transparent = make_depth(true, false, D3D11_COMPARISON_GREATER);
    {
        StencilDesc st;
        st.enable = true;
        st.write = STENCIL_EDGE_BIT;
        st.pass = D3D11_STENCIL_OP_REPLACE;
        R.ds_edge_mark = make_depth(false, false, D3D11_COMPARISON_ALWAYS, st);
    }
    {
        // Stencil light volume: both faces, depth test on (no write). Every face that is *behind*
        // the visible surface (depth test fails) toggles bit 7. A surface inside a convex volume
        // ends up toggled exactly once. Sky/empty samples (shading model 0) are rejected by the
        // stencil test itself, so the volume never touches them.
        StencilDesc st;
        st.enable = true;
        st.read = STENCIL_SM_BITS;
        st.write = STENCIL_LIGHT_BIT;
        st.func = D3D11_COMPARISON_NOT_EQUAL;   // ref 0: skip sky samples
        st.depth_fail = D3D11_STENCIL_OP_INVERT;
        R.ds_light_volume = make_depth(true, false, D3D11_COMPARISON_GREATER, st);
    }
    {
        StencilDesc st;
        st.enable = true;
        st.write = 0xFF;
        st.pass = D3D11_STENCIL_OP_REPLACE;
        R.ds_smaa_mark = make_depth(false, false, D3D11_COMPARISON_ALWAYS, st);
        st.func = D3D11_COMPARISON_EQUAL;
        st.pass = D3D11_STENCIL_OP_KEEP;
        st.write = 0;
        R.ds_smaa_test = make_depth(false, false, D3D11_COMPARISON_ALWAYS, st);
    }

    R.vs_fullscreen = load_vs("fullscreen.hlsl", "vs_main");
    R.ps_clear_fs = load_ps("fullscreen.hlsl", "ps_clear");
    make_light_meshes();

    // SMAA lookup textures (precomputed, from the reference implementation).
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = AREATEX_WIDTH;
        d.Height = AREATEX_HEIGHT;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {areaTexBytes, AREATEX_PITCH, 0};
        gpu.dev->CreateTexture2D(&d, &init, &R.smaa_area.tex);
        gpu.dev->CreateShaderResourceView(R.smaa_area.tex, nullptr, &R.smaa_area.srv);
        d.Width = SEARCHTEX_WIDTH;
        d.Height = SEARCHTEX_HEIGHT;
        d.Format = DXGI_FORMAT_R8_UNORM;
        init = {searchTexBytes, SEARCHTEX_PITCH, 0};
        gpu.dev->CreateTexture2D(&d, &init, &R.smaa_search.tex);
        gpu.dev->CreateShaderResourceView(R.smaa_search.tex, nullptr, &R.smaa_search.srv);
    }

    geometry_init();
    shadows_init();
    lighting_init();
    sky_init();
    bloom_init();
    tonemap_lut_init();
    gi_init();
    gtao_init();
    post_init();
    motion_blur_init();
    sss_init();
    vfx_init();
    decals_init();
    create_targets(s);
}

void renderer_shutdown() {
    release_targets();
    // The process is exiting; the remaining D3D objects are released with the device.
}

void renderer_on_scene_loaded(Scene& scene) {
    R.have_prev = false;
    gi_on_scene_loaded(scene);
}

// ---------------------------------------------------------------------------------------------
// Shared helpers

ID3D11DepthStencilState* ds_stencil_test(UINT8 read_mask, bool clear_light) {
    static ID3D11DepthStencilState* cache[256][2] = {};
    ID3D11DepthStencilState*& s = cache[read_mask][clear_light ? 1 : 0];
    if (!s) {
        StencilDesc st;
        st.enable = true;
        st.read = read_mask;
        st.write = clear_light ? STENCIL_LIGHT_BIT : 0;
        st.func = D3D11_COMPARISON_EQUAL;
        st.pass = clear_light ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
        s = make_depth(false, false, D3D11_COMPARISON_ALWAYS, st);
    }
    return s;
}

void draw_fullscreen() {
    gpu.ctx->IASetInputLayout(nullptr);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.ctx->VSSetShader(R.vs_fullscreen->vs, nullptr, 0);
    gpu.ctx->Draw(3, 0);
}

void bind_frame_resources(Scene& scene) {
    ID3D11Buffer* cbs[3] = {R.cb_frame, R.cb_pass, R.cb_light};
    gpu.ctx->VSSetConstantBuffers(0, 3, cbs);
    gpu.ctx->PSSetConstantBuffers(0, 3, cbs);
    gpu.ctx->CSSetConstantBuffers(0, 3, cbs);
    gpu.ctx->PSSetSamplers(0, 5, R.samplers);
    gpu.ctx->VSSetSamplers(0, 5, R.samplers);
    gpu.ctx->CSSetSamplers(0, 5, R.samplers);
    ID3D11ShaderResourceView* srvs[3] = {R.draw_srv, scene.material_srv, scene.profile_srv};
    gpu.ctx->VSSetShaderResources(8, 3, srvs);
    gpu.ctx->PSSetShaderResources(8, 3, srvs);
}

// Halton(2,3) is not used: SMAA T2x/4x needs exactly two alternating subpixel offsets.
// Projection jitter in pixels, NDC orientation (x right, y UP), exactly the SMAA demo's
// getJitter(), which the subsample indices in pass_antialiasing() assume. With the standard 2x MSAA
// pattern (sample 0 at (+0.25,-0.25), sample 1 at (-0.25,+0.25), y up) the 4x offsets run along
// the *other* diagonal, so the two frames together give SMAA 4x's 4-rook pattern; jittering along
// the samples' own diagonal would put all four samples on one line.
static float2 jitter_for_frame(const Settings& s, uint32_t frame) {
    bool temporal = s.aa == AA_SMAA_T2X || s.aa == AA_SMAA_4X;
    if (!temporal) return {0, 0};
    bool odd = frame & 1;
    if (s.aa == AA_SMAA_4X && s.msaa == 2) return odd ? float2{0.125f, 0.125f} : float2{-0.125f, -0.125f};
    return odd ? float2{0.25f, -0.25f} : float2{-0.25f, 0.25f};  // T2x
}

void renderer_frame(Scene& scene, const Camera& cam, const Settings& s, float time, float dt) {
    if (s.render_w != R.cfg.render_w || s.render_h != R.cfg.render_h || s.msaa != R.cfg.msaa ||
        s.depth_24bit != R.cfg.depth_24bit)
        create_targets(s);
    R.cfg = s;
    R.stats = FrameStats{};

    if (s.animate) scene_animate(scene, time);

    // --- frame constants -------------------------------------------------------------------
    FrameConstants& fc = R.frame_constants;
    float aspect = (float)R.w / R.h;
    float4x4 view = look_to(cam.pos, cam.forward(), {0, 1, 0});
    float4x4 proj_nj = perspective_reverse_z_inf(cam.fov_y, aspect, cam.near_z);
    float2 jit_px = jitter_for_frame(s, R.frame);
    float2 jit_ndc = {jit_px.x * 2.0f / R.w, jit_px.y * 2.0f / R.h};
    float4x4 proj = proj_nj;
    proj.m[0][2] = -jit_ndc.x;  // clip.w = -z_view, so this adds jit_ndc after the divide
    proj.m[1][2] = -jit_ndc.y;
    float4x4 vp_nj = proj_nj * view;
    fc.view = view;
    fc.proj = proj;
    fc.view_proj = proj * view;
    fc.inv_view_proj = inverse(fc.view_proj);
    fc.view_proj_nojitter = vp_nj;
    fc.prev_view_proj_nojitter = R.have_prev ? R.prev_view_proj_nojitter : vp_nj;
    fc.inv_proj = inverse(proj);
    fc.camera_pos = {cam.pos.x, cam.pos.y, cam.pos.z, cam.near_z};
    fc.screen_size = {(float)R.w, (float)R.h, 1.0f / R.w, 1.0f / R.h};
    float2 prev_jit = {fc.jitter.x, fc.jitter.y};
    fc.jitter = {jit_ndc.x, jit_ndc.y, prev_jit.x, prev_jit.y};

    // Sun: direction from the scene file, or from the UI's elevation/azimuth. The file's intensity is
    // the illuminance at the top of the atmosphere; the colour at the camera comes from the
    // atmosphere's transmittance (r_sky.cpp), so a low sun turns orange by itself. The ambient is
    // the sky's irradiance, computed on the GPU from the sky LUTs (pass_sky_luts).
    float3 sun_dir = normalize(float3{-0.3f, -0.6f, -0.5f});
    float sun_lux = 8.0f;
    for (const Light& l : scene.lights)
        if (l.type == LIGHT_DIRECTIONAL) { sun_dir = l.direction; sun_lux = l.intensity; break; }
    if (!s.sun_from_scene) {
        float el = radians(s.sun_elevation), az = radians(s.sun_azimuth);
        sun_dir = -float3{cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az)};
    }
    float sun_toa = sun_lux * s.sun_intensity;
    float3 sun = sky_sun_illuminance(sun_dir, cam.pos.y, sun_toa);
    fc.sun_dir = {sun_dir.x, sun_dir.y, sun_dir.z, 0};
    fc.sun_color = {sun.x, sun.y, sun.z, sun_toa};
    fc.time = time;
    fc.prev_time = R.have_prev ? R.prev_time : time;
    fc.frame_index = R.frame;
    fc.msaa_samples = R.samples;
    fc.debug_view = s.view;
    fc.wind_strength = s.wind ? 1.0f : 0.0f;
    fc.lod_dither_enabled = s.lod_dither ? 1.0f : 0.0f;
    fc.delta_time = dt;
    fc.auto_exposure = s.auto_exposure ? 1.0f : 0.0f;
    fc.texture_unjitter = s.texture_unjitter ? 1.0f : 0.0f;
    // Manual: EV100 -> exposure = 1 / (1.2 * 2^EV100) (Lagarde & de Rousiers 2014). Auto: compensation.
    fc.exposure = s.auto_exposure ? powf(2.0f, s.exposure_ev) : 1.0f / (1.2f * powf(2.0f, s.exposure_ev));
    update_cb(R.cb_frame, fc);

    // Sky LUTs first: the GI probe relight below lights the probes' sky texels with them.
    bind_frame_resources(scene);
    {
        GPU_SCOPE("Sky LUTs");
        pass_sky_luts();
    }

    // GI probe bake / relight when needed (uses the draw buffer before this frame's lists).
    gi_update(scene);

    // --- CPU work: culling, LOD selection, sorting, batching -------------------------------
    build_draw_lists(scene, cam);
    shadows_setup(scene, cam);
    lights_setup(scene, cam);
    vfx_setup(scene, cam, time);
    upload_draw_data();
    bind_frame_resources(scene);

    // --- GPU passes ------------------------------------------------------------------------
    {
        GPU_SCOPE("Shadows");
        pass_shadows(scene);
    }
    {
        GPU_SCOPE("Clears");
        pass_clear();
    }
    {
        GPU_SCOPE("Depth pre-pass");
        pass_prepass(scene);
    }
    {
        GPU_SCOPE("Base pass");
        pass_base(scene);
    }
    {
        GPU_SCOPE("Decals");
        pass_decals(scene);
    }
    {
        GPU_SCOPE("MSAA edge stencil");
        pass_msaa_edges();
    }
    {
        GPU_SCOPE("GTAO");
        pass_gtao();
    }
    {
        GPU_SCOPE("Sun + ambient");
        pass_sun(scene);
    }
    {
        GPU_SCOPE("Local lights");
        pass_local_lights(scene);
    }
    {
        GPU_SCOPE("Subsurface scattering");
        pass_sss();
    }
    {
        GPU_SCOPE("Compose");
        pass_compose();
    }
    {
        GPU_SCOPE("Sky");
        pass_sky();
    }
    {
        GPU_SCOPE("Transparents + VFX");
        pass_transparent(scene);
    }
    {
        GPU_SCOPE("Exposure");
        pass_exposure();
    }
    {
        GPU_SCOPE("Bloom");
        pass_bloom();
    }
    {
        GPU_SCOPE("Tone LUT bake");
        pass_tonemap_lut();
    }
    {
        GPU_SCOPE("Tone map");
        pass_tonemap();
    }
    {
        GPU_SCOPE("Anti-aliasing");
        pass_antialiasing();
    }
    {
        GPU_SCOPE("Motion blur");
        pass_motion_blur(cam, dt);
    }
    if (s.view == VIEW_QUAD_OVERDRAW) {
        GPU_SCOPE("Quad overdraw (debug)");
        pass_quad_overdraw(scene);
    }
    if (s.view != VIEW_FINAL) {
        GPU_SCOPE("Debug view");
        pass_debug_view();
    }

    R.prev_view_proj_nojitter = vp_nj;
    R.prev_time = time;
    R.have_prev = true;
    R.frame++;
}
