// Quad overdraw counter. GPUs shade pixels in 2x2 quads; a triangle that covers one pixel of a
// quad still pays for four lanes. Each live (covered) lane finds how many lanes of its quad are
// live with fine derivatives, and adds 12/live to the quad's counter, so every shaded quad adds
// exactly 12 regardless of coverage (12 is divisible by 1, 2, 3 and 4). Helper lanes have
// SV_Coverage == 0 and don't count. Counter / 12 = quad invocations at that quad.
#include "gbuffer.hlsl"

RWTexture2D<uint> u_overdraw : register(u0);

#if !ALPHA_TEST
[earlydepthstencil]
#endif
void ps_overdraw(VSOut i, uint coverage : SV_Coverage) {
    float live = coverage != 0 ? 1.0 : 0.0;
    uint2 px = uint2(i.pos.xy);
    float dx = ddx_fine(live), dy = ddy_fine(live);
    float h = (px.x & 1) ? live - dx : live + dx;          // horizontal neighbour's value
    float v = (px.y & 1) ? live - dy : live + dy;          // vertical neighbour's value
    float dhy = ddy_fine(h);
    float diag = (px.y & 1) ? h - dhy : h + dhy;           // diagonal neighbour's value
    float count = live + h + v + diag;
    if (live > 0) {
        uint add = (uint)round(12.0 / max(count, 1.0));
        InterlockedAdd(u_overdraw[px / 2], add);
    }
#if ALPHA_TEST
    MaterialData m = materials[i.material];
    float a = m.base_color.a;
    if (m.flags & 1) a *= t_albedo.Sample(s_aniso, i.uv).a;
    if (a < m.alpha_cutoff) discard;
#endif
}
