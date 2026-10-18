// Forward transparents (glass) and particles.
//
// Particles are rendered at HALF resolution against a half-resolution depth buffer built with a
// checkerboard of min and max depths (The Phantom Pain; UE's "CBMINMAX"), so both sides of a depth
// edge are represented and low-resolution depth discontinuities don't produce halos. They are then
// composited at full resolution with a nearest-depth upsample (Jansen & Bavoil 2011). Every
// transparent writes a RESPONSIVE MASK into the scene colour alpha (MAX blend): the 2-frame temporal
// AA blends less where it's set, which removes ghosting on things without motion vectors.
#include "brdf.hlsli"
#include "atmosphere.hlsli"

// ---------------------------------------------------------------------------------------------
// Half-res depth downsample: writes SV_Depth into the half-res depth buffer. It also writes zero
// into the half-res particle colour target bound as RT0: a "hijack clear" (TI): the pass that
// runs anyway clears the next pass's target for (almost) free, so no separate clear is needed.

struct DownsampleOut {
    float4 clear_color : SV_Target0;
    float depth : SV_Depth;
};

DownsampleOut ps_depth_downsample(FullscreenVSOut i) {
    int2 hp = int2(i.pos.xy);
    int2 p = hp * 2;
    float mn = 1, mx = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        float d = t_depth.Load(p + int2(k & 1, k >> 1), 0);
        mn = min(mn, d);
        mx = max(mx, d);
    }
    // Checkerboard: even texels keep the farthest depth (min, reversed Z), odd the nearest (max).
    DownsampleOut o;
    o.depth = ((hp.x + hp.y) & 1) ? mx : mn;
    o.clear_color = 0;
    return o;
}

// ---------------------------------------------------------------------------------------------
// Particles: camera-facing quads expanded from a structured buffer (6 vertices per particle).

struct Particle {
    float3 pos;
    float size;
    float4 color;      // rgb (linear) premultiplied later, a = opacity
    float rotation;
    float emissive;    // 0 = lit smoke, 1 = fire (self-lit, additive-ish)
    float seed;
    float pad;
};
StructuredBuffer<Particle> particles : register(t11);
Texture2D<float> t_half_depth : register(t4);

cbuffer VfxCB : register(b1) {
    float4 half_size;    // w, h, 1/w, 1/h of the particle target
    float soft_distance; // meters for the soft-particle fade
    float full_res;      // 1 when rendering at full resolution (A/B)
    float2 vfx_pad;
};

struct ParticleVSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
    nointerpolation float emissive : EMISSIVE;
    nointerpolation float seed : SEED;
    float view_z : VIEWZ;
};

ParticleVSOut vs_particle(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    Particle p = particles[iid];
    float2 c = corners[vid];
    float s = sin(p.rotation), co = cos(p.rotation);
    float2 rc = float2(c.x * co - c.y * s, c.x * s + c.y * co);
    float3 right = float3(view[0].x, view[0].y, view[0].z);
    float3 up = float3(view[1].x, view[1].y, view[1].z);
    float3 wp = p.pos + (right * rc.x + up * rc.y) * p.size;
    ParticleVSOut o;
    o.pos = mul(view_proj, float4(wp, 1));
    o.uv = c * 0.5 + 0.5;
    o.color = p.color;
    o.emissive = p.emissive;
    o.seed = p.seed;
    o.view_z = o.pos.w;
    return o;
}

float hash31(float3 p) { return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453); }
float noise3(float3 p) {
    float3 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(lerp(hash31(i), hash31(i + float3(1, 0, 0)), f.x), lerp(hash31(i + float3(0, 1, 0)), hash31(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash31(i + float3(0, 0, 1)), hash31(i + float3(1, 0, 1)), f.x), lerp(hash31(i + float3(0, 1, 1)), hash31(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

float4 ps_particle(ParticleVSOut i) : SV_Target {
    // Procedural puff: radial falloff broken up by animated value noise.
    float2 d = i.uv * 2 - 1;
    float r = length(d);
    float n = noise3(float3(i.uv * 3.0 + i.seed * 17.0, time * 0.3 + i.seed)) * 0.6 +
              noise3(float3(i.uv * 7.0 - i.seed * 5.0, time * 0.5)) * 0.4;
    // Eroded by the noise more at the edges than in the middle: wispy rather than round.
    float edge = saturate(1.0 - r);
    float a = edge * edge * saturate((n - (1.0 - edge) * 0.55) * 2.2) * i.color.a;

    // Soft particle: fade out where the particle approaches the scene depth.
    float2 suv = i.pos.xy * half_size.zw;
    float scene_d = full_res > 0.5 ? t_depth.Load(int2(i.pos.xy), 0) : t_half_depth.SampleLevel(s_point_clamp, suv, 0);
    float scene_z = scene_d > 0 ? linear_depth(scene_d) : 1e6;
    a *= saturate((scene_z - i.view_z) / soft_distance);

    float3 col;
    if (i.emissive > 0.5) {
        col = i.color.rgb * (1.5 + n);  // fire: self-lit, HDR
    } else {
        // Smoke: mostly sky-lit; a little sun (particles don't sample the shadow maps yet, so full
        // sunlight would make smoke glow inside shadowed streets).
        float3 sun = sun_color.rgb * (0.04 + 0.04 * n) / PI;
        col = i.color.rgb * (sun + sky_ambient(float3(0, 1, 0)) * 0.8);
    }
    // Premultiplied colour; alpha doubles as the responsive mask (MAX-blended into the alpha).
    return float4(col * a, a);
}

// ---------------------------------------------------------------------------------------------
// Composite the half-res particle buffer into the full-res MSAA scene colour.

Texture2D<float4> t_half_color : register(t5);

float4 ps_composite(FullscreenVSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float d = t_depth.Load(p, 0);
    float z = d > 0 ? linear_depth(d) : 1e6;
    float2 huv = i.pos.xy * screen_size.zw;  // same uv in the half-res target
    // Nearest-depth upsample: if one of the 4 half-res depths around us matches our depth much
    // better than the others, take that texel; otherwise use bilinear.
    float2 hp = huv * half_size.xy - 0.5;
    int2 h0 = int2(floor(hp));
    float best = 1e9, worst = 0;
    int2 best_p = h0;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 q = clamp(h0 + int2(k & 1, k >> 1), int2(0, 0), int2(half_size.xy) - 1);
        float hd = t_half_depth.Load(int3(q, 0));
        float hz = hd > 0 ? linear_depth(hd) : 1e6;
        float diff = abs(hz - z);
        if (diff < best) { best = diff; best_p = q; }
        worst = max(worst, diff);
    }
    float4 c = (worst > 0.05 * z) ? t_half_color.Load(int3(best_p, 0)) : t_half_color.SampleLevel(s_linear_clamp, huv, 0);
    return c;
}

// ---------------------------------------------------------------------------------------------
// Glass: forward-lit, premultiplied alpha, sorted back to front.

#include "geometry.hlsli"

struct GlassVSOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    nointerpolation uint material : MATERIAL;
};

GlassVSOut vs_glass(float3 pos : POSITION, float4 normal : NORMAL, float4 tangent : TANGENT, float2 uv : TEXCOORD0,
                    uint draw_id : DRAWID) {
    DrawData d = draws[draw_id];
    GlassVSOut o;
    o.world = world_position(d, pos, time);
    o.pos = mul(view_proj, float4(o.world, 1));
    o.normal = transform_normal(d.world0, d.world1, d.world2, normal.xyz * 2 - 1);
    o.material = d.material;
    return o;
}

float4 ps_glass(GlassVSOut i, bool front : SV_IsFrontFace) : SV_Target {
    MaterialData m = materials[i.material];
    float3 n = normalize(i.normal) * (front ? 1 : -1);
    float3 v = normalize(camera_pos.xyz - i.world);
    float NoV = saturate(dot(n, v));
    float F = 0.04 + 0.96 * pow(1 - NoV, 5);
    float3 r = reflect(-v, n);
    float3 env = sky_ambient(r);
    float3 L = -sun_dir.xyz;
    float3 h = normalize(L + v);
    float spec = D_GGX(saturate(dot(n, h)), 0.02) * 0.25 * F;
    float3 col = env * F + sun_color.rgb * spec * saturate(dot(n, L));
    float opacity = saturate(m.base_color.a * 0.6 + F);
    // Tinted transmission is approximated by the alpha blend with the scene behind.
    return float4(col + m.base_color.rgb * 0.02 * sky_ambient(n), opacity);
}
