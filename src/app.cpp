// Scene loading and screenshots.
#include "app.h"
#include <stdio.h>
#include <time.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

void app_load_scene(const std::string& path) {
    scene_release(app.scene);
    SceneLoadOptions opts;
    opts.optimize_vertex_cache = opts.optimize_overdraw = app.mesh_opt;
    opts.compressed_textures = app.compressed_textures;
    bool ok = !path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
              scene_load_gltf(app.scene, path.c_str(), opts);
    if (!ok) {
        if (!path.empty()) log_msg("Scene %s not found, using the procedural test scene", path.c_str());
        app.scene = Scene{};
        scene_build_test(app.scene, opts);
    } else {
        std::string dir = path.substr(0, path.find_last_of("\\/") + 1);
        scene_load_bench_path(app.scene, (dir + "bench_path.json").c_str());
    }
    scene_upload(app.scene);
    renderer_on_scene_loaded(app.scene);

    // Start at the requested viewpoint, the first one, or a spot overlooking the scene.
    const Viewpoint* start = nullptr;
    for (const Viewpoint& v : app.scene.viewpoints)
        if (v.name == app.start_viewpoint) start = &v;
    if (!start && !app.scene.viewpoints.empty()) start = &app.scene.viewpoints[0];
    if (start) {
        app.camera.pos = start->pos;
        app.camera.look_at(start->target);
    } else {
        float3 c = app.scene.bounds.center();
        app.camera.pos = c + float3{0, 3, 18};
        app.camera.look_at(c);
    }
}

void app_save_screenshot(const char* path) {
    ID3D11Texture2D* bb = nullptr;
    gpu.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    gpu.dev->CreateTexture2D(&d, nullptr, &staging);
    gpu.ctx->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(gpu.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        std::vector<uint8_t> px(d.Width * d.Height * 4);
        for (UINT y = 0; y < d.Height; y++) memcpy(&px[y * d.Width * 4], (uint8_t*)m.pData + y * m.RowPitch, d.Width * 4);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        gpu.ctx->Unmap(staging, 0);
        std::string file;
        if (path) {
            file = path;
        } else {
            CreateDirectoryA(data_path("screenshots").c_str(), nullptr);
            char name[64];
            time_t t = time(nullptr);
            strftime(name, sizeof(name), "screenshots\\shot_%Y%m%d_%H%M%S.png", localtime(&t));
            file = data_path(name);
        }
        stbi_write_png(file.c_str(), d.Width, d.Height, 4, px.data(), d.Width * 4);
        log_msg("Saved screenshot %s", file.c_str());
    }
    staging->Release();
    bb->Release();
}
