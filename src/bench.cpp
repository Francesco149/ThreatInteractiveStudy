// Benchmark mode: runs a list of configurations (A/B variants of the techniques) over fixed
// camera views and the scene's camera path, and writes the results to bench/.
//
// Method: for every (configuration, view) pair: apply the settings, warm up WARMUP frames (the
// profiler reads queries back 4 frames late and the GPU clocks need to settle), then record
// MEASURE frames of GPU timestamps. Time and camera are driven by the frame counter (fixed 1/60 s
// steps), so every configuration sees exactly the same frames. VSync is forced off.
#include "app.h"
#include <algorithm>
#include <functional>
#include <stdio.h>
#include <time.h>

float ti_budget_ms(const std::string& scope) {
    // Chart passes mapped onto our profiler scopes. Some of our scopes merge several chart passes
    // (noted), and the sun's cascades live in "Shadows" next to the local-light shadow maps.
    static const struct { const char* scope; float ms; } table[] = {
        {"Clears", 0.15f},
        {"Depth pre-pass", 0.30f},
        {"Base pass", 2.80f},
        {"Decals", 0.30f},
        {"MSAA edge stencil", 0.50f},           // "pre-lighting G-buffer resolve"
        {"Shadows", 1.60f},                     // sun cascades 0.60 + local-light shadow maps 1.00
        {"Sun + ambient", 0.54f},               // sun shading 0.36 + shadow projection 0.18
        {"Local lights", 1.50f},
        {"GTAO", 1.00f},                        // GI's screen-space share (chart 2.12)
        {"GI relight", 1.00f},                  // GI's world-space share: probe update from a shadow map (only
                                                // when the sun moves); the probe lookups are in "Sun + ambient"
        {"Subsurface scattering", 0.30f},
        {"Compose", 0.12f},
        {"Sky", 0.18f},
        {"Transparents + VFX", 1.15f},          // transparents 0.15 + VFX 1.00
        {"Tone map", 0.25f},                    // incl. MSAA separation (upper bound 0.93)
        {"Anti-aliasing", 0.63f},               // SMAA S2x 0.38 + temporal 0.25
        {"Motion blur", 0.30f},
        {"Bloom", 0.38f},
        {"Final output + UI composite", 0.25f}, // composite effects 0.15 + UI 0.10
    };
    for (auto& e : table)
        if (scope == e.scope) return e.ms;
    return -1.0f;
}

struct BenchConfig {
    std::string label;
    std::function<void(Settings&)> apply;
};

struct BenchView {
    std::string name;
    bool path;          // true: fly the bench path; false: static viewpoint
    float3 pos, target;
};

struct BenchResult {
    std::string config, view;
    double gpu_mean, gpu_median, gpu_p95, cpu_mean;
    std::vector<std::pair<std::string, double>> passes;  // mean ms per profiler scope ("Parent/Child")
};

static const char* PS_SUFFIX = " [PS invocations]";
static bool is_ps_metric(const std::string& key) { return key.find(PS_SUFFIX) != std::string::npos; }

static std::vector<BenchConfig> g_configs;
static std::vector<BenchView> g_views;
static std::vector<BenchResult> g_results;
static Settings g_base;          // settings before the benchmark started (restored at the end)
static Camera g_camera_before;
static int g_index = 0;          // flat index over configs x views
static const int WARMUP = 90;
static const int MEASURE = 300;
static std::string g_stamp;

static void add_configs(const std::string& name) {
    auto add = [](const char* label, std::function<void(Settings&)> f) { g_configs.push_back({label, f}); };
    bool all = name == "all";
    if (name == "smoke") add("default", [](Settings&) {});
    if (all || name == "prepass") {
        add("prepass=none", [](Settings& s) { s.prepass = PREPASS_NONE; });
        add("prepass=full opaque", [](Settings& s) { s.prepass = PREPASS_FULL_OPAQUE; });
        add("prepass=full incl. alpha", [](Settings& s) { s.prepass = PREPASS_FULL_WITH_ALPHA; });
        add("prepass=partial (TI)", [](Settings& s) { s.prepass = PREPASS_PARTIAL; });
    }
    if (all || name == "order") {
        add("order=by material", [](Settings& s) { s.order = ORDER_BY_MATERIAL; });
        add("order=front to back", [](Settings& s) { s.order = ORDER_FRONT_TO_BACK; });
        add("order=TI (TI)", [](Settings& s) { s.order = ORDER_TI; });
    }
    if (all || name == "clears") {
        add("clears=minimal (TI)", [](Settings& s) { s.clear_mode = CLEAR_MINIMAL; });
        add("clears=all, hardware", [](Settings& s) { s.clear_mode = CLEAR_ALL_HARDWARE; });
        add("clears=all, full-screen draw", [](Settings& s) { s.clear_mode = CLEAR_ALL_FULLSCREEN; });
    }
    if (all || name == "lights") {
        add("lights=stencil volume + scissor (TI)", [](Settings& s) { s.light_cull = LIGHTS_STENCIL_VOLUME; s.light_scissor = true; });
        add("lights=stencil volume, no scissor", [](Settings& s) { s.light_cull = LIGHTS_STENCIL_VOLUME; s.light_scissor = false; });
        add("lights=scissor only", [](Settings& s) { s.light_cull = LIGHTS_SCISSOR_ONLY; s.light_scissor = true; });
        add("lights=full-screen, no culling", [](Settings& s) { s.light_cull = LIGHTS_FULLSCREEN; });
        add("lights=volume mesh (Fox Engine)", [](Settings& s) { s.light_cull = LIGHTS_VOLUME_MESH; });
        add("lights=stencil volume, uber shader", [](Settings& s) { s.light_cull = LIGHTS_STENCIL_VOLUME; s.light_uber_shader = true; });
        add("lights=stencil volume, separate unmark", [](Settings& s) { s.light_cull = LIGHTS_STENCIL_VOLUME; s.light_separate_unmark = true; });
    }
    if (all || name == "msaa") {
        add("msaa=1x", [](Settings& s) { s.msaa = 1; s.aa = AA_SMAA_T2X; });
        add("msaa=2x, edge-detected per-sample (TI)", [](Settings& s) { s.msaa = 2; s.msaa_edge_detect = true; });
        add("msaa=2x, per-sample everywhere", [](Settings& s) { s.msaa = 2; s.msaa_edge_detect = false; });
        add("msaa=4x, edge-detected", [](Settings& s) { s.msaa = 4; s.msaa_edge_detect = true; s.aa = AA_SMAA_T2X; });
    }
    if (all || name == "depth") {
        add("depth=D32F S8 (TI)", [](Settings& s) { s.depth_24bit = false; });
        add("depth=D24 S8", [](Settings& s) { s.depth_24bit = true; });
    }
    if (all || name == "aa") {
        add("aa=none", [](Settings& s) { s.aa = AA_NONE; });
        add("aa=MSAA box resolve", [](Settings& s) { s.aa = AA_MSAA_RESOLVE; });
        add("aa=SMAA 1x", [](Settings& s) { s.aa = AA_SMAA_1X; });
        add("aa=SMAA S2x", [](Settings& s) { s.aa = AA_SMAA_S2X; });
        add("aa=SMAA T2x", [](Settings& s) { s.aa = AA_SMAA_T2X; });
        add("aa=SMAA 4x (TI)", [](Settings& s) { s.aa = AA_SMAA_4X; });
    }
    if (all || name == "shadows") {
        add("shadows=off", [](Settings& s) { s.shadows = false; });
        add("shadows=4x2048 (default)", [](Settings& s) { s.cascade_count = 4; s.cascade_size = 2048; });
        add("shadows=3x2048", [](Settings& s) { s.cascade_count = 3; s.cascade_size = 2048; });
        add("shadows=4x1024", [](Settings& s) { s.cascade_count = 4; s.cascade_size = 1024; });
        add("shadows=4x4096", [](Settings& s) { s.cascade_count = 4; s.cascade_size = 4096; });
        add("shadows=4x2048, D32F", [](Settings& s) { s.shadow_d16 = false; });
        add("shadows=4x2048, 9-tap PCF", [](Settings& s) { s.shadow_filter = PCF_GAUSS_9TAP; });
        add("shadows=4x2048, no slice culling", [](Settings& s) { s.cascade_slice_culling = false; });
        add("shadows=4x2048, amortized (KCD2)", [](Settings& s) { s.cascade_amortize = true; });
        add("shadows=4x2048, distant foliage as cards", [](Settings& s) { s.shadow_foliage_cards = true; });
    }
    if (all || name == "localshadows") {
        add("local shadows=off", [](Settings& s) { s.local_shadows = false; });
        add("local shadows=fixed 1024", [](Settings& s) { s.local_shadow_mode = LOCAL_SHADOWS_FIXED; s.local_shadow_fixed_res = 1024; });
        add("local shadows=fitted, 1080p+8% (TI)", [](Settings& s) { s.local_shadow_mode = LOCAL_SHADOWS_FITTED; });
        // The two levers against foliage canopies next to lamps (the main cost, shadows.md section 3):
        add("local shadows=fitted, half density", [](Settings& s) { s.local_shadow_density = 0.54f; });
        add("local shadows=fitted, all foliage as solid cards (cascades too)", [](Settings& s) {
            s.shadow_foliage_cards = true;
            s.shadow_foliage_distance = 0;
        });
        // Caster clipping (default: beyond the deepest receiver), and caching: tiles of views without
        // moving casters kept between frames. Wind sways most foliage in the courtyard, hence the
        // wind-off pair.
        add("local shadows=fitted, no far clip", [](Settings& s) { s.local_shadow_clip = LOCAL_CLIP_NONE; });
        add("local shadows=fitted, clip at the far plane", [](Settings& s) { s.local_shadow_clip = LOCAL_CLIP_FAR_PLANE; });
        add("local shadows=fitted, cache", [](Settings& s) { s.local_shadow_cache = true; });
        add("local shadows=fitted, cache, moving casters every 2nd frame", [](Settings& s) {
            s.local_shadow_cache = true;
            s.local_shadow_dynamic_interval = 2;
        });
        add("local shadows=fitted, wind off", [](Settings& s) { s.wind = false; });
        add("local shadows=fitted, wind off, cache", [](Settings& s) { s.wind = false; s.local_shadow_cache = true; });
    }
    if (all || name == "post") {
        add("tonemap=GT7 via LUT (default)", [](Settings& s) { s.tonemapper = TM_GT7; });
        add("tonemap=GT", [](Settings& s) { s.tonemapper = TM_GT; });
        add("tonemap=ACES", [](Settings& s) { s.tonemapper = TM_ACES; });
        add("auto exposure off", [](Settings& s) { s.auto_exposure = false; s.exposure_ev = 12; });
        add("tonemap=GT7, per-sample ALU (no LUT)", [](Settings& s) { s.tonemapper = TM_GT7; s.tonemap_lut = false; });
        add("bloom off", [](Settings& s) { s.bloom = false; });
        add("auto white balance off", [](Settings& s) { s.auto_white_balance = false; });
    }
    if (all || name == "gi") {
        // Default first and the visibility toggle last: switching GTAO resolution re-creates its
        // targets and the visibility toggle re-lights the probe grid, and either can shift the cost
        // of the passes reading them by ~0.1-0.15 ms for the rest of the run (GPU memory placement;
        // docs/PERF.md, method).
        add("gi=probes, GTAO full res (default)", [](Settings& s) { s.gi = true; s.ao = AO_GTAO_FULL; });
        add("gi=probes, GTAO half res", [](Settings& s) { s.gi = true; s.ao = AO_GTAO_HALF; });
        add("gi=probes, ao=off", [](Settings& s) { s.gi = true; s.ao = AO_OFF; });
        add("gi=off, ao=off (sky ambient only)", [](Settings& s) { s.gi = false; s.ao = AO_OFF; });
        add("gi=probes relit every frame, GTAO full res", [](Settings& s) { s.gi = true; s.gi_relight_every_frame = true; s.ao = AO_GTAO_FULL; });
        add("gi=probes without visibility test, ao=off", [](Settings& s) { s.gi = true; s.gi_visibility = false; s.ao = AO_OFF; });
    }
    if (all || name == "motionblur") {
        add("mb=off", [](Settings& s) { s.motion_blur = false; });
        add("mb=trailing + separable (TI)", [](Settings& s) { s.motion_blur = true; s.mb_mode = MB_TRAILING; s.mb_separable = true; });
        add("mb=trailing, 1 pass", [](Settings& s) { s.motion_blur = true; s.mb_mode = MB_TRAILING; s.mb_separable = false; });
        add("mb=centered (classic)", [](Settings& s) { s.motion_blur = true; s.mb_mode = MB_CENTERED; });
    }
    if (all || name == "sss") {
        add("sss=off", [](Settings& s) { s.sss = false; });
        add("sss=on, stencil-masked, full res (default)", [](Settings& s) { s.sss = true; });
    }
    if (all || name == "vfx") {
        add("vfx=off", [](Settings& s) { s.particles = false; });
        add("vfx=full res", [](Settings& s) { s.particles = true; s.vfx_half_res = false; });
        add("vfx=half res min/max (TI)", [](Settings& s) { s.particles = true; s.vfx_half_res = true; s.hijack_clear = true; });
        add("vfx=half res, separate clear", [](Settings& s) { s.particles = true; s.vfx_half_res = true; s.hijack_clear = false; });
    }
    if (all || name == "lod") {
        add("lod=off", [](Settings& s) { s.lod_enabled = false; });
        add("lod=on, dithered (default)", [](Settings& s) { s.lod_enabled = true; s.lod_dither = true; });
        add("lod=on, no dither", [](Settings& s) { s.lod_enabled = true; s.lod_dither = false; });
    }
}

static void add_views() {
    for (const Viewpoint& v : app.scene.viewpoints) g_views.push_back({v.name, false, v.pos, v.target});
    if (g_views.empty()) {
        float3 t = app.camera.pos + app.camera.forward();
        g_views.push_back({"current", false, app.camera.pos, t});
    }
    if (app.scene.bench_path.size() >= 2) g_views.push_back({"camera path", true, {}, {}});
}

static void apply_run() {
    const BenchConfig& c = g_configs[g_index / g_views.size()];
    const BenchView& v = g_views[g_index % g_views.size()];
    app.settings = g_base;
    app.settings.vsync = false;
    app.settings.view = VIEW_FINAL;
    c.apply(app.settings);
    if (!v.path) {
        app.camera.pos = v.pos;
        app.camera.look_at(v.target);
    }
    app.bench.frame = 0;
    app.bench.samples.clear();
    char buf[256];
    snprintf(buf, sizeof(buf), "[%d/%d] %s @ %s", g_index + 1, (int)(g_configs.size() * g_views.size()), c.label.c_str(),
             v.name.c_str());
    app.bench.status = buf;
}

static float3 path_lerp(float t, bool target) {
    const auto& k = app.scene.bench_path;
    if (t <= k.front().t) return target ? k.front().target : k.front().pos;
    for (size_t i = 0; i + 1 < k.size(); i++) {
        if (t <= k[i + 1].t) {
            float u = (t - k[i].t) / fmaxf(k[i + 1].t - k[i].t, 1e-4f);
            u = u * u * (3 - 2 * u);
            return target ? lerp(k[i].target, k[i + 1].target, u) : lerp(k[i].pos, k[i + 1].pos, u);
        }
    }
    return target ? k.back().target : k.back().pos;
}

void bench_start(const char* name) {
    g_configs.clear();
    g_views.clear();
    g_results.clear();
    add_configs(name);
    if (g_configs.empty()) {
        app.bench.status = std::string("unknown benchmark '") + name + "'";
        return;
    }
    add_views();
    g_base = app.settings;
    g_camera_before = app.camera;
    g_index = 0;
    app.bench.active = true;
    app.bench.abort = false;
    app.bench.name = name;
    time_t t = time(nullptr);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&t));
    g_stamp = stamp;
    apply_run();
}

void bench_update(float) {
    const BenchView& v = g_views[g_index % g_views.size()];
    // Deterministic time: the same animation frames for every configuration.
    float t = app.bench.frame / 60.0f;
    app.time = 10.0f + t;
    if (v.path) {
        float dur = app.scene.bench_duration > 0 ? app.scene.bench_duration : app.scene.bench_path.back().t;
        float pt = fmodf(t * (dur / ((WARMUP + MEASURE) / 60.0f)), dur);  // whole path within one run
        app.camera.pos = path_lerp(pt, false);
        app.camera.look_at(path_lerp(pt, true));
    }
}

static void write_results() {
    CreateDirectoryA(data_path("bench").c_str(), nullptr);
    std::string base = data_path(("bench\\" + g_stamp + "_" + app.bench.name).c_str());
    FILE* csv = fopen((base + ".csv").c_str(), "w");
    FILE* md = fopen((base + ".md").c_str(), "w");
    if (csv) {
        fprintf(csv, "config,view,metric,ms\n");
        for (const BenchResult& r : g_results) {
            fprintf(csv, "\"%s\",\"%s\",GPU frame (mean),%.4f\n", r.config.c_str(), r.view.c_str(), r.gpu_mean);
            fprintf(csv, "\"%s\",\"%s\",GPU frame (median),%.4f\n", r.config.c_str(), r.view.c_str(), r.gpu_median);
            fprintf(csv, "\"%s\",\"%s\",GPU frame (p95),%.4f\n", r.config.c_str(), r.view.c_str(), r.gpu_p95);
            fprintf(csv, "\"%s\",\"%s\",CPU (mean),%.4f\n", r.config.c_str(), r.view.c_str(), r.cpu_mean);
            for (auto& p : r.passes) fprintf(csv, "\"%s\",\"%s\",\"%s\",%.4f\n", r.config.c_str(), r.view.c_str(), p.first.c_str(), p.second);
        }
        fclose(csv);
    }
    if (md) {
        fprintf(md, "# Benchmark `%s` (%s)\n\n", app.bench.name.c_str(), g_stamp.c_str());
        std::string scene = app.scene.path, root = data_path("");   // relative to the repository
        if (scene.compare(0, root.size(), root) == 0) scene = scene.substr(root.size());
        fprintf(md, "GPU: %s. Resolution %dx%d. Scene: %s. %d warm-up + %d measured frames per row, vsync off.\n\n",
                gpu.adapter_name.c_str(), g_base.render_w, g_base.render_h, scene.c_str(), WARMUP, MEASURE);
        for (const BenchView& v : g_views) {
            fprintf(md, "## View: %s\n\n| config | GPU mean | median | p95 | CPU mean | top passes (ms) |\n|---|---|---|---|---|---|\n",
                    v.name.c_str());
            for (const BenchResult& r : g_results) {
                if (r.view != v.name) continue;
                std::vector<std::pair<std::string, double>> top;
                for (auto& p : r.passes)
                    if (p.first.find('/') == std::string::npos && !is_ps_metric(p.first)) top.push_back(p);
                std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
                std::string tops;
                for (size_t i = 0; i < top.size() && i < 5; i++) {
                    char b[96];
                    snprintf(b, sizeof(b), "%s%s %.3f", i ? ", " : "", top[i].first.c_str(), top[i].second);
                    tops += b;
                }
                fprintf(md, "| %s | %.3f | %.3f | %.3f | %.3f | %s |\n", r.config.c_str(), r.gpu_mean, r.gpu_median, r.gpu_p95,
                        r.cpu_mean, tops.c_str());
            }
            fprintf(md, "\n");
        }
        // The reference configuration (marked "(TI)" or "(default)", else the first row; first view)
        // against TI's chart budgets.
        if (!g_results.empty()) {
            const BenchResult* ref = &g_results[0];
            for (const BenchResult& r : g_results)
                if (r.config.find("(TI)") != std::string::npos || r.config.find("(default)") != std::string::npos) {
                    ref = &r;
                    break;
                }
            const BenchResult& r = *ref;
            fprintf(md, "## Passes vs TI budget (%s @ %s)\n\n| pass | ms | TI budget | |\n|---|---|---|---|\n",
                    r.config.c_str(), r.view.c_str());
            for (auto& p : r.passes) {
                if (p.first.find('/') != std::string::npos || is_ps_metric(p.first)) continue;
                float b = ti_budget_ms(p.first);
                if (b < 0) fprintf(md, "| %s | %.3f | | |\n", p.first.c_str(), p.second);
                else fprintf(md, "| %s | %.3f | %.2f | %s |\n", p.first.c_str(), p.second, b, p.second <= b ? "within" : "OVER");
            }
            fprintf(md, "\n");
        }
        fclose(md);
    }
    log_msg("Benchmark written to %s.{csv,md}", base.c_str());
}

void bench_after_frame() {
    BenchState& b = app.bench;
    b.frame++;
    if (b.frame > WARMUP) {
        BenchSample s;
        s.gpu_ms = profiler_frame_ms();
        s.cpu_ms = app.cpu_ms;
        // Flatten nested scopes as "Parent/Child" so equal child names don't collide.
        std::string parents[8];
        for (const ProfileResult& r : profiler_results()) {
            if (r.depth < 8) parents[r.depth] = r.name;
            std::string key = r.depth == 0 ? r.name : parents[r.depth - 1] + "/" + r.name;
            s.passes.push_back({key, r.ms});
            // With --stats, pixel-shader invocations as extra "metrics" (not milliseconds).
            if (app.settings.pipeline_stats) s.passes.push_back({key + PS_SUFFIX, (double)r.ps_invocations});
        }
        b.samples.push_back(std::move(s));
    }
    if (b.frame < WARMUP + MEASURE && !b.abort) return;

    // Summarize this run.
    BenchResult r;
    r.config = g_configs[g_index / g_views.size()].label;
    r.view = g_views[g_index % g_views.size()].name;
    std::vector<double> gpu;
    double cpu = 0;
    for (auto& s : b.samples) { gpu.push_back(s.gpu_ms); cpu += s.cpu_ms; }
    std::sort(gpu.begin(), gpu.end());
    double sum = 0;
    for (double g : gpu) sum += g;
    size_t n = gpu.size();
    r.gpu_mean = n ? sum / n : 0;
    r.gpu_median = n ? gpu[n / 2] : 0;
    r.gpu_p95 = n ? gpu[std::min(n - 1, (size_t)(n * 0.95))] : 0;
    r.cpu_mean = n ? cpu / n : 0;
    std::vector<std::pair<std::string, double>> acc;
    for (auto& s : b.samples)
        for (auto& p : s.passes) {
            auto it = std::find_if(acc.begin(), acc.end(), [&](auto& a) { return a.first == p.first; });
            if (it == acc.end()) acc.push_back({p.first, p.second});
            else it->second += p.second;
        }
    for (auto& a : acc) a.second /= std::max<size_t>(n, 1);
    r.passes = acc;
    g_results.push_back(r);
    log_msg("bench %s @ %s: GPU mean %.3f ms (median %.3f, p95 %.3f)", r.config.c_str(), r.view.c_str(), r.gpu_mean,
            r.gpu_median, r.gpu_p95);

    g_index++;
    if (b.abort || g_index >= (int)(g_configs.size() * g_views.size())) {
        write_results();
        b.active = false;
        b.status = "finished: bench/" + g_stamp + "_" + b.name + ".md";
        app.settings = g_base;
        app.camera = g_camera_before;
        if (!app.bench_name.empty()) app.quit_requested = true;  // launched from the command line
        return;
    }
    apply_run();
}
