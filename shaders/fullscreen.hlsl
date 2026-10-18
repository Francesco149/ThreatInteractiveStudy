// Full-screen triangle vertex shader, and the "clear with a draw" anti-pattern used only by the
// CLEAR_ALL_FULLSCREEN A/B test.
#include "common.hlsli"

FullscreenVSOut vs_main(uint id : SV_VertexID) { return fullscreen_vs(id); }

struct ClearOut {
    float4 t0 : SV_Target0;
    float4 t1 : SV_Target1;
    float4 t2 : SV_Target2;
    float4 t3 : SV_Target3;
    float4 t4 : SV_Target4;
    float4 t5 : SV_Target5;
};

ClearOut ps_clear(FullscreenVSOut i) {
    ClearOut o;
    o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = 0;
    return o;
}
