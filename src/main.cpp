// Entry point: window, input, main loop, command line.
//
//   ti_engine.exe [--scene assets/scene/courtyard.gltf] [--test-scene] [--msaa N] [--res 1920x1080]
//                 [--debug] [--bench NAME] [--viewpoint NAME] [--screenshot out.png --frames N]
//                 [--view N] [--no-ui] [--profile-dump timings.csv] [--sun ELEVATION,AZIMUTH]
//                 [--tonemapper N] [--tonemap-alu] [--no-bloom] [--no-awb]
//                 [--gi 0|1] [--gi-spacing M] [--gi-max-probes N] [--gi-relight-every-frame] [--gi-visibility 0|1]
//                 [--ao off|full|half] [--camera px,py,pz,tx,ty,tz] [--stats] [--no-wind] [--time T]
//                 [--shadow key=value] (local=fitted|fixed|off, fixed_res=N, max_res=N, density=F,
//                  atlas=N, depth=16|32, pcf=4|9, slice_cull=0|1, amortize=0|1, sun=intensity,
//                  foliage_cards=0|1, foliage_distance=m, far_clip=0|1|2, fade_px=F, cache=0|1,
//                  dynamic_interval=N) [--exposure EV]
#include "app.h"
#include <mmsystem.h>
#include <shellapi.h>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <chrono>
#include <stdio.h>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static bool g_camera_override = false;   // --camera px,py,pz,tx,ty,tz (reproducible screenshots)
static float3 g_camera_pos, g_camera_target;

App app;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (gpu.swap && wp != SIZE_MINIMIZED) gpu_resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_INPUT: {
        // Raw mouse deltas for camera look (no acceleration, no clamping at screen edges).
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            app.mouse_dx += (float)ri.data.mouse.lLastX;
            app.mouse_dy += (float)ri.data.mouse.lLastY;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F1) app.show_ui = !app.show_ui;
        if (wp == VK_F5) reload_all_shaders();
        if (wp == VK_F12) app.screenshot_requested = true;
        if (wp == VK_ESCAPE && app.bench.active) app.bench.abort = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void update_camera(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    bool look = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) && !io.WantCaptureMouse;
    if (look) {
        app.camera.yaw -= app.mouse_dx * 0.0025f;
        app.camera.pitch = clampf(app.camera.pitch - app.mouse_dy * 0.0025f, -1.5f, 1.5f);
    }
    app.mouse_dx = app.mouse_dy = 0;
    if (io.WantCaptureKeyboard || GetForegroundWindow() != app.hwnd) return;
    float speed = app.move_speed * ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4.0f : 1.0f) *
                  ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0.25f : 1.0f);
    float3 f = app.camera.forward();
    float3 r = normalize(cross(f, {0, 1, 0}));
    float3 move = {0, 0, 0};
    if (GetAsyncKeyState('W') & 0x8000) move += f;
    if (GetAsyncKeyState('S') & 0x8000) move -= f;
    if (GetAsyncKeyState('D') & 0x8000) move += r;
    if (GetAsyncKeyState('A') & 0x8000) move -= r;
    if (GetAsyncKeyState('E') & 0x8000) move += float3{0, 1, 0};
    if (GetAsyncKeyState('Q') & 0x8000) move -= float3{0, 1, 0};
    app.camera.pos += move * (speed * dt);
}

// Shadow A/B switches for scripted runs (see docs/techniques/shadows.md).
static void parse_shadow_option(Settings& s, const char* opt) {
    char key[64] = "", val[64] = "";
    if (sscanf(opt, "%63[^=]=%63s", key, val) != 2) return;
    std::string k = key, v = val;
    if (k == "local") {
        s.local_shadows = v != "off";
        s.local_shadow_mode = v == "fixed" ? LOCAL_SHADOWS_FIXED : LOCAL_SHADOWS_FITTED;
    }
    // Lower limits keep the coarsening loops in r_local_shadows.cpp finite: tiles always shrink to
    // fit an atlas of at least 1024^2.
    else if (k == "fixed_res") s.local_shadow_fixed_res = std::max(atoi(val), 64);
    else if (k == "max_res") s.local_shadow_max_res = std::max(atoi(val), 64);
    else if (k == "density") s.local_shadow_density = (float)atof(val);
    else if (k == "atlas") s.shadow_atlas_size = std::min(std::max(atoi(val), 1024), 16384);
    else if (k == "depth") s.shadow_d16 = atoi(val) != 32;
    else if (k == "pcf") s.shadow_filter = atoi(val) == 9 ? PCF_GAUSS_9TAP : PCF_TENT_4TAP;
    else if (k == "slice_cull") s.cascade_slice_culling = atoi(val) != 0;
    else if (k == "amortize") s.cascade_amortize = atoi(val) != 0;
    else if (k == "sun") s.sun_intensity = (float)atof(val);
    else if (k == "foliage_cards") s.shadow_foliage_cards = atoi(val) != 0;
    else if (k == "foliage_distance") s.shadow_foliage_distance = (float)atof(val);
    else if (k == "far_clip") s.local_shadow_clip = (LocalShadowClip)std::min(std::max(atoi(val), 0), 2);
    else if (k == "fade_px") s.local_shadow_fade_px = (float)atof(val);
    else if (k == "cache") s.local_shadow_cache = atoi(val) != 0;
    else if (k == "dynamic_interval") s.local_shadow_dynamic_interval = std::max(atoi(val), 1);
}

static void parse_args(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        char arg[512];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, sizeof(arg), nullptr, nullptr);
        char next[512] = "";
        if (i + 1 < argc) WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, next, sizeof(next), nullptr, nullptr);
        std::string a = arg;
        if (a == "--scene") { app.scene_path = next; i++; }
        else if (a == "--test-scene") app.scene_path = "";
        else if (a == "--msaa") { app.settings.msaa = atoi(next); i++; }
        else if (a == "--res") { sscanf(next, "%dx%d", &app.settings.render_w, &app.settings.render_h); i++; }
        else if (a == "--debug") app.debug_layer = true;
        else if (a == "--bench") { app.bench_name = next; i++; }
        else if (a == "--viewpoint") { app.start_viewpoint = next; i++; }
        else if (a == "--screenshot") { app.screenshot_path = next; i++; }
        else if (a == "--frames") { app.exit_after_frames = atoi(next); i++; }
        else if (a == "--view") { app.settings.view = (DebugView)atoi(next); i++; }
        else if (a == "--no-ui") app.show_ui = false;
        else if (a == "--no-mesh-opt") app.mesh_opt = false;
        else if (a == "--no-bc") app.compressed_textures = false;
        else if (a == "--stats") app.settings.pipeline_stats = true;
        else if (a == "--no-animate") app.settings.animate = false;
        else if (a == "--no-wind") app.settings.wind = false;
        else if (a == "--time") { app.time = (float)atof(next); app.settings.animate = false; i++; }   // frozen scene time (wind phase)
        else if (a == "--no-unjitter") app.settings.texture_unjitter = false;
        else if (a == "--profile-dump") { app.profile_dump_path = next; i++; }
        else if (a == "--shadow") { parse_shadow_option(app.settings, next); i++; }
        else if (a == "--sun") {  // --sun ELEVATION,AZIMUTH (degrees): override the scene's sun
            if (sscanf(next, "%f,%f", &app.settings.sun_elevation, &app.settings.sun_azimuth) >= 1) app.settings.sun_from_scene = false;
            i++;
        }
        // Post A/B switches (docs/techniques/tonemap-sky-bloom.md)
        else if (a == "--tonemapper") { app.settings.tonemapper = (Tonemapper)std::min(std::max(atoi(next), 0), TM_COUNT - 1); i++; }
        else if (a == "--tonemap-alu") app.settings.tonemap_lut = false;
        else if (a == "--no-bloom") app.settings.bloom = false;
        else if (a == "--no-awb") app.settings.auto_white_balance = false;
        else if (a == "--exposure") { app.settings.auto_exposure = false; app.settings.exposure_ev = (float)atof(next); i++; }
        else if (a == "--gi") { app.settings.gi = atoi(next) != 0; i++; }
        else if (a == "--gi-spacing") { app.settings.gi_probe_spacing = std::max((float)atof(next), 0.25f); i++; }
        // >= 8: the grid needs 2 probes per axis; <= 2^18: the sensor atlas stays within 16384 rows
        else if (a == "--gi-max-probes") { app.settings.gi_max_probes = std::min(std::max(atoi(next), 8), 1 << 18); i++; }
        else if (a == "--gi-relight-every-frame") app.settings.gi_relight_every_frame = true;
        else if (a == "--gi-visibility") { app.settings.gi_visibility = atoi(next) != 0; i++; }
        else if (a == "--camera") {
            float3& p = g_camera_pos; float3& t = g_camera_target;
            g_camera_override = sscanf(next, "%f,%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &t.x, &t.y, &t.z) == 6;
            i++;
        }
        else if (a == "--ao") {
            std::string m = next;
            app.settings.ao = m == "off" ? AO_OFF : m == "half" ? AO_GTAO_HALF : AO_GTAO_FULL;
            i++;
        }
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One engine instance at a time on this machine: several instances (parallel test runs,
    // benchmarks) hammering the GPU at once crashed the driver (bugcheck 0x119), and they would
    // corrupt each other's timings anyway. A crashed holder abandons the mutex, which still counts
    // as acquired for the next waiter.
    HANDLE gpu_mutex = CreateMutexW(nullptr, FALSE, L"Local\\ti_engine_gpu");
    WaitForSingleObject(gpu_mutex, INFINITE);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    app.scene_path = data_path("assets\\scene\\courtyard.gltf");
    parse_args(argc, argv);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ti_engine";
    RegisterClassExW(&wc);

    // Window client area = render resolution when it fits on the monitor, else 80% of the work area.
    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int ww = app.settings.render_w, wh = app.settings.render_h;
    int max_w = work.right - work.left, max_h = work.bottom - work.top;
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, style, FALSE);
    if (r.right - r.left > max_w || r.bottom - r.top > max_h) {
        r = {0, 0, max_w * 8 / 10, max_h * 8 / 10};
    }
    app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"TI Study Engine (D3D11)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT client;
    GetClientRect(app.hwnd, &client);
    if (!gpu_init(app.hwnd, client.right, client.bottom, app.debug_layer)) {
        MessageBoxA(nullptr, "Failed to create a D3D11 device", "TI Study Engine", MB_OK);
        return 1;
    }
    ShowWindow(app.hwnd, SW_SHOW);

    RAWINPUTDEVICE rid = {0x01, 0x02, 0, app.hwnd};  // generic desktop, mouse
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(gpu.dev, gpu.ctx);
    ui_init();

    profiler_init();
    renderer_init(app.settings);
    app_load_scene(app.scene_path);
    if (g_camera_override) {
        app.camera.pos = g_camera_pos;
        app.camera.look_at(g_camera_target);
    }
    if (!app.bench_name.empty()) bench_start(app.bench_name.c_str());

    auto last = std::chrono::high_resolution_clock::now();
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = fminf(dt, 0.1f);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        auto cpu0 = std::chrono::high_resolution_clock::now();
        if (app.bench.active) {
            bench_update(dt);  // drives camera, settings and time deterministically
        } else {
            update_camera(dt);
            if (app.settings.animate) app.time += dt;
        }
        profiler_set_pipeline_stats(app.settings.pipeline_stats);
        profiler_begin_frame();
        renderer_frame(app.scene, app.camera, app.settings, app.time, dt);
        if (app.show_ui) ui_draw();
        ImGui::Render();
        if (app.show_ui) {
            GPU_SCOPE("ImGui");
            ID3D11RenderTargetView* ui = renderer_ui_target();
            gpu.ctx->OMSetRenderTargets(1, &ui, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        renderer_final_to_backbuffer(app.show_ui);
        app.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - cpu0).count();
        if (app.screenshot_requested || (!app.screenshot_path.empty() && app.frame == (uint64_t)app.exit_after_frames - 1)) {
            app_save_screenshot(app.screenshot_path.empty() ? nullptr : app.screenshot_path.c_str());
            app.screenshot_requested = false;
        }
        profiler_end_frame();
        gpu_present(app.settings.vsync && !app.bench.active);
        if (app.bench.active) bench_after_frame();
        app.frame++;
        if (app.exit_after_frames > 0 && app.frame >= (uint64_t)app.exit_after_frames) running = false;
        if (app.quit_requested) running = false;
    }

    const FrameStats& st = renderer_stats();
    log_msg("Shadow stats (last frame): cascade draws %d, local views %d (%d cached), local texels %.2fM, local draws %d, "
            "tris %llu, local setup %.3f ms CPU", st.draws_shadow, st.local_shadow_views, st.local_shadow_cached,
            st.local_shadow_texels / 1e6, st.draws_local_shadow, (unsigned long long)st.tris_shadow, st.local_shadow_cpu_ms);
    if (!app.profile_dump_path.empty()) {
        // Smoothed (EMA) per-pass GPU times, one CSV line per profiler scope.
        FILE* f = fopen(app.profile_dump_path.c_str(), "w");
        if (f) {
            fprintf(f, "pass,depth,avg_ms,ps_invocations\n");
            fprintf(f, "GPU frame,0,%.4f,0\n", profiler_frame_avg_ms());
            for (const ProfileResult& r : profiler_results())
                fprintf(f, "%s,%d,%.4f,%llu\n", r.name.c_str(), r.depth, r.avg_ms, (unsigned long long)r.ps_invocations);
            fclose(f);
        }
    }
    renderer_shutdown();
    scene_release(app.scene);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gpu_shutdown();
    timeEndPeriod(1);
    ReleaseMutex(gpu_mutex);
    return 0;
}
