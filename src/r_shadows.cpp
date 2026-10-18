// Shadows: sun cascaded shadow maps (stable, texel-snapped), rendered depth-only from the
// position stream, plus the local light shadow atlas (r_local_shadows.cpp). Floors and other
// INST_NO_SHADOW objects never cast. See docs/techniques/shadows.md.
#include "renderer_internal.h"
#include <algorithm>

// What the map of a cascade was rendered for. Amortized cascades are sampled with the matrix of
// the frame that rendered them, so the lookup always matches the stored depth.
struct CascadeCache {
    bool valid = false;
    float3 center = {0, 0, 0};   // world-space sphere the map covers
    float radius = 0;
    float3 light = {0, 0, 0};
    float4x4 view_proj;
    float texel = 0;
};

static CascadeCache g_cache[4];
static DrawList g_solid[4];      // per cascade: distant foliage drawn as solid cards
static bool g_render[4];         // cascade is (re)rendered this frame
static int g_cache_key = 0;      // size/format/count the cache was made for

void shadows_init() {
    R.cb_shadow = create_constant_buffer(sizeof(ShadowConstants));
    local_shadows_init();
}

static void ensure_shadow_targets(int size, bool d16) {
    DXGI_FORMAT fmt = d16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT;
    if (R.shadow_cascades.tex && R.shadow_cascades.w == size && R.shadow_cascades.format == fmt) return;
    release_texture(R.shadow_cascades);
    R.shadow_cascades = create_texture("shadow_cascades", size, size, fmt, TEX_DEPTH, 1, 4);
}

// Cascade split distances: the "practical split scheme" (Zhang et al. 2006), a blend between
// logarithmic and uniform splits.
static void compute_splits(float near_z, float far_z, int count, float* splits) {
    const float lambda = 0.75f;
    for (int i = 1; i <= count; i++) {
        float f = (float)i / count;
        float log_split = near_z * powf(far_z / near_z, f);
        float uni_split = near_z + (far_z - near_z) * f;
        splits[i - 1] = lambda * log_split + (1 - lambda) * uni_split;
    }
}

// Caster volume of a cascade slice (Dybisz & Witanowski, "Shadows Optimizations in Cyberpunk
// 2077", SIGGRAPH 2021): the slice of the camera frustum swept toward the sun. A caster outside it
// can't throw a shadow into the slice, even if it is inside the cascade's (much larger) box.
// Its planes are the slice faces that moving toward the light can't leave (inward normal not
// facing the light direction), plus a plane parallel to the light through every silhouette edge
// (an edge between a kept and a dropped face).
struct CasterVolume {
    float4 planes[12];
    int count = 0;
};

static CasterVolume slice_caster_volume(const float3 c[8], float3 L) {
    // Corners: 0-3 near face, 4-7 far face (same order); faces as corner quads.
    static const int F[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}};
    float3 center = {0, 0, 0};
    for (int i = 0; i < 8; i++) center += c[i] * 0.125f;
    auto inward = [&](float3 n, float3 p) {
        float4 pl = {n.x, n.y, n.z, -dot(n, p)};
        if (pl.x * center.x + pl.y * center.y + pl.z * center.z + pl.w < 0) pl = pl * -1.0f;
        return pl;
    };
    CasterVolume v;
    bool keep[6];
    for (int f = 0; f < 6; f++) {
        float3 a = c[F[f][0]], b = c[F[f][1]], d = c[F[f][3]];
        float4 pl = inward(normalize(cross(b - a, d - a)), a);
        keep[f] = pl.x * L.x + pl.y * L.y + pl.z * L.z <= 0;
        if (keep[f]) v.planes[v.count++] = pl;
    }
    for (int fa = 0; fa < 6; fa++)
        for (int fb = fa + 1; fb < 6; fb++) {
            if (keep[fa] == keep[fb]) continue;
            int shared[2], n = 0;   // the two corners the faces share, if they are adjacent
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    if (F[fa][i] == F[fb][j] && n < 2) shared[n++] = F[fa][i];
            if (n != 2) continue;
            float3 e = cross(c[shared[1]] - c[shared[0]], L);
            if (length(e) < 1e-6f) continue;
            v.planes[v.count++] = inward(normalize(e), c[shared[0]]);
        }
    return v;
}

static bool volume_test_sphere(const CasterVolume& v, float3 c, float r) {
    for (int i = 0; i < v.count; i++) {
        const float4& p = v.planes[i];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    }
    return true;
}

void shadows_setup(Scene& scene, const Camera& cam) {
    const Settings& s = R.cfg;
    for (DrawList& l : R.list_shadow) l.clear();
    for (DrawList& l : g_solid) l.clear();
    ShadowConstants sc = {};
    sc.shadow_params = {(float)s.cascade_count, (float)s.cascade_size, 1.0f / s.cascade_size, s.shadows ? 1.0f : 0.0f};
    local_shadows_setup(scene, cam, sc);
    if (!s.shadows) {
        update_cb(R.cb_shadow, sc);
        return;
    }
    ensure_shadow_targets(s.cascade_size, s.shadow_d16);

    float3 L = normalize(float3{R.frame_constants.sun_dir.x, R.frame_constants.sun_dir.y, R.frame_constants.sun_dir.z});
    float3 up = fabsf(L.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
    float splits[4];
    compute_splits(cam.near_z, s.shadow_distance, s.cascade_count, splits);
    float aspect = (float)R.w / R.h;
    float th = tanf(cam.fov_y * 0.5f), tw = th * aspect;
    float3 f = cam.forward();
    float3 r = normalize(cross(f, {0, 1, 0}));
    float3 u = cross(r, f);

    // Scene bounds in light space decide the near/far range, so casters between the light and
    // the cascade are included.
    float4x4 light_view = look_to({0, 0, 0}, L, up);
    AABB scene_ls = transform_aabb(light_view, scene.bounds);

    // Amortized updates (KCD2, TI tutorial 44:40): cascades 0-1 every frame, one of the farther
    // ones per frame in rotation. A cached map is reused only while its (padded) sphere still
    // contains the current slice's sphere; otherwise (camera cut, fast turn, sun moved) it is
    // re-rendered this frame, so a stale map is never sampled outside what it covers.
    int key = s.cascade_count | (s.cascade_size << 4) | (s.shadow_d16 ? 1 << 30 : 0);
    if (!s.cascade_amortize || key != g_cache_key)
        for (CascadeCache& cc : g_cache) cc.valid = false;
    g_cache_key = key;
    int far_count = s.cascade_count - 2;
    const float AMORTIZE_PAD = 1.15f;   // padded radius: slack for camera motion between updates

    for (int c = 0; c < s.cascade_count; c++) {
        float n = c == 0 ? cam.near_z : splits[c - 1];
        float fz = splits[c];
        // Bounding sphere of the frustum slice: rotation-invariant, so the cascade doesn't shimmer
        // when the camera turns (Valient, "Stable Rendering of Cascaded Shadow Maps", ShaderX6).
        float3 corners[8];
        int k = 0;
        for (float d : {n, fz})
            for (float sy : {-1.0f, 1.0f})
                for (float sx : {-1.0f, 1.0f}) corners[k++] = cam.pos + f * d + r * (sx * tw * d) + u * (sy * th * d);
        float3 center = {0, 0, 0};
        for (float3 p : corners) center += p;
        center = center / 8.0f;
        float radius = 0;
        for (float3 p : corners) radius = fmaxf(radius, length(p - center));

        bool amortized = s.cascade_amortize && c >= 2;
        CascadeCache& cache = g_cache[c];
        ShadowCascade& cas = R.cascades[c];
        cas.split_near = n;
        cas.split_far = fz;
        (&sc.cascade_splits.x)[c] = fz;
        if (amortized) {
            bool due = (int)(R.frame % far_count) == c - 2;
            bool covered = cache.valid && length(center - cache.center) + radius <= cache.radius &&
                           dot(cache.light, L) > 0.99999f;
            g_render[c] = due || !covered;
            if (!g_render[c]) {
                sc.cascade_view_proj[c] = cas.view_proj = cache.view_proj;
                (&sc.cascade_texel.x)[c] = cas.texel_world = cache.texel;
                continue;
            }
            radius *= AMORTIZE_PAD;
        } else {
            g_render[c] = true;
        }
        radius = ceilf(radius * 16.0f) / 16.0f;

        // Snap the center to whole shadow texels in light space.
        float texel = 2.0f * radius / s.cascade_size;
        float3 c_ls = transform_point(light_view, center);
        c_ls.x = floorf(c_ls.x / texel) * texel;
        c_ls.y = floorf(c_ls.y / texel) * texel;
        // Light-space z: view space looks down -Z, so nearer to the light = larger z.
        float z_near = -scene_ls.hi.z - 1.0f;   // distance range along the light direction
        float z_far = -scene_ls.lo.z + 1.0f;
        float4x4 proj = ortho_reverse_z(c_ls.x - radius, c_ls.x + radius, c_ls.y - radius, c_ls.y + radius, z_near, z_far);
        cas.view_proj = proj * light_view;
        cas.texel_world = texel;
        cas.frustum = frustum_from_matrix(cas.view_proj);
        sc.cascade_view_proj[c] = cas.view_proj;
        (&sc.cascade_texel.x)[c] = texel;
        if (amortized) cache = {true, center, radius, L, cas.view_proj, texel};

        // Caster culling. Default: the camera slice extruded toward the sun, inflated by the PCF
        // reach + normal offset (~4 texels). Cascade selection is by view depth (interval-based),
        // so only casters that can shadow *this* slice are needed. Amortized maps must also serve
        // the next frames' slices, so they keep the full box of their padded sphere.
        bool slice_cull = s.cascade_slice_culling && !amortized;
        CasterVolume vol;
        if (slice_cull) vol = slice_caster_volume(corners, L);
        float margin = 4.0f * texel;

        // Same LOD as the main view so shadows match the visible geometry.
        for (int i = 0; i < (int)scene.instances.size(); i++) {
            const Instance& in = scene.instances[i];
            if (in.flags & INST_NO_SHADOW) continue;
            if (!frustum_test_aabb(cas.frustum, in.world_bounds)) continue;
            if (slice_cull && !volume_test_sphere(vol, in.world_bounds.center(), length(in.world_bounds.extent()) + margin)) continue;
            // Tiny objects in far cascades cover less than a texel or two: skip them.
            if (in.radius * 2.0f < texel * 1.5f) continue;
            float dist = length(in.world_bounds.center() - cam.pos);
            float size = in.radius / (fmaxf(dist, 0.01f) * th);
            for (const Primitive& p : scene.meshes[in.mesh].prims) {
                const Material& mat = scene.materials[p.material];
                if (mat.alpha_mode == ALPHA_BLEND) continue;
                int lod = 0;
                if (s.lod_enabled)
                    for (int l = 1; l < p.lod_count; l++)
                        if (size < p.lods[l].screen_size * s.lod_bias) lod = l;
                // Coarser LODs in far cascades.
                lod = std::min(lod + (c >= 2 ? 1 : 0), p.lod_count - 1);
                uint32_t wind = (in.flags & INST_WIND) ? DRAW_WIND : 0;
                float d_light = dot(in.world_bounds.center(), L);
                DrawList& dst = shadow_solid_card(s, mat, dist) ? g_solid[c] : R.list_shadow[c];
                dst.items.push_back({&p, lod, p.material, i, d_light, 0, wind});
            }
        }
        for (DrawList* list : {&R.list_shadow[c], &g_solid[c]}) {
            finalize_shadow_list(scene, *list);
            R.stats.draws_shadow += (int)list->items.size();
            R.stats.batches_shadow += (int)list->batches.size();
            for (const DrawItem& d : list->items) R.stats.tris_shadow += d.prim->lods[d.lod].index_count / 3;
        }
    }
    update_cb(R.cb_shadow, sc);
}

struct ShadowPassCB {
    float4x4 view_proj;
};

void pass_shadows(Scene& scene) {
    gpu.ctx->VSSetConstantBuffers(3, 1, &R.cb_shadow);
    gpu.ctx->PSSetConstantBuffers(3, 1, &R.cb_shadow);
    if (!R.cfg.shadows) return;
    set_viewport(R.shadow_cascades.w, R.shadow_cascades.h);
    gpu.ctx->OMSetBlendState(R.bs_no_color, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_shadow, 0);
    static const char* names[] = {"Cascade 0", "Cascade 1", "Cascade 2", "Cascade 3"};
    for (int c = 0; c < R.cfg.cascade_count; c++) {
        GPU_SCOPE(names[c]);   // also when skipped, so the profiler averages the amortized cost
        if (!g_render[c]) continue;
        ID3D11DepthStencilView* dsv = R.shadow_cascades.slice_dsvs[c];
        gpu.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        gpu.ctx->OMSetRenderTargets(0, nullptr, dsv);
        ShadowPassCB cb = {R.cascades[c].view_proj};
        update_cb(R.cb_pass, cb);
        draw_shadow_list(scene, g_solid[c], false);   // solid cards first: cheap, and they occlude
        draw_shadow_list(scene, R.list_shadow[c]);
    }
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    pass_local_shadows(scene);
}
