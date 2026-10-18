// Dear ImGui panels and styling.
#include "app.h"
#include "imgui.h"
#include <stdio.h>
#include <algorithm>

static ImFont* g_font_bold = nullptr;
static ImFont* g_font_mono = nullptr;
static ImFont* g_font_big = nullptr;
static float g_frame_history[240] = {};
static int g_frame_history_pos = 0;

static ImVec4 rgb(int hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a);
}

static const ImVec4 ACCENT = rgb(0xF2A65A);      // warm amber (golden hour)
static const ImVec4 ACCENT_DIM = rgb(0xB8743A);
static const ImVec4 GOOD = rgb(0x7FD18B);
static const ImVec4 WARN = rgb(0xE5C07B);
static const ImVec4 BAD = rgb(0xE06C75);
static const ImVec4 MUTED = rgb(0x8B93A1);

static void apply_style(float dpi_scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 14;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowRounding = 10;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 5;
    s.TabRounding = 6;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 4);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgb(0xE6E8EC);
    c[ImGuiCol_TextDisabled] = MUTED;
    c[ImGuiCol_WindowBg] = rgb(0x15171C, 0.94f);
    c[ImGuiCol_ChildBg] = rgb(0x1B1E24, 0.60f);
    c[ImGuiCol_PopupBg] = rgb(0x1B1E24, 0.98f);
    c[ImGuiCol_Border] = rgb(0x2C313B, 0.90f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(0x242832);
    c[ImGuiCol_FrameBgHovered] = rgb(0x2E333F);
    c[ImGuiCol_FrameBgActive] = rgb(0x363C4A);
    c[ImGuiCol_TitleBg] = rgb(0x111317);
    c[ImGuiCol_TitleBgActive] = rgb(0x191C22);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x111317, 0.8f);
    c[ImGuiCol_MenuBarBg] = rgb(0x191C22);
    c[ImGuiCol_ScrollbarBg] = rgb(0x15171C, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x323846);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x3D4455);
    c[ImGuiCol_ScrollbarGrabActive] = ACCENT_DIM;
    c[ImGuiCol_CheckMark] = ACCENT;
    c[ImGuiCol_SliderGrab] = ACCENT_DIM;
    c[ImGuiCol_SliderGrabActive] = ACCENT;
    c[ImGuiCol_Button] = rgb(0x2A2F3A);
    c[ImGuiCol_ButtonHovered] = rgb(0x363C4A);
    c[ImGuiCol_ButtonActive] = ACCENT_DIM;
    c[ImGuiCol_Header] = rgb(0x262B35);
    c[ImGuiCol_HeaderHovered] = rgb(0x303642);
    c[ImGuiCol_HeaderActive] = rgb(0x3A4150);
    c[ImGuiCol_Separator] = rgb(0x2C313B);
    c[ImGuiCol_SeparatorHovered] = ACCENT_DIM;
    c[ImGuiCol_SeparatorActive] = ACCENT;
    c[ImGuiCol_ResizeGrip] = rgb(0x2C313B, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = ACCENT_DIM;
    c[ImGuiCol_ResizeGripActive] = ACCENT;
    c[ImGuiCol_Tab] = rgb(0x1F232B);
    c[ImGuiCol_TabHovered] = rgb(0x363C4A);
    c[ImGuiCol_TabSelected] = rgb(0x2C313C);
    c[ImGuiCol_TabSelectedOverline] = ACCENT;
    c[ImGuiCol_PlotLines] = ACCENT;
    c[ImGuiCol_PlotLinesHovered] = rgb(0xFFD08A);
    c[ImGuiCol_PlotHistogram] = ACCENT_DIM;
    c[ImGuiCol_PlotHistogramHovered] = ACCENT;
    c[ImGuiCol_TableHeaderBg] = rgb(0x1F232B);
    c[ImGuiCol_TableBorderStrong] = rgb(0x2C313B);
    c[ImGuiCol_TableBorderLight] = rgb(0x23272F);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg] = rgb(0xF2A65A, 0.30f);
    c[ImGuiCol_NavCursor] = ACCENT;
    s.ScaleAllSizes(dpi_scale);
}

void ui_init() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // layout is code-defined; no stray imgui.ini files
    float dpi = GetDpiForWindow(app.hwnd) / 96.0f;
    const char* ui_font = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold_font = "C:\\Windows\\Fonts\\seguisb.ttf";
    const char* mono_font = "C:\\Windows\\Fonts\\CascadiaMono.ttf";
    if (GetFileAttributesA(mono_font) == INVALID_FILE_ATTRIBUTES) mono_font = "C:\\Windows\\Fonts\\consola.ttf";
    if (GetFileAttributesA(ui_font) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(ui_font, 16.5f * dpi);
        g_font_bold = io.Fonts->AddFontFromFileTTF(bold_font, 16.5f * dpi);
        g_font_mono = io.Fonts->AddFontFromFileTTF(mono_font, 15.0f * dpi);
        g_font_big = io.Fonts->AddFontFromFileTTF(bold_font, 26.0f * dpi);
    }
    apply_style(dpi);
}

static void section_title(const char* text) {
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::SeparatorText(text);
    if (g_font_bold) ImGui::PopFont();
}

static void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static bool combo(const char* label, int* v, const char* const items[], int count, const char* tip = nullptr) {
    bool changed = ImGui::Combo(label, v, items, count);
    if (tip) help(tip);
    return changed;
}

static void stat_block(const char* label, const char* value, ImVec4 color) {
    ImGui::BeginGroup();
    ImGui::TextColored(MUTED, "%s", label);
    if (g_font_big) ImGui::PushFont(g_font_big);
    ImGui::TextColored(color, "%s", value);
    if (g_font_big) ImGui::PopFont();
    ImGui::EndGroup();
}

static ImVec4 budget_color(double ms, double budget) {
    return ms <= budget ? GOOD : (ms <= budget * 1.25 ? WARN : BAD);
}

static void draw_main_panel() {
    Settings& s = app.settings;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = 390 * GetDpiForWindow(app.hwnd) / 96.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12, vp->WorkPos.y + 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, vp->WorkSize.y - 24), ImGuiCond_FirstUseEver);
    ImGui::Begin("TI Study Engine", nullptr, ImGuiWindowFlags_NoCollapse);

    double gpu_ms = profiler_frame_avg_ms();
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f ms", gpu_ms);
    stat_block("GPU frame", buf, budget_color(gpu_ms, 16.6));
    ImGui::SameLine(0, 28);
    snprintf(buf, sizeof(buf), "%.2f ms", app.cpu_ms);
    stat_block("CPU submit", buf, budget_color(app.cpu_ms, 8.0));
    ImGui::SameLine(0, 28);
    snprintf(buf, sizeof(buf), "%.0f", ImGui::GetIO().Framerate);
    stat_block("FPS", buf, rgb(0xE6E8EC));

    g_frame_history[g_frame_history_pos] = (float)profiler_frame_ms();
    g_frame_history_pos = (g_frame_history_pos + 1) % IM_ARRAYSIZE(g_frame_history);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x1B1E24));
    ImGui::PlotLines("##frametimes", g_frame_history, IM_ARRAYSIZE(g_frame_history), g_frame_history_pos, nullptr, 0.0f,
                     20.0f, ImVec2(-1, 48));
    ImGui::PopStyleColor();
    const FrameStats& st = renderer_stats();
    ImGui::TextColored(MUTED, "%s  |  %dx%d  %dx MSAA", gpu.adapter_name.c_str(), s.render_w, s.render_h, s.msaa);
    ImGui::TextColored(MUTED, "draws: pre %d  base %d  shadow %d  lights %d", st.draws_prepass, st.draws_base,
                       st.draws_shadow, st.lights_visible);
    ImGui::TextColored(MUTED, "batches: pre %d  base %d  shadow %d   tris %.2fM", st.batches_prepass, st.batches_base,
                       st.batches_shadow, st.tris_base / 1e6);
    ImGui::TextColored(MUTED, "local shadows: %d views (%d cached)  %.1fM texels  %d draws  %.2f ms CPU", st.local_shadow_views,
                       st.local_shadow_cached, st.local_shadow_texels / 1e6, st.draws_local_shadow, st.local_shadow_cpu_ms);

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Pipeline")) {
            ImGui::PushItemWidth(-150);
            section_title("Geometry");
            const char* prepass[] = {"None", "Full (opaque)", "Full (incl. alpha-tested)", "Partial, non-alpha-tested (TI)"};
            combo("Depth pre-pass", (int*)&s.prepass, prepass, 4,
                  "TI: pre-pass only large/close occluders, never alpha-tested content (texture fetches make pre-pass "
                  "draws as expensive as their base-pass counterparts).");
            if (s.prepass == PREPASS_PARTIAL) ImGui::SliderFloat("Pre-pass min size", &s.prepass_min_size, 0.0f, 1.0f, "%.2f");
            const char* order[] = {"By material", "Front to back", "TI order"};
            combo("Base pass order", (int*)&s.order, order, 3,
                  "TI order: close alpha-tested -> other opaque -> remaining alpha-tested -> pre-passed (depth EQUAL).");
            const char* clears[] = {"Minimal (TI)", "Clear all (hardware)", "Clear all (full-screen draw)"};
            combo("Clears", (int*)&s.clear_mode, clears, 3,
                  "Minimal: only depth/stencil and targets that are not fully overwritten are cleared.");
            ImGui::Checkbox("Decals", &s.decals);
            help("Box-projected decals blended straight into the G-buffer albedo (no decal buffer); they never touch "
                 "the shading model or material ID, so they shade like the surface they lie on (Fox Engine).");
            ImGui::Checkbox("LODs", &s.lod_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Dithered transitions", &s.lod_dither);
            ImGui::SliderFloat("LOD bias", &s.lod_bias, 0.25f, 4.0f, "%.2f");
            ImGui::Checkbox("Frustum culling", &s.frustum_culling);
            ImGui::SameLine();
            ImGui::Checkbox("Wind", &s.wind);
            ImGui::SameLine();
            ImGui::Checkbox("Animate", &s.animate);

            section_title("Lighting");
            const char* cull[] = {"Stencil volumes (TI)", "Scissor only", "Full-screen", "Volume mesh (Fox Engine)"};
            combo("Light culling", (int*)&s.light_cull, cull, 4,
                  "Stencil volumes: a cheap light-volume mesh marks the affected pixels in stencil; the lighting draw is a "
                  "full-screen triangle per shading model that only runs where the stencil is marked, and clears it.");
            ImGui::Checkbox("Scissor light draws", &s.light_scissor);
            ImGui::SameLine();
            ImGui::Checkbox("Separate unmark", &s.light_separate_unmark);
            help("A/B: the lighting draws only test the stencil mark and a separate draw clears it, instead of the "
                 "lighting draws clearing it themselves (stencil writes may disable coarse stencil culling).");
            ImGui::Checkbox("Uber shader (A/B)", &s.light_uber_shader);
            help("One lighting draw per light that branches on the shading model per pixel (UE style), instead of one "
                 "draw per shading model that only contains that model's code (TI / Days Gone).");
            ImGui::Checkbox("MSAA edge detection", &s.msaa_edge_detect);
            help("Per-sample lighting only where the samples of a pixel differ (geometric edges). Other pixels are lit once.");
            ImGui::Checkbox("Skin SSS", &s.sss);
            help("Separable screen-space subsurface scattering (Jimenez 2015) on the albedo-free diffuse lighting of "
                 "skin pixels only (stencil), full resolution, two pixel-shader draws.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("##sssscale", &s.sss_scale, 0.0f, 4.0f, "width %.2f");
            ImGui::SliderFloat("Sun intensity", &s.sun_intensity, 0.0f, 4.0f);
            ImGui::SliderFloat("Local lights", &s.light_intensity, 0.0f, 4.0f);
            if (ImGui::SliderFloat("Sun elevation", &s.sun_elevation, -5.0f, 90.0f)) s.sun_from_scene = false;
            if (ImGui::SliderFloat("Sun azimuth", &s.sun_azimuth, 0.0f, 360.0f)) s.sun_from_scene = false;
            help("The sky, the sun's colour and the ambient light come from a physically based atmosphere "
                 "(Hillaire 2020 LUTs, re-baked only when the sun or the camera's altitude changes). Try 10-15 "
                 "degrees for golden hour.");

            section_title("Shadows");
            ImGui::Checkbox("Shadows", &s.shadows);
            ImGui::SliderInt("Cascades", &s.cascade_count, 1, 4);
            const char* sizes[] = {"512", "1024", "2048", "4096"};
            int size_idx = s.cascade_size == 512 ? 0 : s.cascade_size == 1024 ? 1 : s.cascade_size == 2048 ? 2 : 3;
            if (ImGui::Combo("Cascade size", &size_idx, sizes, 4)) s.cascade_size = 512 << size_idx;
            ImGui::SliderFloat("Shadow distance", &s.shadow_distance, 20.0f, 400.0f, "%.0f m");
            ImGui::Checkbox("Slice caster culling", &s.cascade_slice_culling);
            help("Cull cascade casters against the camera slice extruded toward the sun (Cyberpunk 2077) instead of "
                 "the cascade's whole box: only casters that can shadow that slice are drawn.");
            ImGui::Checkbox("Amortize far cascades", &s.cascade_amortize);
            help("KCD2: cascades 0-1 every frame, the farther ones one per frame in rotation. A cached cascade is "
                 "re-rendered at once if it no longer covers the camera's slice (camera cut, fast turn).");
            ImGui::Checkbox("Distant foliage as solid cards", &s.shadow_foliage_cards);
            help("Alpha-tested casters (tree crowns) cost a texture fetch + discard per covered shadow texel, many layers "
                 "deep. Beyond the distance below they are drawn depth-only without alpha test (solid cards, no pixel "
                 "shader) in every shadow view: crowns cast blob shadows. TI: the last cascades with alpha-tested "
                 "content are the performance killers; approximate distant foliage shadows.");
            if (s.shadow_foliage_cards) ImGui::SliderFloat("Foliage card distance", &s.shadow_foliage_distance, 0.0f, 100.0f, "%.0f m");
            ImGui::Checkbox("D16 shadow depth", &s.shadow_d16);
            help("16-bit depth for the cascades and the local atlas (TI's suggestion: half the memory and PCF "
                 "bandwidth of D32; fine when density is guaranteed and near/far are tight). Off: D32F.");
            const char* filters[] = {"3x3 tent (4 taps)", "5x5 (9 taps)"};
            combo("PCF", (int*)&s.shadow_filter, filters, 2,
                  "Optimized PCF (Castano, The Witness): bilinear comparison taps weighted to a tent / near-Gaussian. "
                  "TI wants barely softened shadows: the small kernel anti-aliases the edge and needs the least bias.");
            ImGui::Checkbox("Local light shadows", &s.local_shadows);
            const char* modes[] = {"Fitted, 1080p+8% density (TI)", "Fixed size, full cone (brute force)"};
            combo("Local sizing", (int*)&s.local_shadow_mode, modes, 2,
                  "Fitted: each spot light / cube face renders only the part of its frustum the camera can see "
                  "(the intersection of the camera frustum, the light and the visible receivers), at a resolution giving "
                  ">= 1.08 shadow texels per 1080p pixel, capped per view, snapped to a fixed texel grid. Fixed: the whole "
                  "cone and all 6 point-light faces at one resolution, wherever the camera is.");
            const char* res[] = {"512", "1024", "2048", "4096"};
            if (s.local_shadow_mode == LOCAL_SHADOWS_FIXED) {
                int i = s.local_shadow_fixed_res <= 512 ? 0 : s.local_shadow_fixed_res <= 1024 ? 1 : s.local_shadow_fixed_res <= 2048 ? 2 : 3;
                if (ImGui::Combo("Fixed size", &i, res, 4)) s.local_shadow_fixed_res = 512 << i;
            } else {
                ImGui::SliderFloat("Density target", &s.local_shadow_density, 0.25f, 3.0f, "%.2f texels/px");
                help("Shadow texels per 1080p-reference pixel. TI's rule: 1.08 (1080p + 8%), independent of the "
                     "actual output resolution.");
                int i = s.local_shadow_max_res <= 512 ? 0 : s.local_shadow_max_res <= 1024 ? 1 : s.local_shadow_max_res <= 2048 ? 2 : 3;
                if (ImGui::Combo("Max view size", &i, res, 4)) s.local_shadow_max_res = 512 << i;
                help("Cap per spot light / cube face. TI: 2048.");
            }
            const char* atlas[] = {"4096", "8192"};
            int ai = s.shadow_atlas_size >= 8192 ? 1 : 0;
            if (ImGui::Combo("Atlas size", &ai, atlas, 2)) s.shadow_atlas_size = 4096 << ai;
            help("All local shadow views are packed into one atlas, cleared once per frame. When they don't fit, "
                 "every view is coarsened a step.");
            const char* clips[] = {"Off", "At the far plane", "Beyond the receivers"};
            combo("Clip casters", (int*)&s.local_shadow_clip, clips, 3,
                  "Local shadow views render without depth clipping, so casters in front of the near plane still cast "
                  "(pancaking). That also keeps everything beyond the far plane, where it is rasterized and alpha-tested "
                  "only to fail the depth test. A clip distance removes it first: the same maps for less work. Beyond the "
                  "receivers: clip at the deepest visible receiver point instead of the (rounded) far plane; anything "
                  "deeper can't shadow what the camera sees. With the cache on, views clip at the far plane.");
            ImGui::SliderFloat("Fade lights below", &s.local_shadow_fade_px, 0.0f, 512.0f, "%.0f px");
            help("Distance fade: a light whose whole shadow view would get fewer texels than this (at the density it "
                 "asks for) is too small on screen for a shadow map; its shadow fades out over the 4/3 band and it "
                 "renders nothing below. 0 = never.");
            ImGui::Checkbox("Cache static views", &s.local_shadow_cache);
            help("A view without moving casters keeps its tile while that still covers what the camera sees, and "
                 "is sampled in place (no copies, no quad clears: re-rendered tiles go into still-clear atlas rows). "
                 "Views with wind-swayed or animated casters re-render.");
            if (s.local_shadow_cache) {
                ImGui::SliderInt("Moving casters every", &s.local_shadow_dynamic_interval, 1, 4, "%d frames");
                help("Views with moving casters re-render only every N frames: their shadows lag by up to N-1 frames.");
            }

            section_title("Global illumination + AO");
            ImGui::Checkbox("Probe GI", &s.gi);
            help("Baked irradiance probe grid (ambient cubes), sampled from the 8 surrounding probes in the sun/ambient draw: "
                 "noiseless, no temporal accumulation, static and dynamic objects lit the same way. Off = the sky's "
                 "ambient cube alone (no bounce light, no occlusion by the scene).");
            ImGui::SliderFloat("Probe spacing", &s.gi_probe_spacing, 0.5f, 4.0f, "%.2f m");
            help("Distance between probes. Applied on Rebake. The spacing grows automatically if the grid would "
                 "exceed the probe budget.");
            ImGui::SliderInt("Probe budget", &s.gi_max_probes, 1024, 65536);
            help("Maximum number of probes; each costs ~3 KB of sensors (6 x 8 x 8 texels x 8 bytes). Applied on Rebake.");
            ImGui::SliderInt("Bounces", &s.gi_bounces, 1, 4);
            help("1 = sun + sky seen by the probes; each extra bounce relights the probes' sensors with the previous "
                 "bounce's grid. Re-lit immediately (no re-capture).");
            ImGui::Checkbox("Relight when the sun moves", &s.gi_relight_on_sun_change);
            help("Re-light the captured sensors with a fresh whole-scene sun shadow map whenever the sun's direction "
                 "or intensity changes (milliseconds; the geometry is not re-captured).");
            ImGui::Checkbox("Relight every frame (cost test)", &s.gi_relight_every_frame);
            help("Re-light the probes every frame, as a fully dynamic sun / time of day would need (TI's "
                 "shadow-map-driven sensor update). Shows the per-frame cost in the profiler under 'GI relight'.");
            if (ImGui::Button("Rebake GI")) renderer_request_gi_bake();
            const GIInfo& gi = renderer_gi_info();
            ImGui::SameLine();
            if (!gi.baked) ImGui::TextColored(MUTED, "not baked");
            if (gi.baked) {
                ImGui::TextColored(MUTED, "capture %.0f ms + relight %.0f ms", gi.capture_ms, gi.relight_ms);
                ImGui::TextColored(MUTED, "%dx%dx%d = %d probes at %.2f m, %d inside geometry", gi.nx, gi.ny, gi.nz,
                                   gi.probes, gi.spacing, gi.invalid_probes);
                ImGui::TextColored(MUTED, "%.1f MB (+%.1f MB during capture)", gi.memory_mb, gi.capture_memory_mb);
            }
            ImGui::SliderFloat("Normal bias", &s.gi_normal_bias, 0.0f, 1.0f, "%.2f");
            help("Probe lookups start this far (x spacing) off the surface along its normal, so they interpolate "
                 "probes on the side the surface faces: the main light-leak mitigation, with the view bias "
                 "and the invalid-probe (inside geometry) rejection.");
            ImGui::SliderFloat("View bias", &s.gi_view_bias, 0.0f, 1.0f, "%.2f");
            help("Probe lookups also move this far (x spacing) towards the camera.");
            ImGui::Checkbox("Probe visibility test", &s.gi_visibility);
            help("Each of the 8 probes around a shaded point checks its own capture: if it saw a surface closer than "
                 "the point in that direction (a wall in between), its weight drops to 2%. Stops light leaking through walls "
                 "thinner than the probe spacing, for 8 extra texel loads per pixel. Off = plain weighted trilinear.");
            const char* aos[] = {"Off", "GTAO full res", "GTAO half res"};
            combo("Ambient occlusion", (int*)&s.ao, aos, 3,
                  "GTAO (Jimenez et al. 2016): horizon-based, cosine-weighted AO. 4x4 Bayer slice rotation + 4x4 "
                  "depth-aware spatial denoise, no temporal history. Applied to indirect light only, with the "
                  "multi-bounce fit and specular occlusion. Half res adds a depth-aware upsample.");
            if (s.ao != AO_OFF) {
                ImGui::SliderFloat("AO radius", &s.ao_radius, 0.1f, 3.0f, "%.2f m");
                help("World-space radius. Small on purpose: the probes carry large-scale occlusion.");
                ImGui::SliderInt("AO slices", &s.ao_slices, 1, 4);
                ImGui::SliderInt("AO steps", &s.ao_steps, 2, 8);
                help("Samples per side of each slice.");
                ImGui::Checkbox("AO denoise", &s.ao_denoise);
                help("Off shows the raw per-pixel result: one slice rotation per pixel (4x4 Bayer pattern).");
            }

            section_title("Post");
            const char* tms[] = {"GT7 (hue-preserving)", "GT (Uchimura)", "Frostbite display map", "ACES (Hill fit)",
                                 "Reinhard", "Linear clip", "AgX"};
            combo("Tone mapper", (int*)&s.tonemapper, tms, TM_COUNT,
                  "GT7 (Polyphony, SIGGRAPH 2025): the GT curve per channel (toe, linear mid-section, shoulder that "
                  "clips at 250-nit paper white), blended 60/40 with a hue-preserving version built in ICtCp, so "
                  "bright colours head to white without twisting (orange -> yellow -> white). The others are for "
                  "comparison, pre-scaled so mid grey 0.18 stays ~0.18.");
            ImGui::Checkbox("Bake curve into 3D LUT", &s.tonemap_lut);
            help("64^3 RGBA16F LUT with GT7's pow(4) input shaper (more texels in the darks), baked by a compute "
                 "shader only when the curve changes; exposure and white balance are applied before the lookup. "
                 "Off: the curve is evaluated for every MSAA sample (compare the 'Tone map' timing).");
            ImGui::Checkbox("Auto exposure", &s.auto_exposure);
            help("Meters the diffuse-only lighting buffer (no albedo), like an incident light meter (Fox Engine): "
                 "dark or bright materials do not change the exposure, only the light does.");
            ImGui::SliderFloat(s.auto_exposure ? "Exposure comp." : "Exposure EV100", &s.exposure_ev, s.auto_exposure ? -4.0f : 0.0f,
                               s.auto_exposure ? 4.0f : 16.0f, "%.2f");
            ImGui::Checkbox("Auto white balance", &s.auto_white_balance);
            help("Measures the light colour from the same albedo-free buffer (so a red wall is not mistaken for red "
                 "light), converts it to a colour temperature, clamps it to the range below, adapts slowly and "
                 "corrects towards D65 with a Bradford (von Kries) transform, applied with exposure as one 3x3 "
                 "matrix. Partial strength keeps the light's mood: a low sun stays warm.");
            if (s.auto_white_balance) {
                ImGui::SliderFloat("AWB strength", &s.awb_strength, 0.0f, 1.0f, "%.2f");
                ImGui::DragFloatRange2("AWB range", &s.awb_min_kelvin, &s.awb_max_kelvin, 50.0f, 1700.0f, 20000.0f, "%.0f K", "%.0f K");
                help("Lights warmer or cooler than this range are only corrected as far as its limits, like a real "
                     "camera's AWB: candle light stays orange.");
            }
            ImGui::SliderFloat("WB warmth", &s.wb_warmth, -100.0f, 100.0f, "%+.0f mired");
            help("Artist temperature bias, + = warmer picture. In mireds (10^6 / kelvin), where equal steps look "
                 "about equally large. Also works with auto white balance off.");
            const char* aas[] = {"None", "SMAA 1x", "SMAA S2x", "SMAA T2x", "SMAA 4x (TI)", "MSAA box resolve"};
            combo("Anti-aliasing", (int*)&s.aa, aas, 6);
            ImGui::Checkbox("Texture jitter compensation", &s.texture_unjitter);
            help("Sample material textures at the unjittered pixel centre (EvaluateAttributeSnapped), so the camera jitter "
                 "of the temporal AA doesn't move or blur texture detail.");
            ImGui::Checkbox("Bloom", &s.bloom);
            help("Call of Duty: Advanced Warfare bloom (Jimenez 2014): 6 levels from half resolution, 13-tap "
                 "downsamples (Karis average on the first one against fireflies), 3x3 tent upsamples summed on the "
                 "way up. No threshold: the HDR scene is mixed with its blur by a small lerp before exposure, which "
                 "conserves energy.");
            if (s.bloom) ImGui::SliderFloat("Bloom intensity", &s.bloom_intensity, 0.0f, 0.2f, "%.3f");
            section_title("Transparents & VFX");
            ImGui::Checkbox("Particles", &s.particles);
            ImGui::SameLine();
            ImGui::Checkbox("Half resolution", &s.vfx_half_res);
            ImGui::SameLine();
            ImGui::Checkbox("Hijack clear", &s.hijack_clear);
            help("The Phantom Pain: low-frequency particles rendered at half resolution against a checkerboard min/max "
                 "depth, then composited with a nearest-depth upsample: ~4x fewer pixel shader invocations, no edge halos. "
                 "All transparents write a responsive mask that limits temporal blending (no ghosting).");
            section_title("Motion blur");
            ImGui::Checkbox("Motion blur", &s.motion_blur);
            const char* mbm[] = {"Trailing, depth-aware (TI)", "Centered (classic)"};
            combo("Mode", (int*)&s.mb_mode, mbm, 2,
                  "Trailing: like the eye, a moving object leaves a trail only behind it and its leading edge stays sharp; "
                  "background trails never smear over the foreground. Centered: the usual camera-footage blur in both directions.");
            ImGui::Checkbox("Separable 2nd pass", &s.mb_separable);
            help("Crytek 2008 / Fox Engine: run the directional blur again over one pass-1 stride: 8 x 8 = 64 virtual taps, "
                 "smooth without noise or dither patterns.");
            ImGui::Checkbox("Depth-limited camera blur", &s.mb_depth_limited);
            help("Camera motion only blurs what's close to the eye (peripheral vision while moving); the focus distance stays "
                 "sharp, far content only blurs under very fast camera rotation.");
            ImGui::SliderFloat("Trail time", &s.mb_trail_ms, 1.0f, 20.0f, "%.1f ms");
            ImGui::SliderFloat("Near blur end", &s.mb_near_end, 0.5f, 20.0f, "%.1f m");
            ImGui::SliderFloat("Trail falloff", &s.mb_trail_falloff, 0.0f, 1.0f, "%.2f");
            ImGui::PopItemWidth();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("View")) {
            ImGui::PushItemWidth(-150);
            section_title("Debug view");
            const char* views[] = {"Final", "Albedo", "Normals", "Roughness", "Profile blend", "Motion vectors", "Depth",
                                   "Stencil: shading model", "MSAA edges", "Diffuse light", "Specular light",
                                   "Shadow cascades", "Quad overdraw", "Pre-pass depth", "GI: probe irradiance",
                                   "GTAO", "Shadow texel density"};
            ImGui::Combo("View", (int*)&s.view, views, VIEW_COUNT);
            if (s.view == VIEW_SHADOW_DENSITY)
                help("Shadow texels per 1080p-reference pixel. Red < 1, yellow 1-1.08, green 1.08-2 (TI's 1080p+8% rule "
                     "met), blue/purple > 2 (wasted texels). Local lights bright, sun cascades dimmed.");
            const char* msaa[] = {"1x", "2x", "4x"};
            int msaa_idx = s.msaa == 4 ? 2 : s.msaa == 2 ? 1 : 0;
            if (ImGui::Combo("MSAA", &msaa_idx, msaa, 3)) s.msaa = 1 << msaa_idx;
            ImGui::Checkbox("VSync", &s.vsync);
            ImGui::SameLine();
            ImGui::Checkbox("24-bit depth (A/B)", &s.depth_24bit);
            ImGui::Checkbox("Pipeline statistics", &s.pipeline_stats);
            help("Collect D3D11 pipeline statistics (pixel shader invocations, primitives) per profiler scope.");

            section_title("Display");
            ImGui::SliderFloat("Brightness", &s.brightness, -1.0f, 1.0f, "%.2f");
            help("Display calibration applied in the final pass, together with the UI overlay: a curve with fixed black "
                 "and white points, so it scales the image instead of crushing shadows or clipping highlights.");
            ImGui::SliderFloat("Chromatic aberration", &s.chromatic_aberration, 0.0f, 6.0f, "%.1f px");
            ImGui::SliderFloat("Film grain", &s.film_grain, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Vignette", &s.vignette, 0.0f, 1.0f, "%.2f");
            section_title("Camera");
            ImGui::SliderFloat("Move speed", &app.move_speed, 0.5f, 40.0f, "%.1f m/s");
            float fov_deg = app.camera.fov_y * 180.0f / PI;
            if (ImGui::SliderFloat("Vertical FOV", &fov_deg, 30.0f, 100.0f, "%.0f deg")) app.camera.fov_y = radians(fov_deg);
            ImGui::TextColored(MUTED, "pos %.1f %.1f %.1f", app.camera.pos.x, app.camera.pos.y, app.camera.pos.z);
            for (size_t i = 0; i < app.scene.viewpoints.size(); i++) {
                const Viewpoint& v = app.scene.viewpoints[i];
                if (i % 2) ImGui::SameLine();
                if (ImGui::Button(v.name.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * (i % 2 ? 1.0f : 0.5f) - 4, 0))) {
                    app.camera.pos = v.pos;
                    app.camera.look_at(v.target);
                }
            }
            section_title("Scene");
            ImGui::TextWrapped("%s", app.scene.path.c_str());
            ImGui::TextColored(MUTED, "%zu instances, %zu meshes, %.2fM tris, %zu lights, loaded in %.0f ms",
                               app.scene.instances.size(), app.scene.meshes.size(), app.scene.triangle_count / 1e6,
                               app.scene.lights.size(), app.scene.load_ms);
            if (ImGui::Button("Reload shaders (F5)")) reload_all_shaders();
            ImGui::SameLine();
            if (ImGui::Button("Screenshot (F12)")) app.screenshot_requested = true;
            ImGui::TextColored(MUTED, "Shader compile total: %.0f ms", total_shader_compile_ms());
            if (!shader_errors().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, BAD);
                ImGui::TextWrapped("%s", shader_errors().c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopItemWidth();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Benchmark")) {
            ImGui::TextWrapped("Runs a fixed set of configurations over the scene's viewpoints and camera path, and "
                               "writes CSV + markdown into bench/. Press Esc to abort.");
            const char* benches[] = {"all", "prepass", "order", "clears", "lights", "msaa", "depth", "aa", "shadows",
                                     "localshadows", "gi", "post", "motionblur", "sss", "vfx", "lod", "smoke"};
            for (int i = 0; i < IM_ARRAYSIZE(benches); i++) {
                if (i % 3) ImGui::SameLine();
                if (ImGui::Button(benches[i], ImVec2(110, 0))) bench_start(benches[i]);
            }
            if (!app.bench.status.empty()) ImGui::TextWrapped("%s", app.bench.status.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void draw_profiler() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float dpi = GetDpiForWindow(app.hwnd) / 96.0f;
    float w = 430 * dpi;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - w - 12, vp->WorkPos.y + 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("GPU Profiler", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    const auto& res = profiler_results();
    double frame = std::max(profiler_frame_avg_ms(), 0.001);
    bool stats = app.settings.pipeline_stats;
    if (ImGui::BeginTable("passes", stats ? 5 : 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 190 * dpi);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56 * dpi);
        ImGui::TableSetupColumn("TI", ImGuiTableColumnFlags_WidthFixed, 40 * dpi);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120 * dpi);
        if (stats) ImGui::TableSetupColumn("PS inv.", ImGuiTableColumnFlags_WidthFixed, 80 * dpi);
        ImGui::TableHeadersRow();
        for (const ProfileResult& r : res) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent(r.depth * 12.0f * dpi + 0.01f);
            if (r.depth == 0 && g_font_bold) ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted(r.name.c_str());
            if (r.depth == 0 && g_font_bold) ImGui::PopFont();
            ImGui::Unindent(r.depth * 12.0f * dpi + 0.01f);
            ImGui::TableNextColumn();
            if (g_font_mono) ImGui::PushFont(g_font_mono);
            ImGui::TextColored(r.depth == 0 ? rgb(0xE6E8EC) : MUTED, "%6.3f", r.avg_ms);
            if (g_font_mono) ImGui::PopFont();
            // TI's chart budget for this pass (1080p60 on an RTX 3060), coloured by how we compare.
            ImGui::TableNextColumn();
            float budget = r.depth == 0 ? ti_budget_ms(r.name) : -1.0f;
            if (budget >= 0) {
                if (g_font_mono) ImGui::PushFont(g_font_mono);
                ImGui::TextColored(budget_color(r.avg_ms, budget), "%4.2f", budget);
                if (g_font_mono) ImGui::PopFont();
                if (ImGui::BeginItemTooltip()) {
                    ImGui::Text("Threat Interactive's budget for this pass: %.2f ms (1080p 60 fps, RTX 3060)", budget);
                    ImGui::EndTooltip();
                }
            }
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float bar_w = 116 * dpi * (float)std::min(1.0, r.avg_ms / frame);
            float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + bar_w, p.y + h * 0.85f),
                                                      ImGui::GetColorU32(r.depth == 0 ? ACCENT : ACCENT_DIM), 3.0f);
            ImGui::Dummy(ImVec2(116 * dpi, h));
            if (stats) {
                ImGui::TableNextColumn();
                if (g_font_mono) ImGui::PushFont(g_font_mono);
                ImGui::TextColored(MUTED, "%7.2fM", r.ps_invocations / 1e6);
                if (g_font_mono) ImGui::PopFont();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void draw_bench_overlay() {
    if (!app.bench.active) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 24), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##bench", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::TextColored(ACCENT, "BENCHMARK");
    ImGui::SameLine();
    ImGui::TextUnformatted(app.bench.status.c_str());
    ImGui::End();
}

void ui_draw() {
    draw_main_panel();
    draw_profiler();
    draw_bench_overlay();
}
