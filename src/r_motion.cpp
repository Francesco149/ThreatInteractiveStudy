// Motion blur (the video's "velocity compensation"), after anti-aliasing on the tonemapped image
// (SMAA S2x runs per MSAA sample, so post-AA is the first single-sample image; Fox Engine also
// blurs here). See shaders/motion_blur.hlsl and docs/research/08.
#include "renderer_internal.h"

struct MotionBlurCB {
    float4x4 cur_clip_to_prev_clip;
    float inv_screen[2];
    float trail_scale;
    float max_trail_px;
    float near_start, near_end, far_start, far_end;
    float far_amount;
    float camera_blur_scale;
    float rel_soft_z;
    float same_motion_tol;
    float trail_falloff;
    float pass2_span;
    float pad[2];
};

static const int TILE = 32;
static Texture g_packed, g_tile_max, g_neighbor, g_temp;
static Shader *g_cs_pack, *g_ps_neighbor, *g_ps_trail, *g_ps_smooth, *g_ps_centered;
static float3 g_prev_forward = {0, 0, -1};
static bool g_have_prev = false;

void motion_blur_init() {
    g_cs_pack = load_cs("motion_blur.hlsl", "cs_pack");
    g_ps_neighbor = load_ps("motion_blur.hlsl", "ps_neighbor");
    g_ps_trail = load_ps("motion_blur.hlsl", "ps_trail");
    g_ps_smooth = load_ps("motion_blur.hlsl", "ps_smooth");
    g_ps_centered = load_ps("motion_blur.hlsl", "ps_centered");
}

static void ensure_targets() {
    if (g_packed.tex && g_packed.w == R.w && g_packed.h == R.h) return;
    release_texture(g_packed);
    release_texture(g_tile_max);
    release_texture(g_neighbor);
    release_texture(g_temp);
    int tw = (R.w + TILE - 1) / TILE, th = (R.h + TILE - 1) / TILE;
    g_packed = create_texture("mb_packed_velocity", R.w, R.h, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV);
    g_tile_max = create_texture("mb_tile_max", tw, th, DXGI_FORMAT_R16G16_FLOAT, TEX_UAV);
    g_neighbor = create_texture("mb_neighbor_max", tw, th, DXGI_FORMAT_R16G16_FLOAT, TEX_RTV);
    g_temp = create_texture("mb_pass1", R.w, R.h, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RTV);
}

void pass_motion_blur(const Camera& cam, float dt) {
    const Settings& s = R.cfg;
    R.final_srv = R.aa_out.srv;
    float3 fwd = cam.forward();
    float ang_speed = g_have_prev && dt > 0 ? acosf(clampf(dot(fwd, g_prev_forward), -1, 1)) / dt : 0;
    g_prev_forward = fwd;
    g_have_prev = true;
    if (!s.motion_blur) return;
    ensure_targets();

    MotionBlurCB cb = {};
    cb.cur_clip_to_prev_clip = R.frame_constants.prev_view_proj_nojitter * inverse(R.frame_constants.view_proj_nojitter);
    cb.inv_screen[0] = 1.0f / R.w;
    cb.inv_screen[1] = 1.0f / R.h;
    // Trail length is a fixed exposure *time*, so it doesn't depend on the frame rate.
    cb.trail_scale = dt > 0 ? (s.mb_trail_ms * 0.001f) / dt : 1.0f;
    cb.max_trail_px = (float)TILE;
    cb.near_start = s.mb_near_start;
    cb.near_end = s.mb_near_end;
    cb.far_start = 20.0f;
    cb.far_end = 60.0f;
    cb.far_amount = s.mb_depth_limited ? smoothstep(2.0f, 6.0f, ang_speed) : 1.0f;
    cb.camera_blur_scale = 1.0f;
    if (!s.mb_depth_limited) { cb.near_start = 1e5f; cb.near_end = 1e6f; }  // classic: full camera blur everywhere
    cb.rel_soft_z = 0.05f;
    cb.same_motion_tol = 0.05f;
    cb.trail_falloff = s.mb_trail_falloff;
    cb.pass2_span = 1.0f / 8.0f;
    update_cb(R.cb_pass, cb);

    {
        GPU_SCOPE("Velocity pack + tile max");
        gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* srvs[2] = {R.gb_motion.srv, R.depth.srv};
        gpu.ctx->CSSetShaderResources(0, 2, srvs);
        ID3D11UnorderedAccessView* uavs[2] = {g_packed.uav, g_tile_max.uav};
        gpu.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        gpu.ctx->CSSetShader(g_cs_pack->cs, nullptr, 0);
        gpu.ctx->Dispatch(g_tile_max.w, g_tile_max.h, 1);
        ID3D11ShaderResourceView* ns[2] = {};
        ID3D11UnorderedAccessView* nu[2] = {};
        gpu.ctx->CSSetShaderResources(0, 2, ns);
        gpu.ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
    }
    gpu.ctx->RSSetState(R.rs_none);
    gpu.ctx->OMSetBlendState(R.bs_opaque, nullptr, 0xFFFFFFFF);
    gpu.ctx->OMSetDepthStencilState(R.ds_none, 0);
    {
        GPU_SCOPE("Neighbor max");
        gpu.ctx->OMSetRenderTargets(1, &g_neighbor.rtv, nullptr);
        set_viewport(g_neighbor.w, g_neighbor.h);
        gpu.ctx->PSSetShaderResources(0, 1, &g_tile_max.srv);
        gpu.ctx->PSSetShader(g_ps_neighbor->ps, nullptr, 0);
        draw_fullscreen();
    }
    set_viewport(R.w, R.h);
    ID3D11ShaderResourceView* nulls[3] = {};
    if (s.mb_mode == MB_CENTERED) {
        GPU_SCOPE("Motion blur (centered)");
        gpu.ctx->OMSetRenderTargets(1, &R.post_out.rtv, nullptr);
        ID3D11ShaderResourceView* srvs[3] = {R.aa_out.srv, g_packed.srv, g_neighbor.srv};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        gpu.ctx->PSSetShader(g_ps_centered->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 3, nulls);
        R.final_srv = R.post_out.srv;
        return;
    }
    {
        GPU_SCOPE("Motion blur pass 1 (trailing)");
        ID3D11RenderTargetView* rtv = s.mb_separable ? g_temp.rtv : R.post_out.rtv;
        gpu.ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[3] = {R.aa_out.srv, g_packed.srv, g_neighbor.srv};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        gpu.ctx->PSSetShader(g_ps_trail->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 3, nulls);
    }
    if (s.mb_separable) {
        GPU_SCOPE("Motion blur pass 2 (separable)");
        gpu.ctx->OMSetRenderTargets(1, &R.post_out.rtv, nullptr);
        ID3D11ShaderResourceView* srvs[3] = {g_temp.srv, g_packed.srv, g_neighbor.srv};
        gpu.ctx->PSSetShaderResources(0, 3, srvs);
        gpu.ctx->PSSetShader(g_ps_smooth->ps, nullptr, 0);
        draw_fullscreen();
        gpu.ctx->PSSetShaderResources(0, 3, nulls);
    }
    R.final_srv = R.post_out.srv;
}
