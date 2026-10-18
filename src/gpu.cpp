#include "gpu.h"
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <deque>
#include <chrono>

GPU gpu;

// ---------------------------------------------------------------------------------------------
// Logging

static FILE* g_log_file = nullptr;

void log_msg(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (!g_log_file) g_log_file = fopen(data_path("build/engine.log").c_str(), "w");
    if (g_log_file) { fprintf(g_log_file, "%s\n", buf); fflush(g_log_file); }
}

std::string data_path(const char* relative) {
    static std::string root;
    if (root.empty()) {
        // Walk up from the executable until we find the folder that contains shaders/.
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe;
        for (int i = 0; i < 6; i++) {
            size_t slash = dir.find_last_of("\\/");
            if (slash == std::string::npos) break;
            dir = dir.substr(0, slash);
            if (GetFileAttributesA((dir + "\\shaders").c_str()) != INVALID_FILE_ATTRIBUTES) { root = dir; break; }
        }
        if (root.empty()) root = ".";
    }
    return root + "\\" + relative;
}

void set_debug_name(ID3D11DeviceChild* obj, const char* name) {
    if (obj && name) obj->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
}

// ---------------------------------------------------------------------------------------------
// Device and swap chain

static void create_backbuffer_view() {
    ID3D11Texture2D* bb = nullptr;
    gpu.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    // Flip-model swap chains can't use an _SRGB buffer format, but an _SRGB *view* is allowed.
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    gpu.dev->CreateRenderTargetView(bb, &rtv, &gpu.backbuffer_rtv);
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpu.dev->CreateRenderTargetView(bb, &rtv, &gpu.backbuffer_rtv_unorm);
    D3D11_TEXTURE2D_DESC desc;
    bb->GetDesc(&desc);
    gpu.backbuffer_w = desc.Width;
    gpu.backbuffer_h = desc.Height;
    bb->Release();
}

bool gpu_init(HWND hwnd, int w, int h, bool debug_layer) {
    IDXGIFactory5* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    // Pick the adapter with the most dedicated VRAM (the discrete GPU on hybrid systems).
    IDXGIAdapter1* best = nullptr;
    size_t best_vram = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best_vram) {
            SAFE_RELEASE(best);
            best = adapter;
            best_vram = d.DedicatedVideoMemory;
        } else {
            adapter->Release();
        }
    }
    if (best) {
        DXGI_ADAPTER_DESC1 d;
        best->GetDesc1(&d);
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
        gpu.adapter_name = name;
        gpu.adapter_vram_mb = d.DedicatedVideoMemory / (1024 * 1024);
    }

    UINT flags = debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(best, best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 2, D3D11_SDK_VERSION, &gpu.dev, &got, &gpu.ctx);
    if (FAILED(hr) && debug_layer) {
        log_msg("Debug layer unavailable, creating device without it");
        hr = D3D11CreateDevice(best, best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                               D3D11_SDK_VERSION, &gpu.dev, &got, &gpu.ctx);
    }
    SAFE_RELEASE(best);
    if (FAILED(hr)) return false;
    gpu.ctx->QueryInterface(IID_PPV_ARGS(&gpu.ctx1));
    gpu.ctx->QueryInterface(IID_PPV_ARGS(&gpu.annotation));

    BOOL allow_tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
    gpu.tearing = allow_tearing == TRUE;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = gpu.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = factory->CreateSwapChainForHwnd(gpu.dev, hwnd, &sd, nullptr, nullptr, &gpu.swap);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr)) return false;

    // Never queue more than 2 frames: keeps input latency and profiler readback latency low.
    IDXGIDevice1* dxgi_dev = nullptr;
    if (SUCCEEDED(gpu.dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) {
        dxgi_dev->SetMaximumFrameLatency(2);
        dxgi_dev->Release();
    }
    create_backbuffer_view();
    log_msg("GPU: %s (%zu MB), feature level %x, tearing %d", gpu.adapter_name.c_str(), gpu.adapter_vram_mb, got,
            gpu.tearing);
    return true;
}

void gpu_resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SAFE_RELEASE(gpu.backbuffer_rtv);
    SAFE_RELEASE(gpu.backbuffer_rtv_unorm);
    gpu.ctx->Flush();
    gpu.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, gpu.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    create_backbuffer_view();
}

// With --debug, D3D11 debug-layer messages are pulled into our log every frame.
static void flush_debug_messages() {
    ID3D11InfoQueue* q = nullptr;
    if (FAILED(gpu.dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
    UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < 64; i++) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
        if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            log_msg("D3D11: %.*s", (int)m->DescriptionByteLength, m->pDescription);
    }
    q->ClearStoredMessages();
    q->Release();
}

void gpu_present(bool vsync) {
    flush_debug_messages();
    UINT flags = (!vsync && gpu.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    gpu.swap->Present(vsync ? 1 : 0, flags);
}

void gpu_shutdown() {
    SAFE_RELEASE(gpu.backbuffer_rtv);
    SAFE_RELEASE(gpu.backbuffer_rtv_unorm);
    SAFE_RELEASE(gpu.swap);
    SAFE_RELEASE(gpu.annotation);
    SAFE_RELEASE(gpu.ctx1);
    SAFE_RELEASE(gpu.ctx);
    SAFE_RELEASE(gpu.dev);
}

void set_viewport(int w, int h, int x, int y) {
    D3D11_VIEWPORT vp = {(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f};
    gpu.ctx->RSSetViewports(1, &vp);
}

void unbind_all() {
    ID3D11ShaderResourceView* null_srv[16] = {};
    ID3D11UnorderedAccessView* null_uav[8] = {};
    gpu.ctx->PSSetShaderResources(0, 16, null_srv);
    gpu.ctx->VSSetShaderResources(0, 16, null_srv);
    gpu.ctx->CSSetShaderResources(0, 16, null_srv);
    gpu.ctx->CSSetUnorderedAccessViews(0, 8, null_uav, nullptr);
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Textures

// Depth formats need a typeless resource so that the same memory can be viewed as depth (DSV)
// and as a readable float/uint (SRV).
static void depth_formats(DXGI_FORMAT fmt, DXGI_FORMAT* typeless, DXGI_FORMAT* srv_depth, DXGI_FORMAT* srv_stencil) {
    switch (fmt) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        *srv_depth = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        *srv_stencil = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        *typeless = DXGI_FORMAT_R24G8_TYPELESS;
        *srv_depth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        *srv_stencil = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        break;
    case DXGI_FORMAT_D16_UNORM:
        *typeless = DXGI_FORMAT_R16_TYPELESS;
        *srv_depth = DXGI_FORMAT_R16_UNORM;
        *srv_stencil = DXGI_FORMAT_UNKNOWN;
        break;
    default:  // D32_FLOAT
        *typeless = DXGI_FORMAT_R32_TYPELESS;
        *srv_depth = DXGI_FORMAT_R32_FLOAT;
        *srv_stencil = DXGI_FORMAT_UNKNOWN;
        break;
    }
}

Texture create_texture(const char* name, int w, int h, DXGI_FORMAT fmt, uint32_t flags, int samples, int array_size) {
    Texture t;
    t.w = w;
    t.h = h;
    t.samples = samples;
    t.array_size = array_size;
    t.format = fmt;
    bool depth = flags & TEX_DEPTH;
    bool cube = flags & TEX_CUBE;
    DXGI_FORMAT typeless = fmt, srv_fmt = fmt, stencil_fmt = DXGI_FORMAT_UNKNOWN;
    if (depth) depth_formats(fmt, &typeless, &srv_fmt, &stencil_fmt);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = (flags & TEX_MIPS) ? 0 : 1;
    d.ArraySize = array_size;
    d.Format = typeless;
    d.SampleDesc.Count = samples;
    // Standard pattern: known sample positions (SMAA S2x/4x subsample indices depend on them).
    d.SampleDesc.Quality = samples > 1 ? D3D11_STANDARD_MULTISAMPLE_PATTERN : 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (flags & TEX_RTV) d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (flags & TEX_UAV) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    if (depth) d.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    if (flags & TEX_MIPS) d.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS, d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (cube) d.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    HRESULT hr = gpu.dev->CreateTexture2D(&d, nullptr, &t.tex);
    if (FAILED(hr)) {
        log_msg("CreateTexture2D failed for %s (0x%08x)", name, hr);
        return t;
    }
    set_debug_name(t.tex, name);
    t.tex->GetDesc(&d);
    t.mips = d.MipLevels;

    bool ms = samples > 1;
    bool arr = array_size > 1;
    bool ms_srv = ms || (flags & TEX_MS_SRV);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srv_fmt;
    if (cube) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = -1;
    } else if (ms && arr) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
        sd.Texture2DMSArray.ArraySize = array_size;
    } else if (ms_srv) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else if (arr) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = -1;
        sd.Texture2DArray.ArraySize = array_size;
    } else {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = -1;
    }
    gpu.dev->CreateShaderResourceView(t.tex, &sd, &t.srv);
    if (stencil_fmt != DXGI_FORMAT_UNKNOWN) {
        sd.Format = stencil_fmt;
        gpu.dev->CreateShaderResourceView(t.tex, &sd, &t.srv_stencil);
    }

    if (flags & TEX_RTV) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = fmt;
        if (ms && arr) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            rd.Texture2DMSArray.ArraySize = array_size;
        } else if (ms) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (arr || cube) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.ArraySize = array_size;
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }
        gpu.dev->CreateRenderTargetView(t.tex, &rd, &t.rtv);
        if (arr || cube) {
            for (int i = 0; i < array_size; i++) {
                D3D11_RENDER_TARGET_VIEW_DESC sr = rd;
                if (ms) { sr.Texture2DMSArray.FirstArraySlice = i; sr.Texture2DMSArray.ArraySize = 1; }
                else { sr.Texture2DArray.FirstArraySlice = i; sr.Texture2DArray.ArraySize = 1; }
                ID3D11RenderTargetView* v = nullptr;
                gpu.dev->CreateRenderTargetView(t.tex, &sr, &v);
                t.slice_rtvs.push_back(v);
            }
        }
    }

    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = fmt;
        if (ms && arr) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
            dd.Texture2DMSArray.ArraySize = array_size;
        } else if (ms) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        } else if (arr) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.ArraySize = array_size;
        } else {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        }
        gpu.dev->CreateDepthStencilView(t.tex, &dd, &t.dsv);
        D3D11_DEPTH_STENCIL_VIEW_DESC ro = dd;
        ro.Flags = D3D11_DSV_READ_ONLY_DEPTH | (stencil_fmt != DXGI_FORMAT_UNKNOWN ? D3D11_DSV_READ_ONLY_STENCIL : 0);
        gpu.dev->CreateDepthStencilView(t.tex, &ro, &t.dsv_readonly);
        ro.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        gpu.dev->CreateDepthStencilView(t.tex, &ro, &t.dsv_readonly_depth);
        if (arr) {
            for (int i = 0; i < array_size; i++) {
                D3D11_DEPTH_STENCIL_VIEW_DESC sdv = dd;
                if (ms) { sdv.Texture2DMSArray.FirstArraySlice = i; sdv.Texture2DMSArray.ArraySize = 1; }
                else { sdv.Texture2DArray.FirstArraySlice = i; sdv.Texture2DArray.ArraySize = 1; }
                ID3D11DepthStencilView* v = nullptr;
                gpu.dev->CreateDepthStencilView(t.tex, &sdv, &v);
                t.slice_dsvs.push_back(v);
            }
        }
    }

    if (flags & TEX_UAV) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = fmt;
        if (arr) {
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.ArraySize = array_size;
        } else {
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        }
        gpu.dev->CreateUnorderedAccessView(t.tex, &ud, &t.uav);
    }
    return t;
}

Texture create_texture_3d(const char* name, int w, int h, int depth, DXGI_FORMAT fmt, uint32_t flags) {
    Texture t;
    t.w = w;
    t.h = h;
    t.array_size = depth;
    t.format = fmt;
    D3D11_TEXTURE3D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.Depth = depth;
    d.MipLevels = 1;
    d.Format = fmt;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((flags & TEX_UAV) ? D3D11_BIND_UNORDERED_ACCESS : 0) |
                  ((flags & TEX_RTV) ? D3D11_BIND_RENDER_TARGET : 0);
    ID3D11Texture3D* tex = nullptr;
    gpu.dev->CreateTexture3D(&d, nullptr, &tex);
    set_debug_name(tex, name);
    gpu.dev->CreateShaderResourceView(tex, nullptr, &t.srv);
    if (flags & TEX_UAV) gpu.dev->CreateUnorderedAccessView(tex, nullptr, &t.uav);
    if (flags & TEX_RTV) gpu.dev->CreateRenderTargetView(tex, nullptr, &t.rtv);
    tex->Release();  // the views keep the resource alive
    return t;
}

void release_texture(Texture& t) {
    for (auto* v : t.slice_rtvs) v->Release();
    for (auto* v : t.slice_dsvs) v->Release();
    SAFE_RELEASE(t.srv);
    SAFE_RELEASE(t.srv_stencil);
    SAFE_RELEASE(t.rtv);
    SAFE_RELEASE(t.dsv);
    SAFE_RELEASE(t.dsv_readonly);
    SAFE_RELEASE(t.dsv_readonly_depth);
    SAFE_RELEASE(t.uav);
    SAFE_RELEASE(t.tex);
    t = Texture{};
}

// ---------------------------------------------------------------------------------------------
// Buffers and states

ID3D11Buffer* create_constant_buffer(uint32_t size) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (size + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    gpu.dev->CreateBuffer(&d, nullptr, &b);
    return b;
}

ID3D11Buffer* create_buffer(const void* data, uint32_t size, UINT bind_flags, bool dynamic) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size;
    d.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
    d.BindFlags = bind_flags;
    d.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA init = {data};
    ID3D11Buffer* b = nullptr;
    gpu.dev->CreateBuffer(&d, data ? &init : nullptr, &b);
    return b;
}

ID3D11Buffer* create_structured_buffer(uint32_t stride, uint32_t count, const void* data, bool uav, bool dynamic,
                                       ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uav_out) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = stride * count;
    d.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    d.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init = {data};
    ID3D11Buffer* b = nullptr;
    gpu.dev->CreateBuffer(&d, data ? &init : nullptr, &b);
    if (srv) gpu.dev->CreateShaderResourceView(b, nullptr, srv);
    if (uav && uav_out) gpu.dev->CreateUnorderedAccessView(b, nullptr, uav_out);
    return b;
}

void update_buffer(ID3D11Buffer* buf, const void* data, uint32_t size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(gpu.ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, size);
        gpu.ctx->Unmap(buf, 0);
    }
}

ID3D11SamplerState* create_sampler(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address, D3D11_COMPARISON_FUNC cmp,
                                   int max_aniso) {
    D3D11_SAMPLER_DESC d = {};
    d.Filter = filter;
    d.AddressU = d.AddressV = d.AddressW = address;
    d.MaxAnisotropy = max_aniso;
    d.ComparisonFunc = cmp;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* s = nullptr;
    gpu.dev->CreateSamplerState(&d, &s);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Shaders

static std::deque<Shader> g_shaders;  // deque: stable addresses
static std::string g_shader_errors;

static bool compile_shader(Shader& s) {
    std::vector<D3D_SHADER_MACRO> macros;
    for (auto& d : s.defines) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    std::string path = data_path(("shaders\\" + s.file).c_str());
    std::wstring wpath(path.begin(), path.end());
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    auto t0 = std::chrono::high_resolution_clock::now();
    HRESULT hr = D3DCompileFromFile(wpath.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, s.entry.c_str(),
                                    s.profile.c_str(), flags, 0, &code, &errors);
    s.compile_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    if (FAILED(hr)) {
        std::string msg = errors ? (const char*)errors->GetBufferPointer() : "file not found";
        g_shader_errors += s.file + ":" + s.entry + "\n" + msg + "\n";
        log_msg("Shader compile failed %s:%s\n%s", s.file.c_str(), s.entry.c_str(), msg.c_str());
        SAFE_RELEASE(errors);
        return false;
    }
    SAFE_RELEASE(errors);
    const void* bc = code->GetBufferPointer();
    size_t bc_size = code->GetBufferSize();
    std::string debug_name = s.file + ":" + s.entry;
    if (s.profile[0] == 'v') {
        ID3D11VertexShader* vs = nullptr;
        gpu.dev->CreateVertexShader(bc, bc_size, nullptr, &vs);
        set_debug_name(vs, debug_name.c_str());
        SAFE_RELEASE(s.vs);
        s.vs = vs;
    } else if (s.profile[0] == 'p') {
        ID3D11PixelShader* ps = nullptr;
        gpu.dev->CreatePixelShader(bc, bc_size, nullptr, &ps);
        set_debug_name(ps, debug_name.c_str());
        SAFE_RELEASE(s.ps);
        s.ps = ps;
    } else {
        ID3D11ComputeShader* cs = nullptr;
        gpu.dev->CreateComputeShader(bc, bc_size, nullptr, &cs);
        set_debug_name(cs, debug_name.c_str());
        SAFE_RELEASE(s.cs);
        s.cs = cs;
    }
    s.bytecode.assign((const uint8_t*)bc, (const uint8_t*)bc + bc_size);
    code->Release();
    return true;
}

static Shader* load_shader(const char* file, const char* entry, const char* profile,
                           std::initializer_list<ShaderDefine> defines) {
    Shader s;
    s.file = file;
    s.entry = entry;
    s.profile = profile;
    for (auto& d : defines) s.defines.push_back({d.name, d.value ? d.value : "1"});
    // Reuse an identical, already-loaded shader.
    for (Shader& e : g_shaders)
        if (e.file == s.file && e.entry == s.entry && e.profile == s.profile && e.defines == s.defines) return &e;
    g_shaders.push_back(s);
    compile_shader(g_shaders.back());
    return &g_shaders.back();
}

Shader* load_vs(const char* file, const char* entry, std::initializer_list<ShaderDefine> d) { return load_shader(file, entry, "vs_5_0", d); }
Shader* load_ps(const char* file, const char* entry, std::initializer_list<ShaderDefine> d) { return load_shader(file, entry, "ps_5_0", d); }
Shader* load_cs(const char* file, const char* entry, std::initializer_list<ShaderDefine> d) { return load_shader(file, entry, "cs_5_0", d); }

int reload_all_shaders() {
    g_shader_errors.clear();
    int failures = 0;
    for (Shader& s : g_shaders)
        if (!compile_shader(s)) failures++;
    log_msg("Reloaded %zu shaders, %d failed", g_shaders.size(), failures);
    return failures;
}

const std::string& shader_errors() { return g_shader_errors; }

double total_shader_compile_ms() {
    double t = 0;
    for (Shader& s : g_shaders) t += s.compile_ms;
    return t;
}

// ---------------------------------------------------------------------------------------------
// GPU profiler

static const int PROF_FRAMES = 5;   // ring size; results are read back PROF_FRAMES-1 frames later
static const int PROF_MAX_SCOPES = 128;

struct ProfScope {
    const char* name;
    int depth;
    ID3D11Query* t_begin;
    ID3D11Query* t_end;
    ID3D11Query* stats;
};

struct ProfFrame {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* t_frame_begin = nullptr;
    ID3D11Query* t_frame_end = nullptr;
    ID3D11Query* pool_ts[PROF_MAX_SCOPES * 2] = {};
    ID3D11Query* pool_stats[PROF_MAX_SCOPES] = {};
    std::vector<ProfScope> scopes;
    bool pending = false;
    bool stats = false;
};

static ProfFrame g_prof[PROF_FRAMES];
static uint64_t g_prof_frame = 0;
static std::vector<int> g_prof_stack;
static std::vector<ProfileResult> g_prof_results;
static double g_prof_frame_ms = 0, g_prof_frame_avg = 0;
static bool g_prof_stats = false;

void profiler_init() {
    for (ProfFrame& f : g_prof) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        gpu.dev->CreateQuery(&qd, &f.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        gpu.dev->CreateQuery(&qd, &f.t_frame_begin);
        gpu.dev->CreateQuery(&qd, &f.t_frame_end);
        for (auto& q : f.pool_ts) gpu.dev->CreateQuery(&qd, &q);
        qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        for (auto& q : f.pool_stats) gpu.dev->CreateQuery(&qd, &q);
    }
}

void profiler_set_pipeline_stats(bool enabled) { g_prof_stats = enabled; }

void profiler_begin_frame() {
    ProfFrame& f = g_prof[g_prof_frame % PROF_FRAMES];
    f.scopes.clear();
    f.stats = g_prof_stats;
    gpu.ctx->Begin(f.disjoint);
    gpu.ctx->End(f.t_frame_begin);
}

void profiler_push(const char* name) {
    if (gpu.annotation) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
        gpu.annotation->BeginEvent(wname);
    }
    ProfFrame& f = g_prof[g_prof_frame % PROF_FRAMES];
    int i = (int)f.scopes.size();
    if (i >= PROF_MAX_SCOPES) { g_prof_stack.push_back(-1); return; }
    ProfScope s = {name, (int)g_prof_stack.size(), f.pool_ts[i * 2], f.pool_ts[i * 2 + 1], f.pool_stats[i]};
    gpu.ctx->End(s.t_begin);
    if (f.stats) gpu.ctx->Begin(s.stats);
    f.scopes.push_back(s);
    g_prof_stack.push_back(i);
}

void profiler_pop() {
    if (gpu.annotation) gpu.annotation->EndEvent();
    int i = g_prof_stack.back();
    g_prof_stack.pop_back();
    if (i < 0) return;
    ProfFrame& f = g_prof[g_prof_frame % PROF_FRAMES];
    if (f.stats) gpu.ctx->End(f.scopes[i].stats);
    gpu.ctx->End(f.scopes[i].t_end);
}

template <typename T> static void wait_data(ID3D11Query* q, T* out) {
    while (gpu.ctx->GetData(q, out, sizeof(T), 0) == S_FALSE) Sleep(0);
}

void profiler_end_frame() {
    ProfFrame& f = g_prof[g_prof_frame % PROF_FRAMES];
    gpu.ctx->End(f.t_frame_end);
    gpu.ctx->End(f.disjoint);
    f.pending = true;
    g_prof_frame++;

    // Resolve the oldest frame in the ring (PROF_FRAMES - 1 frames ago).
    ProfFrame& r = g_prof[g_prof_frame % PROF_FRAMES];
    if (!r.pending) return;
    r.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    wait_data(r.disjoint, &dj);
    uint64_t t0, t1;
    wait_data(r.t_frame_begin, &t0);
    wait_data(r.t_frame_end, &t1);
    if (dj.Disjoint) {  // still read every query, or D3D warns about abandoned results
        for (ProfScope& s : r.scopes) { wait_data(s.t_begin, &t0); wait_data(s.t_end, &t0); }
        return;
    }
    double to_ms = 1000.0 / (double)dj.Frequency;
    g_prof_frame_ms = (t1 - t0) * to_ms;
    g_prof_frame_avg = g_prof_frame_avg == 0 ? g_prof_frame_ms : g_prof_frame_avg * 0.95 + g_prof_frame_ms * 0.05;

    std::vector<ProfileResult> results;
    for (ProfScope& s : r.scopes) {
        uint64_t b, e;
        wait_data(s.t_begin, &b);
        wait_data(s.t_end, &e);
        ProfileResult pr;
        pr.name = s.name;
        pr.depth = s.depth;
        pr.ms = (e - b) * to_ms;
        if (r.stats) {
            D3D11_QUERY_DATA_PIPELINE_STATISTICS st;
            wait_data(s.stats, &st);
            pr.ps_invocations = st.PSInvocations;
            pr.c_primitives = st.CPrimitives;
            pr.vs_invocations = st.VSInvocations;
            pr.cs_invocations = st.CSInvocations;
        }
        // Smooth against the previous result with the same name.
        pr.avg_ms = pr.ms;
        for (ProfileResult& old : g_prof_results)
            if (old.name == pr.name && old.depth == pr.depth) { pr.avg_ms = old.avg_ms * 0.95 + pr.ms * 0.05; break; }
        results.push_back(pr);
    }
    g_prof_results = std::move(results);
}

const std::vector<ProfileResult>& profiler_results() { return g_prof_results; }
double profiler_frame_ms() { return g_prof_frame_ms; }
double profiler_frame_avg_ms() { return g_prof_frame_avg; }
