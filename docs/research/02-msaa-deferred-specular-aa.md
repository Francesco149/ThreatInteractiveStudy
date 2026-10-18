# R2: 2x MSAA deferred shading in D3D11, the sample-frequency stencil, and specular anti-aliasing

Scope: the MSAA and specular-AA parts of the pipeline from Threat Interactive's "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering" (2026-08-01). Target: D3D11, FL 11_0/11_1, SM 5.0, no vendor SDKs. 1080p, 60 fps, RTX 5060 8 GB. Educational proof of concept, so simple code wins.

Code in this note is written for this project. It implements formulas from the cited sources, but it is not copied from them. Treat it as close to final pseudocode: it has not been compiled.

---

## 0. Decisions at a glance

| Topic | Decision | Why |
|---|---|---|
| MSAA mode | 2x, `SampleDesc = {2, D3D11_STANDARD_MULTISAMPLE_PATTERN}` on every MSAA resource | SMAA S2x assumes the standard 2x sample order. The standard pattern makes `GetSamplePosition` deterministic. (The engine's current `create_texture` leaves `Quality = 0`, so this needs changing.) |
| Shading-model bits in stencil | Written by the **base pass**: `StencilPassOp = REPLACE`, `StencilRef = model << 2` set per draw | Stencil is stored per sample, so this produces a sample-frequency stencil with no extra pass. `SV_StencilRef` does not exist on any NVIDIA GPU (section 2). |
| "Needs per-sample shading" bit (EDGE) | A G-buffer resolve pass: full-screen triangle at pixel frequency. It reads per-sample G-buffer data and `discard`s simple pixels. Stencil REPLACE with write mask = EDGE writes the bit. Option: tag the whole 2x2 quad (Crysis 3). | Same structure as Lauritzen 2010 and Crysis 3. Stencil culling beats in-shader branching. |
| Edge detection | Primary (the video's idea): base pass writes a per-sample bit, `coverage != full && !explainedByFlatSharedEdges`, plus a normal-difference test in the resolve to catch intersections. Baselines for A/B: Lauritzen/Crysis sample comparison, and raw `SV_Coverage`. | Section 4. |
| Flat shared edges | Bake 3 bits per triangle offline ("edge k is shared with a coplanar triangle of the same mesh and material, with continuous attributes"). At runtime a GS adds barycentrics. The PS tests each uncovered sample with `EvaluateAttributeAtSample`. | Per-vertex flags alone cannot represent per-edge facts on indexed meshes (section 5.3). |
| Lighting | For each light and each shading model present: one per-pixel full-screen draw (stencil `EDGE = 0`, writes all samples) and one per-sample draw (`EDGE = 1`, `SV_SampleIndex`). A scissor rect is set from the light bounds. Stencil volume: one INVERT bit, rejects sky/empty samples. | Matches the video's Days Gone-style per-model full-screen draws. Classic two-pass deferred MSAA. |
| Lighting buffer | 2x MSAA `R16G16B16A16_FLOAT` (video) | A/B against `R11G11B10_FLOAT`, which is what Crysis 3 used for light buffers. |
| Resolve / SMAA S2x input | Tonemap **per sample** in one pixel-frequency pass with MRT output: `SV_Target0` = tonemapped sample 0 and `SV_Target1` = tonemapped sample 1, as two separate `R8G8B8A8` (sRGB) textures. Run SMAA 1x on each, with `subsampleIndices` (1,1,1,0) and (2,2,2,0). The second neighbourhood-blending pass blends at 0.5. | This is exactly what the SMAA reference recommends. No `ResolveSubresource` anywhere. |
| Specular AA | Base pass, before the G-buffer write: filtered `alpha^2 = alpha^2 + min(2*sigma^2*(|ddx n|^2 + |ddy n|^2), 0.18)` with `sigma^2 = 1/(2*pi)`, using the **center-interpolated vertex normal** (Tokuyoshi & Kaplanyan, I3D 2019 / JCGT 2021). Offline, bake normal-map variance into the roughness mips. | Cheap (about 0.1 ms at 8K on an RTX 2080), derived from NDF filtering, and better than Valve's 2015 heuristic, which UE still uses. |

---

## 1. What the video proposes (and where it is vague)

Timestamps refer to the transcript.

- **[0:35]** UE still ships Valve's old anti-specular-aliasing shader; use a newer one. Use `SV_Coverage`. Multi-sample rasterization and sample-frequency shading multiply the cost of quad overdraw. According to the video, Crysis 3 avoided `SV_Coverage` for that reason and built a fake coverage mask from depth/normals, but that mask dropped too many real coverage points. The fix: bake vertex information that marks **shared flat discontinuities**, using a few vertex-color bits. Bent edges and lone (boundary) edges keep multisampling.
- **[0:36]** The video says the shaders and baking tools still have to be developed. The scheme is a sketch, not a spec.
- **[0:36-0:38]** A G-buffer resolve after the base pass, modelled on Crysis 3: Crysis stored sample 0 in non-MSAA targets and built a per-pixel MSAA stencil. The video departs from Crysis as follows:
  - It keeps **no non-MSAA G-buffer copies**, arguing the target hardware is not bandwidth-limited here.
  - It builds a **sample-frequency stencil** that also holds the sample's shading-model bits (Days Gone-like).
  - Empty/disabled sub-samples and sky pixels share one stencil value.
- **[0:41-0:43]** Stencil-volume lighting (Fox Engine, Crysis 3, Days Gone). The lighting draws are full-screen triangles, one per shading model (Days Gone), and the lighting draw restores the stencil.
- **[0:44]** Stencil volumes get slower with an MSAA depth-stencil buffer, so the volume must use early stencil testing to skip samples that hold the sky/empty value.
- **[1:20]** SMAA 4x = SMAA S2x (SMAA run once per MSAA sample layer) + temporal. MSAA's job here is not to resolve jaggies, which SMAA does. Its job is to supersample temporally unstable edges.
- **[1:29]** If a triangle covers all samples, MSAA shades once at the pixel center, so textures stay sharp.
- **[1:39], [1:42]** A custom MSAA mask, saved in the motion-vector buffer, is reused later so temporal blending touches only MSAA edges. The video also suggests possibly storing last frame's `SV_Coverage` data.
- **[1:40-1:42]** Programmable sample locations (PSL). **Not available in core D3D11**: it is D3D12/Vulkan, or NVAPI on D3D11, which we excluded. We stay on the standard pattern.

Points that could not be verified:
- No public primary source was found for Days Gone's G-buffer resolve or its per-shading-model full-screen lighting. Those details rest on the video alone.
- "Crysis 3's sample-frequency shadow masking": Sousa (SIGGRAPH 2013) actually says the deferred sun-cascade shadows were rendered at **pixel** frequency and bilaterally upsampled in the composite pass, listed among its performance shortcuts. See section 7.4.

---

## 2. D3D11 MSAA facts this design relies on

| Fact | Detail | Source |
|---|---|---|
| 2x support | D3D11 only requires 1x, 4x and 8x. A device that exposes 2x must also support the standard (or center) 2x pattern. Check with `CheckMultisampleQualityLevels(fmt, 2, &q)`, which needs q > 0, then create with `Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN (0xffffffff)`. | D3D11.3 spec 19.2.4/19.2.5; MS docs |
| Standard sample positions | Offsets in 1/16 pixel from the pixel center, +x right, +y down. **2x**: s0 = (+4,+4) = (+0.25,+0.25); s1 = (-4,-4) = (-0.25,-0.25). **4x**: (-2,-6), (6,-2), (-6,2), (2,6), i.e. (-0.125,-0.375), (0.375,-0.125), (-0.375,0.125), (0.125,0.375). | Crysis 3 code (2x), BF3 code (4x). The SMAA table is the same in bottom-to-top y. |
| Sample counts must match | Every bound RTV and the DSV must have the same sample count. A non-MSAA RT cannot be rendered while the MSAA depth-stencil is bound. | D3D11 OM rules |
| Stencil is per sample | Every stencil test and write happens per sample. A pixel-frequency PS writes its color to every sample that passed depth/stencil. | Spec / standard behaviour |
| `SV_Coverage` input | ps_5_0+. Bit i is set when the **primitive** covers sample i, ANDed with the rasterizer `SampleMask`. **Not affected by depth/stencil tests.** It is 0 for helper pixels. It does not change the shading rate. | Spec 16.3.2; Kerzner & Salvi 2014 also note it ignores depth |
| `SV_Coverage` output (oMask) | ps_4_1+. ANDed with raster coverage, so it can only remove samples. It disables alpha-to-coverage. A PS whose only output is oMask is valid (for example a depth prepass), which means it masks depth/stencil writes too. At sample frequency it is also ANDed with the current sample's bit. | Spec 16.9.4 |
| Sample-frequency execution | Triggered by `SV_SampleIndex` as an input, or by any input with the `sample` interpolation modifier. | Spec 16.3 |
| `EvaluateAttributeAtSample(attr, s)` | Evaluates a linear or noperspective attribute at sample s. Any `centroid`/`sample` modifier is ignored. It is a plane-equation evaluation, so it also extrapolates to samples the primitive does not cover. | Spec 22.4.23 (`eval_sample_index`) |
| Texture2DMS | `Texture2DMS<float4> t; t.Load(int2 pixel, int sample); t.GetSamplePosition(s)` returns the offset from the pixel center in pixels. `GetRenderTargetSamplePosition(s)` gives the same for the bound RT. | HLSL |
| `[earlydepthstencil]` | Depth/stencil tests **and writes** happen before the PS. At sample frequency the test is per sample. Any `discard` or oMask later in the PS does **not** undo the depth/stencil writes. | Spec 16.12.1 |
| Read-only DSV with a depth SRV | `D3D11_DSV_READ_ONLY_DEPTH` lets the depth plane be bound as an SRV while **stencil is still written** through the same DSV. Lauritzen's Intel sample does exactly this (D32S8, depth SRV bound, stencil REPLACE pass). Reading the **stencil** SRV requires a read-only-stencil DSV or no DSV at all. | Intel sample `App.cpp` |
| Stencil SRV | D24S8: `X24_TYPELESS_G8_UINT`, declared `Texture2DMS<uint2>`, stencil in `.y`. D32S8: `X32_TYPELESS_G8X24_UINT`, stencil in `.y`. The engine's `depth_formats()` already maps these. | |
| Stencil arithmetic | Test: `(Ref & ReadMask) FUNC (Stencil & ReadMask)`. Write: `new = (old & ~WriteMask) \| (op(old) & WriteMask)`. INCR/DECR operate on the **full 8-bit value** before masking: carries out of the masked field are lost, and borrows are taken from the higher bits. Counters therefore go in the low bits with `_WRAP` ops. A 1-bit INVERT is order-independent. Read and write masks are shared by front and back faces. | D3D11 API |
| `SV_StencilRef` (per-pixel stencil ref from the PS) | **D3D11.3, optional**: `D3D11_FEATURE_DATA_D3D11_OPTIONS2::PSSpecifiedStencilRefSupported`. **Not supported on any NVIDIA architecture from Fermi through Blackwell** (supported on AMD GCN/RDNA and Intel Gen9+). Crysis 3's authors also wished they had stencil writes from the PS. | MS docs; Wikipedia D3D feature-level table; Crysis 3 GDC'13 notes |
| PSL | Not in D3D11 core. | |

---

## 3. Prior art (what, how, numbers)

### 3.1 Thibieroz (AMD), "Deferred Shading Optimizations", GDC 2011
- With deferred rendering, every render target must be multisampled, and each qualifying sample must be lit individually.
- Detect edges: shade non-edge pixels per pixel and edge pixels per sample.
- The **centroid trick** (declare `SV_Position` centroid and check whether xy still ends in .5) is clever, but the talk considers it of little practical use: it produces too many edges that don't need per-sample shading, especially with tessellation, and it misses alpha-test edges. The talk recommends checking depth and normal discontinuities instead.
- Pitfall: with MSAA, almost every sample has a unique depth, so depth-buffer comparisons need tolerance.
- The talk also describes the classic stencil light volume: Z test LESSEQUAL; on depth fail, increment for back faces and decrement for front faces; then shade where stencil != 0.
- It also notes that additive blending is more expensive with MSAA, and suggests `discard`ing pixels that receive zero light.

### 3.2 Lauritzen, "Deferred Rendering for Current and Future Rendering Pipelines", SIGGRAPH 2010 BPS, with the Intel sample
- Store the G-buffer at sample frequency and apply per-sample shading only where needed.
- Forward MSAA does redundant work because it applies to *all* triangle edges, including continuous tessellated surfaces. This is the problem the video's flat flags attack.
- Edges are found by comparing sample depths against depth derivatives, and by comparing the shading-normal deviation across samples.
- Quad/volume lighting: mark per-sample pixels in stencil, which Lauritzen found faster than in-shader branching on most hardware, then run two passes (per pixel, then per sample). Downsides: duplicated culling work, and poor scheduling of scattered per-sample pixels.
- Tiled CS: shade sample 0 for the whole tile, pack a list of per-sample pixels, redistribute threads. Packing beats branching, and the tiled path loses less performance to MSAA than the quad path. The test hardware was a GTX 480 and an HD 5870 at 1080p with 1024 lights. The charts carry no numeric labels in the extracted text.
- Counterpoint to the video: Lauritzen judged two-pass stencil light volumes to cost more than they save, compared with screen-aligned quads plus a single-direction depth test. We A/B this (section 13).
- **Sample code** (Apache 2.0, mirrored in the GPU Pro 7 sources). Details:
  - G-buffer: RGBA16F normal+specular, RGBA8 albedo, RG16F `ddx/ddy(viewZ)` (written with `ddx_coarse/ddy_coarse`), plus the D32S8 depth.
  - Edge test: for i >= 1, the pixel is an edge if `|z_i - z_0| > |dz/dx| + |dz/dy|` (from sample 0's gradients) or `dot(n_i, n_0) < 0.99` (about 8 degrees).
  - The mark pass is a full-screen triangle with `DepthEnable = FALSE`, stencil ALWAYS/REPLACE, ref 1, and `discard` for simple pixels. It is bound with a read-only-depth DSV.
  - Lighting: the same light draws twice. First the pixel PS (shades sample 0) with stencil EQUAL ref 0, then the per-sample PS (`SV_SampleIndex`) with stencil EQUAL ref 1.

### 3.3 Battlefield 3 (Andersson, GDC 2011)
- Geometry pass with 2x, 4x or 8x MSAA. Lights are evaluated per sample when needed, averaged, and written per pixel.
- **1080p G-buffer + Z at 4x MSAA = 158 MB** (about 19 B/sample).
- The tiled CS builds a coherent per-sample pixel list following Lauritzen, described as a major performance improvement.
- Alpha-test supersampling: the PS shades per pixel but evaluates the alpha test at each of the 4 standard sample offsets (UV + offset.x*ddx(uv) + offset.y*ddy(uv)) and writes `SV_Coverage`. This needs one shader permutation per MSAA level.

### 3.4 Crysis 2/3 (Sousa et al., GDC 2013; Sousa, SIGGRAPH 2013 "CryENGINE 3 Graphics Gems")
- After the G-buffer, a **custom resolve** writes sample 0 into non-MSAA targets for pixel-frequency passes. The same pass builds a per-sample mask by comparing sample similarity with depth/normal thresholds. Crysis deliberately avoided default `SV_Coverage` because it causes redundant processing.
- **Stencil**: 1 reserved bit (0x80) is updated with the mask, and every other stencil user writes with `StencilWriteMask = 0x7F`. Pixel-frequency passes read with mask 0x80 expecting 0, sample-frequency passes expect 0x80 and index samples with `SV_SampleIndex`. Where stencil is too contended, the fallback is `clip`, plus a texture read of the mask.
- **They tag the entire 2x2 quad rather than a single pixel**, because spatial coherence improves stencil-culling efficiency. The speaker notes report about 1 ms saved on average (hardware not stated).
- Alpha-test SSAA builds a custom coverage mask with the 2x offsets (+0.25,+0.25) and (-0.25,-0.25). The slide code has a typo: the shift uses `i` where the loop index is `s`. Following an AMD tip, `EvaluateAttributeAtSample` is preferred over ddx/ddy offsets.
- Shortcuts:
  - Deferred sun-cascade shadows are rendered at pixel frequency and bilaterally upscaled in the composite.
  - Soft particles use max depth instead of per-sample processing.
  - Motion blur and DOF run at pixel frequency and are composited back into the MSAA buffer.
  - If HDR is tonemapped only after the resolve, high-contrast edges lose AA.
- Light buffers are R11G11B10F. The composite is RGBA16F.

### 3.5 Killzone 2 (Valient, GDC 2009 notes)
- 2x "Quincunx" MSAA. The light shader runs once per pixel, evaluates the lighting for *all* samples inside that single invocation, and writes the average. They judged that skipping identical samples via dynamic branching did not pay off at 2x.
- Poisson shadow taps are distributed across samples: 12 taps over 4 samples gives 3 each.
- **This does not fit SMAA S2x**, which needs per-sample outputs. It is mentioned only for completeness.

### 3.6 NVIDIA GameWorks sample "Antialiased Deferred Rendering" (D3D11, 4x)
- It offers two complex-pixel detectors:
  - Store `SV_Coverage` per sample and flag a pixel if any sample's mask is not full. Fast, but it **flags non-edge pixels**: the interior-triangle-edge problem.
  - Search for normal/color/depth discontinuities.
- Normal and complex pixels are split into two stencil-masked passes so that warps stay coherent.
- **Adaptive shading**: shade each *unique coverage mask* once and weight it by its sample fraction. At 2x this collapses to "shade once or twice".

### 3.7 MJP, "Bindless Texturing for Deferred Rendering and Decals" (2016): modern-ish numbers
- GTX 970, 1920x1080, Crytek Sponza with 20 spot lights and 1 sun:

| | Total | G-buffer | Deferred lighting (CS) | MSAA mask | MSAA resolve |
|---|---|---|---|---|---|
| No MSAA | 7.40 ms | 0.67 ms | 3.54 ms | – | – |
| 4x MSAA | 9.86 ms | 1.48 ms | 4.64 ms | 0.16 ms | 0.21 ms |

- Edge detection: material-ID differences (conservative). An "aggressive" mode also flags varying depth gradients, which means non-coplanar triangles; it costs about +1.8 ms at 1440p.

### 3.8 Surface-merging research: prior art for "shade once per surface, not per triangle"
- **SBAA** (Salvi & Vidimče, I3D 2012): primitives covering a pixel usually belong to one connected, low-curvature surface, so shading once per *surface* is enough. Their MERGE algorithm takes the primitive with the highest coverage, collects samples whose normals lie within a small angle of it, and rejects the merge if the depth distribution is bimodal.
  - Measured on an HD 5870 at 720p against Lauritzen's sample: Sponza 4x reference took 6.0/8.7 ms (simple/complex lighting), 8x took 10.5/13.0 ms.
  - Shaded samples for Sponza at 4x: about 1.04 M at 720p, which implies about 4% of pixels were per-sample. GRASS at 8x: 1.99 M, about 16%.
- **Streaming G-Buffer Compression** (Kerzner & Salvi, HPG 2014): fragments merge into a fixed number of per-pixel surfaces when three conditions hold: normals aligned within pi/4, overlapping depth intervals (from depth derivatives), and **mutually exclusive coverage masks** (the authors observe that primitives of one geometric feature rarely overlap). Needs pixel-ordered PS (PixelSync/ROV), so it does not apply to us.
- **AGAA** (Crassin et al., 2015): two pre-filtered aggregates per pixel, quality comparable to 8x MSAA, about 30% less memory, up to 1.3x faster.
- **GPAA** (Persson 2011): proposes extracting only the relevant silhouette/crease edges offline, or skipping internal edges with an adjacency GS.
- **Conclusion**: runtime heuristics (SBAA, SGBC) and offline edge classification (GPAA) both exist. **No published implementation was found of the exact combination**: baked per-edge flatness flags used to suppress `SV_Coverage`-derived per-sample shading in a deferred renderer. The video's idea is effectively an offline, exact version of SGBC's "exclusive coverage + coplanar" merge test.

---

## 4. Detecting "complex" (edge) pixels

There are three detectors. All of them end in the same G-buffer resolve pass, which writes the stencil EDGE bit (section 6).

### Mode A: compare samples (Lauritzen 2010 / Crysis 3; the video's "fake coverage mask")

```hlsl
// Requires an RG16F per-sample target with (ddx, ddy) of linear view depth from the base pass (Lauritzen).
bool EdgeByComparison(int2 p)
{
    float  z0   = LinearViewDepth(gDepthMS.Load(p, 0));
    float2 g0   = gDepthGradMS.Load(p, 0);
    float3 n0   = DecodeNormal(gNormalMS.Load(p, 0));
    float  maxDz = abs(g0.x) + abs(g0.y);                 // how far one triangle's depth can drift inside one pixel
    bool edge = false;
    [unroll] for (uint s = 1; s < MSAA_SAMPLES; ++s) {
        float  z = LinearViewDepth(gDepthMS.Load(p, s));
        float3 n = DecodeNormal(gNormalMS.Load(p, s));
        edge = edge || abs(z - z0) > maxDz || dot(n, n0) < 0.99;   // 0.99 ~ 8 degrees
    }
    return edge;
}
```

Where it fails: samples with similar depth and normal but **different material** are judged "same". Examples are coplanar material boundaries, modelled trim, and thin geometry against a parallel background. Those pixels then get one shade computed from sample 0's albedo, so the edge loses its MSAA. This is the kind of loss the video attributes to Crysis 3.

### Mode B: raw `SV_Coverage` (the NVIDIA sample's first option)

The base-pass PS writes `edgeBit = (SV_Coverage != FULL_MASK)` into one spare G-buffer bit per sample. A pixel is an edge if any sample has the bit set. This is correct for every geometric edge, but it also flags every interior triangle edge (Lauritzen and the AMD talk both identify this cost).

### Mode C: `SV_Coverage` minus flat shared edges (the video), plus a normal test

The base pass writes `edgeBit = CoverageEdgeBit(...)` (section 5.4). The resolve then ORs two things:
- the edge bits of all samples;
- `dot(n_s, n_0) < NORMAL_EDGE_COS`. This catches **mesh intersections**, which coverage cannot see because coverage ignores the depth test, and also some depth-split cases.

Notes:
- A material boundary always keeps MSAA here, because the flat flag requires the same material.
- The default threshold is 0.99, as in Lauritzen. If centroid-interpolated, normal-mapped normals cause false positives across flat edges, relax it toward 0.95 or make the normal test optional ("C+N" vs "C" in the A/B).

### Shared resolve-pass PS

Pixel frequency. Full-screen triangle. No RTVs, MSAA DSV bound read-only-depth. Stencil state `GBUFFER_RESOLVE` from section 12.

```hlsl
Texture2DMS<float4> gNormalMS : register(t0);  // e.g. R10G10B10A2: rgb = normal, a = base-pass edge bit (a > 0.5)

void GBufferResolvePS(float4 svPos : SV_Position)
{
    int2 p = int2(svPos.xy);
    bool edge = false;
#if EDGE_MODE == EDGE_MODE_COMPARE
    edge = EdgeByComparison(p);
#else
    float3 n0 = DecodeNormal(gNormalMS.Load(p, 0).rgb);
    [unroll] for (uint s = 0; s < MSAA_SAMPLES; ++s) {
        float4 g = gNormalMS.Load(p, s);
        edge = edge || (g.a > 0.5);                                   // coverage-based bit from the base pass
    #if EDGE_NORMAL_TEST
        edge = edge || dot(DecodeNormal(g.rgb), n0) < NORMAL_EDGE_COS; // intersections
    #endif
    }
#endif
#if TAG_WHOLE_QUAD   // Crysis 3: tag the whole 2x2 quad so stencil culling sees coherent blocks
    float e = edge ? 1.0 : 0.0;
    e += abs(ddx_fine(e));   // OR with the horizontal neighbour in the quad (values stay >= 0)
    e += abs(ddy_fine(e));   // OR with the other row of the quad
    edge = e > 0.0;
#endif
    if (!edge) discard;      // kept pixels get STENCIL_EDGE in all samples (the full-screen triangle covers every sample)
}
```

How the quad-OR trick works: `ddx_fine` returns `right - left` to both pixels of a row. So `e + |ddx|` is greater than zero exactly when either pixel of the row is set. Applying `ddy_fine` to that row result gives the OR over the whole quad. Both derivatives are taken **before** the `discard`.

---

## 5. Flat shared discontinuities (the video's vertex-flag idea, made exact)

### 5.1 What it should achieve
Consider a pixel where two triangles of the **same flat surface** meet: a quad's diagonal, or the interior of a tessellated wall or floor. Each triangle writes the samples it covers, and the G-buffer values on both sides are (nearly) identical, so shading once is exact. These pixels should not be flagged.

Bent edges (non-coplanar neighbours) and lone edges (open boundaries and silhouettes) keep per-sample shading, as the video specifies. A more aggressive variant also treats smooth bent edges with continuous normals as flat. That is closer to Lauritzen's 8-degree normal tolerance and is an A/B option.

### 5.2 Baking (asset import, once per mesh)

```text
input: positions P[], attributes (normal N[], tangent T[], uv UV[]), index buffer (tri t has corners c0,c1,c2), material per triangle
1. Weld by position (hash-grid quantised to ~1e-5 * bbox size) -> canonical vertex id w(v).
   Welding by position only lets adjacency cross UV/normal splits; the attribute checks in step 3 decide whether that crossing is allowed.
2. Edge map: key = (min(w(a),w(b)), max(w(a),w(b))) -> list of (triangle t, local edge k),
   where local edge k is the edge OPPOSITE corner k: k=0 -> (c1,c2), k=1 -> (c2,c0), k=2 -> (c0,c1).
3. For every key with exactly two entries (t,k) and (u,j):   // 1 entry = boundary, >2 = non-manifold -> not flat
     coplanar   = dot(faceNormal(t), faceNormal(u)) > cos(0.5 deg)
     sameMat    = material(t) == material(u)  (and same shading model)
     continuous = for both shared endpoints: the actual vertices used by t and by u have equal N, T and UV (within epsilon)
                  (no hard edge, no UV seam: so both triangles produce identical G-buffer values at the shared pixels)
     if (coplanar && sameMat && continuous) { flags[t] |= 1<<k; flags[u] |= 1<<j; }
   [aggressive A/B variant: replace "coplanar" by "dihedral < 8 deg && continuous normals"]
4. Output: one byte per triangle (R8_UINT buffer) in index-buffer order.
```

The memory cost is 1 byte per triangle.

### 5.3 Why "a few vertex-color bits" alone are not enough
- Flatness is a property of an **edge**. With indexed meshes, a vertex is shared by many triangles and many edges.
- Take the simplest case, a single wall quad ABCD with diagonal AC. All four vertices lie on boundary edges. No per-vertex flag can be 1 along AC and 0 along AB near A.
- An interpolated per-vertex weight with a threshold only approximates this. It misclassifies regions near the corners and depends on screen-space triangle size.
- Per-vertex bits do work if every triangle has its **own three vertices** (de-indexed or "flat-aware" meshes). Each corner then stores a 2-bit corner id (decoded to a one-hot barycentric in the VS) plus the triangle's 3 flat-edge bits (identical on all three corners, so the provoking vertex does not matter). That is 5 bits, which matches the video's wording, at the cost of about 3x vertex count and VS work.
- For this PoC use the **GS variant** (5.4). It keeps the meshes indexed and the vertex format unchanged, and it can be toggled for A/B just by binding or unbinding the GS.

### 5.4 Runtime (base pass)

```hlsl
// ---- GS: add screen-linear barycentrics and the triangle's flat-edge bits (bound only when FLAT_EDGES mode is on) ----
Buffer<uint> gTriFlatEdges : register(t15);        // R8_UINT per triangle
cbuffer DrawCB : register(b2) { uint gFirstTriangle; } // = StartIndexLocation / 3 of this sub-mesh draw

struct GSOut
{
    BasePassVSOut v;                               // everything the base pass normally interpolates
    noperspective float3 bary      : BARYCENTRIC;  // screen-linear: sign matches the rasterizer's edge functions
    nointerpolation uint flatEdges : FLAT_EDGES;   // bit k = edge opposite corner k is a flat shared edge
};

[maxvertexcount(3)]
void BasePassGS(triangle BasePassVSOut tri[3], uint primId : SV_PrimitiveID, inout TriangleStream<GSOut> outStream)
{
    uint flags = gTriFlatEdges[gFirstTriangle + primId] & 7u;   // SV_PrimitiveID starts at 0 per draw (and per instance)
    [unroll] for (uint i = 0; i < 3; ++i) {
        GSOut o;
        o.v = tri[i];
        o.bary = float3(i == 0, i == 1, i == 2);
        o.flatEdges = flags;
        outStream.Append(o);
    }
}

// ---- PS helper: does this triangle's partial coverage represent a real (MSAA-worthy) edge? ----
static const uint FULL_MASK = (1u << MSAA_SAMPLES) - 1u;

uint CoverageEdgeBit(uint coverage, float3 bary, uint flatEdges)
{
    if (coverage == FULL_MASK) return 0u;              // triangle covers every sample of this pixel
    if (flatEdges == 0u)       return 1u;              // no flat edges on this triangle: any loss is a real edge
    uint missing = ~coverage & FULL_MASK;
    [unroll] for (uint s = 0; s < MSAA_SAMPLES; ++s) {
        if ((missing >> s) & 1u) {
            float3 b = EvaluateAttributeAtSample(bary, s);       // extrapolated to the uncovered sample
            uint crossed = (b.x < 0.0 ? 1u : 0u) | (b.y < 0.0 ? 2u : 0u) | (b.z < 0.0 ? 4u : 0u);
            // crossed == 0: inside by float math but not covered (snapping/tie-break) -> be conservative
            if (crossed == 0u || (crossed & ~flatEdges) != 0u) return 1u;
        }
    }
    return 0u;   // every uncovered sample lies across a flat shared edge: the neighbour continues this surface
}

// In the base-pass PS:  uint cov : SV_Coverage  (input)
//   mode B: edgeBit = (cov != FULL_MASK)
//   mode C: edgeBit = CoverageEdgeBit(cov, i.bary, i.flatEdges)
//   alpha-tested materials: edgeBit |= (alphaCoverage != FULL_MASK)   (section 8)
// Write edgeBit into the reserved G-buffer bit (e.g. RGB10A2.a = edgeBit).
```

Pitfalls:
- Corner order in the bake must match index-buffer order. That order survives the GS.
- `noperspective` barycentrics are used so their signs match raster coverage exactly, apart from snapping.
- Skinned or animated meshes stay flat only if they deform rigidly. Disable the flags (bind no GS) on skinned characters, or accept that the error is small.
- Two **different meshes** placed coplanar (adjacent floor tiles) are never merged. That is conservative.
- With MSAA off, `FULL_MASK = 1` and everything is a no-op.

---

## 6. Building the sample-frequency stencil in D3D11

### 6.1 The simplest option, evaluated: shading-model bits from the base pass
- Clear depth-stencil to (far, 0); 0 = sky/empty.
- For each base-pass draw, call `OMSetDepthStencilState(BASEPASS, model << 2)`. The state is stencil ALWAYS, PassOp REPLACE, WriteMask = MODEL.
- Stencil is stored per sample and is written only for samples that are covered and pass depth. Every sample therefore ends up holding the shading model of the triangle visible **in that sample**. This is already the per-sample shading-model stencil the video asks for, at zero extra passes.

Verdict: **works and is the recommended path**. Limitations:
1. Granularity is per draw. A material that picks its shading model per pixel would need either splitting into one draw per model, or the fallback in 6.4.
2. With a partial depth prepass, the base pass uses EQUAL/LESS_EQUAL, and the stencil write still happens on depth pass, which is correct. The prepass itself does not need to write stencil.
3. Never put `[earlydepthstencil]` on alpha-tested base-pass shaders. Depth and stencil would be written before `discard`/oMask could act.
4. Track a CPU bitmask of the models that were drawn, so the lighting pass only issues draws for models actually present.

### 6.2 The EDGE bit
This is the resolve pass from section 4: discard simple pixels, REPLACE `EDGE` through `WriteMask = EDGE`. It runs at pixel frequency, and because the full-screen triangle covers all samples, the bit is uniform within a pixel (or within a quad, with tagging). This plays the role of the video's custom MSAA mask.

The stencil cannot be read by passes that run at 1x (sample counts must match). The tonemap pass therefore copies the EDGE bit into a non-MSAA `R8_UNORM` mask for later temporal and edge-only passes (video [1:39]); see section 9.

### 6.3 Variant B (closer to the video): "disabled" sub-samples share the sky value
- After the EDGE pass, a second full-screen draw at pixel frequency `discard`s edge pixels.
- For simple pixels it writes `SV_Coverage = 0b10` (sample 1 only), with stencil REPLACE, `WriteMask = MODEL | DISABLED` and `Ref = DISABLED`. Sample 1 then reads as model 0 ("sky/empty") to every lighting test. The DISABLED bit is kept so the sky pass and tonemap can tell it apart from real sky.
- Lighting then needs **one** sample-frequency draw per model and light instead of two. Simple pixels shade only sample 0, because early stencil kills sample 1. Edge pixels shade both.
- The tonemap fills sample 1 from sample 0 wherever DISABLED is set.
- The sky pass must test `MODEL == 0 && !DISABLED`.
- Invocation count is the same as variant A (N + E). It saves the duplicated draw.
- Risk: sample 1 of every simple pixel stays at the clear value in the lighting buffer. That probably defeats MSAA color compression, where identical samples store cheaply, and it adds a stencil read to the tonemap. The video's claim that a sample-frequency stencil helps the hardware read MSAA targets more efficiently is unverified. **Measure A vs B** (section 13).

### 6.4 Fallback: writing arbitrary per-sample stencil values without `SV_StencilRef`
- Use one draw per stencil bit b: a full-screen triangle at pixel frequency whose PS outputs `SV_Coverage` = the mask of samples whose value has bit b set (or discards when none do), with stencil REPLACE, `Ref = 0xFF`, `WriteMask = 1 << b`.
- Alternatively use a sample-frequency PS with `discard`. That costs 2 invocations per pixel instead of 1.
- Only needed if shading models become per-pixel.

---

## 7. Lighting passes with the MSAA stencil

### 7.1 Per light, per shading model m present
1. **Volume mark** (local lights only). Render the light's convex volume mesh with a null PS and no RTV:
   - DSV read-only depth; depth test LESS_EQUAL (GREATER_EQUAL with reversed Z); `CullMode NONE`; `DepthClipEnable FALSE`, so back faces beyond the far plane are clamped rather than clipped.
   - Stencil: `Func NOT_EQUAL, Ref 0, ReadMask = MODEL`. This rejects sky, empty and disabled samples: the video's early-stencil requirement.
   - `DepthFailOp = INVERT` on both faces, `WriteMask = LIGHT`.
   - Result: LIGHT = 1 exactly for samples inside the volume.

   | Sample position | Front face | Back face | Inverts | LIGHT |
   |---|---|---|---|---|
   | In front of the volume | fails | fails | 2 | 0 |
   | Inside the volume | passes | fails | 1 | 1 |
   | Behind the volume | passes | passes | 0 | 0 |

   - Camera inside the volume: the front faces are near-clipped, but they would have passed anyway.
   - The method is order-independent and needs a single bit.
   - If the stencil-volume note prefers INCR/DECR counting, use a 2-bit `_WRAP` counter in bits 0-1 (section 2, stencil arithmetic).
2. **Per-pixel lighting draw**: full-screen triangle with scissor = the light's screen bounds, depth disabled.
   - Stencil: `Func EQUAL`, `ReadMask = LIGHT|MODEL|EDGE`, `Ref = LIGHT | m<<2 | 0`, `PassOp ZERO` with `WriteMask = LIGHT`, which clears the mark (the video's lighting draw that restores the stencil).
   - The PS loads **sample 0**. Its output goes to every passing sample of the pixel, so simple pixels end up with identical samples, which is good for compression and correct for SMAA S2x.
3. **Per-sample lighting draw**: the same, but with `Ref = LIGHT | m<<2 | EDGE` and a PS that takes `SV_SampleIndex`.
   - Each sample passes exactly one (model, EDGE) draw, so every LIGHT bit is cleared once.

Lights without a volume (sun, sky ambient): skip step 1 and drop LIGHT from the masks.

Additive blend (ONE, ONE) into the 2x lighting buffer. Following the AMD advice, `discard` when the light contribution is 0. That is compatible with `[earlydepthstencil]` because the stencil update has already happened.

### 7.2 Shader skeleton

```hlsl
Texture2DMS<float>  gDepthMS : register(t0);
// + per-sample G-buffer Texture2DMS SRVs (t1..)

float3 ShadeSample(int2 p, uint s)   // compiled once per shading model (no shading-model branching)
{
    float2 pixelPos = float2(p) + 0.5 + gDepthMS.GetSamplePosition(s);   // exact location of this sample
    float  rawDepth = gDepthMS.Load(p, s);
    Surface surf    = LoadGBuffer(p, s);                                  // .Load(p, s) on every G-buffer RT
    float3  posWS   = ReconstructWorldPosition(pixelPos, rawDepth);
    return EvaluateLight(surf, posWS);                                    // BRDF + shadow lookup for this light
}

[earlydepthstencil]
float4 LightPerPixelPS(float4 svPos : SV_Position) : SV_Target
{
    return float4(ShadeSample(int2(svPos.xy), 0), 0);   // written to all samples that passed the stencil test
}

[earlydepthstencil]
float4 LightPerSamplePS(float4 svPos : SV_Position, uint s : SV_SampleIndex) : SV_Target
{
    return float4(ShadeSample(int2(svPos.xy), s), 0);   // SV_SampleIndex => one invocation per passing sample
}
```

### 7.3 Draw count and cost notes
- Each light costs 1 volume draw plus 2 draws per present model. Example: 3 models gives 7 draws. Draws are cheap in D3D11 at the light counts of a PoC.
- Volume rasterization with an MSAA DSV costs 2x the depth/stencil sample work. That is the video's slower-with-MSAA point; no PS runs. Scissoring keeps the full-screen draws from touching off-light pixels.
- Lauritzen's per-sample scheduling problem: scattered edge samples make sparse quads. Quad tagging (4) and the A/B in 13 address it.

### 7.4 Shadows
- Evaluate the shadow term **inside** each lighting draw. It then follows the per-pixel/per-sample split automatically, with no separate mask.
- If a screen-space sun shadow mask is used instead, the options are:
  - Compute it with the same two-pass stencil split into a 2x R8 MSAA mask (exact).
  - Use Crysis 3's shortcut: pixel frequency plus bilateral upsampling in the composite. This is what Sousa 2013 actually describes, not a sample-frequency mask.
- That choice belongs to the shadow note.

---

## 8. Alpha-tested geometry (foliage) under MSAA

| Option | How | Notes |
|---|---|---|
| No alpha AA | `clip()` | Alpha edges stay aliased, apart from SMAA. |
| Alpha-to-coverage | `AlphaToCoverageEnable = TRUE` in the blend state | Only 3 coverage levels at 2x, and the dither patterns are visible. Crysis calls it a corner most games cut. |
| **Alpha-test SSAA (recommended)** | Per pixel: for each sample s, `uv_s = EvaluateAttributeAtSample(uv, s)` (or `uv + o.x*ddx(uv) + o.y*ddy(uv)` with the standard offsets). Set bit s if `alpha(uv_s) >= ref`, then output `SV_Coverage = mask`. | Shading stays per pixel and only visibility is supersampled (BF3, Crysis 3). |

Pitfalls:
- Use one shader permutation per MSAA count.
- A depth prepass for alpha-tested objects must output the **same** coverage, so that a base pass running with EQUAL matches it.
- Do not use `[earlydepthstencil]` in these shaders.
- Set `edgeBit |= (mask != FULL_MASK)` so alpha edges get per-sample lighting. This can be expensive in dense foliage, so measure it (T8).

---

## 9. Lighting buffer, tonemap, SMAA S2x, "resolve"

**SMAA S2x needs separate per-sample inputs.** The SMAA reference (`SMAA.hlsl`) works like this:
1. Render with MSAA 2x, created with the standard pattern so that sample order matches its table.
2. Write each subsample to a separate buffer. This can be its own pass (`SMAASeparatePS` does two `Load`s into two RTs), or it can be folded into the tonemap pass. The reference recommends the latter because SMAA then sees tonemapped subsamples.
3. Run the full SMAA 1x pipeline on each buffer, with `subsampleIndices = float4(1,1,1,0)` for sample 0 and `float4(2,2,2,0)` for sample 1. The second run's neighbourhood blending alpha-blends at 0.5 onto the first run's output.

This matches the video's description.

Color-space rules from SMAA:
- Edge detection must read **gamma** (non-sRGB) color.
- Only the neighbourhood-blending input read and its output write use sRGB views.
- So create each layer as `R8G8B8A8_TYPELESS`: write it through a `_UNORM_SRGB` RTV, read it through a `_UNORM` SRV for edge detection, and through a `_UNORM_SRGB` SRV for blending.

Recommended layout:

```hlsl
Texture2DMS<float4> gLightingMS : register(t0);   // 2x R16G16B16A16_FLOAT
Texture2DMS<uint2>  gStencilMS  : register(t1);   // X24_TYPELESS_G8_UINT view; stencil in .y (no DSV bound in this pass)
Texture2D<float4>   gBloom      : register(t2);   // optional non-MSAA input computed from an HDR box resolve

struct TonemapOut {
    float4 layer0 : SV_Target0;  // SMAA input for sample 0 (RGBA8 sRGB RTV)
    float4 layer1 : SV_Target1;  // SMAA input for sample 1
    float  edge   : SV_Target2;  // R8_UNORM non-MSAA edge mask for later temporal/edge-only passes (video 1:39)
};

TonemapOut TonemapSeparatePS(float4 svPos : SV_Position)
{
    int2 p = int2(svPos.xy);
    float3 bloom = gBloom.Load(int3(p, 0)).rgb;
    uint   st0   = gStencilMS.Load(p, 0).y;
    TonemapOut o;
    o.layer0 = float4(Tonemap(gLightingMS.Load(p, 0).rgb + bloom), 1);   // tonemap BEFORE averaging samples
    o.layer1 = float4(Tonemap(gLightingMS.Load(p, 1).rgb + bloom), 1);
#if VARIANT_B
    if (gStencilMS.Load(p, 1).y & STENCIL_DISABLED) o.layer1 = o.layer0;
#endif
    o.edge = (st0 & STENCIL_EDGE) ? 1.0 : 0.0;
    return o;
}
```

- Use **two separate `Texture2D`s**, not a `Texture2DArray`. SMAA's passes expect `Texture2D` SRVs, and separate textures avoid any array-slice view questions.
- Sequential reuse: run SMAA on layer 0 into the final target, then on layer 1, blending 0.5 into the same target. One set of edge/blend textures (RG8 + RGBA8) is then enough.
- **Tonemap before resolve.** Per-sample tonemapping keeps high-contrast HDR edges antialiased (Sousa 2013). MJP's resolve-filter experiments note the side effect: dark silhouettes look slightly thicker against bright sky, and bloom and exposure must come from HDR data rather than the resolved LDR image.
- The only "resolve" is SMAA's final 0.5 blend. **Never call `ResolveSubresource` on the HDR buffer.**
- Passes that need one HDR value per pixel (bloom input, auto-exposure, perhaps motion blur) get a cheap box "HDR resolve": the average of the two samples into a non-MSAA RGBA16F target. Crysis ran DOF and motion blur at pixel frequency and composited them back into the MSAA buffer. Where these passes go in the post chain belongs to the post-processing note.
- With variant A, simple pixels have identical samples, so SMAA does the same work on both layers there. S2x costs about 2x SMAA 1x. The video quotes up to about 1 ms worst case (Crysis 3 grass).

---

## 10. Specular anti-aliasing

### 10.1 Survey

| Method | Year | Idea | Fit for us |
|---|---|---|---|
| Toksvig | 2004/05 | Shorter averaged normal in a mip means a wider lobe | Needs unnormalised normals. **BC5 reconstructs unit normals, so runtime Toksvig is impossible.** Use it offline instead. |
| LEAN / CLEAN | 2010 / 2011 | Store first and second slope moments in mips | Exact for Beckmann, but needs 5 (LEAN) or 3+ extra channels at 16-bit. Too heavy for this pipeline. |
| Normal-map variance baked into roughness mips | Neubelt & Pettineo 2013 (The Order); Valve 2015; UE "Composite Texture" (normal to roughness channel); Unity `TextureNormalVariance` | Per mip: average the mip-0 normals under the texel, convert length to vMF kappa, add variance to alpha^2 | **Yes**: offline, costs nothing at runtime. |
| Valve geometric specular AA (Vlachos, GDC 2015) | 2015 | `geo = (saturate(max(|ddx n|^2, |ddy n|^2)))^(1/3)`, then `roughness = max(roughness, geo)` | Valve itself presents it as a hack. UE's `NormalCurvatureToRoughness` is the same formula (exponent cvar default 0.333, scale 1, bias 0), applied with `max` to `GBuffer.Roughness`. The video's claim that UE uses Valve's method is accurate. |
| Kaplanyan, Hill, Patney, Lefohn (HPG 2016) | 2016 | Filter the NDF in slope space with a kernel from half-vector derivatives. The deferred approximation uses the averaged normal and a max rectangle width. kappa = 0.18 clamp. | Superseded. |
| **Tokuyoshi & Kaplanyan, "Improved Geometric Specular AA" (I3D 2019)** | 2019 | For deferred: `alpha^2 += min(2*sigma^2*(|dn_u|^2 + |dn_v|^2), kappa)` using world-space normal derivatives (the trace of the kernel covariance is a conservative bound) | **Recommended.** Simplest form, tangent-free. |
| Tokuyoshi & Kaplanyan, JCGT 2021 "Projected-Space NDF Filtering" | 2021 | Same deferred kernel, sigma^2 changed to 1/(2*pi). Forward variants filter in projected space. "Less conservative" option drops the factor 2. | Use its constants. |

The forward-only variants filter with half-vector derivatives, which a deferred base pass cannot know. Kaplanyan 2016 and Tokuyoshi 2019 therefore use the light-independent normal-based approximation for deferred.

### 10.2 Recommended HLSL (base pass, before writing roughness to the G-buffer)

```hlsl
// Geometric specular AA, deferred variant (Tokuyoshi & Kaplanyan I3D 2019, constants from JCGT 2021).
// alpha2   : GGX alpha^2 (alpha = perceptualRoughness^2) AFTER sampling the variance-baked roughness map
// nCenter  : normalized, CENTER-interpolated world-space vertex normal (not the normal-mapped normal)
float FilterGGXAlpha2(float alpha2, float3 nCenter)
{
    const float kSigma2 = 0.15915494;   // 1/(2*pi): variance of the Gaussian pixel filter (JCGT 2021; I3D 2019 used 0.25)
    const float kKappa  = 0.18;         // clamp: limits derivative-estimation error (Kaplanyan 2016)
    float3 dndx = ddx(nCenter);
    float3 dndy = ddy(nCenter);
    float kernel = 2.0 * kSigma2 * (dot(dndx, dndx) + dot(dndy, dndy));   // drop "2.0 *" for the less conservative variant
    return saturate(alpha2 + min(kernel, kKappa));
}

// usage in the base-pass PS:
//   float pr     = roughnessTex.Sample(s, uv).g;           // perceptual roughness, mips contain normal-map variance
//   float alpha2 = (pr * pr) * (pr * pr);
//   alpha2       = FilterGGXAlpha2(alpha2, normalize(i.normalCenterWS));
//   gbuffer.roughness = sqrt(sqrt(alpha2));                // back to perceptual for 8-bit storage
```

Interpolate the normal twice. One copy is center-interpolated (`nCenter`, used for derivatives). The other is `centroid`, used for shading. Valve's trick: if `dot(n, n) >= 1.01` (the normal was extrapolated outside the triangle), switch to the centroid copy, which avoids silhouette sparkles. UE explicitly avoids derivatives of centroid-interpolated normals because MSAA makes the centroid samples non-uniform.

### 10.3 Offline normal-map variance into roughness mips

```text
for each mip L >= 1, texel t:
    nAvg   = mean of the UNIT tangent-space normals of the mip-0 texels under t (do not renormalize)
    r      = clamp(length(nAvg), 1e-4, 0.9999)
    kappa  = (3r - r^3) / (1 - r^2)                 // vMF concentration estimate
    alpha2 = mean(alpha2 of mip-0 roughness texels under t) + 1 / (2 * kappa)   // 1/(2 kappa) = 2 * variance
    store perceptualRoughness = sqrt(sqrt(saturate(alpha2)))
```

- Filter in alpha^2 space, which is variance space.
- Mip 0 gets no addition.
- Requires the roughness and normal maps to share UVs, or compensate for their resolution ratio.
- Geometric (vertex-normal) and texture (normal-map) variances add in alpha^2 because they come from independent sources. Use addition, not Valve's `max`.

### 10.4 Pitfalls and cost
- KAPPA bounds the damage at silhouettes and on helper lanes, where derivatives explode.
- Edge pixels shaded per sample use each sample's own triangle roughness, which is correct.
- Do not differentiate the normal-mapped normal. It double-counts the baked variance and flickers.
- Cost: the normal-based isotropic filter added about 0.1 ms at **8K** forward shading on an RTX 2080 (JCGT 2021 Table 2). On an RX Vega 56 at 8K it added about 0.1 ms (I3D 2019 Table 2: Sponza 1.80 to 1.88-1.90 ms). At 1080p it is negligible.
- Quality (JCGT 2021, deferred, GGX, 1080p, error against 16384-spp references): Kaplanyan 2016 RMSE 0.453, eq. 13 0.445, less-conservative eq. 14 0.396 (Sponza).

---

## 11. Cost of MSAA: what is published, and estimates for the RTX 5060

Published numbers (all on old hardware; nothing comparable was found for Ada/Blackwell):

| Source | Hardware / resolution | Result |
|---|---|---|
| MJP 2016 | GTX 970, 1080p, 4x | G-buffer 0.67 to 1.48 ms (2.2x). CS lighting 3.54 to 4.64 ms (+31%). Mask 0.16 ms, resolve 0.21 ms. Frame +33%. |
| SBAA 2012 (Lauritzen reference) | HD 5870, 720p | Sponza 4x 6.0/8.7 ms, 8x 10.5/13.0 ms (simple/complex lighting). About 4% per-sample pixels at 4x (Sponza); about 16% at 8x (GRASS). |
| BF3 2011 | 1080p 4x | G-buffer + Z = 158 MB |
| Crysis 3 2013 | not stated | Quad tagging of the per-sample mask saved about 1 ms on average |

Back-of-envelope for the RTX 5060 (448 GB/s: 128-bit GDDR7 at 28 Gbps; 3840 shaders; Guru3D lists 48 ROPs; sources disagree on L2, 24 vs 32 MB). These are **upper bounds assuming no compression and peak bandwidth, not measurements**:
- 1080p has 2,073,600 pixels; at 2x that is 4,147,200 samples.
- G-buffer at 16 B/sample + D24S8 at 4 B/sample = 83 MB. One uncompressed write is about 0.19 ms of bandwidth. Interior pixels, where both samples are equal, should compress (the mechanism is undocumented on NVIDIA).
- Lighting buffer RGBA16F 2x is 33 MB. One full-screen additive light pass (read sample 0's G-buffer at about 16 B/pixel, plus a blend read-modify-write of 2 x 8 B per pixel) is about 66-100 MB, or 0.15-0.22 ms. Scissoring and stencil culling are what keep local lights cheap.
- Total MSAA-related memory at 1080p is about 150 MB: depth-stencil 16.6 MB, G-buffer about 66 MB, lighting 33 MB, LDR layers 16.6 MB, SMAA about 12 MB, edge mask 2 MB. That is easily within 8 GB.

**Quad-overdraw multiplier.** With MSAA, a triangle gets a PS invocation for every pixel where it covers **any** sample, not just the pixel center. Every pixel on a triangle edge is therefore shaded by both triangles, and partial 2x2 quads add helper lanes. The extra base-pass cost grows with total edge length in pixels, so it is large for small triangles. Sample-frequency shading multiplies again, but only on EDGE pixels. Measure it with `PSInvocations` (section 13). This is the video's cost-multiplier statement, and it argues for good LOD and quad-friendly meshes. Flat flags do **not** reduce base-pass invocations; they only reduce per-sample lighting.

---

## 12. Recommended implementation

### 12.1 Pass list (MSAA-relevant parts)

| # | Pass | Freq. | RT / DSV | Notes |
|---|---|---|---|---|
| 1 | (Partial) depth prepass | pixel | 2x DSV | Alpha-tested objects output the same `SV_Coverage` as the base pass. |
| 2 | Base pass (G-buffer) | pixel | 2x G-buffer RTs + 2x DSV | `StencilRef = model << 2` per draw. Writes the per-sample edge bit (mode B/C) and the spec-AA roughness. GS bound only in flat-edge mode. |
| 3 | G-buffer resolve / EDGE mark | pixel | DSV read-only depth, no RTV | Discards simple pixels. Optional quad tagging. Variant B adds the disable draw. |
| 4 | (Debug) edge-sample occlusion query | pixel | read-only DSV | `D3D11_QUERY_OCCLUSION` around a stencil-test-EDGE draw counts edge samples. |
| 5 | Lighting: sun/ambient per model x {pixel, sample}; local lights: volume mark + per model x {pixel, sample} | pixel / sample | 2x lighting RT + DSV read-only depth | Scissor per light. `[earlydepthstencil]`. Additive blending. |
| 6 | Sky | pixel | 2x lighting RT | Stencil MODEL == 0 (and not DISABLED). |
| 7 | Forward/transparent | pixel | 2x | Other note. |
| 8 | HDR box resolve (if bloom/exposure need it) | pixel | 1x RGBA16F | Average of 2 samples. |
| 9 | Tonemap + separate | pixel | 3 x 1x RTs | Per-sample tonemap into 2 LDR layers + R8 edge mask. |
| 10 | SMAA S2x | – | 1x | SMAA 1x per layer; second layer blends at 0.5. Temporal part is in the TAA note. |

### 12.2 Formats

| Resource | Format | Samples | Size at 1080p |
|---|---|---|---|
| Depth-stencil | `R24G8_TYPELESS` (DSV `D24_UNORM_S8_UINT`, SRVs `R24_UNORM_X8_TYPELESS` / `X24_TYPELESS_G8_UINT`). A/B against `R32G8X24` (8 B) with reversed Z, which belongs to the depth note. | 2, standard | 16.6 MB |
| G-buffer RTs | Defined in the G-buffer note. **Reserve 1 bit per sample for the EDGE flag** (e.g. A of an `R10G10B10A2` normal target, or a spare bit of a 16-bit material-ID channel holding 13-bit IDs). | 2, standard | ~66 MB at 16 B/sample |
| Lighting | `R16G16B16A16_FLOAT` (A/B `R11G11B10_FLOAT`) | 2, standard | 33.2 MB |
| LDR layers | 2 x `R8G8B8A8_TYPELESS` (sRGB RTV, UNORM and sRGB SRVs) | 1 | 16.6 MB |
| Edge mask | `R8_UNORM` | 1 | 2.1 MB |

### 12.3 Stencil bit layout (8 bits)

| Bits | Mask | Name | Written by | Meaning |
|---|---|---|---|---|
| 0 | 0x01 | LIGHT | light volume (INVERT), cleared by lighting draws (ZERO) | Sample is inside the current light volume |
| 1 | 0x02 | LIGHT_AUX | reserved for the stencil-volume note | A second volume bit, or bits 0-1 as a 2-bit `_WRAP` counter if INCR/DECR is preferred |
| 2-4 | 0x1C | MODEL | base pass (REPLACE, per-draw ref) | 0 = sky/empty/disabled; 1..7 = shading models (default, skin/dual-normal, foliage, ...) |
| 5 | 0x20 | EDGE | G-buffer resolve | Pixel (or quad) needs per-sample shading |
| 6 | 0x40 | DISABLED | variant B only | Sample 1 of a simple pixel; fill from sample 0 at tonemap |
| 7 | 0x80 | free | – | e.g. Crysis-style dynamic-object or decal mask |

The counter bits sit at the bottom so INCR/DECR carries cannot corrupt other fields (section 2). If 3 models are enough, MODEL can shrink to 2 bits.

### 12.4 Depth-stencil states

`DE/DW/DF` = DepthEnable / DepthWriteMask / DepthFunc. Front and back faces are identical unless noted.

| State | DE/DW/DF | Read / Write mask | Func | Pass / Fail / DepthFail | Ref |
|---|---|---|---|---|---|
| `BASEPASS` | 1 / ALL (ZERO after prepass) / LESS_EQUAL (EQUAL after prepass) | 0x00 / 0x1C | ALWAYS | REPLACE / KEEP / KEEP | `model << 2` per draw |
| `GBUFFER_RESOLVE` (EDGE mark) | 0 / ZERO / – | 0x00 / 0x20 | ALWAYS | REPLACE / KEEP / KEEP | 0x20 |
| `DISABLE_SAMPLE1` (variant B; PS outputs oMask = 0b10) | 0 / ZERO / – | 0x00 / 0x5C | ALWAYS | REPLACE / KEEP / KEEP | 0x40 |
| `LIGHT_VOLUME` (null PS, `CullMode NONE`, `DepthClipEnable FALSE`) | 1 / ZERO / LESS_EQUAL | 0x1C / 0x01 | NOT_EQUAL | KEEP / KEEP / **INVERT** | 0 |
| `LIGHT_PIXEL(m)` | 0 / ZERO / – | 0x3D / 0x01 | EQUAL | ZERO / KEEP / KEEP | `0x01 \| m<<2` |
| `LIGHT_SAMPLE(m)` | 0 / ZERO / – | 0x3D / 0x01 | EQUAL | ZERO / KEEP / KEEP | `0x01 \| m<<2 \| 0x20` |
| `SUN_PIXEL(m)` / `SUN_SAMPLE(m)` | 0 / ZERO / – | 0x3C / 0x00 | EQUAL | KEEP / KEEP / KEEP | `m<<2` / `m<<2 \| 0x20` |
| `SKY` | 0 / ZERO / – | 0x5C / 0x00 | EQUAL | KEEP | 0 |
| Variant B lighting (sample freq.) | 0 / ZERO / – | 0x1D / 0x01 | EQUAL | ZERO / KEEP / KEEP | `0x01 \| m<<2` |

Notes:
- Every lighting, mark or volume pass binds the **read-only-depth DSV** (the engine already creates `dsv_readonly_depth`) so the depth SRV can be sampled while stencil is written.
- The tonemap binds no DSV, so it may read the stencil SRV.
- Standard depth is shown; swap LESS_EQUAL and GREATER_EQUAL for reversed Z.

---

## 13. A/B test plan

**Instrumentation** (build once):
- GPU timestamps per pass (`D3D11_QUERY_TIMESTAMP_DISJOINT` + `TIMESTAMP`). Average over at least 300 frames on fixed camera paths with vsync off.
- `D3D11_QUERY_PIPELINE_STATISTICS.PSInvocations` per pass.
- Edge-sample count: `D3D11_QUERY_OCCLUSION` around a full-screen draw with `Func EQUAL, ReadMask = EDGE, Ref = EDGE` and a trivial PS. Edge % = samples / (2 x W x H).
- Debug views:
  - EDGE mask overlay, coloured by cause: coverage bit, normal test, removed by flat flags.
  - "Missed edge" view: pixels where mode C flags an edge but mode A does not. These are the coverage points the video says Crysis threw away.
  - Per-pixel model ID.
- Image metrics against a **reference = same MSAA count, per-sample shading everywhere** (EDGE forced to 1):
  - RMSE/PSNR, or NVIDIA FLIP run offline on captured frames.
  - Temporal flicker: mean |L(t) - L(t-1)| over a slow pan (0.1-0.3 px/frame) and a slowly rotating object.
  - Optional ground truth: 4x SSAA (render 3840x2160, box-downsample).

**Scenes**:
1. Interior with large flat walls and floors made of many triangles (flat flags should matter most).
2. Dense curved props or characters (bent edges).
3. Alpha-tested foliage.
4. Glossy, low-roughness, high-curvature objects plus a distant normal-mapped plane (specular AA).
5. Many local lights (e.g. 64 point and spot lights).

| Test | Configurations | Measure | Hypothesis / video claim |
|---|---|---|---|
| T1 MSAA cost | off / 2x / 4x (4x ends in a box resolve, since S2x is 2x-only) | base pass ms and PSInvocations; lighting ms; total | Base-pass PSInvocations rise with edge density (the video's cost multiplier). 2x is affordable at 1080p on the 5060. |
| T2 Edge mode | ALL / A compare / B raw coverage / C coverage+flat / C+N | edge %, lighting ms, RMSE vs ALL, flicker | Ordering: B is the most expensive and most correct; A cheapest but loses material-boundary and thin-geometry edges; C is close to B in quality and close to A in cost. |
| T3 Flat-edge overhead | no GS / GS bound with flags = 0 / GS + flags / de-indexed vertex-bit meshes | base pass ms | GS cost on Blackwell is small enough that the savings in T2 outweigh it. |
| T4 Quad tagging | per pixel / per 2x2 quad | per-sample pass ms, edge % | Crysis reports about 1 ms saved. Expect a net win despite more per-sample pixels. |
| T5 Variant | A (2 draws per model, pixel + sample) / B (1 sample-freq draw, disabled samples) | lighting ms, tonemap ms, lighting-buffer bandwidth (Nsight if available) | The video expects B to be faster. B may lose MSAA compression. |
| T6 Light culling | stencil volume + full-screen with scissor / scissor only (no volume) / volume mesh as the lighting draw | lighting ms per light vs light screen size | The video favours stencil volumes. Lauritzen 2010 found two-pass stencil not worth its cost. Measure 2x, where volume ROP cost doubles. |
| T7 Lighting format | RGBA16F / R11G11B10F | lighting + tonemap ms, banding | R11G11B10F saves bandwidth (Crysis 3 light buffers). |
| T8 Alpha | clip / A2C / alpha-test SSAA via `SV_Coverage` | edge %, lighting ms, flicker on foliage | SSAA gives the best stability, at the cost of more per-sample lighting. |
| T9 Specular AA | none / Valve-UE / Tokuyoshi conservative / Tokuyoshi less-conservative; normal-map variance mips on/off | flicker, RMSE vs supersampled reference, base pass ms | Tokuyoshi lowers error and cost compared with Valve's heuristic (JCGT 2021). Cost is about 0. |
| T10 EDGE-mask reuse | temporal blend everywhere / only where edge mask = 1 | texture sharpness (MTF-like test chart), flicker | Hook for the TAA note: tests the video's idea that temporal logic should only touch MSAA edges. |

Success criteria for the video's MSAA claims:
- (a) Mode C's edge % is significantly below B's on scene 1, with RMSE versus ALL no worse than B's.
- (b) Mode A shows visible missed edges in the "missed edge" view where C does not.
- (c) Total frame time at 2x stays within the 16.6 ms budget alongside the rest of the pipeline.

---

## 14. Open questions / coordination items

1. **G-buffer layout note**: which channel holds the per-sample EDGE bit (RGB10A2.a or a spare material-ID bit)? Is the shading model per draw (stencil ref works) or per pixel (needs the 6.4 fallback)? How many shading models are there: 2 or 3 MODEL bits?
2. **Stencil-volume note**: confirm the 1-bit INVERT z-fail marking (or a 2-bit `_WRAP` counter in bits 0-1). Also scissor plus full-screen per model versus the light mesh as the lighting draw.
3. **NVIDIA early-stencil/ZCull behaviour** with frequent ref/mask changes and with lighting draws that both test and write stencil. Undocumented, so measure (T5, T6).
4. **GS throughput on Blackwell** for the flat-edge mode, versus de-indexed meshes (T3).
5. **Post chain**: which passes need a non-MSAA HDR image (bloom, exposure, motion blur, which the video puts before tonemap), and whether the box resolve is enough. Belongs to the post-processing note.
6. **Shadows**: in-shader evaluation in each lighting draw, or an MSAA-aware screen-space mask (7.4).
7. **Temporal**: store last frame's EDGE mask or coverage (video [1:42]). Needs a 1x R8 history texture.
8. PSL is unavailable in D3D11 core, so the video's custom 2-sample pattern [1:41] cannot be implemented. Decide whether that idea is dropped or deferred to a D3D12 port.

---

## Sources

| Source | What it provided |
|---|---|
| Video transcript (local: `scratchpad/yt/transcript_wrapped.txt`), https://www.youtube.com/watch?v=SnNm7rSSvlg | The proposals and timestamps in section 1 |
| Microsoft, D3D11.3 Functional Spec: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm | Input coverage (16.3.2, not affected by depth/stencil), oMask (16.9.4), forced early depth-stencil (16.12.1), standard sample patterns and required counts (19.2.4/19.2.5), `eval_sample_index` (22.4.23) |
| MS docs, Shader Specified Stencil Reference Value (D3D11): https://learn.microsoft.com/en-us/windows/win32/direct3d11/shader-specified-stencil-reference-value | `SV_StencilRef` is optional in D3D11.3, `PSSpecifiedStencilRefSupported` |
| Wikipedia, Feature levels in Direct3D (raw wikitext of the support table): https://en.wikipedia.org/wiki/Feature_levels_in_Direct3D | PS stencil ref unsupported on all NVIDIA architectures from Fermi to Blackwell; supported on AMD and Intel Gen9+ |
| MS docs, D3D11_STANDARD_MULTISAMPLE_QUALITY_LEVELS: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_standard_multisample_quality_levels | Standard and center pattern quality values; 2x support optional |
| Lauritzen 2010 slides (Intel CDN blocked; archived copy): https://web.archive.org/web/2020id_/https://software.intel.com/sites/default/files/m/d/4/1/d/8/lauritzen_deferred_shading_siggraph_2010.pdf ; article page: https://www.intel.com/content/www/us/en/developer/articles/technical/deferred-rendering-for-current-and-future-rendering-pipelines.html | Edge identification, stencil vs branching, two-pass per-pixel/per-sample, per-sample scheduling, tiled packing, two-pass stencil volumes judged not worth their cost |
| Intel deferred shading sample code (Apache 2.0), mirrored in GPU Pro 7 sources: https://github.com/QianMo/GPU-Pro-Books-Source-Code/tree/master/GPU-Pro-7/03_Rendering/02_Deferred%20Coarse%20Pixel%20Shading (`GBuffer.hlsl`, `GPUQuad.hlsl`, `App.cpp`, `Texture2D.cpp`) | Exact edge test, mark-pass and lighting stencil states, read-only-depth DSV with depth SRV during stencil writes, D32S8 formats |
| Sousa, "CryENGINE 3 Graphics Gems", SIGGRAPH 2013: https://web.archive.org/web/2016id_/http://www.crytek.com/download/Sousa_Graphics_Gems_CryENGINE3.pdf (also https://www.yumpu.com/en/document/read/51951811/cryengine-3-graphics-gems-crytek/5) | Custom resolve + per-sample mask, avoiding SV_Coverage, stencil bit 0x80 / write mask 0x7F, quad tagging, alpha-test SSAA code, shortcuts (pixel-frequency shadows + bilateral, max-depth soft particles), tonemap-after-resolve issue |
| Sousa, Wenzel, Raine, "The Rendering Technologies of Crysis 3", GDC 2013 (SlideShare, read via the agent laptop): https://www.slideshare.net/slideshow/rendering-technologies-from-crysis-3-gdc-2013/25052434 | Speaker notes: quad tagging saves about 1 ms, depth/normal-threshold mask, `EvaluateAttributeAtSample` tip, wish for PS stencil writes, R11G11B10F light buffers, stencil usage, DOF/MB at pixel frequency |
| Thibieroz (AMD), "Deferred Shading Optimizations", GDC 2011: https://web.archive.org/web/2015id_/http://developer.amd.com/wordpress/media/2012/10/Deferred%20Shading%20Optimizations.pps | Centroid trick yields too many edges; depth/normal detection; stencil light-volume inc/dec method; MSAA blending cost; discard zero-light pixels |
| Andersson (DICE), "DirectX 11 Rendering in Battlefield 3", GDC 2011: https://web.archive.org/web/2012id_/http://publications.dice.se/attachments/GDC11_DX11inBF3_Public.pdf | 158 MB for 4x G-buffer at 1080p, CS per-sample list, 4x standard offsets, alpha-test SSAA via SV_Coverage |
| Valient, "Rendering Technology of Killzone 2" (GDC 2009 extended notes): https://www.guerrilla-games.com/media/News/Files/GDC09_Valient_Rendering_Technology_Of_Killzone_2_Extended_Presenter_Notes.pdf | Per-pixel shader lighting all samples and averaging; branching not worth it at 2x |
| NVIDIA GameWorks sample "Antialiased Deferred Rendering": https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/d3d_samples/antialiaseddeferredrendering.htm | SV_Coverage vs discontinuity detection trade-off; two stencil passes; unique-coverage adaptive shading |
| MJP, "Bindless Texturing for Deferred Rendering and Decals" (2016): https://therealmjp.github.io/posts/bindless-texturing-for-deferred-rendering-and-decals/ | GTX 970 1080p timings, no MSAA vs 4x MSAA deferred |
| MJP, "Experimenting with Reconstruction Filters for MSAA Resolve": https://therealmjp.github.io/posts/msaa-resolve-filters/ | Tonemap-before-resolve trade-offs |
| Salvi & Vidimče, "Surface Based Anti-Aliasing", I3D 2012: https://vidimce.org/publications/sbaa/sbaa_i3d2012.pdf | Surface-merging prior art; Lauritzen reference timings and shaded-sample counts on HD 5870 |
| Kerzner & Salvi, "Streaming G-Buffer Compression for MSAA", HPG 2014: https://web.archive.org/web/2016id_/https://software.intel.com/sites/default/files/managed/50/32/gbufcomp.pdf | Merge metric (normals within pi/4, overlapping depth intervals, exclusive coverage); coverage input ignores depth |
| Crassin et al., "Aggregate G-Buffer Anti-Aliasing": https://research.nvidia.com/labs/rtr/publication/crassin2015agaa | AGAA summary and numbers |
| Persson, GPAA / GBAA: https://www.humus.name/index.php?page=3D&ID=86 , https://www.humus.name/index.php?page=3D&ID=87 | Offline or adjacency-GS extraction of relevant edges, skipping internal edges |
| SMAA reference implementation: https://github.com/iryoku/smaa/blob/master/SMAA.hlsl | S2x procedure, `SMAASeparatePS`, separating samples in the tonemap pass, subsample indices, sRGB rules, standard-pattern requirement |
| Tokuyoshi & Kaplanyan, "Improved Geometric Specular Antialiasing", I3D 2019: http://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf | Deferred isotropic kernel (trace bound, world-space normal derivatives), sigma = 0.5, kappa = 0.18, Vega 56 timings |
| Tokuyoshi & Kaplanyan, "Stable Geometric Specular Antialiasing with Projected-Space NDF Filtering", JCGT 2021: https://www.jcgt.org/published/0010/02/02/paper-lowres.pdf | Deferred listing constants (sigma^2 = 1/(2*pi)), less-conservative variant, RTX 2080 cost (about 0.1 ms at 8K), RMSE comparison |
| Vlachos (Valve), "Advanced VR Rendering", GDC 2015: https://alex.vlachos.com/graphics/Alex_Vlachos_Advanced_VR_Rendering_GDC2015.pdf | Valve geometric-roughness formula, centroid/center double-normal trick, roughness mips from normal maps |
| Unreal Engine `BasePassPixelShader.usf` / `SceneRendering.cpp` (UE4-era copies on GitHub, e.g. https://github.com/OpenHUTB/engine/blob/hutb/Engine/Shaders/Private/BasePassPixelShader.usf) | UE `NormalCurvatureToRoughness` equals Valve's formula (exponent 0.333, scale 1, bias 0, `max`); centroid-derivative caveat |
| UE docs, Composite Texture: https://docs.unrealengine.com/4.26/en-US/RenderingAndGraphics/Textures/Composite | Normal-map variance into a roughness channel, via mips |
| Unity SRP `CommonMaterial.hlsl`: https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.core/ShaderLibrary/CommonMaterial.hlsl | Cross-check of the kernel; `TextureNormalVariance` (vMF kappa to variance, The Order 1886 / Han et al.) |
| Guru3D, GeForce RTX 5060 8 GB review: https://www.guru3d.com/review/geforce-rtx-5060-8gb-review/ (plus itc.ua and Geeks3D listings for the L2 and ROP disagreement) | 448 GB/s, 3840 shaders, 48 ROPs |
| Background cited but not re-read: Toksvig 2005 (JGT); Olano & Baker, LEAN (I3D 2010); Baker, CLEAN (2011); Kaplanyan et al., HPG 2016; Neubelt & Pettineo, SIGGRAPH 2013 (The Order: 1886) | Context for 10.1, as summarised in the papers and code above |
