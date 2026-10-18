// The renderer: one struct that owns every render target and pipeline object, and a set of plain
// functions, one per pass, called in order by renderer_frame(). The pass functions are split across
// r_*.cpp files by pipeline stage (geometry, shadows, lighting, post).
#pragma once
#include "gpu.h"
#include "vmath.h"
#include "scene.h"
#include <vector>

// ---------------------------------------------------------------------------------------------
// Settings: everything the UI and the benchmark can toggle. Most technique choices have an "A/B"
// alternative so their cost can be measured against what they replace.

enum PrepassMode : int {
    PREPASS_NONE = 0,
    PREPASS_FULL_OPAQUE,         // every non-alpha-tested object
    PREPASS_FULL_WITH_ALPHA,     // everything, including alpha-tested (common in Forward+ engines)
    PREPASS_PARTIAL,             // TI: only large/close non-alpha-tested occluders (default)
};

enum BasePassOrder : int {
    ORDER_BY_MATERIAL = 0,       // naive: grouped by material only
    ORDER_FRONT_TO_BACK,         // classic: opaque front-to-back, then alpha-tested
    ORDER_TI,                    // TI: close alpha-tested, other opaque, far alpha-tested, pre-passed (depth EQUAL)
};

enum ClearMode : int {
    CLEAR_MINIMAL = 0,           // TI: only depth/stencil + buffers that are not fully overwritten
    CLEAR_ALL_HARDWARE,          // clear every G-buffer target with ClearRenderTargetView
    CLEAR_ALL_FULLSCREEN,        // clear every target by drawing a full-screen triangle (anti-pattern)
};

enum LightCullMode : int {
    LIGHTS_STENCIL_VOLUME = 0,   // TI/Crysis 3/Days Gone: stencil-marked volume + full-screen triangle per shading model
    LIGHTS_SCISSOR_ONLY,         // full-screen triangle per light clipped to the light's screen rect, no stencil
    LIGHTS_FULLSCREEN,           // no culling at all
    LIGHTS_VOLUME_MESH,          // Fox Engine: stencil-marked, then lit by drawing the volume mesh itself
};

// Local (spot/point) light shadow sizing, see docs/techniques/shadows.md.
enum LocalShadowMode : int {
    LOCAL_SHADOWS_FITTED = 0,    // TI: frustum fitted to what the camera sees, resolution from the 1080p+8% rule
    LOCAL_SHADOWS_FIXED,         // brute force: full cone / all 6 cube faces at a fixed resolution
};

// Where local shadow views clip their casters. Depth clip is off (pancaking), so without a clip
// distance everything beyond the far plane is clamped to depth 0 and rasterized for nothing.
enum LocalShadowClip : int {
    LOCAL_CLIP_NONE = 0,
    LOCAL_CLIP_FAR_PLANE,        // the view's far plane: B's depth range rounded up to a quarter octave
    LOCAL_CLIP_RECEIVERS,        // B's exact maximum depth: nothing deeper can shadow a visible receiver (default)
};

enum ShadowFilter : int {
    PCF_TENT_4TAP = 0,           // 3x3 tent from 4 bilinear comparison taps (Castano, The Witness)
    PCF_GAUSS_9TAP,              // 5x5 near-Gaussian from 9 bilinear taps
};

enum Tonemapper : int { TM_GT7 = 0, TM_GT, TM_FROSTBITE, TM_ACES, TM_REINHARD, TM_LINEAR, TM_AGX, TM_COUNT };  // shaders/tonemap_ops.hlsli

enum AAMode : int {
    AA_NONE = 0,
    AA_SMAA_1X,
    AA_SMAA_S2X,                 // SMAA per MSAA sample layer
    AA_SMAA_T2X,                 // SMAA 1x + 2-frame temporal
    AA_SMAA_4X,                  // S2x + T2x (TI default)
    AA_MSAA_RESOLVE,             // plain box resolve of the MSAA samples, for reference
};

enum MotionBlurMode : int {
    MB_TRAILING = 0,             // TI: trail only behind the motion, sharp leading edge, depth-aware
    MB_CENTERED,                 // classic per-pixel blur centred on the pixel (camera-footage look)
};

enum AOMode : int {
    AO_OFF = 0,
    AO_GTAO_FULL,                // GTAO at full resolution (default)
    AO_GTAO_HALF,                // GTAO at half resolution + depth-aware upsample
};

enum DebugView : int {
    VIEW_FINAL = 0,
    VIEW_ALBEDO,
    VIEW_NORMALS,
    VIEW_ROUGHNESS,
    VIEW_PROFILE_BLEND,
    VIEW_MOTION,
    VIEW_DEPTH,
    VIEW_STENCIL_SM,
    VIEW_MSAA_EDGES,
    VIEW_DIFFUSE_LIGHT,
    VIEW_SPECULAR_LIGHT,
    VIEW_SHADOW_CASCADES,
    VIEW_QUAD_OVERDRAW,
    VIEW_PREPASS_DEPTH,
    VIEW_GI_IRRADIANCE,          // probe-grid irradiance alone (no AO, no albedo)
    VIEW_AO,                     // GTAO visibility
    VIEW_SHADOW_DENSITY,         // shadow texels per 1080p-reference pixel vs the 1080p+8% target
    VIEW_COUNT,
};

struct Settings {
    int render_w = 1920, render_h = 1080;
    int msaa = 2;
    bool depth_24bit = false;           // A/B: D24S8 instead of TI's 32-bit float depth (reverse-Z needs float to pay off)
    bool vsync = false;

    // geometry
    PrepassMode prepass = PREPASS_PARTIAL;
    float prepass_min_size = 0.12f;   // partial pre-pass: min projected size (fraction of screen height)
    BasePassOrder order = ORDER_TI;
    float close_alpha_distance = 12.0f; // ORDER_TI: alpha-tested objects closer than this go first
    ClearMode clear_mode = CLEAR_MINIMAL;
    bool lod_enabled = true;
    bool lod_dither = true;
    float lod_bias = 1.0f;
    bool frustum_culling = true;
    bool wind = true;
    bool animate = true;

    // lighting
    LightCullMode light_cull = LIGHTS_STENCIL_VOLUME;
    bool light_scissor = true;          // also scissor the full-screen light triangles
    bool light_uber_shader = false;     // A/B: one branching draw per light instead of one per shading model
    bool light_separate_unmark = false; // A/B: lighting draws only test stencil; a separate draw clears the mark
    bool msaa_edge_detect = true;       // per-sample shading only on complex pixels
    float sun_intensity = 1.0f;
    float light_intensity = 1.0f;
    bool sss = true;                    // screen-space subsurface scattering on skin
    float sss_scale = 1.0f;             // 1 = physically sized d'Eon skin profile
    float sun_elevation = 12.0f, sun_azimuth = 230.0f;  // degrees; overrides the file's sun when edited
    bool sun_from_scene = true;

    // shadows
    bool shadows = true;
    int cascade_count = 4;
    int cascade_size = 2048;
    float shadow_distance = 120.0f;
    bool shadow_d16 = true;             // D16 (TI default) vs D32F shadow depth, cascades + atlas
    ShadowFilter shadow_filter = PCF_TENT_4TAP;
    bool cascade_slice_culling = true;  // cull casters against the camera slice extruded toward the sun
    bool cascade_amortize = false;      // KCD2: cascades 0-1 every frame, the rest round-robin
    bool shadow_foliage_cards = false;  // distant alpha-tested casters cast as solid cards (no alpha test)
    float shadow_foliage_distance = 20.0f;
    bool local_shadows = true;
    LocalShadowMode local_shadow_mode = LOCAL_SHADOWS_FITTED;
    float local_shadow_density = 1.08f; // shadow texels per 1080p-reference pixel ("1080p + 8%")
    int local_shadow_max_res = 2048;    // per projector cap
    int local_shadow_fixed_res = 1024;  // LOCAL_SHADOWS_FIXED resolution
    int shadow_atlas_size = 8192;
    LocalShadowClip local_shadow_clip = LOCAL_CLIP_RECEIVERS;  // clip casters beyond the receivers (depth clip is off for pancaking)
    float local_shadow_fade_px = 96.0f; // lights whose whole view would get fewer texels fade to unshadowed (R3 4.9); 0 = never
    bool local_shadow_cache = false;    // keep the tiles of unchanged views without moving casters between frames
    int local_shadow_dynamic_interval = 1; // with the cache: views with moving casters re-render every N frames

    // global illumination (baked irradiance probe grid) and ambient occlusion
    bool gi = true;                     // off: the sky ambient cube alone
    float gi_probe_spacing = 1.5f;      // meters; applied on the next bake
    int gi_max_probes = 16384;          // budget: the spacing grows until the grid fits
    int gi_bounces = 3;
    bool gi_relight_on_sun_change = true;
    bool gi_relight_every_frame = false;    // cost test: TI-style per-frame relighting of the sensors
    float gi_normal_bias = 0.25f;       // lookup offsets, fractions of the probe spacing
    float gi_view_bias = 0.15f;
    bool gi_visibility = true;          // per-probe visibility test against light leaks (DDGI-like)
    AOMode ao = AO_GTAO_FULL;
    float ao_radius = 0.75f;            // meters
    int ao_slices = 2, ao_steps = 4;    // directions per pixel, samples per side
    bool ao_denoise = true;

    // post
    Tonemapper tonemapper = TM_GT7;
    bool tonemap_lut = true;            // bake the curve into a 64^3 LUT; off = evaluate it per sample (cost A/B)
    bool auto_exposure = true;
    float exposure_ev = 0.0f;           // manual EV100 when auto exposure is off; compensation otherwise
    bool auto_white_balance = true;
    float awb_strength = 0.6f;          // partial correction: a low sun stays warm
    float awb_min_kelvin = 3000.0f, awb_max_kelvin = 10000.0f;  // lights outside are only corrected to the limit
    float wb_warmth = 0.0f;             // artist bias in mireds, + = warmer picture (works with AWB off too)
    AAMode aa = AA_SMAA_4X;
    bool texture_unjitter = true;       // sample material textures at the unjittered pixel centre
    bool motion_blur = true;
    MotionBlurMode mb_mode = MB_TRAILING;
    bool mb_separable = true;           // second, stride-filling pass (8 x 8 = 64 virtual taps)
    bool mb_depth_limited = true;       // camera blur only up close (+ far under fast rotation)
    float mb_trail_ms = 8.0f;           // exposure time of the trail (frame-rate independent)
    float mb_near_start = 1.5f, mb_near_end = 3.0f;  // meters: full camera blur below, none beyond
    float mb_trail_falloff = 0.5f;      // 0 = box shutter, 1 = comet (clear point of origin)
    bool bloom = true;
    float bloom_intensity = 0.04f;      // lerp weight of the bloom in the tone-map pass

    bool decals = true;                 // G-buffer decals (albedo), no decal buffer

    // transparents / VFX
    bool particles = true;
    bool vfx_half_res = true;           // TI / Phantom Pain: half-res particles with min/max depth
    bool hijack_clear = true;           // the half-res depth downsample also zeroes the particle buffer

    // display
    float brightness = 0.0f;            // calibration curve, black and white fixed
    float chromatic_aberration = 1.5f;  // pixels at the corners
    float film_grain = 0.6f;
    float vignette = 0.25f;

    DebugView view = VIEW_FINAL;
    bool pipeline_stats = false;
};

struct Camera {
    float3 pos = {0, 1.7f, 5};
    float yaw = 0, pitch = 0;   // radians; yaw 0 looks down -Z
    float fov_y = 1.0f;         // ~57 degrees
    float near_z = 0.1f;
    float3 forward() const { return {cosf(pitch) * -sinf(yaw), sinf(pitch), cosf(pitch) * -cosf(yaw)}; }
    void look_at(float3 target) {
        float3 d = normalize(target - pos);
        pitch = asinf(clampf(d.y, -1, 1));
        yaw = atan2f(-d.x, -d.z);
    }
};

// CPU-side view of one draw: one primitive of one instance at one LOD.
struct DrawItem {
    const Primitive* prim;
    int lod;
    int material;
    int instance;
    float distance;
    float fade;       // LOD cross-fade threshold (0 = fully visible)
    uint32_t flags;   // DRAW_* below
};

enum DrawFlags : uint32_t {
    DRAW_WIND = 1,
    DRAW_FADE_IN = 2,    // dithered: visible where dither < fade
    DRAW_FADE_OUT = 4,   // dithered: visible where dither >= fade
};

// A contiguous run of DrawData entries drawn with one DrawIndexedInstanced call.
struct DrawBatch {
    const Primitive* prim;
    int lod;
    int material;
    uint32_t first_draw;   // index into the frame's DrawData buffer
    uint32_t count;
    uint32_t flags;        // union of item flags (selects shader permutation)
};

struct DrawList {
    std::vector<DrawItem> items;
    std::vector<DrawBatch> batches;
    void clear() { items.clear(); batches.clear(); }
};

struct FrameStats {
    int draws_prepass = 0, draws_base = 0, draws_shadow = 0, draws_lights = 0;
    int batches_prepass = 0, batches_base = 0, batches_shadow = 0;
    uint64_t tris_base = 0, tris_shadow = 0;
    int lights_visible = 0;
    int local_shadow_views = 0, draws_local_shadow = 0;
    int local_shadow_cached = 0;        // views sampled from an earlier frame's tile (Settings::local_shadow_cache)
    uint64_t local_shadow_texels = 0;   // atlas texels rendered this frame
    double local_shadow_cpu_ms = 0;     // fitting, packing and caster lists
    double cpu_ms = 0;
};

struct Renderer;  // defined in renderer.cpp's private header (renderer_internal.h)

void renderer_init(const Settings& s);
void renderer_shutdown();
void renderer_frame(Scene& scene, const Camera& cam, const Settings& s, float time, float dt);
ID3D11RenderTargetView* renderer_ui_target();  // cleared UI target at window resolution (ImGui renders here)
void renderer_final_to_backbuffer(bool ui);      // final pass: scene + camera effects + calibration + UI
const FrameStats& renderer_stats();
void renderer_on_scene_loaded(Scene& scene);

// GI probe grid status (r_gi.cpp), shown in the UI.
struct GIInfo {
    int nx = 0, ny = 0, nz = 0, probes = 0, invalid_probes = 0;
    float spacing = 0;
    double capture_ms = 0, relight_ms = 0;   // wall time of the last full bake (GPU-synchronized)
    double shadow_ms = 0, bounces_ms = 0;    // the relight's two halves
    double memory_mb = 0;                    // persistent: sensors + grids + sun shadow map
    double capture_memory_mb = 0;            // transient, during the capture only
    int capture_draws = 0;                   // instanced draws of the last capture
    int capture_waits = 0;                   // GPU waits the capture was split into (pacing)
    double capture_triangles = 0;            // triangles submitted by the last capture (all probes)
    bool baked = false;
};
const GIInfo& renderer_gi_info();
void renderer_request_gi_bake();   // re-capture the probes (e.g. after changing the spacing)
