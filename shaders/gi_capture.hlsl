// GI probe capture: renders a low-resolution cube (6 faces x GI_FACE_RES^2 texels) around every
// probe and stores, per texel, what the probe "sees" there: a sensor (TI's irradiance sensors)
// with albedo, normal and hit distance, or "sky". The sensors are lit later, and again whenever
// the sun moves, by gi_relight.hlsl; the geometry is rasterized only once per bake.
//
// Probes are captured in small blocks, one cube face at a time: ONE instanced draw per batch of
// objects, instance = (probe of the block, object). Each (probe, face) owns a tile of an atlas;
// the vertex shader projects with that face's 90-degree camera and remaps clip space into the
// tile, and four clip distances cut the triangles at the tile's frustum, so no geometry spills
// into neighbouring tiles (no viewport arrays or geometry shader needed on D3D11).
#include "gi.hlsli"


cbuffer CaptureCB : register(b1) {
    uint cap_first_item;     // first entry of the batch in t_capture_items
    uint cap_item_count;     // objects in the batch
    uint cap_face;           // cube face 0-5: +X, -X, +Y, -Y, +Z, -Z
    uint cap_two_sided;      // material is double-sided: back faces are real surfaces, not "inside"
    uint3 cap_block;         // grid cell of the block's first probe
    uint cap_probes_per_row; // atlas layout: probes per atlas row (each probe = 6 tiles side by side)
    uint2 cap_block_size;    // probes per block along x and y (z: whatever the instance count covers)
    uint2 cap_pad;
    float4 cap_atlas_size;   // w, h, 1/w, 1/h
    float cap_near;
};

StructuredBuffer<uint> t_capture_items : register(t1);   // per block: DrawData indices, grouped by batch
Texture2D t_albedo : register(t0);
Texture2D t_emissive : register(t4);

// Cube faces in the D3D convention: texel (u, v) of face f looks along fwd + u*right - v*up.
static const float3 FACE_FWD[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
static const float3 FACE_RIGHT[6] = {float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0)};
static const float3 FACE_UP[6] = {float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0)};

struct VSIn {
    float3 pos : POSITION;
    float4 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 clip : SV_ClipDistance0;   // the tile's 4 frustum side planes
    float3 rel : TEXCOORD0;           // position relative to the probe
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    nointerpolation uint material : MATERIAL;
};

VSOut vs_capture(VSIn v, uint instance : SV_InstanceID) {
    uint obj = instance % cap_item_count;
    uint local = instance / cap_item_count;   // probe within the block
    uint3 cell = cap_block + uint3(local % cap_block_size.x, (local / cap_block_size.x) % cap_block_size.y,
                                   local / (cap_block_size.x * cap_block_size.y));
    uint3 n = (uint3)gi_grid_count.xyz;
    uint probe = cell.x + n.x * (cell.y + n.y * cell.z);
    uint face = cap_face;

    DrawData d = draws[t_capture_items[cap_first_item + obj]];
    float3 wp = transform_point(d.world0, d.world1, d.world2, v.pos);
    VSOut o;
    o.rel = wp - (gi_grid_origin.xyz + float3(cell) * gi_grid_origin.w);
    o.normal = transform_normal(d.world0, d.world1, d.world2, v.normal.xyz * 2 - 1);
    o.uv = v.uv;
    o.material = d.material;

    // The face's camera: x = right, y = up, w = forward distance. Reversed Z: z = near, so
    // depth = near / distance (1 at the near plane, -> 0 far away), depth test GREATER.
    float x = dot(o.rel, FACE_RIGHT[face]), y = dot(o.rel, FACE_UP[face]), w = dot(o.rel, FACE_FWD[face]);
    o.clip = all(cell < n) ? float4(w - x, w + x, w - y, w + y) : -1.0;

    // Tile -> atlas: the face's [-1,1] NDC square maps to the tile's pixel rectangle. Written in
    // clip space (multiplied by w) so the mapping stays linear before the perspective divide.
    const float R = GI_FACE_RES;
    float2 tile_px = float2(((probe % cap_probes_per_row) * 6 + face) * R, (probe / cap_probes_per_row) * R);
    float2 scale = R * cap_atlas_size.zw;
    float2 offset = float2((2 * tile_px.x + R) * cap_atlas_size.z - 1, 1 - (2 * tile_px.y + R) * cap_atlas_size.w);
    o.pos = float4(x * scale.x + w * offset.x, y * scale.y + w * offset.y, cap_near, w);
    return o;
}

// Sensor encoding, 8 bytes (R32G32_UINT):
//   x: albedo rgb (sqrt-encoded, 8 bits each) | flags << 24   (flags: 1 = hit, 2 = back face)
//   y: octahedral normal (8 + 8 bits) | hit distance (f16) << 16
// A cleared texel (0) is sky.
struct CaptureOut {
    uint2 sensor : SV_Target0;
    float4 emissive : SV_Target1;   // emissive radiance, projected once into a static grid
};

CaptureOut ps_capture(VSOut i, bool front : SV_IsFrontFace) {
    MaterialData m = materials[i.material];
    float4 albedo = m.base_color;
    if (m.flags & 1) albedo *= t_albedo.Sample(s_aniso, i.uv);
    if ((m.flags & 32) && albedo.a < m.alpha_cutoff) discard;

    float3 n = normalize(i.normal);
    bool back = !front && !cap_two_sided;   // seeing the inside of a closed single-sided mesh
    if (!front) n = -n;

    uint3 a = (uint3)(sqrt(saturate(albedo.rgb)) * 255.0 + 0.5);
    uint flags = back ? 3 : 1;
    uint2 oct = (uint2)(encode_normal_oct(n) * 255.0 + 0.5);
    CaptureOut o;
    o.sensor = uint2(a.r | (a.g << 8) | (a.b << 16) | (flags << 24),
                     oct.x | (oct.y << 8) | (f32tof16(length(i.rel)) << 16));
    float3 e = m.emissive;
    if (m.flags & 16) e *= t_emissive.Sample(s_aniso, i.uv).rgb;
    o.emissive = float4(back ? 0.0 : e, 0);
    return o;
}
