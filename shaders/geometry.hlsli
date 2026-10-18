// Vertex transformation shared by every geometry pass (pre-pass, base pass, shadows, overdraw).
// The pre-pass and the base pass must produce bit-identical depth for the depth EQUAL test, so the
// position math lives here once and is marked `precise`.
#ifndef GEOMETRY_HLSLI
#define GEOMETRY_HLSLI
#include "shadow.hlsli"   // ShadowCB: the cascade matrices for the shadow passes

// Simple wind sway: bends the object around its origin, with the square of the object-space
// height h, plus a leaf flutter. `bend` is per instance (scene.cpp, wind_bend()): 0.04 for small
// plants, less for tall ones so that their top sways at most 25 cm (0.85 x 0.25 m at the peak).
// The instance bounds include the farthest this can move a vertex (scene.cpp, instance_bounds()).
// Evaluated for this frame and the previous one so motion vectors include the sway (needed by TAA
// and motion blur).
float3 wind_offset(float3 object_pos, float3 world_origin, float bend, float t) {
    float h = max(object_pos.y, 0.0);
    float phase = dot(world_origin.xz, float2(0.37, 0.21));
    float sway = sin(t * 1.3 + phase) * 0.6 + sin(t * 2.9 + phase * 1.7) * 0.25;
    float flutter = sin(t * 7.0 + dot(object_pos, float3(3.1, 2.3, 4.7))) * 0.06;
    float3 dir = normalize(float3(1.0, 0.0, 0.4));
    return (dir * (sway * bend * h * h) + float3(0, flutter * 0.05 * h, flutter * 0.08 * h)) * wind_strength;
}

float3 world_position(DrawData d, float3 object_pos, float t) {
    precise float3 p = transform_point(d.world0, d.world1, d.world2, object_pos);
    if (d.flags & 1) p += wind_offset(object_pos, float3(d.world0.w, d.world1.w, d.world2.w), d.wind_bend, t);
    return p;
}

float3 prev_world_position(DrawData d, float3 object_pos, float t) {
    float3 p = transform_point(d.prev0, d.prev1, d.prev2, object_pos);
    if (d.flags & 1) p += wind_offset(object_pos, float3(d.prev0.w, d.prev1.w, d.prev2.w), d.wind_bend, t);
    return p;
}

#endif
