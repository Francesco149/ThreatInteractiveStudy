// Application state shared by main.cpp, ui.cpp and bench.cpp.
#pragma once
#include "renderer.h"
#include <string>
#include <vector>

struct BenchSample {        // one frame of a benchmark run
    double gpu_ms;
    double cpu_ms;
    std::vector<std::pair<std::string, double>> passes;
};

struct BenchState {
    bool active = false;
    bool abort = false;
    std::string name;
    int config = 0;          // index into the benchmark's configuration list
    int view = 0;            // viewpoint index (or -1 = camera path)
    int frame = 0;           // frame within the current measurement
    int warmup = 60;
    int measure = 240;
    std::vector<BenchSample> samples;
    std::string status;
};

struct App {
    HWND hwnd = nullptr;
    Settings settings;
    Camera camera;
    Scene scene;
    std::string scene_path;
    float time = 0;
    uint64_t frame = 0;
    double cpu_ms = 0;
    float mouse_dx = 0, mouse_dy = 0;
    float move_speed = 4.0f;
    bool show_ui = true;
    bool debug_layer = false;
    bool mesh_opt = true;             // --no-mesh-opt: skip meshoptimizer triangle reordering (A/B)
    bool compressed_textures = true;  // --no-bc: load the PNGs (RGBA8) instead of the BC7 DDS files
    bool quit_requested = false;
    bool screenshot_requested = false;
    std::string screenshot_path;
    int exit_after_frames = 0;
    std::string bench_name;
    std::string start_viewpoint;
    std::string profile_dump_path;  // --profile-dump: write averaged pass timings here on exit
    BenchState bench;
};
extern App app;

void ui_init();
void ui_draw();
void app_load_scene(const std::string& path);
void app_save_screenshot(const char* path);  // null: timestamped file in screenshots/
// Threat Interactive's per-pass budget (ms) for a top-level profiler scope, from the video's
// pipeline chart (1080p 60 fps on an RTX 3060; docs/research/00-video-pipeline-chart.md); -1 if
// the chart has no matching pass.
float ti_budget_ms(const std::string& scope);
void bench_start(const char* name);
void bench_update(float dt);
void bench_after_frame();
