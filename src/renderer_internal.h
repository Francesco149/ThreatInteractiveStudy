// Private renderer state shared by renderer.cpp and the r_*.cpp pass files.
#pragma once
#include "renderer.h"

// Must match FrameCB in shaders/common.hlsli.
struct FrameConstants {
    float4x4 view;
    float4x4 proj;
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4x4 view_proj_nojitter;
    float4x4 prev_view_proj_nojitter;
    float4x4 inv_proj;
    float4 camera_pos;
    float4 screen_size;
    float4 jitter;
    float4 sun_dir;
    float4 sun_color;        // rgb: illuminance at the camera, w: illuminance at the top of the atmosphere
    float time;
    float prev_time;
    float exposure;
    uint32_t frame_index;
    uint32_t msaa_samples;
    uint32_t debug_view;
    float wind_strength;
    float lod_dither_enabled;
    float delta_time;
    float auto_exposure;
    float texture_unjitter;
    float frame_pad;
};

// Must match DrawData in shaders/common.hlsli.
struct GpuDrawData {
    float4 world[3];
    float4 prev[3];
    uint32_t material;
    uint32_t flags;
    float lod_fade;
    float wind_bend;
};

struct ShadowCascade {
    float4x4 view_proj;
    float split_near, split_far;
    float texel_world;   // world-space size of one shadow texel (for normal offset bias)
    Frustum frustum;
};

struct LocalLightDraw {
    int light;
    float4x4 volume_world;   // transforms the unit light volume mesh
    D3D11_RECT scissor;
    bool camera_inside;
};

struct Renderer {
    Settings cfg;            // settings the targets were created for
    int w = 0, h = 0, samples = 1;
    uint32_t frame = 0;

    // --- render targets -----------------------------------------------------------------
    Texture depth;           // D32_FLOAT_S8X24, MSAA
    Texture gb_albedo;       // RGBA8_SRGB: albedo, a = roughness
    Texture gb_normal;       // RGB10A2: rg = octahedral normal, b = specular, a = flags
    Texture gb_material;     // RGBA8: r = profile pair, g = profile blend, b = AO, a = unused
    Texture gb_motion;       // RG16F: screen-space motion (uv delta, current - previous)
    Texture gb_emissive;     // R11G11B10F: emissive radiance (cleared: only emissive materials write it)
    Texture light_diffuse;   // RGBA16F MSAA: diffuse lighting without albedo (also auto-exposure input)
    Texture light_specular;  // RGBA16F MSAA: specular lighting; becomes the scene color after composition
    Texture velocity;        // RG16F non-MSAA: packed velocity for TAA / motion blur
    Texture ldr[4];          // RGBA8 typeless: tone-mapped colour per MSAA sample (UNORM + SRGB views)
    ID3D11ShaderResourceView* ldr_srgb_srv[4] = {};
    ID3D11RenderTargetView* ldr_srgb_rtv[4] = {};
    Texture smaa_edges, smaa_weights;
    Texture smaa_out[2];     // ping-pong: this frame's SMAA result and last frame's (T2x blends exactly these two)
    ID3D11ShaderResourceView* smaa_out_srgb_srv[2] = {};
    ID3D11RenderTargetView* smaa_out_srgb_rtv[2] = {};
    Texture aa_out;          // after temporal resolve (RGBA8 SRGB)
    Texture post_out;        // after motion blur (RGBA8 SRGB)
    Texture overdraw;        // R32_UINT UAV: quad overdraw counter
    Texture smaa_area, smaa_search;
    Texture shadow_cascades; // D16 or D32F array (Settings::shadow_d16)
    ID3D11Buffer* histogram = nullptr;
    ID3D11UnorderedAccessView* histogram_uav = nullptr;

    // --- geometry ------------------------------------------------------------------------
    ID3D11Buffer* draw_buffer = nullptr;       // StructuredBuffer<DrawData>, rewritten every frame
    ID3D11ShaderResourceView* draw_srv = nullptr;
    ID3D11Buffer* draw_id_vb = nullptr;        // uint 0..N, per-instance stream giving each instance its DrawData index
    uint32_t draw_capacity = 1 << 18;
    std::vector<GpuDrawData> draw_data;        // CPU staging for this frame
    ID3D11Buffer* sphere_vb = nullptr; ID3D11Buffer* sphere_ib = nullptr; uint32_t sphere_index_count = 0;
    ID3D11Buffer* cone_vb = nullptr; ID3D11Buffer* cone_ib = nullptr; uint32_t cone_index_count = 0;

    DrawList list_prepass, list_base[4], list_shadow[4];
    bool shading_model_used[SM_COUNT] = {};
    std::vector<LocalLightDraw> local_lights;
    ShadowCascade cascades[4];

    // --- constant buffers ------------------------------------------------------------------
    ID3D11Buffer* cb_frame = nullptr;
    ID3D11Buffer* cb_pass = nullptr;
    ID3D11Buffer* cb_light = nullptr;
    ID3D11Buffer* cb_shadow = nullptr;
    FrameConstants frame_constants;
    float4x4 prev_view_proj_nojitter = identity();
    bool have_prev = false;
    float prev_time = 0;

    // --- states ----------------------------------------------------------------------------
    ID3D11InputLayout* layout_full = nullptr;
    ID3D11InputLayout* layout_position = nullptr;
    ID3D11InputLayout* layout_volume = nullptr;
    ID3D11RasterizerState* rs_back = nullptr;      // cull back faces
    ID3D11RasterizerState* rs_none = nullptr;      // two-sided
    ID3D11RasterizerState* rs_volume = nullptr;    // two-sided, no depth clip, scissor
    ID3D11RasterizerState* rs_scissor = nullptr;   // full-screen draws with scissor
    ID3D11RasterizerState* rs_shadow = nullptr;    // slope-scaled depth bias
    ID3D11RasterizerState* rs_shadow_none = nullptr;
    ID3D11BlendState* bs_opaque = nullptr;
    ID3D11BlendState* bs_gbuffer = nullptr;            // emissive target masked off
    ID3D11BlendState* bs_gbuffer_emissive = nullptr;
    ID3D11BlendState* bs_additive = nullptr;
    ID3D11BlendState* bs_no_color = nullptr;
    ID3D11BlendState* bs_blend_factor = nullptr;       // SMAA S2x: second sample averaged in
    ID3D11BlendState* bs_alpha = nullptr;              // premultiplied alpha
    ID3D11DepthStencilState* ds_prepass = nullptr;
    ID3D11DepthStencilState* ds_base = nullptr;        // GREATER, write, stencil REPLACE shading model
    ID3D11DepthStencilState* ds_base_equal = nullptr;  // EQUAL, no write, stencil REPLACE
    ID3D11DepthStencilState* ds_edge_mark = nullptr;
    ID3D11DepthStencilState* ds_light_volume = nullptr;
    ID3D11DepthStencilState* ds_shadow = nullptr;
    ID3D11DepthStencilState* ds_none = nullptr;
    ID3D11DepthStencilState* ds_smaa_mark = nullptr;
    ID3D11DepthStencilState* ds_smaa_test = nullptr;
    ID3D11DepthStencilState* ds_transparent = nullptr;
    ID3D11SamplerState* samplers[5] = {};

    // --- shaders -----------------------------------------------------------------------------
    Shader *vs_gbuffer, *vs_depth, *vs_depth_alpha, *vs_shadow, *vs_shadow_alpha, *vs_volume, *vs_fullscreen;
    Shader *ps_gbuffer[4];         // [alpha_test | dither<<1]
    Shader *ps_depth_alpha, *ps_shadow_alpha, *ps_depth_dither, *ps_overdraw, *ps_overdraw_alpha;
    Shader *ps_edges;
    Shader *ps_sun[SM_COUNT][2];   // [shading model][per-sample]
    Shader *ps_light[SM_COUNT][2];
    Shader *ps_light_uber[2];
    Shader *ps_compose[2];
    Shader *ps_sky;
    Shader *ps_clear_fs;           // full-screen "clear" anti-pattern for the A/B test
    Shader *vs_smaa_edge, *ps_smaa_edge, *vs_smaa_weight, *ps_smaa_weight, *vs_smaa_blend, *ps_smaa_blend;

    FrameStats stats;
    ID3D11ShaderResourceView* final_srv = nullptr;  // what the final output pass presents
};

extern Renderer R;

// Stencil bits (see shaders/common.hlsli).
static const UINT STENCIL_SM_BITS = 0x07;
static const UINT STENCIL_EDGE_BIT = 0x08;
static const UINT STENCIL_LIGHT_BIT = 0x80;

// Must match ShadowCB in shaders/shadow.hlsli.
struct ShadowConstants {
    float4x4 cascade_view_proj[4];
    float4 cascade_splits;      // far distance of each cascade (view-space distance)
    float4 cascade_texel;       // world size of a texel per cascade (normal offset)
    float4 shadow_params;       // x = cascade count, y = map size, z = 1/size, w = enabled
    float4 shadow_filter;       // x = PCF kernel, y = atlas size, z = 1/atlas size, w = local projector count
};

// Pipeline stages (r_*.cpp)
void geometry_init();
void build_draw_lists(Scene& scene, const Camera& cam);
void upload_draw_data();
void finalize_shadow_list(Scene& scene, DrawList& list);
int select_lod(const Primitive& p, float size, float bias, float* fade);  // main-view LOD + dither fade
void draw_shadow_list(Scene& scene, const DrawList& list, bool alpha_test = true);
void pass_clear();
void pass_prepass(Scene& scene);
void pass_base(Scene& scene);
void pass_msaa_edges();
void pass_quad_overdraw(Scene& scene);

void shadows_init();
void shadows_setup(Scene& scene, const Camera& cam);
void pass_shadows(Scene& scene);
// Foliage A/B (TI: "the last cascades ... alpha tested content" are the performance killers):
// alpha-tested casters farther than shadow_foliage_distance from the camera are drawn as solid
// cards, depth-only with no pixel shader, in every shadow view. Their leaf gaps are sub-pixel
// in the shadow at that distance anyway; a crown then casts a blob.
inline bool shadow_solid_card(const Settings& s, const Material& m, float camera_distance) {
    return s.shadow_foliage_cards && m.alpha_mode == ALPHA_MASK && camera_distance > s.shadow_foliage_distance;
}

// Local light shadows (r_local_shadows.cpp), driven by the three functions above.
void local_shadows_init();
void local_shadows_setup(Scene& scene, const Camera& cam, ShadowConstants& sc);  // also fills sc.shadow_filter
void pass_local_shadows(Scene& scene);
float4 local_shadow_constants(int light);   // LightCB.shadow for a light (x < 0: unshadowed)
void local_shadows_bind(bool bind);          // atlas + projector buffer on PS t14/t15

void lighting_init();
void lights_setup(Scene& scene, const Camera& cam);
void pass_sun(Scene& scene);
void pass_local_lights(Scene& scene);
void pass_compose();
void pass_sky();

// Sky and atmosphere (r_sky.cpp)
void sky_init();
float3 sky_sun_illuminance(float3 sun_dir, float camera_y, float toa_illuminance);  // atmosphere transmittance applied
void pass_sky_luts();   // re-bakes the sky LUTs when the sun moves, binds them (t20-t22) for the frame; runs before gi_update

// Bloom and the tone-curve LUT (r_bloom.cpp, r_tonemap_lut.cpp), both read by pass_tonemap
void bloom_init();
void pass_bloom();
ID3D11ShaderResourceView* bloom_srv();   // null when bloom is off
int bloom_level_count();
void tonemap_lut_init();
void pass_tonemap_lut();                 // re-bakes only when the tone mapper changes
ID3D11ShaderResourceView* tonemap_lut_srv();

void gi_init();
void gi_on_scene_loaded(Scene& scene);
void gi_update(Scene& scene);   // bakes / relights the probe grid when needed, binds it (b4, t24-t26)
void gtao_init();
void pass_gtao();               // binds the AO result at t25

void post_init();
void pass_velocity();
void pass_exposure();
void pass_tonemap();
void pass_antialiasing();
void pass_debug_view();

// Depth-stencil state for full-screen draws: no depth test, stencil EQUAL under `read_mask`;
// `clear_light` also zeroes the light-volume bit of the samples that pass. Cached by key.
ID3D11DepthStencilState* ds_stencil_test(UINT8 read_mask, bool clear_light);
void decals_init();
void pass_decals(Scene& scene);
void vfx_init();
void vfx_setup(Scene& scene, const Camera& cam, float time);
void pass_transparent(Scene& scene);
void sss_init();
void pass_sss();
void motion_blur_init();
void pass_motion_blur(const Camera& cam, float dt);
void bind_frame_resources(Scene& scene);
void draw_fullscreen();
