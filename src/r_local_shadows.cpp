// Local light shadows: "density synced perspective shadow maps" (Threat Interactive, "How To
// Optimize Almost Every Step In Modern Game Rendering", 39:00-41:20). See docs/techniques/shadows.md
// and docs/research/03-shadows.md section 4 for the derivations.
//
// A shadowed spot light is one shadow view ("projector"); a point light is six, one per cube face.
// A projector keeps its light's fixed orientation; only the off-center window of its perspective
// projection changes from frame to frame:
//   1. B = the parts of the visible receivers (instance bounds) that lie in the projector pyramid
//      and in the camera frustum. B empty -> nothing the camera sees can be lit through this
//      projector: skip it (a point light usually skips the faces pointing away from the view).
//   2. Window = bounding rectangle of B on the light's image plane (tan space, x/z and y/z).
//   3. Texel size from the "1080p + 8%" rule: at least 1.08 shadow texels per 1080p-reference
//      screen pixel everywhere in B. Quantized to quarter octaves with hysteresis and snapped to its
//      own fixed grid, so texels stay put while the camera moves. Tile capped at 2048 texels.
//   4. Tiles are packed into one D16 atlas with a shelf packer; one full (fast) clear per frame.
//   5. Casters are culled against the fitted pyramid: exactly the volume that can cast into view,
//      and clipped on the GPU at B's maximum depth (depth clip stays off for the near plane).
//   6. Lights so far away that their whole view would get under ~96 texels fade to unshadowed
//      (docs/research/03-shadows.md 4.9) and render nothing.
//   7. Optional cache (Settings::local_shadow_cache): a view without moving casters keeps its tile
//      while it still covers the fitted view; it is sampled in place, never copied.
// LOCAL_SHADOWS_FIXED is the brute-force baseline the video criticises: the whole cone (or all six
// faces) at a fixed resolution, regardless of what the camera sees.
#include "renderer_internal.h"
#include <algorithm>
#include <chrono>
#include <limits.h>

static const int PAD_TEXELS = 5;             // PCF reach (3 for the 9-tap kernel) + normal offset (~1.5), rounded up
static const float NEAR_MIN = 0.05f;         // closest allowed projector near plane (m)
static const float NEAR_FRACTION = 1.0f / 32; // near plane >= range / 32 (D16 precision, see near_plane())
static const float FOCUS_MIN = 1.0f;         // camera depths below this count as this (a light next to the camera doesn't demand the cap)
static const float DELTA0 = 1.0f / 4096.0f;  // base texel size (tan units); levels are DELTA0 * 2^(k/4)
static const int COARSEN_FRAMES = 30;        // hysteresis: frames a coarser level must persist before use
static const float RECEIVER_PAD = 0.1f;      // receiver bounds padding (normal offset; the bounds include the wind sway)
static const float LARGE_RECEIVER = 0.25f;   // receivers whose half-size exceeds this * range are also clipped to the view
static const float CACHE_SLACK = 0.125f;     // cache: a static view's window grows by this fraction per side (camera motion)

struct GpuProjector {  // must match ShadowProjector in shaders/shadow.hlsli
    float4x4 world_to_atlas;
    float4 pos_texel;
    float4 axis_cone;
    float4 info;
    float4 pad;
};

struct Projector {
    int light, face;          // face: 0-5 = cube face (+X -X +Y -Y +Z -Z), -1 = spot
    int gpu_index;            // slot in the projector buffer
    float3 pos, axis, right, up;
    float tan_limit;          // half-extent of the projector pyramid (tan space)
    float l, r, b, t;         // fitted window, tan space, before snapping
    float z_min, z_max;       // depth range of B along the axis
    int k;                    // texel level: delta = DELTA0 * 2^(k/4)
    float delta;              // texel size, tan units
    int x0, y0, nx, ny;       // snapped window: first texel index and size in texels
    int ax, ay;               // atlas position
    float4x4 view_proj;
    float delta_req;          // texel size the density rule asked for, before quantization (distance fade)
    float near_z, far_z;      // depth range of the projection
    float clip_z;             // casters deeper than this (along the axis) are clipped (Settings::local_shadow_clip)
    int slot;                 // light * 6 + face: index into g_levels and g_tiles
    int slice;                // atlas slice: 0 = this frame only, 1 = kept (cache)
    bool render;              // rendered this frame (false: sampled from its cached tile)
    bool dynamic;             // some caster moves (wind, animation, LOD dither fade)
    uint64_t casters;         // hash of its caster lists
};

struct LevelState {
    int k = INT_MAX;          // level used last frame
    int coarser_frames = 0;   // consecutive frames that asked for a coarser level
};

static Texture g_atlas;
static bool g_atlas_d16 = true;
static std::vector<Projector> g_proj;
static std::vector<GpuProjector> g_gpu;
static std::vector<DrawList> g_lists, g_solid;   // per projector: casters, solid-card foliage
static std::vector<float4> g_light_shadow; // per scene light: LightCB.shadow (x = first projector, y = count)
static std::vector<LevelState> g_levels;   // per scene light * 6 faces
static ID3D11Buffer* g_proj_buffer = nullptr;
static ID3D11ShaderResourceView* g_proj_srv = nullptr;
static uint32_t g_proj_capacity = 0;
static ID3D11RasterizerState* g_rs[2][2] = {};  // [d16][two-sided]
static Shader* g_vs_far_clip[2] = {};           // FAR_CLIP variants of R.vs_shadow / R.vs_shadow_alpha
static bool g_warned_overflow = false;

// Cache (Settings::local_shadow_cache): what the tile of each light / cube face holds.
struct CachedTile {
    bool valid = false;
    int k = 0, x0 = 0, y0 = 0, nx = 0, ny = 0;   // window it was rendered for
    int ax = 0, ay = 0;                          // atlas position
    float delta = 0, near_z = 0, far_z = 0;
    float4 light = {};                           // light position and range it was rendered for
    float3 axis = {};
    uint64_t casters = 0;                        // hash of its caster lists
};
static std::vector<CachedTile> g_tiles;  // per scene light * 6 faces, like g_levels
static int g_clean_row = 0;              // slice 1 rows from here down are still clear since it was last cleared
static bool g_restart = true;            // this frame clears slice 1 and renders every view kept there
static bool g_tiles_valid = false;       // slice 1 still holds the tiles g_tiles describes
static uint32_t g_cache_key = 0;         // atlas settings the tiles were rendered with

void local_shadows_init() {
    // Reversed Z: casters are pushed *away* from the light with negative bias. D16 constant bias is
    // in units of 1/65536 of the depth range; D32F constant bias is negligible, slope does the work.
    for (int d16 = 0; d16 < 2; d16++)
        for (int two = 0; two < 2; two++) {
            D3D11_RASTERIZER_DESC d = {};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = two ? D3D11_CULL_NONE : D3D11_CULL_BACK;
            d.FrontCounterClockwise = TRUE;
            d.DepthBias = d16 ? -4 : 0;
            d.SlopeScaledDepthBias = -1.5f;
            d.DepthClipEnable = FALSE;   // pancaking: casters in front of the near plane still cast
            d.ScissorEnable = TRUE;      // atlas tile
            gpu.dev->CreateRasterizerState(&d, &g_rs[d16][two]);
        }
    g_vs_far_clip[0] = load_vs("depth.hlsl", "vs_depth", {{"SHADOW", "1"}, {"FAR_CLIP", "1"}});
    g_vs_far_clip[1] = load_vs("depth.hlsl", "vs_depth_alpha", {{"SHADOW", "1"}, {"FAR_CLIP", "1"}});
}

// ---------------------------------------------------------------------------------------------
// Convex hulls (camera frustum, projector pyramid, receiver box) and their intersection

struct Hull {
    float3 c[8];          // 0-3 one face (in order around it), 4-7 the opposite face in the same order
    float4 planes[6];     // inward: dot(n, p) + d >= 0
};

static const int HULL_FACES[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 2, 6, 7}};

static float plane_dist(float4 p, float3 v) { return p.x * v.x + p.y * v.y + p.z * v.z + p.w; }

static Hull make_hull(const float3 c[8]) {
    Hull h;
    float3 center = {0, 0, 0};
    for (int i = 0; i < 8; i++) {
        h.c[i] = c[i];
        center += c[i] * 0.125f;
    }
    for (int f = 0; f < 6; f++) {
        float3 a = c[HULL_FACES[f][0]], b = c[HULL_FACES[f][1]], d = c[HULL_FACES[f][3]];
        float3 n = normalize(cross(b - a, d - a));
        float4 p = {n.x, n.y, n.z, -dot(n, a)};
        if (plane_dist(p, center) < 0) p = p * -1.0f;  // orient inward
        h.planes[f] = p;
    }
    return h;
}

// Frustum-shaped hull from an apex, an orthonormal basis and a tan-space rectangle, between two
// distances along `fwd`.
static Hull frustum_hull(float3 apex, float3 fwd, float3 right, float3 up, float tx, float ty, float near_z, float far_z) {
    float3 c[8];
    int k = 0;
    for (float z : {near_z, far_z}) {
        float3 o = apex + fwd * z, rx = right * (tx * z), uy = up * (ty * z);
        c[k++] = o - rx + uy;
        c[k++] = o + rx + uy;
        c[k++] = o + rx - uy;
        c[k++] = o - rx - uy;
    }
    return make_hull(c);
}

static Hull box_hull(const AABB& b) {
    float3 c[8] = {{b.lo.x, b.hi.y, b.lo.z}, {b.hi.x, b.hi.y, b.lo.z}, {b.hi.x, b.lo.y, b.lo.z}, {b.lo.x, b.lo.y, b.lo.z},
                   {b.lo.x, b.hi.y, b.hi.z}, {b.hi.x, b.hi.y, b.hi.z}, {b.hi.x, b.lo.y, b.hi.z}, {b.lo.x, b.lo.y, b.hi.z}};
    return make_hull(c);
}

// Sutherland-Hodgman: keep the part of a convex polygon on the inner side of a plane.
static void clip_poly(std::vector<float3>& poly, float4 plane, std::vector<float3>& tmp) {
    tmp.clear();
    size_t n = poly.size();
    for (size_t i = 0; i < n; i++) {
        float3 a = poly[i], b = poly[(i + 1) % n];
        float da = plane_dist(plane, a), db = plane_dist(plane, b);
        if (da >= -1e-5f) tmp.push_back(a);
        if ((da >= -1e-5f) != (db >= -1e-5f)) tmp.push_back(a + (b - a) * (da / (da - db)));
    }
    poly.swap(tmp);
}

// Vertices of the intersection of convex hulls: each face of each hull clipped by the planes of
// all the others. That covers every vertex of the result (vertices on a face of any hull,
// including corners of one hull inside the others). Empty result: no overlap.
static void intersect_hulls(const Hull* hulls, int count, std::vector<float3>& out) {
    static std::vector<float3> poly, tmp;
    out.clear();
    for (int h = 0; h < count; h++)
        for (int f = 0; f < 6; f++) {
            poly.clear();
            for (int i = 0; i < 4; i++) poly.push_back(hulls[h].c[HULL_FACES[f][i]]);
            for (int o = 0; o < count && !poly.empty(); o++)
                for (int p = 0; o != h && p < 6 && !poly.empty(); p++) clip_poly(poly, hulls[o].planes[p], tmp);
            out.insert(out.end(), poly.begin(), poly.end());
        }
}

static Hull camera_hull(const Camera& cam, float aspect, float near_z, float far_z) {
    float3 f = cam.forward();
    float3 r = normalize(cross(f, {0, 1, 0}));
    float3 u = cross(r, f);
    float th = tanf(cam.fov_y * 0.5f);
    return frustum_hull(cam.pos, f, r, u, th * aspect, th, near_z, far_z);
}

// ---------------------------------------------------------------------------------------------
// Projector setup

// Fixed view basis of a projector: never changes while the light doesn't move, so texels stay on
// the same grid (look_to's convention: rows are right, up, -forward).
static void projector_basis(Projector& p) {
    float3 up = fabsf(p.axis.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
    p.right = normalize(cross(p.axis, up));
    p.up = cross(p.right, p.axis);
}

static void snap_window(Projector& p) {
    p.delta = DELTA0 * exp2f(p.k / 4.0f);
    int x1 = (int)ceilf(p.r / p.delta) + PAD_TEXELS, y1 = (int)ceilf(p.t / p.delta) + PAD_TEXELS;
    p.x0 = (int)floorf(p.l / p.delta) - PAD_TEXELS;
    p.y0 = (int)floorf(p.b / p.delta) - PAD_TEXELS;
    p.nx = x1 - p.x0;
    p.ny = y1 - p.y0;
}

// Off-center reversed-Z perspective for the snapped window. Clip w = depth along the axis.
static float4x4 projector_view_proj(const Projector& p, float near_z, float far_z) {
    float l = p.x0 * p.delta, r = (p.x0 + p.nx) * p.delta, b = p.y0 * p.delta, t = (p.y0 + p.ny) * p.delta;
    float a = near_z / (far_z - near_z);
    float4x4 proj = {{{2 / (r - l), 0, (r + l) / (r - l), 0},
                      {0, 2 / (t - b), (t + b) / (t - b), 0},
                      {0, 0, a, far_z * a},
                      {0, 0, -1, 0}}};
    return proj * look_to(p.pos, p.axis, fabsf(p.axis.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0});
}

static bool hull_test_aabb(const Hull& h, const AABB& b) {
    float3 c = b.center(), e = b.extent();
    for (const float4& p : h.planes) {
        float r = e.x * fabsf(p.x) + e.y * fabsf(p.y) + e.z * fabsf(p.z);
        if (plane_dist(p, c) < -r) return false;
    }
    return true;
}

// Receivers closer to the light than this are not shadowed: they project in front of the near
// plane, to depth > 1, which the GREATER_EQUAL comparison always reads as lit (whatever texels the
// PCF taps land on). That lets the fit ignore them (a lamp's own housing, a few cm from the light,
// would otherwise make every cube face full size) and keeps D16 precise: with near = range/32 a
// 10 m lamp quantizes depth to ~5 mm at 10 m instead of ~15 mm with near = 0.1 m.
static float near_plane(const Light& l) { return fmaxf(NEAR_MIN, l.range * NEAR_FRACTION); }

// Fits the window to B and picks the texel level. Returns false if B is empty.
//
// B is gathered per receiver instead of from one box around all of them: a single box around a
// floor and a few props near the light contains the light itself, and then every direction of
// every face looks "visible". Large receivers (floors, walls) are clipped exactly against the
// camera frustum too (that cut is what makes the fitted window small: the pond example in the
// video); small receivers already passed the frustum test, their pyramid part is enough.
static bool fit_projector(Projector& p, const Light& l, const Camera& cam, Scene& scene, const std::vector<int>& receivers,
                          const Settings& s) {
    static std::vector<float3> pts;
    float aspect = (float)R.w / R.h;
    float cam_far = length(l.position - cam.pos) + l.range;
    float3 fwd = cam.forward();
    float3 pad = {RECEIVER_PAD, RECEIVER_PAD, RECEIVER_PAD};
    Hull hulls[3] = {frustum_hull(p.pos, p.axis, p.right, p.up, p.tan_limit, p.tan_limit, near_plane(l), l.range), {},
                     camera_hull(cam, aspect, cam.near_z, cam_far)};
    Hull focus = camera_hull(cam, aspect, FOCUS_MIN, fmaxf(cam_far, FOCUS_MIN * 2));
    p.l = p.b = 1e30f;
    p.r = p.t = -1e30f;
    p.z_min = 1e30f;
    p.z_max = 0;
    float ratio = 1e30f;
    for (int i : receivers) {
        AABB box = scene.instances[i].world_bounds;
        box.lo = box.lo - pad;
        box.hi = box.hi + pad;
        if (!hull_test_aabb(hulls[0], box)) continue;
        float3 e = box.extent();
        bool large = fmaxf(e.x, fmaxf(e.y, e.z)) > LARGE_RECEIVER * l.range;
        hulls[1] = box_hull(box);
        intersect_hulls(hulls, large ? 3 : 2, pts);
        if (pts.empty()) continue;
        float zc_min = 1e30f;
        for (float3 v : pts) {
            float3 d = v - p.pos;
            float z = fmaxf(dot(d, p.axis), NEAR_MIN);
            float x = dot(d, p.right) / z, y = dot(d, p.up) / z;
            p.l = fminf(p.l, x); p.r = fmaxf(p.r, x);
            p.b = fminf(p.b, y); p.t = fmaxf(p.t, y);
            p.z_min = fminf(p.z_min, z); p.z_max = fmaxf(p.z_max, z);
            float zc = dot(v - cam.pos, fwd);
            zc_min = fminf(zc_min, zc);
            ratio = fminf(ratio, fmaxf(zc, FOCUS_MIN) / z);
        }
        // Density rule (docs/research/03-shadows.md 2.4): a shadow texel of angular size delta
        // covers delta * zL metres at light depth zL; a 1080p-reference pixel covers pixel * zc
        // metres at camera depth zc, with pixel = 2 tan(fovy/2) / 1080. `density` texels per pixel
        // everywhere in B needs delta <= pixel / density * min(zc / zL). zc/zL is linear-fractional,
        // so the minimum over a convex piece is at a vertex. Camera depths are clamped to FOCUS_MIN
        // (otherwise standing near a light always demands the cap); a large receiver reaching that
        // close is also clipped at FOCUS_MIN, which adds the vertices of that cut as candidates.
        if (large && zc_min < FOCUS_MIN) {
            Hull clipped[3] = {hulls[0], hulls[1], focus};
            intersect_hulls(clipped, 3, pts);
            for (float3 v : pts) ratio = fminf(ratio, dot(v - cam.pos, fwd) / fmaxf(dot(v - p.pos, p.axis), NEAR_MIN));
        }
    }
    if (p.r < p.l) return false;
    p.l = fmaxf(p.l, -p.tan_limit); p.r = fminf(p.r, p.tan_limit);
    p.b = fmaxf(p.b, -p.tan_limit); p.t = fminf(p.t, p.tan_limit);
    p.z_max = fminf(p.z_max, l.range);
    float pixel = 2.0f * tanf(cam.fov_y * 0.5f) / 1080.0f;
    float delta_req = pixel / fmaxf(s.local_shadow_density, 0.05f) * ratio;
    p.delta_req = delta_req;
    int k_req = (int)floorf(4.0f * log2f(delta_req / DELTA0));   // finer or equal
    k_req = std::max(-40, std::min(k_req, 60));

    // Hysteresis: a finer level is taken at once; a coarser one only after it was asked for
    // COARSEN_FRAMES frames in a row, so a camera hovering at a level boundary doesn't re-grid
    // the texels back and forth (re-gridding is the only time the shadow edges can move).
    LevelState& ls = g_levels[p.slot];
    p.k = k_req;
    if (ls.k != INT_MAX && k_req > ls.k) {
        if (++ls.coarser_frames < COARSEN_FRAMES) p.k = ls.k;
    } else {
        ls.coarser_frames = 0;
    }
    snap_window(p);
    int cap = std::min(s.local_shadow_max_res, s.shadow_atlas_size);
    while (std::max(p.nx, p.ny) > cap) {   // cap: coarsen until the tile fits
        p.k++;
        snap_window(p);
    }
    return true;
}

// Brute-force baseline: the whole pyramid at a fixed resolution.
static void fixed_projector(Projector& p, int res) {
    p.delta = 2.0f * p.tan_limit / (res - 2 * PAD_TEXELS);
    p.x0 = p.y0 = -res / 2;
    p.nx = p.ny = res;
    p.k = 0;
}

// The atlas is a texture array: slice 0 holds the views rendered for this frame only and is
// cleared every frame; slice 1 exists only with the cache and holds the tiles kept between frames.
static void ensure_atlas(const Settings& s) {
    int slices = s.local_shadow_cache ? 2 : 1;
    if (g_atlas.tex && g_atlas.w == s.shadow_atlas_size && g_atlas_d16 == s.shadow_d16 && g_atlas.array_size == slices) return;
    release_texture(g_atlas);
    g_atlas_d16 = s.shadow_d16;
    g_atlas = create_texture("local_shadow_atlas", s.shadow_atlas_size, s.shadow_atlas_size,
                             s.shadow_d16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT, TEX_DEPTH, 1, slices);
    if (slices == 1) {   // the lookup reads a Texture2DArray either way
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = s.shadow_d16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        SAFE_RELEASE(g_atlas.srv);
        gpu.dev->CreateShaderResourceView(g_atlas.tex, &sd, &g_atlas.srv);
    }
    g_tiles_valid = false;
}

static ID3D11DepthStencilView* atlas_dsv(int slice) { return g_atlas.array_size > 1 ? g_atlas.slice_dsvs[slice] : g_atlas.dsv; }

// Shelf packer: tallest tiles first, filled left to right in rows ("shelves"), from atlas row
// `first_row` down. Packs the projectors that render into `slice` this frame; on success,
// g_clean_row is the first row nothing was packed into.
static bool pack_atlas(int size, int slice, int first_row) {
    std::vector<int> order;
    for (int i = 0; i < (int)g_proj.size(); i++)
        if (g_proj[i].render && g_proj[i].slice == slice) order.push_back(i);
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return g_proj[a].ny != g_proj[b].ny ? g_proj[a].ny > g_proj[b].ny : g_proj[a].nx > g_proj[b].nx;
    });
    int x = 0, y = first_row, shelf = 0;
    for (int i : order) {
        Projector& p = g_proj[i];
        if (x + p.nx > size) { y += shelf; x = 0; shelf = 0; }
        if (y + p.ny > size) return false;
        p.ax = x;
        p.ay = y;
        x += p.nx;
        shelf = std::max(shelf, p.ny);
    }
    g_clean_row = y + shelf;
    return true;
}

// Plane test that skips the near plane (index 4): casters between the light and the near plane
// still cast (pancaked by the disabled depth clip).
static bool caster_visible(const Frustum& f, const AABB& b) {
    float3 c = b.center(), e = b.extent();
    for (int i = 0; i < 6; i++) {
        if (i == 4) continue;
        const float4& p = f.planes[i];
        float r = e.x * fabsf(p.x) + e.y * fabsf(p.y) + e.z * fabsf(p.z);
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    }
    return true;
}

static uint64_t hash_mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }   // FNV-1a step

// Fills the caster lists of a projector's current view (items only; finalize_shadow_list() turns
// them into draws once the view is known to render). Also tells whether any caster moves and
// hashes the lists, which is all the cache needs to know about them.
static void build_caster_list(Scene& scene, const Camera& cam, const Settings& s, Projector& p, DrawList& list,
                              DrawList& solid) {
    list.clear();
    solid.clear();
    p.dynamic = false;
    p.casters = 14695981039346656037ull;
    Frustum fr = frustum_from_matrix(p.view_proj);
    float th = tanf(cam.fov_y * 0.5f);
    for (int i = 0; i < (int)scene.instances.size(); i++) {
        const Instance& in = scene.instances[i];
        if (in.flags & INST_NO_SHADOW) continue;
        if (!caster_visible(fr, in.world_bounds)) continue;
        float3 c = in.world_bounds.center(), e = in.world_bounds.extent();
        if (dot(c - p.pos, p.axis) - (e.x * fabsf(p.axis.x) + e.y * fabsf(p.axis.y) + e.z * fabsf(p.axis.z)) > p.clip_z)
            continue;   // all of it deeper than the clip depth
        // Casters smaller than ~1.5 texels at their distance from the light can't show up.
        float zl = fmaxf(dot(c - p.pos, p.axis), NEAR_MIN);
        if (in.radius * 2.0f < 1.5f * p.delta * zl) continue;
        // LOD and dithered cross-fade exactly as in the main view, so the shadow matches the
        // visible mesh and doesn't pop on its own (TI, 34:40).
        float d_cam = length(c - cam.pos);
        float size = in.radius / (fmaxf(d_cam, 0.01f) * th);
        uint32_t wind = (in.flags & INST_WIND) ? DRAW_WIND : 0;
        bool moves = (wind && s.wind) || in.anim != ANIM_NONE;
        for (const Primitive& prim : scene.meshes[in.mesh].prims) {
            const Material& mat = scene.materials[prim.material];
            if (mat.alpha_mode == ALPHA_BLEND) continue;
            float fade = 0;
            int lod = s.lod_enabled ? select_lod(prim, size, s.lod_bias, &fade) : 0;
            if (!s.lod_dither) fade = 0;
            float d_light = length(c - p.pos);
            bool card = shadow_solid_card(s, mat, d_cam);
            DrawList& dst = card ? solid : list;
            dst.items.push_back({&prim, lod, prim.material, i, d_light, fade, wind | (fade > 0 ? DRAW_FADE_OUT : 0u)});
            if (fade > 0) dst.items.push_back({&prim, lod - 1, prim.material, i, d_light, fade, wind | DRAW_FADE_IN});
            p.dynamic |= moves || fade > 0;   // the dither pattern changes every frame
            p.casters = hash_mix(hash_mix(hash_mix(p.casters, (uint64_t)i << 1 | (card ? 1u : 0u)), (uint64_t)(uintptr_t)&prim), (uint64_t)lod);
        }
    }
}

// Near/far: B's depth range along the axis, rounded to quarter octaves so the depth mapping only
// changes in steps. Casters in front of the near plane are pancaked.
//
// Casters are clipped at B's exact maximum depth rather than the rounded far plane: no visible
// receiver point lies deeper (its bounds include the wind sway, padded by RECEIVER_PAD), so a
// caster there can't shadow anything the camera sees. The clip leaves the depth mapping alone.
// With the cache, views clip at the far plane: a kept tile is reused while the receivers change.
static void depth_range(Projector& p, const Light& l, const Settings& s) {
    bool fitted = s.local_shadow_mode == LOCAL_SHADOWS_FITTED;
    p.near_z = fitted ? fmaxf(near_plane(l), exp2f(floorf(log2f(p.z_min) * 4) / 4)) : near_plane(l);
    p.far_z = fitted ? exp2f(ceilf(log2f(p.z_max) * 4) / 4) : l.range;
    p.far_z = fmaxf(p.far_z, p.near_z * 1.5f);
    bool tight = fitted && s.local_shadow_clip == LOCAL_CLIP_RECEIVERS && !s.local_shadow_cache;
    p.clip_z = tight ? fminf(p.z_max, p.far_z) : p.far_z;
}

// Cache: a tile can stand in for the fitted view if it was rendered on the same texel grid (same
// light, axis and texel size: texels sit at i * delta, so a larger window holds the same texels)
// with a window and a depth range that contain the fitted view's.
static bool tile_covers(const CachedTile& t, const Projector& p, const Light& l) {
    return t.valid && t.delta == p.delta && t.x0 <= p.x0 && t.y0 <= p.y0 && t.x0 + t.nx >= p.x0 + p.nx &&
           t.y0 + t.ny >= p.y0 + p.ny && t.near_z <= p.near_z && t.far_z >= p.far_z && t.light.x == l.position.x &&
           t.light.y == l.position.y && t.light.z == l.position.z && t.light.w == l.range && t.axis.x == p.axis.x &&
           t.axis.y == p.axis.y && t.axis.z == p.axis.z;
}

// Cache: a static view renders a larger window (CACHE_SLACK per side, within the pyramid and the
// size cap) and depth range (a quarter octave each way), so that it keeps covering the fitted view,
// and keeps its tile, while the camera moves a little.
static void add_slack(Projector& p, const Light& l, int cap) {
    int edge = (int)ceilf(p.tan_limit / p.delta) + PAD_TEXELS;   // texel index of the padded pyramid edge
    int sx = std::max(8, (int)(p.nx * CACHE_SLACK)), sy = std::max(8, (int)(p.ny * CACHE_SLACK));
    int x0 = std::max(p.x0 - sx, -edge), x1 = std::min(p.x0 + p.nx + sx, edge);
    int y0 = std::max(p.y0 - sy, -edge), y1 = std::min(p.y0 + p.ny + sy, edge);
    if (x1 - x0 <= cap && y1 - y0 <= cap) {
        p.x0 = x0;
        p.nx = x1 - x0;
        p.y0 = y0;
        p.ny = y1 - y0;
    }
    p.near_z = fmaxf(near_plane(l), p.near_z * 0.8408964f);   // 2^(-1/4)
    p.far_z *= 1.1892071f;
    p.clip_z = p.far_z;
}

static void prepare_view(Scene& scene, const Camera& cam, const Settings& s, Projector& p, size_t i) {
    p.view_proj = projector_view_proj(p, p.near_z, p.far_z);
    build_caster_list(scene, cam, s, p, g_lists[i], g_solid[i]);
}

// Packs the views that render into `slice` from row 0; on overflow, coarsens them one level
// (fixed mode: halves them) and retries.
static void pack_slice(Scene& scene, const Camera& cam, const Settings& s, int slice, bool fitted) {
    bool coarsened = false;
    while (!pack_atlas(s.shadow_atlas_size, slice, 0)) {
        if (!g_warned_overflow) {
            log_msg("Local shadow atlas (%d^2) overflow: coarsening all projectors", s.shadow_atlas_size);
            g_warned_overflow = true;
        }
        for (Projector& p : g_proj) {
            if (!p.render || p.slice != slice) continue;
            if (fitted) { p.k++; snap_window(p); }
            else fixed_projector(p, std::max(p.nx / 2, 64));
        }
        coarsened = true;
    }
    if (coarsened)
        for (size_t i = 0; i < g_proj.size(); i++)
            if (g_proj[i].render && g_proj[i].slice == slice) prepare_view(scene, cam, s, g_proj[i], i);
}

static void setup(Scene& scene, const Camera& cam, ShadowConstants& sc);

void local_shadows_setup(Scene& scene, const Camera& cam, ShadowConstants& sc) {
    auto t0 = std::chrono::high_resolution_clock::now();
    setup(scene, cam, sc);
    R.stats.local_shadow_cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

static void setup(Scene& scene, const Camera& cam, ShadowConstants& sc) {
    const Settings& s = R.cfg;
    g_proj.clear();
    g_gpu.clear();
    g_light_shadow.assign(scene.lights.size(), float4{-1, 0, 0, 0});
    if (g_levels.size() != scene.lights.size() * 6) g_levels.assign(scene.lights.size() * 6, LevelState{});
    sc.shadow_filter = {(float)s.shadow_filter, (float)s.shadow_atlas_size, 1.0f / s.shadow_atlas_size, 0};
    if (!s.shadows || !s.local_shadows) return;

    // Receivers: instances in the camera frustum. Each light fits its projectors to the bounds of
    // the visible receivers inside its range (no receivers there: nothing to shadow).
    Frustum view = frustum_from_matrix(R.frame_constants.view_proj_nojitter);
    static std::vector<int> visible, receivers;
    visible.clear();
    for (int i = 0; i < (int)scene.instances.size(); i++)
        if (frustum_test_aabb(view, scene.instances[i].world_bounds)) visible.push_back(i);

    bool fitted = s.local_shadow_mode == LOCAL_SHADOWS_FITTED;
    for (int li = 0; li < (int)scene.lights.size(); li++) {
        const Light& l = scene.lights[li];
        if (!l.shadow || l.type == LIGHT_DIRECTIONAL) continue;
        if (!frustum_test_sphere(view, l.position, l.range)) continue;
        receivers.clear();
        for (int i : visible) {
            const AABB& b = scene.instances[i].world_bounds;
            float3 q = max3(b.lo, min3(l.position, b.hi));   // closest point of the box to the light
            if (dot(q - l.position, q - l.position) <= l.range * l.range) receivers.push_back(i);
        }
        if (fitted && receivers.empty()) continue;

        static const float3 face_axis[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        bool point = l.type == LIGHT_POINT;
        int first = (int)g_gpu.size();
        size_t proj_begin = g_proj.size();
        float delta_min = 1e30f;   // finest texel size any face asked for
        for (int f = 0; f < (point ? 6 : 1); f++) {
            Projector p = {};
            p.light = li;
            p.face = point ? f : -1;
            p.pos = l.position;
            p.axis = point ? face_axis[f] : normalize(l.direction);
            p.tan_limit = point ? 1.0f : tanf(fminf(l.outer_cone, 1.3f));
            p.gpu_index = first + f;
            p.slot = li * 6 + (point ? f : 0);
            projector_basis(p);
            if (fitted) {
                if (!fit_projector(p, l, cam, scene, receivers, s)) continue;
                delta_min = fminf(delta_min, p.delta_req);
            } else {
                fixed_projector(p, std::min(s.local_shadow_fixed_res, s.shadow_atlas_size));
            }
            g_proj.push_back(p);
        }
        if (g_proj.size() == proj_begin) continue;

        // Distance fade (docs/research/03-shadows.md 4.9, phase 1: to unshadowed). A light whose
        // whole view (all of a cube face, or the spot's cone) would get fewer than fade_px texels at
        // the density it asks for covers so few pixels that its shadow isn't worth a map: the shadow
        // fades out between 4/3 fade_px and fade_px, below that the light renders nothing. From the
        // unquantized texel size, so the fade is continuous while the camera moves.
        float fade = 1;
        if (fitted && s.local_shadow_fade_px > 0) {
            float res = 2.0f * g_proj[proj_begin].tan_limit / delta_min;
            fade = std::min(std::max((res - s.local_shadow_fade_px) / (s.local_shadow_fade_px / 3), 0.0f), 1.0f);
            if (fade <= 0) {
                g_proj.resize(proj_begin);
                continue;
            }
        }
        g_light_shadow[li] = {(float)first, point ? 6.0f : 1.0f, fade, 0};
        g_gpu.resize(first + (point ? 6 : 1), GpuProjector{});
    }
    if (g_proj.empty()) return;
    if (g_lists.size() < g_proj.size()) {
        g_lists.resize(g_proj.size());
        g_solid.resize(g_proj.size());
    }

    // Which views render. Without the cache, all of them, into slice 0, cleared every frame. With
    // it (Settings::local_shadow_cache), a view samples an earlier frame's tile in place while
    // that tile covers the fitted view and its casters are the same and don't move. Views with
    // moving casters render every frame, or every local_shadow_dynamic_interval frames (their
    // shadows then lag the motion by up to that many frames minus one).
    ensure_atlas(s);
    if (g_tiles.size() != scene.lights.size() * 6) {
        g_tiles.assign(scene.lights.size() * 6, CachedTile{});
        g_tiles_valid = false;
    }
    bool cache = s.local_shadow_cache;
    uint32_t key = (uint32_t)s.shadow_atlas_size | (s.shadow_d16 ? 1u << 30 : 0) | (uint32_t)s.local_shadow_clip << 28;
    g_restart = cache && (!g_tiles_valid || key != g_cache_key);
    int interval = std::max(s.local_shadow_dynamic_interval, 1);
    int cap = std::min(s.local_shadow_max_res, s.shadow_atlas_size);
    for (size_t i = 0; i < g_proj.size(); i++) {
        Projector& p = g_proj[i];
        const Light& l = scene.lights[p.light];
        depth_range(p, l, s);
        p.render = true;
        p.slice = 0;
        bool known_static = false;
        const CachedTile& t = g_tiles[p.slot];
        if (cache && !g_restart && tile_covers(t, p, l)) {
            Projector c = p;
            c.x0 = t.x0;
            c.y0 = t.y0;
            c.nx = t.nx;
            c.ny = t.ny;
            c.near_z = t.near_z;
            c.far_z = c.clip_z = t.far_z;
            prepare_view(scene, cam, s, c, i);
            bool due = (R.frame + p.slot) % interval == 0;
            if (c.dynamic ? !due : c.casters == t.casters) {
                p = c;
                p.render = false;
                p.slice = 1;
                p.ax = t.ax;
                p.ay = t.ay;
                continue;
            }
            p.dynamic = c.dynamic;
            known_static = !c.dynamic;
        }
        if (!known_static) prepare_view(scene, cam, s, p, i);
        if (cache && !p.dynamic) {
            add_slack(p, l, cap);
            prepare_view(scene, cam, s, p, i);
        }
        // Kept views go to slice 1: static ones, and moving ones that wait for their next update.
        if (cache && (!p.dynamic || interval > 1)) p.slice = 1;
    }

    // Atlas space. A depth view can only be cleared whole (and TI bans quad clears and copies of
    // cached maps), so new tiles in slice 1 go into rows that are still clear since it was last
    // cleared; when those run out, slice 1 starts over: one clear, and every view kept there
    // renders again. Slice 0 is packed from scratch every frame.
    if (cache && !g_restart && !pack_atlas(s.shadow_atlas_size, 1, g_clean_row)) g_restart = true;
    if (g_restart) {
        for (Projector& p : g_proj)
            if (p.slice == 1) p.render = true;
        pack_slice(scene, cam, s, 1, fitted);
        for (CachedTile& t : g_tiles) t.valid = false;
    }
    int clean_row = g_clean_row;   // pack_atlas() below moves it for slice 0
    pack_slice(scene, cam, s, 0, fitted);
    g_clean_row = clean_row;

    float A = (float)s.shadow_atlas_size;
    for (size_t i = 0; i < g_proj.size(); i++) {
        Projector& p = g_proj[i];
        const Light& l = scene.lights[p.light];
        if (fitted) g_levels[p.slot].k = p.k;

        // world -> atlas: NDC xy -> the tile's UV rectangle (y flipped), depth and w untouched.
        float sx = p.nx / A, sy = p.ny / A, ox = p.ax / A, oy = p.ay / A;
        float4x4 to_atlas = {{{0.5f * sx, 0, 0, 0.5f * sx + ox}, {0, -0.5f * sy, 0, 0.5f * sy + oy}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
        GpuProjector& g = g_gpu[p.gpu_index];
        g.world_to_atlas = to_atlas * p.view_proj;
        g.pos_texel = {p.pos.x, p.pos.y, p.pos.z, p.delta};
        g.axis_cone = {p.axis.x, p.axis.y, p.axis.z, p.face < 0 ? cosf(fminf(l.outer_cone, 1.3f)) : 0.0f};
        g.info = {l.range, 1, (float)p.slice, 0};

        R.stats.local_shadow_views++;
        if (!p.render) {
            R.stats.local_shadow_cached++;
            continue;
        }
        finalize_shadow_list(scene, g_lists[i]);
        finalize_shadow_list(scene, g_solid[i]);
        if (p.slice == 1)
            g_tiles[p.slot] = {true, p.k, p.x0, p.y0, p.nx, p.ny, p.ax, p.ay, p.delta, p.near_z, p.far_z,
                               {l.position.x, l.position.y, l.position.z, l.range}, p.axis, p.casters};
        else
            g_tiles[p.slot].valid = false;   // slice 0 is cleared next frame
        R.stats.local_shadow_texels += (uint64_t)p.nx * p.ny;
        for (const DrawList* dl : {&g_lists[i], &g_solid[i]}) {
            R.stats.draws_local_shadow += (int)dl->items.size();
            R.stats.batches_shadow += (int)dl->batches.size();
            for (const DrawItem& d : dl->items) R.stats.tris_shadow += d.prim->lods[d.lod].index_count / 3;
        }
    }

    if (g_gpu.size() > g_proj_capacity) {
        SAFE_RELEASE(g_proj_srv);
        SAFE_RELEASE(g_proj_buffer);
        g_proj_capacity = std::max<uint32_t>(64, (uint32_t)g_gpu.size() * 2);
        g_proj_buffer = create_structured_buffer(sizeof(GpuProjector), g_proj_capacity, nullptr, false, true, &g_proj_srv, nullptr);
    }
    update_buffer(g_proj_buffer, g_gpu.data(), (uint32_t)(g_gpu.size() * sizeof(GpuProjector)));
    sc.shadow_filter.w = (float)g_gpu.size();
    g_tiles_valid = cache;
    g_cache_key = key;
}

float4 local_shadow_constants(int light) {
    if (light < 0 || light >= (int)g_light_shadow.size()) return {-1, 0, 0, 0};
    return g_light_shadow[light];
}

void local_shadows_bind(bool bind) {
    ID3D11ShaderResourceView* srvs[2] = {};
    if (bind && !g_proj.empty()) {
        srvs[0] = g_atlas.srv;
        srvs[1] = g_proj_srv;
    }
    gpu.ctx->PSSetShaderResources(14, 2, srvs);
}

struct ShadowPassCB {   // PassCB in depth.hlsl
    float4x4 view_proj;
    float4 clip;          // x: clip depth along the view axis (FAR_CLIP)
};

void pass_local_shadows(Scene& scene) {
    local_shadows_bind(false);  // the atlas is about to be a depth target
    if (g_proj.empty()) return;
    GPU_SCOPE("Local lights");
    // Full clears only: a metadata fast clear on current GPUs. Never quad-clear tiles (TI: the
    // Crysis 3 / Callisto Protocol / Dead Space Remake anti-pattern). Slice 0 every frame, the
    // cache's slice 1 only when it starts over.
    gpu.ctx->ClearDepthStencilView(atlas_dsv(0), D3D11_CLEAR_DEPTH, 0.0f, 0);
    if (g_restart) gpu.ctx->ClearDepthStencilView(atlas_dsv(1), D3D11_CLEAR_DEPTH, 0.0f, 0);
    gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_shadow, 0);
    // draw_shadow_list() picks R.rs_shadow / R.rs_shadow_none and R.vs_shadow / R.vs_shadow_alpha;
    // swap in the atlas states (perspective bias, scissor) and the clipping vertex shaders for the
    // duration of this pass.
    ID3D11RasterizerState* saved[2] = {R.rs_shadow, R.rs_shadow_none};
    Shader* saved_vs[2] = {R.vs_shadow, R.vs_shadow_alpha};
    R.rs_shadow = g_rs[g_atlas_d16][0];
    R.rs_shadow_none = g_rs[g_atlas_d16][1];
    if (R.cfg.local_shadow_clip != LOCAL_CLIP_NONE) {
        R.vs_shadow = g_vs_far_clip[0];
        R.vs_shadow_alpha = g_vs_far_clip[1];
    }
    int bound_slice = -1;
    for (size_t i = 0; i < g_proj.size(); i++) {
        const Projector& p = g_proj[i];
        if (!p.render) continue;
        if (p.slice != bound_slice) {
            gpu.ctx->OMSetRenderTargets(0, nullptr, atlas_dsv(p.slice));
            bound_slice = p.slice;
        }
        set_viewport(p.nx, p.ny, p.ax, p.ay);
        D3D11_RECT rect = {p.ax, p.ay, p.ax + p.nx, p.ay + p.ny};
        gpu.ctx->RSSetScissorRects(1, &rect);
        ShadowPassCB cb = {p.view_proj, {p.clip_z, 0, 0, 0}};
        update_cb(R.cb_pass, cb);
        draw_shadow_list(scene, g_solid[i], false);   // solid cards first: cheap, and they occlude
        draw_shadow_list(scene, g_lists[i]);
    }
    R.rs_shadow = saved[0];
    R.rs_shadow_none = saved[1];
    R.vs_shadow = saved_vs[0];
    R.vs_shadow_alpha = saved_vs[1];
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
