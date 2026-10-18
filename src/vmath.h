// Small hand-rolled math library.
//
// Conventions (used everywhere in the engine and the shaders):
//  - Right-handed world space, +Y up, meters (same as glTF). Cameras look down -Z.
//  - Column vectors: p' = M * p. float4x4 stores rows: m[row][col].
//  - Shaders declare `#pragma pack_matrix(row_major)` so `mul(M, v)` in HLSL matches `M * v` here.
//  - Projections use REVERSED depth: near plane -> 1, far plane (or infinity) -> 0, depth test GREATER.
#pragma once
#include <math.h>
#include <stdint.h>

static const float PI = 3.14159265358979f;

struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };

inline float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline float2 operator*(float2 a, float s) { return {a.x * s, a.y * s}; }

inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator-(float3 a) { return {-a.x, -a.y, -a.z}; }
inline float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float s, float3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline float3& operator+=(float3& a, float3 b) { a = a + b; return a; }
inline float3& operator-=(float3& a, float3 b) { a = a - b; return a; }
inline float3& operator*=(float3& a, float s) { a = a * s; return a; }

inline float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }

inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float3 cross(float3 a, float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(float3 a) { return sqrtf(dot(a, a)); }
inline float3 normalize(float3 a) { float l = length(a); return l > 0 ? a / l : float3{0, 0, 0}; }
inline float3 min3(float3 a, float3 b) { return {fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)}; }
inline float3 max3(float3 a, float3 b) { return {fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)}; }
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float radians(float deg) { return deg * (PI / 180.0f); }
inline float smoothstep(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3 - 2 * t); }

struct float4x4 {
    float m[4][4];
};

inline float4x4 identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

inline float4x4 operator*(const float4x4& a, const float4x4& b) {
    float4x4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

inline float4 operator*(const float4x4& a, float4 v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w,
            a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w};
}

inline float3 transform_point(const float4x4& a, float3 p) {
    float4 r = a * float4{p.x, p.y, p.z, 1};
    return {r.x, r.y, r.z};
}

inline float3 transform_dir(const float4x4& a, float3 d) {
    float4 r = a * float4{d.x, d.y, d.z, 0};
    return {r.x, r.y, r.z};
}

inline float3 project_point(const float4x4& a, float3 p) {
    float4 r = a * float4{p.x, p.y, p.z, 1};
    return {r.x / r.w, r.y / r.w, r.z / r.w};
}

inline float4x4 transpose(const float4x4& a) {
    float4x4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i];
    return r;
}

float4x4 inverse(const float4x4& a);

inline float4x4 translation(float3 t) {
    float4x4 r = identity();
    r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
    return r;
}

inline float4x4 scaling(float3 s) {
    float4x4 r = identity();
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
    return r;
}

// Rotation by angle (radians) around a unit axis.
inline float4x4 rotation_axis(float3 axis, float angle) {
    float c = cosf(angle), s = sinf(angle), t = 1 - c;
    float x = axis.x, y = axis.y, z = axis.z;
    return {{{t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0},
             {t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0},
             {t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0},
             {0, 0, 0, 1}}};
}

// Quaternion (x, y, z, w) to rotation matrix.
inline float4x4 rotation_quat(float4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0},
             {0, 0, 0, 1}}};
}

// View matrix for a camera at `eye` looking along `forward` (right-handed, camera looks down -Z).
inline float4x4 look_to(float3 eye, float3 forward, float3 up) {
    float3 f = normalize(forward);
    float3 r = normalize(cross(f, up));
    float3 u = cross(r, f);
    return {{{r.x, r.y, r.z, -dot(r, eye)},
             {u.x, u.y, u.z, -dot(u, eye)},
             {-f.x, -f.y, -f.z, dot(f, eye)},
             {0, 0, 0, 1}}};
}

// Reversed-Z perspective with an infinite far plane: view depth -near -> 1, -inf -> 0.
// Precision is great everywhere with a D32_FLOAT buffer (see NVIDIA "Depth Precision Visualized").
inline float4x4 perspective_reverse_z_inf(float fov_y, float aspect, float near_z) {
    float f = 1.0f / tanf(fov_y * 0.5f);
    return {{{f / aspect, 0, 0, 0},
             {0, f, 0, 0},
             {0, 0, 0, near_z},
             {0, 0, -1, 0}}};
}

// Reversed-Z perspective with a finite far plane (spot light shadows).
inline float4x4 perspective_reverse_z(float fov_y, float aspect, float near_z, float far_z) {
    float f = 1.0f / tanf(fov_y * 0.5f);
    float a = near_z / (far_z - near_z);
    return {{{f / aspect, 0, 0, 0},
             {0, f, 0, 0},
             {0, 0, a, far_z * a},
             {0, 0, -1, 0}}};
}

// Reversed-Z orthographic (sun shadow cascades). Box given in view space; near -> 1, far -> 0.
inline float4x4 ortho_reverse_z(float l, float r, float b, float t, float near_z, float far_z) {
    return {{{2 / (r - l), 0, 0, -(r + l) / (r - l)},
             {0, 2 / (t - b), 0, -(t + b) / (t - b)},
             {0, 0, 1 / (far_z - near_z), far_z / (far_z - near_z)},
             {0, 0, 0, 1}}};
}

struct AABB {
    float3 lo = {1e30f, 1e30f, 1e30f};
    float3 hi = {-1e30f, -1e30f, -1e30f};
    void add(float3 p) { lo = min3(lo, p); hi = max3(hi, p); }
    float3 center() const { return (lo + hi) * 0.5f; }
    float3 extent() const { return (hi - lo) * 0.5f; }
};

// Transform an AABB conservatively (Arvo's method).
inline AABB transform_aabb(const float4x4& m, const AABB& b) {
    float3 c = transform_point(m, b.center());
    float3 e = b.extent();
    float3 r = {fabsf(m.m[0][0]) * e.x + fabsf(m.m[0][1]) * e.y + fabsf(m.m[0][2]) * e.z,
                fabsf(m.m[1][0]) * e.x + fabsf(m.m[1][1]) * e.y + fabsf(m.m[1][2]) * e.z,
                fabsf(m.m[2][0]) * e.x + fabsf(m.m[2][1]) * e.y + fabsf(m.m[2][2]) * e.z};
    return {c - r, c + r};
}

// Six planes (ax + by + cz + d >= 0 means inside) extracted from a view-projection matrix
// (Gribb/Hartmann). With reversed infinite Z the far plane degenerates, which is harmless.
struct Frustum {
    float4 planes[6];
};

inline Frustum frustum_from_matrix(const float4x4& vp) {
    Frustum f;
    const float(*m)[4] = vp.m;
    auto row = [&](int i) { return float4{m[i][0], m[i][1], m[i][2], m[i][3]}; };
    float4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    f.planes[0] = r3 + r0;              // left
    f.planes[1] = r3 + r0 * -1.0f;      // right
    f.planes[2] = r3 + r1;              // bottom
    f.planes[3] = r3 + r1 * -1.0f;      // top
    f.planes[4] = r3 + r2 * -1.0f;      // near (reversed Z: z <= w)
    f.planes[5] = r2;                   // far  (reversed Z: z >= 0)
    for (auto& p : f.planes) {
        float l = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        if (l > 0) p = p * (1.0f / l);
    }
    return f;
}

inline bool frustum_test_aabb(const Frustum& f, const AABB& b) {
    float3 c = b.center(), e = b.extent();
    for (const float4& p : f.planes) {
        float r = e.x * fabsf(p.x) + e.y * fabsf(p.y) + e.z * fabsf(p.z);
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    }
    return true;
}

inline bool frustum_test_sphere(const Frustum& f, float3 c, float radius) {
    for (const float4& p : f.planes)
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -radius) return false;
    return true;
}

inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
