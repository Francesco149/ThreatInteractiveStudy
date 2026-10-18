#include "scene.h"
#include "json.h"
#include <chrono>
#include <stdio.h>
#include <algorithm>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "meshoptimizer/meshoptimizer.h"

// ---------------------------------------------------------------------------------------------
// Vertex packing

static uint32_t pack_unorm_1010102(float3 v, float w) {
    auto q = [](float f) { return (uint32_t)(saturate(f * 0.5f + 0.5f) * 1023.0f + 0.5f); };
    uint32_t a = w < 0 ? 0 : 3;
    return q(v.x) | (q(v.y) << 10) | (q(v.z) << 20) | (a << 30);
}

static uint32_t pack_half2(float2 uv) {
    return (uint32_t)meshopt_quantizeHalf(uv.x) | ((uint32_t)meshopt_quantizeHalf(uv.y) << 16);
}

// ---------------------------------------------------------------------------------------------
// Material profiles (see MaterialProfile in scene.h). Values are linear F0 from measured data
// (Real-Time Rendering 4th ed. table 9.2, Naty Hoffman's "Physics and Math of Shading").

static void add_default_profiles(Scene& s) {
    auto add = [&](const char* name, float3 f0, float metal, float coat = 0, float coat_rough = 0, float sheen = 0,
                   float wrap = 0) { s.profiles.push_back({name, f0, metal, coat, coat_rough, sheen, wrap}); };
    float3 d4 = {0.04f, 0.04f, 0.04f};
    add("default", d4, 0);
    add("stone", d4, 0);
    add("brick", d4, 0);
    add("plaster", d4, 0);
    add("concrete", d4, 0);
    add("wood", d4, 0);
    add("bark", d4, 0, 0, 0, 0, 0.1f);
    add("ceramic", d4, 0, 0.5f, 0.05f);
    add("cloth", d4, 0, 0, 0, 0.6f, 0.25f);
    add("paint", {0.05f, 0.05f, 0.05f}, 0, 0.35f, 0.15f);
    add("iron", {0.56f, 0.57f, 0.58f}, 1);
    add("gold", {1.00f, 0.71f, 0.29f}, 1);
    add("brass", {0.91f, 0.78f, 0.42f}, 1);
    add("copper", {0.95f, 0.64f, 0.54f}, 1);
    add("glass", d4, 0);
    add("skin", {0.028f, 0.028f, 0.028f}, 0);
    add("foliage", {0.03f, 0.03f, 0.03f}, 0, 0, 0, 0.5f);
    add("rubber", {0.03f, 0.03f, 0.03f}, 0);
}

int scene_find_profile(const Scene& s, const char* name) {
    for (size_t i = 0; i < s.profiles.size(); i++)
        if (s.profiles[i].name == name) return (int)i;
    return -1;
}

// "paint_iron" -> pair (paint, iron); "brick" -> pair (brick, brick). Unknown names fall back to default.
static int find_or_add_pair(Scene& s, const std::string& profile, float metallic) {
    int a = 0, b = 0;
    size_t us = profile.find('_');
    if (us != std::string::npos) {
        a = scene_find_profile(s, profile.substr(0, us).c_str());
        b = scene_find_profile(s, profile.substr(us + 1).c_str());
    } else if (!profile.empty()) {
        a = b = scene_find_profile(s, profile.c_str());
    } else {
        // No profile given: plain glTF metal/rough. metallic=1 maps to iron-like metal.
        a = 0;
        b = metallic > 0 ? scene_find_profile(s, "iron") : 0;
    }
    if (a < 0) a = 0;
    if (b < 0) b = a;
    for (size_t i = 0; i < s.profile_pairs.size(); i++)
        if (s.profile_pairs[i].a == a && s.profile_pairs[i].b == b) return (int)i;
    s.profile_pairs.push_back({a, b});
    return (int)s.profile_pairs.size() - 1;
}

// ---------------------------------------------------------------------------------------------
// Textures: decoded with stb_image, mip chain built on the CPU so we control filtering:
// sRGB textures are filtered in linear space, and alpha-tested textures keep their alpha-test
// coverage per mip (Castano, "Computing Alpha Mipmaps"), otherwise foliage thins out at distance.

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

static float alpha_coverage(const std::vector<float>& rgba, int n, float cutoff, float scale) {
    int covered = 0;
    for (int i = 0; i < n; i++) covered += rgba[i * 4 + 3] * scale >= cutoff;
    return (float)covered / n;
}

static Texture create_texture_rgba8(const char* name, const uint8_t* pixels, int w, int h, bool srgb, float alpha_cutoff) {
    // Convert to float (linear for sRGB) once, then build mips by 2x2 box filtering.
    std::vector<std::vector<float>> levels;
    std::vector<float> base((size_t)w * h * 4);
    for (int i = 0; i < w * h * 4; i++) {
        float v = pixels[i] / 255.0f;
        base[i] = (srgb && (i & 3) != 3) ? srgb_to_linear(v) : v;
    }
    levels.push_back(base);
    std::vector<int> ws = {w}, hs = {h};
    float base_coverage = alpha_cutoff > 0 ? alpha_coverage(base, w * h, alpha_cutoff, 1.0f) : 0;
    while (ws.back() > 1 || hs.back() > 1) {
        int pw = ws.back(), ph = hs.back();
        int nw = std::max(1, pw / 2), nh = std::max(1, ph / 2);
        const std::vector<float>& src = levels.back();
        std::vector<float> dst((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    int x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1);
                    int y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
                    dst[(y * nw + x) * 4 + c] = 0.25f * (src[(y0 * pw + x0) * 4 + c] + src[(y0 * pw + x1) * 4 + c] +
                                                         src[(y1 * pw + x0) * 4 + c] + src[(y1 * pw + x1) * 4 + c]);
                }
        if (alpha_cutoff > 0) {
            // Binary search an alpha scale that preserves the base level's coverage.
            float lo = 0, hi = 4, scale = 1;
            for (int it = 0; it < 10; it++) {
                scale = (lo + hi) * 0.5f;
                if (alpha_coverage(dst, nw * nh, alpha_cutoff, scale) < base_coverage) lo = scale; else hi = scale;
            }
            for (int i = 0; i < nw * nh; i++) dst[i * 4 + 3] = std::min(1.0f, dst[i * 4 + 3] * scale);
        }
        levels.push_back(std::move(dst));
        ws.push_back(nw);
        hs.push_back(nh);
    }

    std::vector<std::vector<uint8_t>> bytes(levels.size());
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
    for (size_t l = 0; l < levels.size(); l++) {
        int n = ws[l] * hs[l] * 4;
        bytes[l].resize(n);
        for (int i = 0; i < n; i++) {
            float v = levels[l][i];
            if (srgb && (i & 3) != 3) v = linear_to_srgb(v);
            bytes[l][i] = (uint8_t)(saturate(v) * 255.0f + 0.5f);
        }
        init[l] = {bytes[l].data(), (UINT)ws[l] * 4, 0};
    }

    Texture t;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = (UINT)levels.size();
    d.ArraySize = 1;
    d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    gpu.dev->CreateTexture2D(&d, init.data(), &t.tex);
    set_debug_name(t.tex, name);
    gpu.dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    t.w = w;
    t.h = h;
    t.mips = d.MipLevels;
    t.format = d.Format;
    return t;
}

// Block-compressed textures from a DDS file (DX10 header, written by texconv). Hand-rolled: the
// header is 4 + 124 + 20 bytes, then every mip level's blocks back to back. Returns false if the
// file doesn't exist or isn't a format we handle.
static bool load_dds(const std::string& path, const char* name, Texture* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    fseek(f, 0, SEEK_END);
    data.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (data.size() < 148 || memcmp(data.data(), "DDS ", 4) != 0 || memcmp(&data[84], "DX10", 4) != 0) return false;
    uint32_t height = *(uint32_t*)&data[12], width = *(uint32_t*)&data[16], mips = *(uint32_t*)&data[28];
    DXGI_FORMAT fmt = (DXGI_FORMAT) * (uint32_t*)&data[128];
    uint32_t block_bytes;
    switch (fmt) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: block_bytes = 8; break;
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: block_bytes = 16; break;
    default: return false;
    }
    mips = mips ? mips : 1;
    std::vector<D3D11_SUBRESOURCE_DATA> init(mips);
    size_t offset = 148;
    for (uint32_t m = 0; m < mips; m++) {
        uint32_t w = std::max(1u, width >> m), h = std::max(1u, height >> m);
        uint32_t pitch = std::max(1u, (w + 3) / 4) * block_bytes;
        uint32_t size = pitch * std::max(1u, (h + 3) / 4);
        if (offset + size > data.size()) return false;
        init[m] = {&data[offset], pitch, 0};
        offset += size;
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width;
    d.Height = height;
    d.MipLevels = mips;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(gpu.dev->CreateTexture2D(&d, init.data(), &out->tex))) return false;
    set_debug_name(out->tex, name);
    gpu.dev->CreateShaderResourceView(out->tex, nullptr, &out->srv);
    out->w = width;
    out->h = height;
    out->mips = mips;
    out->format = fmt;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Geometry

struct BuildVertex {
    float3 pos, normal;
    float4 tangent;
    float2 uv;
};

static void compute_tangents(std::vector<BuildVertex>& v, const std::vector<uint32_t>& idx) {
    std::vector<float3> tan(v.size(), {0, 0, 0}), bit(v.size(), {0, 0, 0});
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        BuildVertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        float3 e1 = b.pos - a.pos, e2 = c.pos - a.pos;
        float2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        float r = d1.x * d2.y - d2.x * d1.y;
        r = fabsf(r) < 1e-12f ? 0 : 1.0f / r;
        float3 t = (e1 * d2.y - e2 * d1.y) * r;
        float3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (int k = 0; k < 3; k++) { tan[idx[i + k]] += t; bit[idx[i + k]] += bt; }
    }
    for (size_t i = 0; i < v.size(); i++) {
        float3 n = v[i].normal;
        float3 t = normalize(tan[i] - n * dot(n, tan[i]));
        if (length(t) < 0.5f) t = normalize(cross(n, fabsf(n.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}));
        float w = dot(cross(n, t), bit[i]) < 0 ? -1.0f : 1.0f;
        v[i].tangent = {t.x, t.y, t.z, w};
    }
}

// Appends one primitive's geometry: optimizes triangle order for the post-transform vertex cache
// and for overdraw (meshoptimizer's implementations of Forsyth/Tipsify-style reordering), then
// builds simplified LODs. The LODs share the vertex range, only indices differ.
static Primitive add_primitive(Scene& s, std::vector<BuildVertex>& verts, std::vector<uint32_t>& idx, int material,
                               const SceneLoadOptions& opts) {
    Primitive p;
    p.material = material;
    size_t vcount = verts.size();

    if (opts.optimize_vertex_cache) meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), vcount);
    if (opts.optimize_overdraw)
        meshopt_optimizeOverdraw(idx.data(), idx.data(), idx.size(), &verts[0].pos.x, vcount, sizeof(BuildVertex), 1.05f);
    std::vector<BuildVertex> fetch(vcount);
    vcount = meshopt_optimizeVertexFetch(fetch.data(), idx.data(), idx.size(), verts.data(), vcount, sizeof(BuildVertex));
    fetch.resize(vcount);

    p.base_vertex = (int)s.positions.size();
    for (const BuildVertex& v : fetch) {
        s.positions.push_back(v.pos);
        s.attribs.push_back({pack_unorm_1010102(v.normal, 0), pack_unorm_1010102({v.tangent.x, v.tangent.y, v.tangent.z}, v.tangent.w),
                             pack_half2(v.uv)});
        p.bounds.add(v.pos);
    }

    auto push_lod = [&](const std::vector<uint32_t>& lod_idx, float screen_size) {
        MeshLod& l = p.lods[p.lod_count++];
        l.first_index = (uint32_t)s.indices.size();
        l.index_count = (uint32_t)lod_idx.size();
        l.screen_size = screen_size;
        s.indices.insert(s.indices.end(), lod_idx.begin(), lod_idx.end());
    };
    p.lod_count = 0;
    push_lod(idx, 1e9f);

    if (opts.generate_lods && idx.size() >= 3 * 256) {
        // Each LOD halves the triangle count; it is used once the object covers less of the screen.
        const float ratios[] = {0.5f, 0.25f, 0.12f};
        const float screen_sizes[] = {0.25f, 0.12f, 0.05f};  // fraction of screen height (bounding sphere diameter)
        float3 ext = p.bounds.extent();
        float scale = std::max(ext.x, std::max(ext.y, ext.z)) * 2;
        for (int i = 0; i < 3; i++) {
            std::vector<uint32_t> lod(idx.size());
            size_t target = (size_t)(idx.size() * ratios[i]) / 3 * 3;
            float err = 0;
            size_t n = meshopt_simplify(lod.data(), idx.data(), idx.size(), &fetch[0].pos.x, vcount, sizeof(BuildVertex),
                                        target, 0.05f, 0, &err);
            lod.resize(n);
            if (n == 0 || n > idx.size() * 0.8f || err * scale > 0.5f) break;
            if (opts.optimize_vertex_cache) meshopt_optimizeVertexCache(lod.data(), lod.data(), lod.size(), vcount);
            push_lod(lod, screen_sizes[i]);
        }
    }
    s.triangle_count += idx.size() / 3;
    return p;
}

// ---------------------------------------------------------------------------------------------
// glTF

static float4x4 from_gltf_matrix(const float* m) {  // glTF is column-major
    float4x4 r;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r.m[row][col] = m[col * 4 + row];
    return r;
}

static Json read_extras(cgltf_data* data, const cgltf_extras& extras) {
    Json j;
    size_t size = 0;
    if (cgltf_copy_extras_json(data, &extras, nullptr, &size) != cgltf_result_success || size == 0) return j;
    std::string buf(size, '\0');
    cgltf_copy_extras_json(data, &extras, buf.data(), &size);
    json_parse(buf.c_str(), strlen(buf.c_str()), j);
    return j;
}

bool scene_load_gltf(Scene& s, const char* path, const SceneLoadOptions& opts) {
    auto t0 = std::chrono::high_resolution_clock::now();
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        log_msg("Failed to parse %s", path);
        return false;
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        log_msg("Failed to load buffers for %s", path);
        cgltf_free(data);
        return false;
    }
    s.path = path;
    add_default_profiles(s);
    std::string dir = path;
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);

    // Which images are color data (sRGB) and which are alpha-tested (coverage-preserving mips)?
    std::vector<bool> img_srgb(data->images_count, false);
    std::vector<float> img_cutoff(data->images_count, 0.0f);
    auto img_index = [&](const cgltf_texture_view& v) -> int {
        return v.texture && v.texture->image ? (int)(v.texture->image - data->images) : -1;
    };
    for (size_t i = 0; i < data->materials_count; i++) {
        cgltf_material& m = data->materials[i];
        int a = img_index(m.pbr_metallic_roughness.base_color_texture);
        int e = img_index(m.emissive_texture);
        if (a >= 0) {
            img_srgb[a] = true;
            if (m.alpha_mode == cgltf_alpha_mode_mask) img_cutoff[a] = m.alpha_cutoff;
        }
        if (e >= 0) img_srgb[e] = true;
    }

    for (size_t i = 0; i < data->images_count; i++) {
        cgltf_image& img = data->images[i];
        int w = 0, h = 0, n = 0;
        uint8_t* pixels = nullptr;
        if (img.uri && opts.compressed_textures) {
            // Pre-compressed BC7 version (same name, .dds, in a dds/ folder next to the glTF)?
            std::string uri = img.uri;
            cgltf_decode_uri(&uri[0]);
            uri = uri.c_str();
            std::string stem = uri.substr(uri.find_last_of("/\\") + 1);
            stem = stem.substr(0, stem.find_last_of('.'));
            Texture t;
            if (load_dds(dir + "dds/" + stem + ".dds", img.uri, &t)) {
                s.textures.push_back(t);
                s.texture_srgb.push_back(img_srgb[i]);
                s.compressed_texture_count++;
                continue;
            }
        }
        if (img.uri) {
            std::string file = dir + img.uri;
            cgltf_decode_uri(&file[dir.size()]);
            file = file.c_str();
            pixels = stbi_load(file.c_str(), &w, &h, &n, 4);
        } else if (img.buffer_view) {
            const uint8_t* bytes = (const uint8_t*)img.buffer_view->buffer->data + img.buffer_view->offset;
            pixels = stbi_load_from_memory(bytes, (int)img.buffer_view->size, &w, &h, &n, 4);
        }
        if (!pixels) {
            log_msg("Failed to load image %s", img.uri ? img.uri : "(embedded)");
            uint8_t white[4] = {255, 255, 255, 255};
            s.textures.push_back(create_texture_rgba8("missing", white, 1, 1, false, 0));
        } else {
            s.textures.push_back(create_texture_rgba8(img.uri ? img.uri : "embedded", pixels, w, h, img_srgb[i], img_cutoff[i]));
            stbi_image_free(pixels);
        }
        s.texture_srgb.push_back(img_srgb[i]);
    }

    for (size_t i = 0; i < data->materials_count; i++) {
        cgltf_material& gm = data->materials[i];
        Material m;
        m.name = gm.name ? gm.name : "material";
        const cgltf_pbr_metallic_roughness& pbr = gm.pbr_metallic_roughness;
        m.base_color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
        m.roughness = pbr.roughness_factor;
        m.metallic = pbr.metallic_factor;
        m.tex_albedo = img_index(pbr.base_color_texture);
        m.tex_mr = img_index(pbr.metallic_roughness_texture);
        m.tex_normal = img_index(gm.normal_texture);
        m.tex_ao = img_index(gm.occlusion_texture);
        m.tex_emissive = img_index(gm.emissive_texture);
        float strength = gm.has_emissive_strength ? gm.emissive_strength.emissive_strength : 1.0f;
        m.emissive = float3{gm.emissive_factor[0], gm.emissive_factor[1], gm.emissive_factor[2]} * strength;
        m.alpha_mode = gm.alpha_mode == cgltf_alpha_mode_mask ? ALPHA_MASK : gm.alpha_mode == cgltf_alpha_mode_blend ? ALPHA_BLEND : ALPHA_OPAQUE;
        m.alpha_cutoff = gm.alpha_cutoff;
        m.double_sided = gm.double_sided;
        Json extras = read_extras(data, gm.extras);
        std::string sm = extras.string("shading_model", "default");
        m.shading_model = sm == "skin" ? SM_SKIN : sm == "foliage" ? SM_FOLIAGE : sm == "eye" ? SM_EYE : SM_DEFAULT;
        if (sm == "glass") m.alpha_mode = ALPHA_BLEND;
        m.profile_pair = find_or_add_pair(s, extras.string("profile"), m.metallic);
        s.materials.push_back(m);
    }
    if (s.materials.empty()) s.materials.push_back(Material{});
    if (s.profiles.empty()) add_default_profiles(s);

    // Meshes: one Scene::Mesh per glTF mesh, shared by every node that references it.
    for (size_t mi = 0; mi < data->meshes_count; mi++) {
        cgltf_mesh& gmesh = data->meshes[mi];
        Mesh mesh;
        mesh.name = gmesh.name ? gmesh.name : "mesh";
        for (size_t pi = 0; pi < gmesh.primitives_count; pi++) {
            cgltf_primitive& prim = gmesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *tan = nullptr, *uv = nullptr;
            for (size_t a = 0; a < prim.attributes_count; a++) {
                cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                if (at.type == cgltf_attribute_type_tangent) tan = at.data;
                if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
            }
            if (!pos) continue;
            std::vector<BuildVertex> verts(pos->count);
            for (size_t v = 0; v < pos->count; v++) {
                BuildVertex& bv = verts[v];
                cgltf_accessor_read_float(pos, v, &bv.pos.x, 3);
                bv.normal = {0, 1, 0};
                if (nrm) cgltf_accessor_read_float(nrm, v, &bv.normal.x, 3);
                bv.uv = {0, 0};
                if (uv) cgltf_accessor_read_float(uv, v, &bv.uv.x, 2);
                bv.tangent = {1, 0, 0, 1};
                if (tan) cgltf_accessor_read_float(tan, v, &bv.tangent.x, 4);
            }
            std::vector<uint32_t> idx;
            if (prim.indices) {
                idx.resize(prim.indices->count);
                for (size_t k = 0; k < idx.size(); k++) idx[k] = (uint32_t)cgltf_accessor_read_index(prim.indices, k);
            } else {
                idx.resize(pos->count);
                for (size_t k = 0; k < idx.size(); k++) idx[k] = (uint32_t)k;
            }
            if (!tan) compute_tangents(verts, idx);
            int material = prim.material ? (int)(prim.material - data->materials) : 0;
            Primitive p = add_primitive(s, verts, idx, material, opts);
            mesh.bounds.add(p.bounds.lo);
            mesh.bounds.add(p.bounds.hi);
            mesh.prims.push_back(p);
        }
        s.meshes.push_back(mesh);
    }

    // Nodes: instances, lights, VFX emitters.
    for (size_t ni = 0; ni < data->nodes_count; ni++) {
        cgltf_node& node = data->nodes[ni];
        float m[16];
        cgltf_node_transform_world(&node, m);
        float4x4 world = from_gltf_matrix(m);
        Json extras = read_extras(data, node.extras);
        if (node.mesh) {
            Instance inst;
            inst.name = node.name ? node.name : "node";
            inst.mesh = (int)(node.mesh - data->meshes);
            inst.rest_world = inst.world = inst.prev_world = world;
            if (extras.num("no_shadow") != 0) inst.flags |= INST_NO_SHADOW;
            if (extras.num("wind") != 0) inst.flags |= INST_WIND;
            std::string anim = extras.string("anim");
            inst.anim = anim == "rotate" ? ANIM_ROTATE : anim == "swing" ? ANIM_SWING : anim == "slide" ? ANIM_SLIDE : ANIM_NONE;
            if (const Json* axis = extras.get("anim_axis"); axis && axis->items.size() == 3)
                inst.anim_axis = {(float)axis->items[0].number, (float)axis->items[1].number, (float)axis->items[2].number};
            inst.anim_speed = (float)extras.num("anim_speed", 1.0);
            inst.anim_amp = (float)extras.num("anim_amp", 0.5);
            s.instances.push_back(inst);
        }
        if (node.light) {
            cgltf_light& gl = *node.light;
            Light l;
            l.name = node.name ? node.name : "light";
            l.type = gl.type == cgltf_light_type_directional ? LIGHT_DIRECTIONAL : gl.type == cgltf_light_type_spot ? LIGHT_SPOT : LIGHT_POINT;
            l.position = transform_point(world, {0, 0, 0});
            l.direction = normalize(transform_dir(world, {0, 0, -1}));
            l.color = {gl.color[0], gl.color[1], gl.color[2]};
            l.intensity = gl.intensity;
            l.range = gl.range > 0 ? gl.range : 10.0f;
            l.inner_cone = gl.spot_inner_cone_angle;
            l.outer_cone = gl.spot_outer_cone_angle;
            l.shadow = extras.num("shadow") != 0;
            s.lights.push_back(l);
        }
        std::string decal = extras.string("decal");
        if (!decal.empty()) {
            int kind = decal == "moss" ? 1 : decal == "cracks" ? 2 : 0;
            s.decals.push_back({world, kind, {1, 1, 1, (float)extras.num("opacity", 1.0)}});
        }
        std::string vfx = extras.string("vfx");
        if (!vfx.empty()) s.emitters.push_back({vfx, transform_point(world, {0, 0, 0})});
    }
    cgltf_free(data);

    s.load_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    log_msg("Loaded %s: %zu meshes, %zu instances, %zu materials, %zu textures (%d BC-compressed), %zu lights, %llu tris, %.0f ms",
            path, s.meshes.size(), s.instances.size(), s.materials.size(), s.textures.size(), s.compressed_texture_count,
            s.lights.size(), (unsigned long long)s.triangle_count, s.load_ms);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Procedural test scene (used when no glTF is available, and for synthetic stress tests).

static void make_box(std::vector<BuildVertex>& v, std::vector<uint32_t>& idx, float3 lo, float3 hi, float uv_scale) {
    float3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
        float3 N = n[f];
        float3 U = fabsf(N.y) > 0.5f ? float3{1, 0, 0} : float3{0, 1, 0};
        float3 T = cross(U, N);
        uint32_t base = (uint32_t)v.size();
        float3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
        for (int k = 0; k < 4; k++) {
            float su = (k == 1 || k == 2) ? 1.f : -1.f, sv = (k >= 2) ? 1.f : -1.f;
            float3 p = c + float3{N.x * e.x, N.y * e.y, N.z * e.z} + float3{T.x * e.x, T.y * e.y, T.z * e.z} * su +
                       float3{U.x * e.x, U.y * e.y, U.z * e.z} * sv;
            float2 uv = {(su * 0.5f + 0.5f) * uv_scale * length(float3{T.x * e.x, T.y * e.y, T.z * e.z}) * 2,
                         (sv * 0.5f + 0.5f) * uv_scale * length(float3{U.x * e.x, U.y * e.y, U.z * e.z}) * 2};
            v.push_back({p, N, {T.x, T.y, T.z, 1}, uv});
        }
        uint32_t q[6] = {0, 1, 2, 0, 2, 3};  // counter-clockwise seen from outside
        for (uint32_t i : q) idx.push_back(base + i);
    }
}

static void make_sphere(std::vector<BuildVertex>& v, std::vector<uint32_t>& idx, float r, int seg) {
    for (int y = 0; y <= seg; y++)
        for (int x = 0; x <= seg * 2; x++) {
            float th = PI * y / seg, ph = 2 * PI * x / (seg * 2);
            float3 n = {sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph)};
            v.push_back({n * r, n, {-sinf(ph), 0, cosf(ph), 1}, {(float)x / (seg * 2) * 4, (float)y / seg * 2}});
        }
    int row = seg * 2 + 1;
    for (int y = 0; y < seg; y++)
        for (int x = 0; x < seg * 2; x++) {
            uint32_t a = y * row + x, b = a + 1, c = a + row, d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
}

// Grass clump: crossed alpha-tested cards.
static void make_grass_clump(std::vector<BuildVertex>& v, std::vector<uint32_t>& idx, int cards, float size, uint32_t seed) {
    for (int c = 0; c < cards; c++) {
        float a = (c + (hash_u32(seed + c) & 255) / 255.0f) * PI / cards;
        float3 d = {cosf(a) * size * 0.5f, 0, sinf(a) * size * 0.5f};
        float3 n = normalize(float3{-d.z, 0.3f, d.x});
        uint32_t base = (uint32_t)v.size();
        v.push_back({-d, n, {1, 0, 0, 1}, {0, 1}});
        v.push_back({d, n, {1, 0, 0, 1}, {1, 1}});
        v.push_back({d + float3{0, size * 0.8f, 0}, n, {1, 0, 0, 1}, {1, 0}});
        v.push_back({-d + float3{0, size * 0.8f, 0}, n, {1, 0, 0, 1}, {0, 0}});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

static int add_solid_texture(Scene& s, const char* name, uint8_t r, uint8_t g, uint8_t b, bool srgb) {
    uint8_t px[4] = {r, g, b, 255};
    s.textures.push_back(create_texture_rgba8(name, px, 1, 1, srgb, 0));
    s.texture_srgb.push_back(srgb);
    return (int)s.textures.size() - 1;
}

static int add_grass_texture(Scene& s) {
    const int N = 256;
    std::vector<uint8_t> px(N * N * 4);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = x / (float)N, vv = y / (float)N;
            // A few tapered blades.
            float a = 0;
            for (int b = 0; b < 7; b++) {
                float cx = (b + 0.5f) / 7 + 0.05f * sinf(b * 3.1f);
                float width = 0.035f * (vv * 0.9f + 0.1f);
                float bend = (1 - vv) * (1 - vv) * 0.08f * ((b & 1) ? 1 : -1);
                float top = 0.1f + 0.3f * ((hash_u32(b * 7 + 1) & 255) / 255.0f);
                if (vv > top && fabsf(u - cx - bend) < width) a = 1;
            }
            uint8_t* p = &px[(y * N + x) * 4];
            float g = 0.45f + 0.35f * vv;
            p[0] = (uint8_t)(70 * g); p[1] = (uint8_t)(160 * g); p[2] = (uint8_t)(40 * g); p[3] = a > 0 ? 255 : 0;
        }
    s.textures.push_back(create_texture_rgba8("grass", px.data(), N, N, true, 0.5f));
    s.texture_srgb.push_back(true);
    return (int)s.textures.size() - 1;
}

void scene_build_test(Scene& s, const SceneLoadOptions& opts) {
    s.path = "procedural test scene";
    add_default_profiles(s);
    int white = add_solid_texture(s, "white", 200, 200, 200, true);
    int red = add_solid_texture(s, "red", 180, 40, 30, true);
    int grass_tex = add_grass_texture(s);

    auto add_mat = [&](const char* name, int albedo, float rough, const char* profile, ShadingModel sm, AlphaMode am) {
        Material m;
        m.name = name;
        m.tex_albedo = albedo;
        m.roughness = rough;
        m.shading_model = sm;
        m.alpha_mode = am;
        m.double_sided = am == ALPHA_MASK;
        m.profile_pair = find_or_add_pair(s, profile, 0);
        s.materials.push_back(m);
        return (int)s.materials.size() - 1;
    };
    int m_ground = add_mat("ground", white, 0.8f, "concrete", SM_DEFAULT, ALPHA_OPAQUE);
    int m_wall = add_mat("wall", red, 0.6f, "brick", SM_DEFAULT, ALPHA_OPAQUE);
    int m_metal = add_mat("metal", white, 0.3f, "paint_iron", SM_DEFAULT, ALPHA_OPAQUE);
    int m_skin = add_mat("skin", red, 0.5f, "skin", SM_SKIN, ALPHA_OPAQUE);
    int m_grass = add_mat("grass", grass_tex, 0.7f, "foliage", SM_FOLIAGE, ALPHA_MASK);
    int m_glass = add_mat("glass", white, 0.05f, "glass", SM_DEFAULT, ALPHA_BLEND);
    s.materials[m_glass].base_color = {0.6f, 0.8f, 0.75f, 0.25f};

    auto add_mesh = [&](const char* name, std::vector<BuildVertex>& v, std::vector<uint32_t>& i, int mat) {
        Mesh m;
        m.name = name;
        Primitive p = add_primitive(s, v, i, mat, opts);
        m.bounds = p.bounds;
        m.prims.push_back(p);
        s.meshes.push_back(m);
        return (int)s.meshes.size() - 1;
    };
    auto add_inst = [&](const char* name, int mesh, float4x4 world, uint32_t flags) {
        Instance in;
        in.name = name;
        in.mesh = mesh;
        in.rest_world = in.world = in.prev_world = world;
        in.flags = flags;
        s.instances.push_back(in);
        return &s.instances.back();
    };

    std::vector<BuildVertex> v;
    std::vector<uint32_t> idx;
    make_box(v, idx, {-50, -0.5f, -50}, {50, 0, 50}, 0.5f);
    int ground = add_mesh("ground", v, idx, m_ground);
    v.clear(); idx.clear();
    make_box(v, idx, {-4, 0, -0.5f}, {4, 6, 0.5f}, 1.0f);
    int wall = add_mesh("wall", v, idx, m_wall);
    v.clear(); idx.clear();
    make_sphere(v, idx, 1.0f, 48);
    int sphere = add_mesh("sphere", v, idx, m_metal);
    v.clear(); idx.clear();   // add_primitive remaps idx in place: rebuild before reusing
    make_sphere(v, idx, 1.0f, 48);
    int head = add_mesh("head", v, idx, m_skin);
    v.clear(); idx.clear();
    make_grass_clump(v, idx, 6, 0.8f, 1);
    int grass = add_mesh("grass", v, idx, m_grass);

    add_inst("ground", ground, identity(), INST_NO_SHADOW);
    for (int i = 0; i < 6; i++)
        add_inst("wall", wall, translation({-25.0f + i * 10.0f, 0, -12.0f}) * rotation_axis({0, 1, 0}, i * 0.3f), 0);
    for (int i = 0; i < 5; i++) {
        Instance* in = add_inst("sphere", sphere, translation({-8.0f + i * 4.0f, 1.0f, 0}), 0);
        if (i == 2) { in->anim = ANIM_SLIDE; in->anim_axis = {1, 0, 0}; in->anim_amp = 3; in->anim_speed = 1.5f; }
    }
    add_inst("head", head, translation({0, 1.5f, 4}) * scaling({0.6f, 0.8f, 0.6f}), 0);
    v.clear(); idx.clear();
    make_box(v, idx, {-1.5f, 0, -0.05f}, {1.5f, 2.5f, 0.05f}, 1.0f);
    int pane = add_mesh("glass_pane", v, idx, m_glass);
    add_inst("glass", pane, translation({6, 0, 3}) * rotation_axis({0, 1, 0}, 0.4f), 0);
    for (int i = 0; i < 6; i++) {
        float4x4 w = translation({-10.0f + i * 4.0f, 0, 1.5f}) * rotation_axis({0, 1, 0}, i * 1.1f) * scaling({2.5f, 1.0f, 2.5f});
        s.decals.push_back({w, i % 3, {1, 1, 1, 1}});
    }
    s.emitters.push_back({"smoke", {8, 0.5f, -6}});
    s.emitters.push_back({"fire", {-6, 0.2f, 2}});
    s.emitters.push_back({"spray", {-3, 0.3f, 6}});
    // Pillars near the shadowed lights (local shadow tests, docs/techniques/shadows.md).
    v.clear(); idx.clear();
    make_box(v, idx, {-0.25f, 0, -0.25f}, {0.25f, 3.0f, 0.25f}, 1.0f);
    int pillar = add_mesh("pillar", v, idx, m_metal);
    add_inst("pillar", pillar, translation({-15.5f, 0, 0.5f}), 0);
    add_inst("pillar", pillar, translation({-12.0f, 0, 4.5f}), 0);
    add_inst("pillar", pillar, translation({7.0f, 0, -3.0f}), 0);
    for (int z = 0; z < 40; z++)
        for (int x = 0; x < 40; x++) {
            uint32_t h = hash_u32(z * 40 + x);
            float3 p = {-20.0f + x * 1.0f + (h & 255) / 512.0f, 0, 6.0f + z * 1.0f + ((h >> 8) & 255) / 512.0f};
            add_inst("grass", grass, translation(p) * rotation_axis({0, 1, 0}, (h >> 16) * 0.001f), INST_WIND | INST_NO_SHADOW);
        }

    // GI light-leak test room (research note 05, bookmark S4): a closed white box with 0.3 m walls,
    // thinner than the probe spacing, and one door facing the sun. Outside the default view.
    {
        const float3 boxes[][2] = {
            {{30.0f, 0, -6.0f}, {30.3f, 4, 0}},         // -X wall
            {{35.7f, 0, -6.0f}, {36.0f, 4, 0}},         // +X wall
            {{30.3f, 0, -6.0f}, {35.7f, 4, -5.7f}},     // back wall
            {{30.3f, 0, -0.3f}, {32.4f, 4, 0}},         // front wall, left of the door
            {{33.6f, 0, -0.3f}, {35.7f, 4, 0}},         // front wall, right of the door
            {{32.4f, 2.5f, -0.3f}, {33.6f, 4, 0}},      // lintel
            {{30.0f, 4, -6.0f}, {36.0f, 4.3f, 0}},      // roof
        };
        for (const auto& b : boxes) {
            v.clear(); idx.clear();
            make_box(v, idx, b[0], b[1], 1.0f);
            add_inst("leak_room", add_mesh("leak_room", v, idx, m_ground), identity(), 0);
        }
    }

    Light sun;
    sun.name = "sun";
    sun.type = LIGHT_DIRECTIONAL;
    sun.direction = normalize(float3{-0.4f, -0.5f, -0.6f});
    sun.color = {1.0f, 0.85f, 0.7f};
    sun.intensity = 8.0f;
    sun.shadow = true;
    s.lights.push_back(sun);
    for (int i = 0; i < 8; i++) {
        Light l;
        l.name = "point";
        l.type = LIGHT_POINT;
        l.position = {-14.0f + i * 4.0f, 2.0f, 2.0f};
        l.color = {1.0f, 0.6f + 0.05f * i, 0.3f};
        l.intensity = 40;
        l.range = 8;
        l.shadow = i < 2;
        s.lights.push_back(l);
    }
    // Shadowed spot lights: one close above the head, one lighting the spheres toward the walls.
    Light spot;
    spot.name = "spot";
    spot.type = LIGHT_SPOT;
    spot.position = {3.0f, 5.0f, 7.5f};
    spot.direction = normalize(float3{-0.5f, 0.0f, 2.5f} - spot.position);
    spot.color = {0.7f, 0.85f, 1.0f};
    spot.intensity = 150;
    spot.range = 14;
    spot.inner_cone = 0.35f;
    spot.outer_cone = 0.5f;
    spot.shadow = true;
    s.lights.push_back(spot);
    spot.position = {9.0f, 6.0f, 5.0f};
    spot.direction = normalize(float3{5.0f, 0.0f, -6.0f} - spot.position);
    spot.color = {1.0f, 0.95f, 0.85f};
    spot.intensity = 300;
    spot.range = 22;
    spot.inner_cone = 0.3f;
    spot.outer_cone = 0.45f;
    s.lights.push_back(spot);
    // The first viewpoint is the old default start position.
    s.viewpoints.push_back({"overview", {0, 5.75f, 18}, {0, 2.75f, 0}});
    s.viewpoints.push_back({"point shadows", {-7.5f, 3.0f, 10.0f}, {-11.5f, 0.5f, 1.0f}});
    s.viewpoints.push_back({"inside point light", {-10.0f, 1.7f, 6.0f}, {-11.0f, 0.5f, -1.0f}});
    s.viewpoints.push_back({"spot", {5.0f, 3.0f, 13.0f}, {0.0f, 0.5f, 3.0f}});
    s.viewpoints.push_back({"leak room", {33.0f, 1.7f, 6.0f}, {33.0f, 1.2f, -3.0f}});   // GI leak test, through the door
    log_msg("Built procedural test scene: %zu instances, %llu tris", s.instances.size(), (unsigned long long)s.triangle_count);
}

// ---------------------------------------------------------------------------------------------
// Wind sway (wind_offset() in shaders/geometry.hlsli): an instance bends along a fixed direction
// by sway(t) * bend * h², h = object-space height above its origin, |sway(t)| <= 0.85, plus a
// leaf flutter of at most 0.0057 h. A bend of 0.04 suits plants up to a couple of metres; on an
// 11 m tree it would move the crown by 4.4 m. Taller meshes get a smaller bend instead, so that
// their top sways no more than WIND_TOP_AMP, and the bounds include the sway.

static const float WIND_BEND = 0.04f;     // 1/m: grass, flowers, bushes (a 2 m bush: 14 cm at the top)
static const float WIND_TOP_AMP = 0.25f;  // m: largest top amplitude (peak 0.85 x 0.25 = 21 cm)

static float wind_bend(const Mesh& m) {
    float h = fmaxf(m.bounds.hi.y, 0.0f);
    return WIND_BEND * h * h > WIND_TOP_AMP ? WIND_TOP_AMP / (h * h) : WIND_BEND;
}

// World bounds of an instance, grown by the farthest its wind sway can move a vertex, so that
// culling and the local shadow fit stay conservative for swaying objects.
static AABB instance_bounds(const Scene& s, const Instance& in) {
    const Mesh& m = s.meshes[in.mesh];
    AABB b = transform_aabb(in.world, m.bounds);
    if (in.flags & INST_WIND) {
        float h = fmaxf(m.bounds.hi.y, 0.0f);
        float reach = 0.85f * in.wind_bend * h * h + 0.0057f * h;
        b.lo = b.lo - float3{reach, reach, reach};
        b.hi = b.hi + float3{reach, reach, reach};
    }
    return b;
}

// ---------------------------------------------------------------------------------------------

struct GpuMaterial {  // must match shaders/common.hlsli MaterialData
    float4 base_color;
    float3 emissive;
    float roughness;
    float metallic;
    float alpha_cutoff;
    uint32_t shading_model;
    uint32_t profile_pair;
    uint32_t flags;  // 1 = has albedo tex, 2 = normal, 4 = mr, 8 = ao, 16 = emissive, 32 = alpha mask
    uint32_t pad[3];
};

struct GpuProfile {  // must match shaders/common.hlsli ProfileData
    float3 f0;
    float metal;
    float clearcoat;
    float clearcoat_rough;
    float sheen;
    float diffuse_wrap;
};

void scene_upload(Scene& s) {
    s.vb_position = create_buffer(s.positions.data(), (uint32_t)(s.positions.size() * sizeof(float3)), D3D11_BIND_VERTEX_BUFFER);
    s.vb_attribs = create_buffer(s.attribs.data(), (uint32_t)(s.attribs.size() * sizeof(VertexAttribs)), D3D11_BIND_VERTEX_BUFFER);
    s.ib = create_buffer(s.indices.data(), (uint32_t)(s.indices.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER);

    std::vector<GpuMaterial> mats;
    for (const Material& m : s.materials) {
        GpuMaterial g = {};
        g.base_color = m.base_color;
        g.emissive = m.emissive;
        g.roughness = m.roughness;
        g.metallic = m.metallic;
        g.alpha_cutoff = m.alpha_cutoff;
        g.shading_model = m.shading_model;
        g.profile_pair = m.profile_pair;
        g.flags = (m.tex_albedo >= 0 ? 1 : 0) | (m.tex_normal >= 0 ? 2 : 0) | (m.tex_mr >= 0 ? 4 : 0) |
                  (m.tex_ao >= 0 ? 8 : 0) | (m.tex_emissive >= 0 ? 16 : 0) | (m.alpha_mode == ALPHA_MASK ? 32 : 0);
        mats.push_back(g);
    }
    s.material_buffer = create_structured_buffer(sizeof(GpuMaterial), (uint32_t)mats.size(), mats.data(), false, false,
                                                 &s.material_srv, nullptr);

    // Profile pairs are flattened: entry 2*i = profile a, 2*i+1 = profile b.
    std::vector<GpuProfile> profs;
    for (const ProfilePair& p : s.profile_pairs) {
        for (int k = 0; k < 2; k++) {
            const MaterialProfile& mp = s.profiles[k == 0 ? p.a : p.b];
            profs.push_back({mp.f0, mp.metal, mp.clearcoat, mp.clearcoat_rough, mp.sheen, mp.diffuse_wrap});
        }
    }
    if (profs.empty()) profs.push_back({{0.04f, 0.04f, 0.04f}, 0, 0, 0, 0, 0});
    s.profile_buffer = create_structured_buffer(sizeof(GpuProfile), (uint32_t)profs.size(), profs.data(), false, false,
                                                &s.profile_srv, nullptr);

    s.bounds = AABB{};
    for (Instance& in : s.instances) {
        if (in.flags & INST_WIND) in.wind_bend = wind_bend(s.meshes[in.mesh]);
        in.world_bounds = instance_bounds(s, in);
        in.radius = length(transform_aabb(in.world, s.meshes[in.mesh].bounds).extent());   // without the sway
        s.bounds.add(in.world_bounds.lo);
        s.bounds.add(in.world_bounds.hi);
    }
}

void scene_animate(Scene& s, float time) {
    for (Instance& in : s.instances) {
        in.prev_world = in.world;
        if (in.anim == ANIM_NONE) continue;
        float3 origin = transform_point(in.rest_world, {0, 0, 0});
        float4x4 to_origin = translation(-origin), back = translation(origin);
        if (in.anim == ANIM_ROTATE)
            in.world = back * rotation_axis(normalize(in.anim_axis), time * in.anim_speed) * to_origin * in.rest_world;
        else if (in.anim == ANIM_SWING)
            in.world = back * rotation_axis(normalize(in.anim_axis), sinf(time * in.anim_speed) * in.anim_amp) * to_origin * in.rest_world;
        else if (in.anim == ANIM_SLIDE)
            in.world = translation(normalize(in.anim_axis) * (sinf(time * in.anim_speed) * in.anim_amp)) * in.rest_world;
        in.world_bounds = instance_bounds(s, in);
    }
}

bool scene_load_bench_path(Scene& s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json j;
    if (!json_parse(text.c_str(), text.size(), j)) return false;
    auto vec3 = [](const Json* a) {
        return a && a->items.size() == 3 ? float3{(float)a->items[0].number, (float)a->items[1].number, (float)a->items[2].number}
                                         : float3{0, 0, 0};
    };
    s.bench_duration = (float)j.num("duration", 30);
    if (const Json* keys = j.get("keys"))
        for (const Json& k : keys->items) s.bench_path.push_back({(float)k.num("t"), vec3(k.get("pos")), vec3(k.get("target"))});
    if (const Json* vps = j.get("viewpoints"))
        for (const Json& v : vps->items) s.viewpoints.push_back({v.string("name"), vec3(v.get("pos")), vec3(v.get("target"))});
    log_msg("Bench path: %zu keys, %zu viewpoints", s.bench_path.size(), s.viewpoints.size());
    return true;
}

void scene_release(Scene& s) {
    for (Texture& t : s.textures) release_texture(t);
    SAFE_RELEASE(s.vb_position);
    SAFE_RELEASE(s.vb_attribs);
    SAFE_RELEASE(s.ib);
    SAFE_RELEASE(s.material_buffer);
    SAFE_RELEASE(s.material_srv);
    SAFE_RELEASE(s.profile_buffer);
    SAFE_RELEASE(s.profile_srv);
    s = Scene{};
}
