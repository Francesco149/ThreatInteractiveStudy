// Scene data: meshes (with generated LODs), materials, textures, instances, lights.
// Loaded from glTF (cgltf) or built procedurally for synthetic tests.
#pragma once
#include "gpu.h"
#include "vmath.h"
#include <string>
#include <vector>

// Shading models get their own stencil value and their own full-screen lighting draw (Days Gone
// style), so each lighting shader only does the work its pixels need.
enum ShadingModel : uint32_t {
    SM_DEFAULT = 0,
    SM_SKIN = 1,     // subsurface scattering + dual normal
    SM_FOLIAGE = 2,  // two-sided transmission
    SM_EYE = 3,
    SM_COUNT = 4,
};

enum AlphaMode : uint32_t { ALPHA_OPAQUE = 0, ALPHA_MASK = 1, ALPHA_BLEND = 2 };

// A BRDF "material profile" (see docs: dual material profiles). Pixels store a profile *pair* id
// and a blend factor (what used to be "metallic"), so e.g. paint can thin out to bare iron with
// both sides keeping their own physically measured parameters.
struct MaterialProfile {
    std::string name;
    float3 f0;               // specular reflectance at normal incidence (linear RGB)
    float metal;             // 1: specular tinted by albedo, no diffuse
    float clearcoat;         // extra GGX lobe (car/lacquer paint)
    float clearcoat_rough;
    float sheen;             // cloth-like grazing sheen
    float diffuse_wrap;      // 0 = Lambert, >0 = wrapped diffuse (foliage, cloth)
};

struct ProfilePair {
    int a, b;  // indices into Scene::profiles
};

struct Material {
    std::string name;
    int tex_albedo = -1, tex_normal = -1, tex_mr = -1, tex_ao = -1, tex_emissive = -1;
    float4 base_color = {1, 1, 1, 1};
    float roughness = 1.0f, metallic = 0.0f;
    float3 emissive = {0, 0, 0};
    float alpha_cutoff = 0.5f;
    AlphaMode alpha_mode = ALPHA_OPAQUE;
    ShadingModel shading_model = SM_DEFAULT;
    bool double_sided = false;
    int profile_pair = 0;
};

static const int MAX_LODS = 4;

struct MeshLod {
    uint32_t first_index = 0, index_count = 0;
    float screen_size = 0;  // switch to this LOD when the projected size drops below this
};

struct Primitive {
    int material = 0;
    int base_vertex = 0;
    MeshLod lods[MAX_LODS];
    int lod_count = 1;
    AABB bounds;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> prims;
    AABB bounds;
};

enum InstanceFlags : uint32_t {
    INST_NO_SHADOW = 1 << 0,  // floors etc. never cast (they can't occlude anything that matters)
    INST_WIND = 1 << 1,       // vertex-shader wind sway
};

enum AnimType : uint32_t { ANIM_NONE, ANIM_ROTATE, ANIM_SWING, ANIM_SLIDE };

struct Instance {
    std::string name;
    int mesh = -1;
    float4x4 rest_world = identity();  // transform from the file
    float4x4 world = identity();       // after animation, this frame
    float4x4 prev_world = identity();  // last frame (motion vectors)
    AABB world_bounds;                 // includes how far the wind can sway it (scene.cpp, instance_bounds())
    float radius = 0;                  // bounding sphere radius (world) of the mesh at rest: LOD and size tests
    uint32_t flags = 0;
    float wind_bend = 0;               // INST_WIND: sway coefficient of wind_offset() (1/m), see wind_bend()
    AnimType anim = ANIM_NONE;
    float3 anim_axis = {0, 1, 0};
    float anim_speed = 0, anim_amp = 0;
};

enum LightType : uint32_t { LIGHT_DIRECTIONAL = 0, LIGHT_POINT = 1, LIGHT_SPOT = 2 };

struct Light {
    std::string name;
    LightType type = LIGHT_POINT;
    float3 position = {0, 0, 0};
    float3 direction = {0, -1, 0};  // direction the light travels (spot/directional)
    float3 color = {1, 1, 1};
    float intensity = 1;            // candela for point/spot, lux for directional (we rescale on load)
    float range = 10;
    float inner_cone = 0, outer_cone = 0.785f;  // radians (spot)
    bool shadow = false;
};

struct VfxEmitter {
    std::string type;  // "smoke", "fire", "spray"
    float3 position;
};

// A box-projected G-buffer decal: unit box [-0.5, 0.5]^3 transformed by `world`, projected along
// the box's local Y axis.
struct Decal {
    float4x4 world;
    int kind;        // 0 stain, 1 moss, 2 cracks (procedural textures, src/r_decals.cpp)
    float4 tint;     // rgb tint, a = opacity
};

struct CameraKey {
    float t;
    float3 pos, target;
};

struct Viewpoint {
    std::string name;
    float3 pos, target;
};

// Vertex streams are split so depth-only passes (pre-pass, shadows) fetch 12 bytes per vertex.
struct VertexAttribs {     // stream 1: 12 bytes
    uint32_t normal;       // R10G10B10A2_UNORM, xyz * 0.5 + 0.5
    uint32_t tangent;      // R10G10B10A2_UNORM, xyz * 0.5 + 0.5, w (bitangent sign) in A
    uint32_t uv;           // R16G16_FLOAT
};

struct Scene {
    std::string path;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<MaterialProfile> profiles;
    std::vector<ProfilePair> profile_pairs;
    std::vector<Texture> textures;
    std::vector<bool> texture_srgb;
    std::vector<Instance> instances;
    std::vector<Light> lights;
    std::vector<VfxEmitter> emitters;
    std::vector<Decal> decals;
    std::vector<CameraKey> bench_path;
    float bench_duration = 0;
    std::vector<Viewpoint> viewpoints;
    AABB bounds;

    // CPU copies (kept for LOD generation statistics and GI baking)
    std::vector<float3> positions;
    std::vector<VertexAttribs> attribs;
    std::vector<uint32_t> indices;

    // GPU
    ID3D11Buffer* vb_position = nullptr;
    ID3D11Buffer* vb_attribs = nullptr;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* material_buffer = nullptr;
    ID3D11ShaderResourceView* material_srv = nullptr;
    ID3D11Buffer* profile_buffer = nullptr;
    ID3D11ShaderResourceView* profile_srv = nullptr;

    uint64_t triangle_count = 0;
    int compressed_texture_count = 0;
    double load_ms = 0;
};

struct SceneLoadOptions {
    bool optimize_vertex_cache = true;   // meshopt: Forsyth-like vertex cache order
    bool optimize_overdraw = true;       // meshopt: Tipsify-style overdraw-aware triangle order
    bool generate_lods = true;
    bool compressed_textures = true;     // use dds/<name>.dds (BC7, made by tools/textures/compress_bc.py) when present
};

bool scene_load_gltf(Scene& scene, const char* path, const SceneLoadOptions& opts);
void scene_build_test(Scene& scene, const SceneLoadOptions& opts);  // procedural fallback/stress scene
void scene_upload(Scene& scene);
void scene_animate(Scene& scene, float time);  // updates world/prev_world
void scene_release(Scene& scene);
bool scene_load_bench_path(Scene& scene, const char* path);
int scene_find_profile(const Scene& scene, const char* name);
