// Thin helpers around Direct3D 11: device/swap chain, textures, buffers, shaders (with hot
// reload), and the GPU timestamp profiler. No abstraction layer on purpose: everything else in the
// engine talks to D3D11 directly through `gpu.ctx`.
#pragma once
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <stdint.h>
#include <string>
#include <vector>

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct GPU {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DeviceContext1* ctx1 = nullptr;          // for DiscardView (D3D11.1)
    ID3DUserDefinedAnnotation* annotation = nullptr; // PIX/RenderDoc/Nsight markers
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* backbuffer_rtv = nullptr;  // sRGB view of the back buffer
    ID3D11RenderTargetView* backbuffer_rtv_unorm = nullptr;  // plain view: the final pass encodes gamma itself
    int backbuffer_w = 0, backbuffer_h = 0;
    bool tearing = false;
    std::string adapter_name;
    size_t adapter_vram_mb = 0;
};
extern GPU gpu;

bool gpu_init(HWND hwnd, int w, int h, bool debug_layer);
void gpu_resize(int w, int h);
void gpu_present(bool vsync);
void gpu_shutdown();
void set_debug_name(ID3D11DeviceChild* obj, const char* name);

// ---------------------------------------------------------------------------------------------
// Textures. One struct for every kind of 2D texture: render targets, depth buffers, UAV targets,
// loaded textures. Views that don't apply stay null.
enum TexFlags : uint32_t {
    TEX_RTV = 1 << 0,
    TEX_UAV = 1 << 1,
    TEX_DEPTH = 1 << 2,   // depth-stencil (D32_FLOAT_S8X24_UINT or D32_FLOAT); gets SRVs for depth and stencil
    TEX_MIPS = 1 << 3,    // full mip chain + GenerateMips support
    TEX_CUBE = 1 << 4,
    TEX_MS_SRV = 1 << 5,  // SRV is a Texture2DMS even with 1 sample, so shaders don't need 1x/MSAA permutations
};

struct Texture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11ShaderResourceView* srv_stencil = nullptr; // depth textures with stencil only
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilView* dsv_readonly = nullptr;  // depth+stencil read-only: lets the same depth be bound as SRV
    ID3D11DepthStencilView* dsv_readonly_depth = nullptr; // read-only depth, writable stencil
    ID3D11UnorderedAccessView* uav = nullptr;
    std::vector<ID3D11RenderTargetView*> slice_rtvs;  // arrays: one RTV per slice
    std::vector<ID3D11DepthStencilView*> slice_dsvs;
    int w = 0, h = 0, samples = 1, array_size = 1, mips = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

Texture create_texture(const char* name, int w, int h, DXGI_FORMAT fmt, uint32_t flags, int samples = 1, int array_size = 1);
Texture create_texture_3d(const char* name, int w, int h, int d, DXGI_FORMAT fmt, uint32_t flags);
void release_texture(Texture& t);

// ---------------------------------------------------------------------------------------------
// Buffers.
ID3D11Buffer* create_constant_buffer(uint32_t size);
ID3D11Buffer* create_buffer(const void* data, uint32_t size, UINT bind_flags, bool dynamic = false);
ID3D11Buffer* create_structured_buffer(uint32_t stride, uint32_t count, const void* data, bool uav, bool dynamic,
                                       ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uav_out);
void update_buffer(ID3D11Buffer* buf, const void* data, uint32_t size);  // Map(WRITE_DISCARD)

template <typename T> void update_cb(ID3D11Buffer* buf, const T& data) { update_buffer(buf, &data, sizeof(T)); }

// ---------------------------------------------------------------------------------------------
// Fixed-function state helpers.
ID3D11SamplerState* create_sampler(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address,
                                   D3D11_COMPARISON_FUNC cmp = D3D11_COMPARISON_NEVER, int max_aniso = 1);

// ---------------------------------------------------------------------------------------------
// Shaders. Compiled at runtime from shaders/*.hlsl with D3DCompileFromFile; F5 recompiles all of
// them in place, so pointers stay valid. If a recompile fails, the old shader is kept.
struct ShaderDefine { const char* name; const char* value; };

struct Shader {
    std::string file, entry, profile;          // profile: "vs_5_0", "ps_5_0", "cs_5_0"
    std::vector<std::pair<std::string, std::string>> defines;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    std::vector<uint8_t> bytecode;              // kept for input layout creation
    double compile_ms = 0;
};

Shader* load_vs(const char* file, const char* entry, std::initializer_list<ShaderDefine> defines = {});
Shader* load_ps(const char* file, const char* entry, std::initializer_list<ShaderDefine> defines = {});
Shader* load_cs(const char* file, const char* entry, std::initializer_list<ShaderDefine> defines = {});
int reload_all_shaders();  // returns number of failures
const std::string& shader_errors();
double total_shader_compile_ms();
std::string data_path(const char* relative);  // resolves "shaders/x.hlsl", "assets/..." against the project root

// ---------------------------------------------------------------------------------------------
// GPU profiler: D3D11 timestamp queries around named scopes, read back a few frames later.
// Also optionally collects pipeline statistics (pixel shader invocations etc.) per scope.
struct ProfileResult {
    std::string name;
    int depth = 0;
    double ms = 0;           // last resolved value
    double avg_ms = 0;       // smoothed
    uint64_t ps_invocations = 0, c_primitives = 0, vs_invocations = 0, cs_invocations = 0;
};

void profiler_init();
void profiler_begin_frame();
void profiler_end_frame();
void profiler_push(const char* name);
void profiler_pop();
void profiler_set_pipeline_stats(bool enabled);
const std::vector<ProfileResult>& profiler_results();  // most recently resolved frame
double profiler_frame_ms();                            // GPU time of the whole resolved frame
double profiler_frame_avg_ms();

struct ProfileScope {
    ProfileScope(const char* name) { profiler_push(name); }
    ~ProfileScope() { profiler_pop(); }
};
#define PROFILE_CONCAT2(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT2(a, b)
#define GPU_SCOPE(name) ProfileScope PROFILE_CONCAT(_scope_, __LINE__)(name)

// Unbind everything that could alias (SRV/RTV hazards produce silent D3D11 warnings otherwise).
void unbind_all();
void set_viewport(int w, int h, int x = 0, int y = 0);
void log_msg(const char* fmt, ...);
