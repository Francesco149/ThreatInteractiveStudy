# R1: Frame setup, clears, depth pre-pass, base pass order, G-buffer layout

Research note for our D3D11 educational engine (Windows, FL 11_0/11_1, SM 5.0, no vendor SDKs,
deferred, 2x MSAA, 1080p / 60 fps, test machine RTX 5060 8 GB + Ryzen 9 5900X).

It covers the part of Threat Interactive's video "Tutorial: How To Optimize Almost Every Step In
Modern Game Rendering" (2026-08-01) from about 8:00 to 39:00: resource clears, the partial
non-alpha-tested depth pre-pass, base pass ordering, quad overdraw, depth format, G-buffer layout,
LOD dithering and decals. Statements marked **[video]** are the video's claims, from Threat
Interactive's own captures. Statements marked **[ours]** are our design decisions. Everything else
is backed by the sources at the end.

Other notes cover MSAA coverage/resolve, deferred lighting and shadows. This note only fixes the
interfaces they depend on: stencil bits, the motion-vector/coverage buffer, and the light buffers.

---

## 0. Decisions at a glance

| Topic | Decision |
|---|---|
| Depth | `D32_FLOAT_S8X24_UINT`, reverse-Z, infinite far plane, clear to depth 0 / stencil 0, depth func `GREATER_EQUAL` for every main-view draw. The comparison direction never flips. |
| Clears | Hardware-clear only: depth+stencil (together), the emissive / diffuse-light buffer, the motion-vector+coverage buffer and (optionally) the dual-normal buffer. Always clear to 0, batched at frame start. Never clear GB0–GB2 or any full-screen-overwritten target. Never clear with a draw. |
| Not-cleared targets | Every consumer must mask reads by stencil (shading model 0 means sky or an empty sample) or by `depth == 0`. Calling `DiscardView` on them is an optional A/B. |
| Pre-pass | Two steps. **Step A:** large/close, opaque, non-alpha-tested, non-fading static meshes, position-only stream, null PS. **Step B:** dynamic or dual-normal (skin) occluders, writing depth plus motion vectors with an ALU-only PS. No texture sampling ever. Target < 0.3 ms. |
| Base pass order | (1) close alpha-tested, depth write on → (2) non-pre-passed opaque, front-to-back → (3) remaining alpha-tested → (4) pre-passed meshes, `GREATER_EQUAL` (≡ `EQUAL`), no depth write. |
| G-buffer (all 2x MSAA) | GB0 `R10G10B10A2_UNORM` = world normal xyz*0.5+0.5, A2 = {aniso flag, lighting-channel bit}. GB1 `R8G8B8A8_UNORM_SRGB` = albedo + linear roughness. GB2 `R8G8B8A8_UNORM` = {profile blend ("old metallic"), specular/IOR scale, DMP ID low, DMP ID high}. Emissive goes into the diffuse-light `R16G16B16A16_FLOAT` (cleared). MV+coverage `R16G16B16A16_FLOAT` (cleared). Dual normal `R10G10B10A2_UNORM` (skin only). |
| Shading model ID | Lives in **stencil bits 0–2**, written by the base pass through `StencilRef` + `REPLACE`, not in a G-buffer channel. Decals never touch it. |
| Quad overdraw | Built-in debug view plus statistics using UAV quad locking (Hill 2012). This is the primary content metric, ahead of triangle count. |
| Content prep | meshoptimizer: vertex cache → overdraw (1.05) → fetch. Separate position stream plus a shadow/pre-pass index buffer. Quantized formats (§3.6). |
| LOD fade | ALU 2x2 Bayer, complementary between the two LODs, pattern shifted by (1,1) on odd frames. MSAA variant through `SV_Coverage` (8 levels). Fading objects leave the pre-pass. Shadows get the same dither (discard variant). |
| Decals | Box-projected, written straight into GB0–GB2 with per-RT blend states (`IndependentBlendEnable`). Soft pass for RGB channels, opaque-threshold pass for roughness and DMP ID. No D-buffer, no stencil writes. |

---

## 1. Frame setup and resource clearing

### 1.1 What the video proposes [video]
- G-buffers that are fully rewritten each frame (base color, normals, and so on) are never cleared. Clearing depth/stencil is enough; stale data is overwritten "for free" by the pixels that write it.
- Buffers that are only partly written **must** be hardware-cleared:
  - the emissive/diffuse RGBA16 buffer (only emissive materials write it);
  - the buffer holding MSAA coverage, which is stored with the motion vectors;
  - the "dual normal" buffer (only skin writes it).
- The specular RGBA16 buffer is not cleared because a later pass overwrites or recycles it.
- Post-process outputs should be fully overwritten rather than cleared. Examples: Crysis 3's post stage, NFS 2015's HBAO pass. Counter-example: UE5 clears its tonemapper output before overwriting it.
- Never clear with a full-screen triangle or quad. On modern hardware that is always slower than a hardware clear.
- "Hijack clear": a pass that runs earlier (for example in the previous frame) writes an extra MRT output holding the clear value, so the next user of that buffer needs no clear.
- Hardware clear cost differs between engines. Crysis 3 cleared an RGBA8 target 2–3x faster than UE5 DX12 did.
- The expected total win is at least about 0.1 ms. Studios should publish their clear costs with hardware and resolution.

### 1.2 How hardware clears work (background)
- **Fast clears only touch metadata.** On ATI/AMD, a fast Z clear marks every compression tile as "cleared" instead of writing every sample (Persson 2007). AMD's RDNA guide says fast clears are designed to be about 100x faster than normal clears. NVIDIA's guidance is "never use clear shaders as a replacement for API clears": clear shaders disable the hardware optimisations and cost CPU and GPU time. The only exception is a compute clear overlapped with other compute work (NVIDIA 2022). This is the mechanism behind the video's full-screen-triangle rule: a draw must write every sample through the ROPs, while a fast clear writes a few bits per tile.
- **Fast-clear values.**
  - AMD: colour fast clears are limited to RGBA (0,0,0,0), (0,0,0,1), (1,1,1,0) and (1,1,1,1). Depth fast clears need 0.0 or 1.0 with stencil 0. Fast clears must cover the whole image, so a rect `ClearView` is not fast (AMD RDNA guide).
  - NVIDIA: any colour works, but the hardware tracks only a limited set of clear colours per frame: 25 on Ampere, 10 on Turing (NVIDIA 2022). Khronos' validation layers encode the NVIDIA rule as follows. 0.0 and 1.0 are always compressed. Custom colours compress only for RGBA8/BGRA8, RGB10A2, RGBA16 (UNORM/SNORM/UINT/SINT/FLOAT), RGBA32F and R11G11B10F. The layer warns once more than 16 distinct custom colours are used.
  - For depth with a `GREATER` test, NVIDIA recommends clear values in [0, 0.5).
  - **[ours]** Always clear to 0 (or 1). That is fast on every vendor and every format.
- **Clear depth and stencil together.** On ATI/AMD, depth and stencil share a buffer, so clearing only one part becomes a slower read-modify-write (Persson 2007). AMD also requires stencil = 0 for a depth fast clear (RDNA guide).
- **Batch clears.** NVIDIA recommends grouping clears and not interleaving single clears with rendering.
- **Depth must be cleared every frame.** Without a clear, tiles drift into the uncompressed state frame after frame until Z compression is effectively gone. The direction-dependent Hi-Z/Z-cull state is also only reset by a clear (Persson 2007; Khronos validation layers' NVIDIA Z-cull check).
- **Colour targets you fully overwrite do not need a clear.** AMD's DCC article: clear only when needed; if the whole surface will be written, a clear is usually unnecessary (Brennan 2016). Stale compressed tiles are simply replaced on write. Their old state only matters if something reads them, and our consumers never read stale pixels (§1.4).
- **`DiscardView` / `DiscardResource` (D3D11.1).** These tell the driver that a view's contents are no longer needed (Microsoft docs). In DX12/Vulkan, AMD recommends Discard / `LOAD_OP_DONT_CARE` to break dependencies when a clear is skipped. In D3D11 the driver tracks hazards itself, and what desktop drivers do with the hint is not documented. Treat it as an A/B experiment (§10, T2), not a requirement.
- **`ClearView` (D3D11.1)** can clear rects and works on RTVs and UAVs, and on DSVs only when the format has no stencil. Partial clears are not fast clears on AMD. Avoid it.

### 1.3 Clear rules for our engine [ours]
Issue all clears in one batch at the start of the frame, before the pre-pass:

```cpp
// Frame start: one batch, fast-clear values only.
ctx->ClearDepthStencilView(dsvMain, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f /*reverse-Z far*/, 0);
static const float kZero[4] = { 0, 0, 0, 0 };
ctx->ClearRenderTargetView(rtvDiffuseLight /*emissive is written here*/, kZero);
ctx->ClearRenderTargetView(rtvMotionCoverage, kZero);   // A = 0: "no object motion written"
ctx->ClearRenderTargetView(rtvDualNormal, kZero);       // optional: see 1.4
// NOT cleared: GB0, GB1, GB2, specular light buffer, every full-screen post-process output.
// Optional A/B (needs ID3D11DeviceContext1): ctx1->DiscardView(rtvGB0); ... rtvGB2;
```

| Target | Clear? | Why |
|---|---|---|
| Depth/stencil | Yes, 0.0 / 0, together | Required for correctness, compression and Hi-Z reset |
| GB0 / GB1 / GB2 | **No** | Every geometry sample overwrites them; sky and empty samples are masked by stencil |
| Diffuse light (holds emissive) | Yes, 0 | Only emissive materials write it during the base pass, and lighting adds on top |
| Motion + coverage | Yes, 0 | Only dynamic objects write motion. A = 0 means "static: reproject from depth and camera". Coverage layout belongs to the MSAA note |
| Dual normal | Yes (cheap) or No | Only skin writes it. Safe without a clear only if every read is stencil-masked to skin samples |
| Specular light | No | The lighting note overwrites or recycles it [video] |
| Post outputs (tonemap, AO, ...) | **No** | They are written in full by a full-screen pass: no blending, no discard, full write mask |

### 1.4 Requirements for not clearing
- Sky and samples not covered by geometry keep **last frame's data** in GB0–GB2 (on the first frame, uninitialised memory). Every pass that reads the G-buffer must reject those samples. Either test stencil `(stencil & 0x07) != 0` (shading model 0 = empty, see §6.4) or check `depth == 0`. The per-shading-model lighting draws are stencil-tested, so they already satisfy this.
- Debug views and screenshot tools will show garbage in the sky. That is expected.
- Any filter that reads neighbours (SSAO, SSS blur, SSR) must treat empty samples as invalid instead of trusting their contents.
- Resizing the swap chain or recreating targets changes nothing, because the contents are never trusted.
- Any base-pass RT that a draw does not write must have `RenderTargetWriteMask = 0` for that draw. The D3D11 spec says that if enabled write-mask components receive no shader output, **undefined values are written** (D3D11.3 functional spec §17.15). Missing this corrupts the cleared emissive and motion buffers.

### 1.5 Why a full-screen triangle "clear" is slower
A full-screen draw writes every sample: at 1080p 2x MSAA that is 4,147,200 samples × bytes per sample.
- RGBA16F 2x MSAA = 33 MB.
- At the RTX 5060's 448 GB/s peak that is about 0.074 ms for one target, before compression.
- The ROP limit (48 ROPs × about 2.5 GHz) gives about 0.035 ms or more.

A fast clear writes only metadata, a few microseconds. The draw also leaves the target in a "written" compression state instead of the cheap "cleared" state. UE even ships a cvar for the bad option (`r.ClearSceneMethod 2` = "Quad at max z"; options 0 = no clear, 1 = RHI clear).

### 1.6 Full-screen overwrite of post-process outputs
Any pass that covers the whole viewport with a full-screen triangle and writes all channels needs no prior clear. Requirements: no blending, no discard/clip, write mask 0xF, a viewport and scissor that cover the target, and every pixel written. **[ours]** All post-process targets follow this rule. Never put a clear in front of a pass like this.

### 1.7 "Hijack clear" in D3D11: mechanics and constraints [ours, analysis]
How it works: a pass P, which runs after the last read of buffer X (possibly in the previous frame), binds X as an extra RTV and outputs the clear value (for example 0) to it. That pass must:
1. **Cover every sample of X**: a full-screen triangle with no stencil or depth rejection, no discard, and full write mask.
2. **Match X's sample count.** D3D11 requires every RTV and the DSV bound together to share size and sample count. A post-process pass on a 1x target cannot hijack-clear a 2x MSAA G-buffer.
3. **Not read X in the same pass.** D3D11 forbids binding a subresource as SRV and RTV at once; the runtime unbinds the SRV.

Cost model: the extra MRT export is a real full-surface write. A constant colour compresses well, but it still costs ROP and bandwidth work. A fast clear costs only metadata. So a hijack clear can only win when:
- the hardware clear is *not* fast (wrong value, an incompressible format, or a driver slow path);
- **or** pass P is ALU-bound, so the extra export is hidden.

In our pipeline, the 2x MSAA buffers that need clearing (diffuse light, motion+coverage, dual normal) have no natural full-coverage 2x MSAA host pass after their last read: the lighting draws are stencil-masked, and the resolve writes 1x. **Recommendation:** do not build the frame around hijack clears. Implement them as a micro-benchmark (T1f) to test the claim, and keep them as an option for 1x post-process scratch buffers.

The "Crysis 3 clears RGBA8 2–3x faster than UE5 DX12" observation [video] has candidate explanations we can't check from outside:
- In D3D12, clears that don't match the resource's optimized clear value take a slow path.
- UE's render graph adds barriers or transitions around the clear.
- Some drivers need a fast-clear-eliminate before the target is sampled.

We are on D3D11, where the driver picks the clear path. We will measure our own clears (T1) rather than assume.

### 1.8 Pitfalls
- Clearing only depth on a depth+stencil surface leads to a read-modify-write slow path. Always clear both.
- A render target bound in the base pass but not written by a permutation, without write mask 0, gets undefined data (§1.4).
- Too many distinct custom clear colours: NVIDIA tracks only a limited set. Stick to 0 and 1.
- `ClearView` with rects is not a fast clear on AMD.
- `DiscardView` on a target that is later read unmasked returns garbage, by design.

### 1.9 How to measure (details in §10)
- T1: isolated clear micro-benchmarks per format, sample count and method.
- T2: in-frame clear policy, comparing "clear all" vs "clear only required" vs "full-screen-triangle clears" vs "+DiscardView".
- Record the clear pass time **and** the following base-pass and lighting times, since compression state can move cost between passes.

---

## 2. Depth buffer: reverse-Z, D32_FLOAT_S8X24_UINT

### 2.1 Why
- **[video]** 32-bit depth because of AMD hardware behaviour, plus quality for large environments. Reverse-Z, following NVIDIA's article.
- AMD: 24-bit depth costs the same as 32-bit on GCN and RDNA; use 32-bit for precision and reverse-Z for a better float distribution (RDNA guide). On GCN, depth and stencil live in separate planes (a 32-bit depth plane plus an 8-bit stencil plane, per a Microsoft engineer on gamedev.net). So `D32S8` should not cost the 64 bits/sample its format description suggests. The real cost on NVIDIA is undocumented; measure it (T4).
- Reed (NVIDIA, 2015) "Depth Precision Visualized": a float depth buffer with reversed Z is close to ideal for any perspective projection. With reverse-Z the choice of far plane, including an infinite one, has no measurable effect on precision. With integer 24-bit, reverse-Z barely helps.
- Fox Engine (MGSV) shipped a reversed 32-bit float depth buffer (Courrèges 2017).

### 2.2 Exact setup
Projection, DirectXMath row-vector convention (`v' = v * M`), left-handed, infinite far plane:

```cpp
// clip = (x*xs, y*ys, zNear, z)  ->  depth = zNear / z :  z = zNear -> 1,  z -> infinity -> 0
XMMATRIX ReverseZInfinitePerspectiveLH(float fovY, float aspect, float zNear)
{
    float ys = 1.0f / tanf(0.5f * fovY);
    float xs = ys / aspect;
    return XMMATRIX(xs, 0, 0,     0,
                    0, ys, 0,     0,
                    0,  0, 0,     1,
                    0,  0, zNear, 0);
}
// Finite far, reverse-Z: XMMatrixPerspectiveFovLH(fovY, aspect, /*NearZ=*/zFar, /*FarZ=*/zNear)
//   gives m22 = n/(n-f), m32 = n*f/(f-n)  (near and far swapped).
```

- Linear view depth from the stored depth `d`: infinite far gives `viewZ = zNear / d`. Finite far gives `viewZ = n*f / (d*(f-n) + n)`.
- Clear to 0.0. Depth func `D3D11_COMPARISON_GREATER_EQUAL` (or `GREATER`) for every main-view draw.
- Viewport `MinDepth = 0`, `MaxDepth = 1`. `DepthClipEnable = TRUE` is fine: nothing is ever beyond the far plane because depth only tends to 0.
- Sky: draw it last with depth = 0 (a full-screen triangle whose VS outputs `z = 0, w = 1`), test `GREATER_EQUAL`, no write. It passes only where nothing was drawn. Alternatively, let lighting treat stencil shading model 0 as sky and skip the draw.
- Depth bias (shadow maps use separate buffers): with reverse-Z the bias signs flip. Not needed for the main view.
- Float32 relative precision: Δz/z ≈ Δd/d ≈ 2^-23 ≈ 1.2e-7, so about 1.2 mm at 10 km. The vertex transform is then the precision bottleneck; use camera-relative world positions for big worlds.
- HLSL matrix convention pitfall: DirectXMath stores row-major, while HLSL cbuffers default to column-major. Either upload `XMMatrixTranspose(M)` and use `mul(M, v)`, or declare `row_major` and use `mul(v, M)`. Pick one and route **every** pass through one shared `TransformPosition()` (§3.5).

Resource and views (2x MSAA):

```cpp
D3D11_TEXTURE2D_DESC td = {};
td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1;
td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;          // typeless only so it can also be read as an SRV
td.SampleDesc = { 2, 0 };                            // check CheckMultisampleQualityLevels() > 0 first
td.Usage = D3D11_USAGE_DEFAULT;
td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
// DSV (writable):  Format D32_FLOAT_S8X24_UINT, ViewDimension TEXTURE2DMS
// DSV (read-only): same + Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL
//                  -> lets decals/lighting depth+stencil test while the depth SRV is bound
// SRV depth:   R32_FLOAT_X8X24_TYPELESS  -> HLSL Texture2DMS<float>
// SRV stencil: X32_TYPELESS_G8X24_UINT   -> HLSL Texture2DMS<uint2>, stencil in .g
```

### 2.3 Hi-Z / Z-cull and the comparison direction
- Hi-Z stores one bound per tile: the max for LESS-family tests, the min for GREATER-family tests. The driver picks the mode from the first depth test after a clear. If the direction changes mid-frame, Hi-Z is disabled until the next clear or until the original direction comes back (Persson 2007).
- NVIDIA works the same way. When a depth buffer is drawn with both LESS and GREATER, Z-cull is disabled for the less-used direction until a clear, an UNDEFINED-layout transition or a DONT_CARE load/store resets it. The Khronos validation layers warn when either direction exceeds 20% of the draws.
- **[ours]** The main depth buffer only ever sees `GREATER`, `GREATER_EQUAL` or `EQUAL`. This affects other notes:
  - **Deferred light volumes:** the classic "back faces, depth LESS" test flips the direction. Use a GREATER-only scheme instead. Front faces `GREATER_EQUAL` set a stencil bit; back faces `GREATER_EQUAL` clear it; then light where the bit is set and reset it in the same draw. The camera-inside-volume case needs a fallback.
  - **Decals:** front faces with `GREATER_EQUAL`; when the camera is inside the box, depth test off (§8).
- Stencil `fail` or `zfail` ops other than KEEP disable Hi-Z on ATI. The stencil `pass` op is harmless (Persson 2007). All our stencil writes use `StencilPassOp` only.

---

## 3. Depth pre-pass

### 3.1 What the video proposes [video]
- GPU cost is roughly shaded surface area × shader efficiency. Depth/stencil testing is the cheapest way to cull pixel-shader invocations.
- A pre-pass trades overdraw in expensive passes for overdraw in a depth-only pass. Depth-only, motion-vector and visibility-style passes "chew through" overlap because they sample no textures.
- They stop being cheap when they sample textures: alpha test (opacity maps) or per-pixel displacement. Then the pre-pass draw costs about as much as its base-pass twin (Days Gone, Crysis 3 examples).
- Dense geometry is the other failure. Quad overdraw is unaffected by the pre-pass, and pre-passing can act as a "2x quad-overdraw multiplier" for dense meshes.
- So: a **partial, non-alpha-tested** pre-pass of only **large/close** occluders, with no per-pixel texture sampling. Fox Engine's per-*vertex* texture displacement (terrain) is acceptable.
- Distant and small geometry is not pre-passed: it is small on screen, has poor occlusion value and high quad overdraw.
- Budget below 0.3 ms, the number Half-Life: Alyx reaches for a *full* non-alpha-tested pre-pass in dense urban scenes. We found no public source for that figure; it comes from Threat Interactive's own captures.
- Content prep: triangle reordering and minimal bits per vertex, exposed to artists as simple checkbox workflows.
- A second pre-pass step renders "semi-rare" content that needs dedicated outputs (motion vectors / MSAA data). The video prefers dual-normal (skin) objects for this step over emissives. With that choice the base pass never needs both a motion and a dual-normal output, and alpha-tested content can still be emissive.
- The video argues forward+ usually needs a full alpha-tested pre-pass, which is one reason the pipeline is deferred.
- Occlusion culling driven by the pre-pass is "not well researched" yet.

### 3.2 Published evidence on when a pre-pass pays off
- **ATI/AMD rule of thumb** (Persson 2007): triangles under about 100 pixels are almost certainly not worth putting in a pre-Z pass. Triangles over about 1,000 pixels should seriously be considered unless the pixel shader is trivial. A detailed distant character with a cheap PS probably gains nothing. Render alpha-tested primitives *after* opaque ones, including in the pre-pass, so Hi-Z can reject them.
- **AMD RDNA guide:** use an EQUAL depth test instead of `discard` for pixels already rejected in a Z pre-pass. This argues for an alpha-tested pre-pass, the opposite of the video for alpha-tested content. The guide also suggests overlapping the Z pre-pass with async compute (not available in D3D11) and putting position in its own vertex stream for depth-only passes.
- **MJP 2016** (GTX 970, 1080p, Crytek Sponza): his deferred path once had a depth pre-pass option and dropped it because it was never faster with a cheap G-buffer PS. The G-buffer pass cost 0.67 ms (no MSAA) / 1.48 ms (4x MSAA). Forward shading was close to useless without a pre-pass.
- **AC Unity** (Haar & Aaltonen, SIGGRAPH 2015): a depth pre-pass of the **300 best occluders** cost about 600 µs on PS4. It is rendered at full resolution for Hi-Z/early-Z, then downsampled to 512x256 for GPU occlusion culling (about 100 µs), combined with last frame's reprojected depth (about 50 µs).
- **Crysis 3** (Sousa et al., GDC 2013): a "fast double-Z pre-pass" for some of the closest geometry helped slightly.
- **Fox Engine / MGSV** (Courrèges 2017): the pre-pass draws only terrain, from a 16-bit heightmap in tiles of 16x16-vertex grids, about 150 draws.
- **DOOM 2016** (Courrèges 2016): a full opaque pre-pass in the order weapon → static → dynamic. Dynamic meshes also write per-pixel velocity in this pass; static velocity comes from depth plus camera motion. Decals are applied in the opaque pass with depth EQUAL.
- **Unreal Engine** cvars (Epic docs):
  - `r.EarlyZPass` 0 = off, **1 = "good occluders only: not masked, and large on screen"**, 2 = all opaque including masked, 3 = heuristic (default).
  - `r.MinScreenRadiusForDepthPrepass` = 0.03.
  - `r.EarlyZSortMasked` sorts masked draws last.
  - `r.VelocityOutputPass` 0 = velocity in the depth pass, which "splits the depth pass into 2 phases: with and without velocity". This is the precedent for our step B.
  - `r.StencilForLODDither` does LOD dithering with stencil in the pre-pass and depth-equal in the base pass.
  - DBuffer decals force a full pre-pass.
- **Wihlidal 2016** (GDC, Frostbite):
  - GCN rasterizers produce up to 16 pixels/clock per triangle. Partially covered quads still occupy helper lanes; his example: 12 valid pixels from 16 threads is 75% efficiency.
  - Small-primitive culling: a triangle whose snapped screen bounds give `any(round(min) == round(max))` covers no pixel centre.
  - Per-triangle compute culling saved 15–30% of a 443k-triangle 1080p G-buffer pass (Fury X: 0.79 → 0.53 ms async).
  - Many engines have only a partial pre-pass. A depth-only pass can reuse vertices better through a separate index buffer.
- **[video]** Crysis 3 alpha-tested tree: 0.13 ms with a pre-pass twin; base-only with depth test (no write) is at least 13% faster; base-only *with* depth write costs about the same as the pre-pass draw alone, about 40% better overall. Silent Hill 2 remake asset: 0.60 ms with a pre-pass twin vs 0.44 ms without (−27%). KCD2 gained performance when its partial alpha-tested pre-pass was disabled. A UE video showing +30% from pre-passing alpha-tested content lacked context. Guerrilla's opposite advice was not verified here.

**Synthesis:** the published guidance agrees on three points:
1. Pre-pass big, cheap-to-rasterize occluders.
2. Leave out small or dense meshes.
3. Never pay a texture fetch in the pre-pass unless the shading pass is very expensive.

The vendors' "alpha-test in the pre-pass, then EQUAL" advice and the video's "never" are both workload-dependent claims. Our T5/T7 tests settle it for our content.

### 3.3 Early-Z rules that matter (D3D11)
| Situation | Early-Z (per pixel) | Hi-Z (per tile) | Source |
|---|---|---|---|
| No discard, no SV_Depth, no UAV | on | on | spec / Persson 2007 |
| `discard`/`clip`/alpha-to-coverage/SV_Coverage output **with** depth or stencil writes | early test may run; writes happen late (or early-Z is off on old HW) | on | Persson 2007 table |
| Same, with depth+stencil writes off | on | on | Persson 2007 |
| `SV_Depth` output | off | off, and written tiles end up uncompressed | Persson 2007 |
| Conservative depth `SV_DepthLessEqual` (reverse-Z: pushing *away*) | test stays early | on | SM5 feature; push direction must match the GREATER test |
| PS writes a UAV | early-Z disabled unless `[earlydepthstencil]` | — | D3D11.3 spec §16.12 |
| `[earlydepthstencil]` | forced: depth/stencil test **and write** happen before the PS | — | spec §16.12.1. Never combine with discard on a depth-writing draw: discarded pixels still write depth |
| Stencil fail/zfail op ≠ KEEP | on | **off** | Persson 2007 |
| Depth func EQUAL | on | on (since X1xxx) | Persson 2007 |
| Depth func NOTEQUAL | on | off | Persson 2007 |
| Comparison direction reversed mid-frame | on | **off until clear** | Persson 2007, Khronos validation layers |

### 3.4 Occluder selection heuristic [ours]
Selection is per draw item, per frame, on the CPU. It is a simple, inspectable function with artist overrides:

```cpp
enum class PrepassMode : uint8_t { Auto, Always, Never };   // per-asset artist override

// Projected radius in pixels of the bounding sphere (sphere near the screen centre).
float ProjectedRadiusPx(XMFLOAT3 c, float r, const Camera& cam, float viewportHeightPx)
{
    float d2 = DistanceSq(c, cam.position);
    if (d2 <= r * r) return FLT_MAX;                                      // camera inside the bounds
    float cotHalfFov = 1.0f / tanf(0.5f * cam.fovY);                      // = proj._22
    return r / sqrtf(d2 - r * r) * cotHalfFov * 0.5f * viewportHeightPx;  // tan(asin(r/d)) = r/sqrt(d^2-r^2)
}

bool ShouldPrepass(const DrawItem& it, const Camera& cam, float viewportH)
{
    if (it.prepassMode == PrepassMode::Never) return false;
    if (it.material.alphaTested || it.material.pixelDepthOrDisplacement) return false; // no texture fetch in pre-pass
    if (it.lodFadeActive) return false;                  // dithered LOD fade: draw in base groups instead (sec. 7)
    if (it.prepassMode == PrepassMode::Always) return true;
    float rPx       = ProjectedRadiusPx(it.boundsCenter, it.boundsRadius, cam, viewportH);
    float areaPx    = 3.14159f * rPx * rPx;              // upper bound of the screen footprint
    float frontTris = max(1.0f, 0.5f * it.lodTriangleCount); // about half face the camera
    float pxPerTri  = areaPx / frontTris;
    return rPx >= kMinOccluderRadiusPx                   // start: 100 px (about 1/5 of 1080p height across)
        && pxPerTri >= kMinPixelsPerTriangle;            // start: 100 px/tri (Persson's lower bound)
}
// Then: sort candidates by rPx descending and cap by count (start 300, like AC Unity)
// and/or by a triangle budget. Tune the constants with T5 and T8; stay under 0.3 ms.
```

UE's `r.MinScreenRadiusForDepthPrepass 0.03` is a much looser threshold: a radius of about 3% of the screen, roughly 30 px at 1080p. The video's position, and ours, is stricter.

### 3.5 Implementation
**Pass P1, step A (static occluders):**
- Bind the DSV only, no RTVs; `PSSetShader(nullptr)` for a pure Z-only pass.
- Input layout: position stream only (slot 0). Index buffer: the "shadow/pre-pass" index buffer from meshoptimizer (§3.6).
- Depth: `DepthEnable=TRUE, DepthWriteMask=ALL, DepthFunc=GREATER_EQUAL`, stencil off.
- Rasterizer: same cull mode as the base pass, `DepthBias = 0`.
- Order: front-to-back by the nearest point of the bounds (`viewDepth(center) - radius`).

**Pass P2, step B (dynamic and dual-normal occluders):**
- Same depth state. One RTV: the motion+coverage buffer (2x MSAA).
- ALU-only PS: motion = current − previous clip position, unjittered, in pixels; A = 1 marks "object motion written". Coverage data per the MSAA note.
- Skinned meshes are skinned twice (here and in the base pass); measure it. Pre-skinning once per frame is a later optimisation.

**Position invariance.** It is mandatory for the EQUAL / `GREATER_EQUAL`-no-write base pass. The pre-pass VS and the base-pass VS must produce bit-identical `SV_Position`:
- the same quantized position stream;
- the same constant-buffer matrix;
- the same code path, marked `precise`.

```hlsl
// shared/transform.hlsli -- included by EVERY vertex shader that writes main-view depth
cbuffer PerDraw : register(b1)
{
    row_major float4x4 g_ObjectToClip;   // CPU: dequant(scale,bias) * world * view * proj, one matrix
};
float4 TransformPosition(float3 posQ)    // posQ from R16G16B16A16_SNORM -> [-1,1]
{
    precise float4 clip = mul(float4(posQ, 1.0), g_ObjectToClip);
    return clip;
}
// prepass VS
float4 PrepassVS(float4 posQ : POSITION) : SV_Position { return TransformPosition(posQ.xyz); }
```

### 3.6 Content preparation [video + meshoptimizer]
**Offline mesh pipeline,** in the order meshoptimizer documents (README):
1. `meshopt_generateVertexRemap` + remap: indexing.
2. `meshopt_optimizeVertexCache`: adaptive, modern-GPU friendly. Forsyth 2006 is the classic greedy LRU-score method; Tipsify (Sander, Nehab & Barczak 2007) the linear-time one.
3. `meshopt_optimizeOverdraw(indices, ..., 1.05f)`. This is the Tipsify-style step: split the cache-optimised order into clusters and sort them by view-independent occlusion potential, outer and front-facing first, allowing up to 5% worse cache efficiency. AMD's Tootle did the same (Persson 2007). It helps every pass that writes depth while drawing (the pre-pass and base groups 1–3).
4. `meshopt_optimizeVertexFetch` (or `...Remap` for multiple streams).
5. Quantization (below).
6. `meshopt_filterIndexBuffer`: remove degenerate or duplicate triangles.
7. `meshopt_generateShadowIndexBuffer` on the position stream, then `optimizeVertexCache` on it. This gives a second index buffer used by the pre-pass and shadows, with fewer unique vertices because UV and normal seams disappear.

Check the results with `meshopt_analyzeVertexCache` (ACMR, ATVR) and `meshopt_analyzeOverdraw`, and keep the numbers in the asset report.

**Vertex formats** (all are required IA vertex-buffer formats on FL 11.0 per Microsoft's format-support tables):

| Attribute | Format | Bytes | Decode | Notes |
|---|---|---|---|---|
| Position (stream 0) | `R16G16B16A16_SNORM` | 8 | fold AABB scale/bias into `g_ObjectToClip` | step = extent/65534: a 10 m mesh gets 0.15 mm. No 3-component 16-bit DXGI format exists. Use `R32G32B32_FLOAT` for huge meshes |
| Normal (stream 1) | `R10G10B10A2_UNORM` | 4 | `n*2-1`, normalize | DXGI has no 10:10:10:2 SNORM, so meshoptimizer's SNORM10 example doesn't apply. A2 = tangent handedness |
| Tangent | `R10G10B10A2_UNORM` | 4 | same | or `R8G8B8A8_SNORM` |
| UV0 | `R16G16_UNORM` + per-mesh UV rect | 4 | `uv*scale+bias` | Avoid `R16G16_FLOAT` for UVs ≥ 1: half has a 2^-10 step in [1,2), about 4 texels on a 4096 texture. Use `R32G32_FLOAT` for world-tiling UVs |
| Colour / artist bits | `R8G8B8A8_UNORM` | 4 | — | Only if the material needs it. Candidate home for the MSAA "flat-discontinuity" vertex bits (MSAA note) |

Total: 8 B for the pre-pass stream; 20–24 B for the full vertex vs 48+ B in float32.

```cpp
D3D11_INPUT_ELEMENT_DESC kBaseLayout[] = {
  { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  1, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "TEXCOORD", 0, DXGI_FORMAT_R16G16_UNORM,       1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
D3D11_INPUT_ELEMENT_DESC kPrepassLayout[] = { kBaseLayout[0] };   // stream 0 only
```

### 3.7 Pitfalls
- Non-invariant positions between passes make `EQUAL` fail sporadically: missing pixels or z-fighting. Use the same stream, matrix and code with `precise`.
- Pre-passing an object that dithers (LOD fade) needs discard in the pre-pass. Excluding fading objects instead is simpler.
- A pre-pass PS bound "just in case" wastes the Z-only fast path. Use a null PS in step A.
- An over-inclusive heuristic turns the pre-pass into a second base pass of dense meshes: double vertex work plus raster/Z work with no shading saved.
- Skinned meshes in the pre-pass pay skinning twice.
- The pre-pass is pointless if the base pass shader is trivial (MJP 2016). Re-check after G-buffer shaders change.

### 3.8 How to measure
T5 (pre-pass strategy matrix), T7 (single alpha-tested asset), T8 (dense-mesh crossover), T10 (vertex formats), T11 (triangle order). See §10.

---

## 4. Base pass ordering

### 4.1 What the video proposes [video]
1. **Close alpha-tested content first**, meaning everything an alpha-tested partial pre-pass would have drawn, with depth write. This recovers the occlusion that alpha-tested pre-passing would have given, without the double cost. The video says the 13% gain held even when the draws in between were removed.
2. **Non-pre-passed, non-alpha-tested geometry.** This is small and distant content. Inaccurate Z-sorting causes some overlap, but it is minor next to the content's inherent quad overdraw. A visibility buffer would not help, since quad overdraw remains and material evaluation still follows.
3. **Remaining alpha-tested content.** It suffers most from overlap and benefits from nearly complete scene depth.
4. **Pre-passed content last,** depth EQUAL, no depth write, with full G-buffer outputs.

Claim: alpha-tested content costs about 13–40% less without a pre-pass twin (Crysis 3, Silent Hill 2 remake, UE tests).

### 4.2 Why it works, and the counter-view
- With discard and depth write, depth is *written* late, but tiles can still be rejected against existing depth by Hi-Z (Persson 2007 table). So alpha-tested draws benefit from depth already in the buffer, and from their own depth writes for later draws.
- An alpha-tested pre-pass draw pays the texture fetch twice: once in the pre-pass and once in the base pass, even with EQUAL. The video's measurements put that at the 13–40% above.
- Persson (2007) recommends drawing alpha-tested primitives after opaque ones. Groups 2–3 follow that; group 1 deliberately breaks it for *large, close* alpha-tested occluders. AMD (RDNA guide) recommends a pre-pass plus EQUAL instead of discard. These are workload-dependent; T6 and T7 measure the difference.

### 4.3 Pass list and D3D11 states [ours]
`DSS_*` = depth-stencil state. Stencil ref and masks are in §6.4.

| # | Pass | Draws | Depth | Stencil | PS | RTs |
|---|---|---|---|---|---|---|
| P0 | Clears | — | clear 0 | clear 0 | — | §1.3 |
| P1 | Pre-pass A | static occluders (§3.4), front-to-back | `GE`, write | off | **null** | none |
| P2 | Pre-pass B | dynamic and dual-normal occluders, front-to-back | `GE`, write | off | ALU motion | MV+cov |
| P3 | Base 1 | close alpha-tested (`rPx ≥ kMinOccluderRadiusPx`), front-to-back | `GE`, write | REPLACE | clip(α−cutoff) | GB0-2 (+opt) |
| P4 | Base 2 | opaque not pre-passed, front-to-back | `GE`, write | REPLACE | no discard | GB0-2 (+opt) |
| P5 | Base 3 | remaining alpha-tested, front-to-back | `GE`, write | REPLACE | clip | GB0-2 (+opt) |
| P6 | Base 4 | everything from P1+P2, sorted by shader/material | `GE` (≡`EQUAL`), **no write** | REPLACE | no discard (`[earlydepthstencil]` allowed) | GB0-2 (+opt) |
| P7 | Decals | §8 | read-only DSV | test only | §8 | GB0-2 |
| P8 | Sky (optional) | full-screen triangle at depth 0 | `GE`, no write | — | sky | light buffer |

"(+opt)" = emissive → diffuse light (RT3), motion (RT4; only for dynamic draws not in P2), dual normal (RT5; skin). Each is enabled through the blend state's per-RT `RenderTargetWriteMask` (0xF or 0), which needs `IndependentBlendEnable = TRUE`. Every permutation declares only the outputs it writes; the others must be masked to 0 (§1.4). That is 2^3 = 8 small blend states; UE's `r.SelectiveBasePassOutputs` is the same idea. Bind all six RTVs once for P3–P6 so render targets never change between draws.

`GREATER_EQUAL` without depth write is exactly equivalent to `EQUAL` for pre-passed meshes. Depth only grows under a GREATER test, and the mesh's own pre-pass depth is already stored, so a sample passes `GE` only if nothing nearer covers it. Using `GE` everywhere keeps one comparison family. Hi-Z keeps working with EQUAL since the X1xxx generation (Persson 2007); T9 confirms either way.

```cpp
D3D11_DEPTH_STENCIL_DESC BaseWriteDSS()
{
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = TRUE; d.StencilReadMask = 0xFF; d.StencilWriteMask = kStencilBasePassWriteMask; // 0x1F
    d.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
    d.BackFace  = d.FrontFace;            // fail/zfail stay KEEP -> Hi-Z stays enabled
    return d;
}
D3D11_DEPTH_STENCIL_DESC BaseEqualDSS() { auto d = BaseWriteDSS(); d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; return d; }
// per draw: ctx->OMSetDepthStencilState(dss, MakeStencilRef(shadingModel, lightChannel, receivesDecals));
```

Bucketing:

```cpp
for (const DrawItem& it : visible) {
    if (ShouldPrepass(it, cam, H)) {
        (it.dynamic || it.material.dualNormal ? prepassB : prepassA).push_back(&it);
        base4.push_back(&it);
    } else if (it.material.alphaTested) {
        float rPx = ProjectedRadiusPx(it.boundsCenter, it.boundsRadius, cam, H);
        (rPx >= kMinOccluderRadiusPx ? base1 : base3).push_back(&it);   // "what an alpha-tested pre-pass would draw"
    } else {
        base2.push_back(&it);
    }
}
SortFrontToBack(prepassA, prepassB, base1, base2, base3);  // key: viewDepth(center) - radius
SortByStateKey(base4);                                      // shader, then material: order does not affect overdraw here
```

### 4.4 Pitfalls
- Alpha-tested materials never get `[earlydepthstencil]`; it would write depth for discarded pixels.
- Two-sided foliage: `CullMode NONE`, flip the normal with `SV_IsFrontFace`.
- Group-4 meshes rely on invariance (§3.5).
- Dynamic meshes that are *not* pre-passed must write motion in the base pass. The RT4 mask is on only for them.
- When a mesh changes buckets between frames (crossing the radius threshold), nothing visible changes; only cost does. Optionally add hysteresis (for example ±10% on `kMinOccluderRadiusPx`) to keep timings stable during measurement.

### 4.5 How to measure
T5, T6, T7. Record per-group GPU time, PS invocations, quads shaded (§5) and occlusion-query samples passed.

---

## 5. Quad overdraw

### 5.1 Concept
Pixel shaders run in 2x2 quads. Pixels of a quad that aren't covered by the triangle run as **helper** lanes: they are needed for derivatives, and their outputs and atomics are ignored (D3D11.3 spec §16.13). So thin or small triangles waste lanes, and adjacent triangles sharing a quad shade it twice.
- Wihlidal (2016): a quad with 12 valid pixels out of 16 threads is 75% efficient. Helper lanes still cost time to pack and prepare.
- Humus (2009, "Triangulation"): for the same circle coverage, fan and strip triangulations get slower as vertex count rises, while a "max area" triangulation barely does. MSAA makes edges more expensive. This is the "quad-friendly retriangulation" family the video refers to.
- **[video]** The real metric is quad invocations, not pixel invocations or polygon count. Trouble starts when about 70% of material surface area falls in quads with 3 of 4 lanes failed. Pre-passing does nothing for the quad overdraw of the base-pass twin, and doubles raster/Z work for dense meshes. UE has a quad-overdraw view, but it doesn't show overlap with respect to what was pre-passed. Nanite's quad-overdraw view is misleading because that path shades through compute or a visibility buffer.

Note that the D3D11 pipeline-statistics counter `PSInvocations` may or may not include helper pixels (spec), so it can't measure quad overdraw portably. SM5 also has no wave intrinsics, so a shader can't directly ask "am I a helper lane?". `SV_Coverage` *input* is 0 for helpers, but it ignores depth (spec §16.3.2), so it is wrong under early-Z rejection.

### 5.2 Measurement tool (D3D11 / SM5)
This follows Hill's "Counting Quads" (2012). Atomics from helper lanes are ignored, so the first live lane of a quad can take a per-quad lock tagged with its triangle ID. Sibling lanes of the same quad, running the same instruction, then see their own ID and stand down. Other triangles retry. The lock is held for a couple of iterations and then released. The loop has a fixed bound; Hill reports UAV-dependent `while(1)` loops misbehaving on NVIDIA drivers.

```hlsl
// Debug permutation of any opaque PS (substitute for the real PS in the pass being analysed).
// Bindings: OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, dsv, 0, 4, uavs, nullptr)
RWTexture2D<uint> g_QuadLock  : register(u0);   // (W/2)x(H/2) R32_UINT, cleared to 0xFFFFFFFF
RWTexture2D<uint> g_QuadCount : register(u1);   // (W/2)x(H/2), cleared to 0: quads shaded per screen quad
RWTexture2D<uint> g_LiveCount : register(u2);   // (W/2)x(H/2), cleared to 0: scratch live-lane counter
RWBuffer<uint>    g_Histogram : register(u3);   // [0..3] = quads with 1..4 live lanes; [4] = live pixels
cbuffer QuadDebug : register(b3) { uint g_DrawIndex; };
static const uint kUnlocked = 0xFFFFFFFF;

[earlydepthstencil]   // required: a PS with UAVs otherwise loses early-Z (spec 16.12.2)
void QuadCountPS(float4 svPos : SV_Position, uint primId : SV_PrimitiveID)
{
    uint2 q  = uint2(svPos.xy) >> 1;
    uint  me = (g_DrawIndex << 20) | (primId & 0xFFFFF);     // unique-enough triangle tag
    bool  processed = false;
    uint  ownedFor  = 0;                                      // >0 while this lane owns the quad lock
    [loop] for (uint i = 0; i < 24; ++i)
    {
        uint prev = 0;
        if (!processed)
        {
            InterlockedCompareExchange(g_QuadLock[q], kUnlocked, me, prev);
            if (prev == kUnlocked || prev == me)              // we own it, or a sibling lane of our quad does
            {
                processed = true;
                InterlockedAdd(g_LiveCount[q], 1);
                if (prev == kUnlocked) ownedFor = 1;
            }
        }
        else if (ownedFor > 0)        // NOTE: SM5 HLSL '&&' does not short-circuit -> keep the ++ in its own block
        {
            ownedFor += 1;
            if (ownedFor == 3)        // held 2 extra iterations so sibling lanes saw 'me'; now publish + release
            {
                uint live;
                InterlockedExchange(g_LiveCount[q], 0, live);
                InterlockedAdd(g_Histogram[clamp(live, 1u, 4u) - 1], 1);
                InterlockedAdd(g_Histogram[4], live);
                InterlockedAdd(g_QuadCount[q], 1);
                InterlockedExchange(g_QuadLock[q], kUnlocked, prev);
            }
        }
    }
}
```

**Outputs:**
- A visualisation pass maps `g_QuadCount[q]` to a colour ramp (1 = blue … 8+ = red).
- Quad efficiency = `hist[4] / (4 * Σ hist[0..3])`.
- **"3/4-failed fraction"** = `hist[0] / Σ hist[0..3]`, which is the video's 70% metric. Weight by area, for example compute it per material.

**Limits:**
- The fixed iteration count can under-count under heavy contention. Report how many quads stayed unprocessed by also counting lanes that never got `processed`.
- `discard` materials: `[earlydepthstencil]` writes depth before the discard. For alpha-tested passes, run the tool with depth write off against the final depth, which shows pre-pass+EQUAL behaviour, and use timings for the real case.
- `SV_PrimitiveID` restarts per draw, hence the draw index.

Use it on the pre-pass (with a trivial PS substituted, purely to count raster quads), on each base group, and on single assets at forced LODs (T8).

Rough targets [ours, starting points, to be validated in T8]:
- quad efficiency ≥ about 0.7 on hero content;
- pre-pass candidates ≥ 100 px/triangle;
- LOD switch distances chosen so the current LOD stays above about 20–30 px/triangle.

Report these numbers per asset in the content pipeline, as the video's "checkbox workflow" for artists.

---

## 6. G-buffer layout

### 6.1 Shipped layouts (reference)
| Game / engine | Depth | Targets (format: channels) | Notes |
|---|---|---|---|
| Crysis 3 (CryEngine 3, GDC 2013) | D24S8 (stencil: ambient ID, decals) | RGBA8: N.x N.y (stereographic), gloss + Z sign, translucency · RGBA8: albedo Y, CbCr interleaved, specular Y, per-project | 1 stencil bit (0x80) reserved for the per-sample mask, write mask 0x7F elsewhere; G-buffer resolve stores sample 0 for pixel-frequency passes |
| Frostbite 2014 PBR ("Moving Frostbite to PBR"; NFS 2015 is Frostbite) | — | GB0: normal (10:10) + smoothness + MaterialId (2 bits) · GB1: base colour + MatData(5)/normal(3) · GB2: metal mask, reflectance, AO · GB3: radiosity/emissive | Rule: decal-blendable attributes in RGB, non-blendable ones (MaterialId, MatData) in alpha, no encodings that break blending. The **[video]** says NFS 2015 stores albedo + roughness in one RGBA8 |
| UE4 (base layout, `DeferredShadingCommon`; from memory of the source, not re-verified here) | D24S8 / D32S8 | A `R10G10B10A2`: world normal xyz*0.5+0.5, A = per-object bits · B RGBA8: metallic, specular, roughness, shading model ID (4 bits) + selective-output mask · C RGBA8: base colour + AO/indirect · D/E RGBA8: custom data, precomputed shadow · velocity RG16 | `r.GBufferFormat` 0/1/3/5 select precision; DBuffer decals force a full pre-pass |
| Callisto Protocol (modified UE4, SIGGRAPH 2023) | — | UE4-style (10-bit normals per **[video]**) + **dual normals** split for specular and diffuse (sweat) | Source for our dual-normal buffer |
| Fox Engine / MGSV (Courrèges 2017) | **D32F reversed** | BGRA8 albedo + transmittance · BGRA8 normal + view-dependent roughness · BGRA8 roughness, specular, **material ID**, SSS translucency | Terrain-only pre-pass; decals drawn last in the G-buffer pass. The **[video]** says Fox decals write every channel except shading model ID |
| DOOM 2016 (Courrèges 2016) | — | normals R16G16F, specular RGBA8 (A = smoothness), velocity RG | Full pre-pass with velocity; decals in the forward pass with EQUAL |
| Days Gone, KCD2 | — | — | Only Threat Interactive's captures; no public layout found |

### 6.2 Normal encoding choice
Cigolle et al. (JCGT 2014), angular error over random unit vectors:

| Encoding | Bits | Mean error | Max error | Hardware-blendable (decals) |
|---|---|---|---|---|
| snorm8×3 (RGB8 raw xyz) | 24 | 0.170° | 0.386° | yes (renormalise) |
| Crytek best-fit normals | 24 | 0.040° | 4.78° | no (lookup table) |
| oct20 (octahedral 2×10) | 20 | 0.084° | 0.235° | **no** |
| oct24 | 24 | 0.021° | 0.059° | no |
| **snorm10×3 (RGB10 raw xyz)** | 30 | **0.042°** | **0.096°** | **yes** |

**[ours]** Store raw xyz as `n*0.5+0.5` in `R10G10B10A2_UNORM`. It matches the precision of snorm10×3 (same step 2/1023), so it is about 2x more accurate than octahedral in 2×10 bits. It decodes trivially, is linearly blendable for decals, and leaves the 2 alpha bits for flags as the video wants. Frostbite notes that 2-component encodings (x, y with reconstructed z) and octahedral don't blend correctly. Decode with `normalize(gb0.xyz * 2 - 1)`. Store world-space normals: stable under camera rotation and easy for decals.

### 6.3 Our layout (all 2x MSAA, 1920x1080)
| RT | Format | R | G | B | A | Written by | Clear | Size |
|---|---|---|---|---|---|---|---|---|
| DS | `D32_FLOAT_S8X24_UINT` | depth (reverse-Z) | | | | P1–P6 | 0 / 0 | 21–33 MB |
| GB0 | `R10G10B10A2_UNORM` | N.x | N.y | N.z | bit0 anisotropic flag, bit1 lighting channel | P3–P6, decals | no | 16.6 MB |
| GB1 | `R8G8B8A8_UNORM_SRGB` | albedo R | G | B | roughness (alpha is stored **linear** in _SRGB formats) | P3–P6, decals | no | 16.6 MB |
| GB2 | `R8G8B8A8_UNORM` | profile blend (the old "metallic") | specular / IOR scale | DMP ID low byte | DMP ID high byte | P3–P6, decals | no | 16.6 MB |
| RT3 diffuse light | `R16G16B16A16_FLOAT` | emissive.rgb, then + diffuse lighting | | | spare | emissive draws, then lighting | **0** | 33.2 MB |
| RT4 motion+cov | `R16G16B16A16_FLOAT` | motion x (px) | motion y (px) | coverage / discontinuity data (MSAA note) | 1 = object motion written | P2 + dynamic base draws | **0** | 33.2 MB |
| RT5 dual normal | `R10G10B10A2_UNORM` | second (diffuse) normal xyz | | | spare | skin draws | 0 (or none, §1.3) | 16.6 MB |
| Specular light | `R16G16B16A16_FLOAT` | — | | | | lighting note | no | 33.2 MB |

Total is about 190 MB at 1080p 2x MSAA, which is acceptable on 8 GB.

Base-pass write bandwidth per covered sample: 12 B (GB0–2) + 4–5 B depth/stencil, plus 8 B per optional target. One full-screen layer of GB0–2 is about 50 MB, about 0.11 ms at peak bandwidth before compression. That is the price of every layer of base-pass overdraw, and the reason for §3–§5.

Pack/unpack:

```hlsl
float4 PackGB0(float3 nWS, bool aniso, bool lightCh)
{
    uint bits = (aniso ? 1u : 0u) | (lightCh ? 2u : 0u);
    return float4(nWS * 0.5 + 0.5, bits / 3.0);          // 2-bit UNORM: 0, 1/3, 2/3, 1
}
void UnpackGB0(float4 g, out float3 n, out bool aniso, out bool lightCh)
{
    n = normalize(g.xyz * 2.0 - 1.0);
    uint bits = (uint)round(g.w * 3.0);
    aniso = (bits & 1u) != 0; lightCh = (bits & 2u) != 0;
}
float4 PackGB2(float profileBlend, float specScale, uint dmpId)   // dmpId < 8192 (13 bits) [video: "8,000 slots"]
{
    return float4(profileBlend, specScale, (dmpId & 0xFFu) / 255.0, ((dmpId >> 8) & 0xFFu) / 255.0);
}
uint UnpackDmpId(float4 g) { return (uint)round(g.z * 255.0) | ((uint)round(g.w * 255.0) << 8); }
// GB1: return float4(albedoLinear, roughness); the _SRGB RTV encodes rgb, alpha stays linear.
```

Material semantics [video], detailed in the BRDF/material note:
- The DMP ID selects a premade *pair* of BRDF profiles.
- The old metallic channel becomes the blend factor between the two profiles.
- The specular channel scales a high-precision IOR stored in the profile, which means preprocessing legacy specular/cavity maps.
- The DMP ID is separate from the shading model ID.
- The anisotropic flag needs tangent data. Where it lives is open (UE4 uses an extra target); defined by the BRDF note.

Never hardware-resolve GB2. IDs can't be averaged, and the pipeline keeps MSAA G-buffers unresolved [video].

### 6.4 Stencil allocation [ours]
| Bits | Mask | Meaning | Written by | Read by |
|---|---|---|---|---|
| 0–2 | `0x07` | **shading model ID**, 0 = sky / empty sample | base pass via `StencilRef`, `REPLACE` | per-shading-model lighting, decals (≠ 0), G-buffer readers |
| 3 | `0x08` | lighting channel 1 (also mirrored in GB0.A bit 1) | base pass | lighting |
| 4 | `0x10` | **receives decals** (1 = yes; sky and empty samples are 0, so they are excluded automatically) | base pass | decals |
| 5 | `0x20` | MSAA complex/edge pixel | MSAA classification pass (MSAA note) | decals (sample-frequency path), lighting |
| 6–7 | `0xC0` | light-volume marking scratch; must be 0 outside each light's draws | lighting note | lighting |

- `kStencilBasePassWriteMask = 0x1F`. `StencilRef = shadingModel | lightCh << 3 | receivesDecals << 4`.
- D3D11 SM5 has no per-pixel stencil export (`SV_StencilRef` is an optional D3D11.3 feature), so per-pixel stencil data is written either through per-draw refs or by full-screen passes that use `clip()` plus `REPLACE`.
- Stencil is per sample in MSAA, so the base pass automatically produces a sample-frequency shading-model stencil, as the video wants. Empty samples and sky share value 0 because they were cleared and never written.
- Crysis 3 shows the masking discipline: one reserved bit, plus read and write masks so passes never stomp other bits.

---

## 7. Dithered LOD cross-fade

### 7.1 What the video proposes [video]
LOD transitions should use only an ALU-based "two-position jittered" 2x2 Bayer dither: arithmetic, no noise texture. Days Gone did this far better than Decima. Shadow-map draws must be dithered too, since KCD2 had shadow popping.

### 7.2 Algorithm [ours, our reading of "two-position jitter"]
- During a fade of length T (UE's default `r.LODFadeTime` is 0.25 s), both LODs are drawn.
- The incoming LOD gets `fade = a ∈ (0,1]`; the outgoing LOD gets `fade = a − 1 ∈ [−1,0)`. Visibility tests are complementary, so each pixel shows exactly one LOD: no holes, no double coverage.
- The 2x2 pattern is shifted by (1,1) on odd frames, so its temporal average is a checkerboard (two positions). This only reads as smooth with a temporal resolve or at high frame rate. Without one, expect 30 Hz shimmer during fades. This is an open question for the AA note.

```hlsl
cbuffer PerFrame { uint g_FrameIndex; };
cbuffer PerDraw  { float g_LodFade; };      // only bound for "fading" permutations; others don't dither

uint Bayer2x2Rank(uint2 p)                  // [0 2; 3 1]
{
    return ((p.x ^ p.y) & 1u) * 2u + (p.y & 1u);
}
bool LodVisible(float threshold, float fade)
{
    return (fade >= 0.0) ? (threshold < fade) : (threshold >= 1.0 + fade);
}
void LodDitherClip(float2 svPos)            // 1x targets (shadow maps) or the simple MSAA path
{
    uint2 p = uint2(svPos) + (g_FrameIndex & 1u);            // 2-position jitter
    float t = (Bayer2x2Rank(p) + 0.5) / 4.0;
    clip(LodVisible(t, g_LodFade) ? 1.0 : -1.0);
}
uint LodDitherCoverage2x(float2 svPos)       // 2x MSAA: 8 levels, write to SV_Coverage
{
    uint2 p = uint2(svPos) + (g_FrameIndex & 1u);
    uint  r = Bayer2x2Rank(p);
    uint  mask = 0;
    [unroll] for (uint s = 0; s < 2; ++s)
    {
        float t = (r + 4u * s + 0.5) / 8.0;                  // sample 0: ranks 0-3, sample 1: ranks 4-7
        mask |= LodVisible(t, g_LodFade) ? (1u << s) : 0u;
    }
    return mask;                                             // at fade 0.5 every pixel has exactly 1 of 2 samples
}
```

The `SV_Coverage` variant is similar to alpha-to-coverage, but with a known and complementary pattern; the hardware alpha-to-coverage pattern is implementation-defined. Coverage output is ANDed with raster coverage and disables runtime alpha-to-coverage.

Interactions:
- **Pre-pass:** objects in a fade leave the pre-pass (`ShouldPrepass` returns false) and are drawn in base groups 1–3. This avoids discard in the pre-pass and EQUAL mismatches; UE's `r.StencilForLODDither` is the alternative.
- **Early-Z:** dithering permutations use clip or coverage output, so depth writes happen late. Only fading objects pay, and only for T seconds.
- **MSAA deferred:** with the coverage variant, pixels mix samples from two surfaces. They become complex pixels with per-sample lighting during fades, so the edge classifier must catch them (MSAA note). The clip variant keeps whole pixels.
- **Shadows:** run the same `LodDitherClip` in shadow-map draws with the same `g_LodFade`. The shadow map's own pixel grid is fine.

---

## 8. Decals written directly into the G-buffer

### 8.1 What the video proposes [video]
- No decal buffers (D-buffer). They need extra resolve steps and memory.
- Decals write directly into the G-buffer channels *except the shading model ID*, as in Fox Engine. They inherit the base surface's shading.
- Add a workflow where an artist-set opacity threshold lets a decal override the material ID.

### 8.2 Background
- Frostbite (2014): decals and surfaces must share a parameter set. MaterialId and MatData can't be blended, only replaced, so they live in alpha with blend factors that preserve them. Their decals accept artefacts on non-default MaterialIds. A decal pre-pass (UE4's D-buffer) means drawing receivers twice.
- AMD (Brennan 2016; RDNA guide): masked partial channel writes force a read-decompress-modify-write on compressed targets. Prefer full-channel writes. Decal areas are small, so this is acceptable, but measure it.
- Blending precision (D3D11.3 spec §17.5): blend inputs are clamped to the RT's range, and blending may run at precision *equal* to the RT format. On `R10G10B10A2`, a per-pixel weight taken from the output's 2-bit alpha could legally be quantized to 4 levels. **Test on our GPU (T13).**

### 8.3 Implementation [ours]
Run after the base pass and after MSAA edge classification, before lighting.

- **Geometry:** a unit cube (36 indices) with the decal's world matrix.
  - Camera outside the box (CPU test with a near-plane margin): `CullMode BACK`, depth `GREATER_EQUAL`, no write.
  - Camera inside: `CullMode FRONT`, `DepthEnable FALSE`. The comparison direction never flips.
- **Bindings:** RTVs GB0–GB2, **read-only DSV** (depth+stencil), depth SRV `Texture2DMS<float>`.
- **Stencil:** `StencilEnable TRUE`, `StencilReadMask = 0x10`, `StencilWriteMask = 0`, func `EQUAL`, ref `0x10`. This passes only on samples whose material receives decals; sky and empty samples (stencil 0) fail. All ops KEEP. Never write stencil.

```hlsl
Texture2DMS<float> g_DepthMS    : register(t0);
Texture2D          g_DecalColor : register(t1);   // rgb albedo, a opacity
Texture2D          g_DecalNormal: register(t2);   // decal-space normal map
Texture2D          g_DecalMat   : register(t3);   // r profile blend, g spec scale, b roughness, a "override" opacity
SamplerState       g_Aniso      : register(s0);
cbuffer Frame : register(b0) { row_major float4x4 g_InvViewProj; float2 g_InvViewport; float3 g_CameraPos; };
cbuffer Decal : register(b2) { row_major float4x4 g_WorldToDecal; float3 g_Right; float g_AngleFadeStart;
                                float3 g_Up; float g_OverrideThreshold; float3 g_Forward; uint g_DmpIdOverride; };

float3 ReconstructWS(float2 svPos, float depth)
{
    float2 ndc = svPos * g_InvViewport * float2(2, -2) + float2(-1, 1);
    float4 p = mul(float4(ndc, depth, 1), g_InvViewProj);
    return p.xyz / p.w;
}
struct DecalOut { float4 gb0 : SV_Target0; float4 gb1 : SV_Target1; float4 gb2 : SV_Target2; };

DecalOut DecalSoftPS(float4 svPos : SV_Position)
{
    float  depth = g_DepthMS.Load(int2(svPos.xy), 0);          // pixel-frequency: sample 0
    float3 posWS = ReconstructWS(svPos.xy, depth);
    float3 l     = mul(float4(posWS, 1), g_WorldToDecal).xyz;   // unit box [-0.5,0.5]^3
    clip(0.5 - abs(l));
    float2 uv = float2(l.x + 0.5, 0.5 - l.y);
    float2 dx = ddx(uv), dy = ddy(uv);                           // jump at depth edges -> clamp
    const float kMaxGrad = 0.05;
    if (dot(dx, dx) > kMaxGrad * kMaxGrad) dx = 0;
    if (dot(dy, dy) > kMaxGrad * kMaxGrad) dy = 0;
    float4 c  = g_DecalColor.SampleGrad(g_Aniso, uv, dx, dy);
    float3 nT = g_DecalNormal.SampleGrad(g_Aniso, uv, dx, dy).xyz * 2 - 1;
    float4 m  = g_DecalMat.SampleGrad(g_Aniso, uv, dx, dy);
    float3 gN = normalize(cross(ddx(posWS), ddy(posWS)));       // geometric normal from depth
    gN = dot(gN, g_CameraPos - posWS) < 0 ? -gN : gN;
    float a  = c.a * saturate((dot(gN, -g_Forward) - g_AngleFadeStart) / max(1e-3, 1 - g_AngleFadeStart));
    float3 nWS = normalize(nT.x * g_Right + nT.y * g_Up - nT.z * g_Forward);
    DecalOut o;
    o.gb0 = float4(nWS * 0.5 + 0.5, a);   // alpha = blend weight only (RT alpha masked)
    o.gb1 = float4(c.rgb, a);             // blended in linear space on the _SRGB RT
    o.gb2 = float4(m.r, m.g, 0, a);       // B/A (DMP ID) masked
    return o;
}
// Pass 2 "override" (only decals with the flag): clip(m.a - g_OverrideThreshold);
//   outputs gb1.a = roughness, gb2.ba = PackGB2(...).ba with g_DmpIdOverride; blending OFF.
```

Blend states (`IndependentBlendEnable = TRUE`):

| Pass | RT | Blend | Src / Dest (RGB) | Write mask |
|---|---|---|---|---|
| soft | GB0 | on | `SRC_ALPHA` / `INV_SRC_ALPHA` | RGB (0x7): flags preserved |
| soft | GB1 | on | `SRC_ALPHA` / `INV_SRC_ALPHA` | RGB (0x7): roughness preserved |
| soft | GB2 | on | `SRC_ALPHA` / `INV_SRC_ALPHA` | RG (0x3): DMP ID preserved |
| override | GB0 | — | — | 0 |
| override | GB1 | off | — | A (0x8): roughness replaced |
| override | GB2 | off | — | BA (0xC): DMP ID replaced |

The source alpha drives the blend even for channels excluded by the write mask (the write mask applies after blending). Roughness can't be soft-blended because GB1's alpha *is* the roughness. Soft-blending roughness would need a layout change, for example moving roughness into GB2, which is an open question. The shading model ID is never touched because it lives in stencil.

**MSAA:** the pixel-frequency decal reads sample-0 depth and writes every covered sample. At edge pixels it can decal the wrong surface. Improvement: a second draw restricted to edge pixels (stencil bit 5), with a sample-frequency PS taking `SV_SampleIndex` that loads that sample's depth. Ship v1 at pixel frequency and measure the artefact.

---

## 9. Consolidated frame (R1 scope)

```
P0  Clears (batch): DS(0,0) | diffuse-light(0) | motion+cov(0) | dual-normal(0, optional) | [DiscardView GB0-2: A/B]
P1  Pre-pass A   : static occluders, pos-only stream + pre-pass IB, null PS, GE write          (budget <0.3 ms total P1+P2)
P2  Pre-pass B   : dynamic / dual-normal occluders, GE write, ALU PS -> motion+cov
P3  Base 1       : close alpha-tested, GE write, stencil REPLACE, clip
P4  Base 2       : non-pre-passed opaque, front-to-back, GE write
P5  Base 3       : remaining alpha-tested, GE write
P6  Base 4       : pre-passed meshes, GE (=EQUAL) no write, early-Z guaranteed
--  (MSAA note)  : edge/complex classification -> stencil bit 5, G-buffer "resolve" stage
P7  Decals       : soft pass + override pass, read-only DSV
--  (lighting)   : per-shading-model stencil-tested full-screen lighting; stencil bits 6-7
P8  Sky          : depth==0 pixels (or folded into lighting via stencil 0)
```

---

## 10. A/B test plan (RTX 5060, 1080p, 2x MSAA)

### 10.1 Methodology
- **Timing:** D3D11 timestamp queries around every pass (`D3D11_QUERY_TIMESTAMP`) inside a `D3D11_QUERY_TIMESTAMP_DISJOINT` per frame. Read back 3–5 frames later with `GetData`, and drop disjoint frames. Report the **median** and p95 over 600 frames after a 120-frame warm-up.
- **Stability:**
  - **Interleave variants ABAB** every 60 frames within one run, to cancel thermal and clock drift.
  - Optionally fix clocks. Either create a throwaway D3D12 device on the same adapter and call `SetStablePowerState(TRUE)`, which works only in Windows Developer Mode and lowers clocks *system-wide* (Microsoft docs), or use Nsight Graphics' lock-to-base option. Locked results give ratios; confirm absolute numbers unlocked.
- **GPU-bound setup:** flip-model swap chain, `SyncInterval 0` + `DXGI_PRESENT_ALLOW_TEARING`, no overlays or browser on the GPU.
- **Counters:**
  - `D3D11_QUERY_PIPELINE_STATISTICS` per group (`PSInvocations`, `CPrimitives`; helper inclusion is implementation-defined);
  - `D3D11_QUERY_OCCLUSION` per group (samples passed);
  - the quad tool (§5.2) for quads shaded, efficiency and the 1-live-lane fraction;
  - Nsight Graphics GPU Trace for 1–2 representative frames per variant (unit throughputs, Z-cull rejection).
- **Scenes:**
  - Amazon Lumberyard Bistro exterior (NVIDIA ORCA): large buildings plus dense alpha-tested foliage.
  - Crytek or Intel Sponza: large walls, plants, curtains.
  - A synthetic stress scene: instanced alpha-tested trees, large occluder boxes, and a dense mesh at forced LOD levels.
  - 5 fixed camera poses per scene, plus one scripted fly-through.
- **Noise floor:** treat differences below 2% or 0.01 ms as noise unless they reproduce across 3 runs.

### 10.2 Tests
| ID | Question | Variants | Metrics | Video claim / what refutes it |
|---|---|---|---|---|
| **T1** clear micro-bench | Cost of each clear method | Per format (RGBA8, RGBA8_SRGB, RGB10A2, RGBA16F, R11G11B10F, D32S8, D24S8) × {1x, 2x}: (a) ClearRTV 0, (b) ClearRTV 1, (c) ClearRTV (0.5,0.25,0.125,1), (d) ClearView full rect, (e) ClearView half rect, (f) full-screen triangle writing 0, (f2) "hijack": an extra MRT output in an existing full-screen 2x pass, (g) CS clear through a UAV (separate resource with UAV bind) | time per clear (100 back-to-back clears / 100, plus a single clear in situ); time of the next draw into the target; time of a pass sampling it | "Full-screen triangle always slower" holds if f > a for every format. Hijack only pays if f2 < a |
| **T2** clear policy in frame | Does "clear only what's needed" win ≥ 0.1 ms? | (a) clear all G-buffers + light buffers, (b) clear only DS/diffuse/MV/dual (ours), (b+) b with DiscardView on GB0–2, (c) b using full-screen-triangle clears | P0 + base + lighting time | Claim holds if a − b ≥ 0.1 ms. On fast-clear hardware we expect a − b of only µs, and c − b of tens of µs per target |
| **T3** post overwrite | Clear+overwrite vs overwrite | tonemap/AO output with and without a preceding clear | pass time | b < a expected |
| **T4** depth formats | Cost of D32S8, SRV flag, reverse vs forward | D32S8 vs D24S8 vs D32 (no stencil); with and without `BIND_SHADER_RESOURCE` (AMD: SRV flagging can hurt MSAA depth compression); reverse vs forward Z (same pass list); depth-only clear vs depth+stencil clear | P1–P6 times | Expect ≈ equal for reverse vs forward and D24 vs D32 (AMD); measure NVIDIA |
| **T5** pre-pass strategy | Which pre-pass wins | P0 none · P1 full incl. alpha-tested (+EQUAL, no clip in base, AMD-style) · P2 full non-alpha-tested (HL:Alyx-style) · P3 partial non-alpha-tested (ours; sweep `kMinOccluderRadiusPx` ∈ {0, 50, 100, 200}, `kMinPixelsPerTriangle` ∈ {0, 100, 1000}) · P4 partial alpha-tested (Crysis 3-style) | pre-pass time, each base group, sum; PS invocations; quads; occluder count and triangles | Claim: P3 has the lowest total, P3's pre-pass < 0.3 ms, P1/P4 worse whenever alpha-tested area is significant |
| **T6** base order | Does the 4-group order matter? | O1 video order · O2 pre-passed group first, then the rest front-to-back · O3 opaque front-to-back then all alpha-tested (no close group) · O4 O1 without group 1 (close alpha-tested moved into group 3) | per-group time, PS invocations | Claim: O1 ≤ others, with the gain concentrated in group 3 |
| **T7** single alpha-tested asset (reproduces 13% / 40%) | Pre-pass twin vs none for one large tree | (i) alpha pre-pass + base EQUAL/no write; (ii) base only, depth test, write off; (iii) base only, write on. Run with and without other scene depth | per-asset time summed across passes | Claim: (ii) ≥ 13% cheaper than (i), and (iii) ≈ cost of (i)'s pre-pass draw alone |
| **T8** density crossover | When does pre-passing a dense mesh stop paying? | one opaque mesh at forced LODs spanning 1–1000 px/tri, pre-passed vs not, at 3 screen sizes | time, quads, 1-live-lane fraction | Calibrates `kMinPixelsPerTriangle` and tests the "2x quad multiplier" claim |
| **T9** depth func | EQUAL vs GE; direction-flip demo | group 4 with EQUAL vs GE; then add one LESS draw mid-frame | group times; Z-cull stats in Nsight | Expect EQUAL ≈ GE; the flip should visibly slow later draws until the next clear |
| **T10** vertex formats | Bandwidth savings | float32 interleaved vs quantized split streams (§3.6) | pre-pass and base times | Expect pre-pass gains in vertex-bound scenes |
| **T11** triangle order | meshoptimizer value | raw vs optimizeVertexCache vs +optimizeOverdraw(1.05) | pre-pass and base times, quads | — |
| **T12** LOD dither | Cost of dithering | pop vs clip dither vs SV_Coverage dither (during forced continuous fades) | base time, complex-pixel count, visual check with and without temporal AA | Confirms ALU dither is "nothing on modern hardware" [video] |
| **T13** decals | Cost and precision | 0/10/50% screen coverage; soft only vs soft+override; RGB10A2 gradient-alpha precision check in RenderDoc | decal pass time; quantization of GB0 blend weights | If blend weights quantize to 4 levels, switch normal blending to a threshold or a per-draw blend factor |

---

## 11. Open questions
1. What exactly is "two-position jittered" Bayer (a per-frame spatial shift vs two alternating matrices)? Is there a temporal resolve to integrate it? Without one, fades shimmer at 30 Hz.
2. Is the hijack clear feasible at all with 2x MSAA? It needs the same sample count, full coverage and no read of the target in the same pass. Which pass could host it?
3. Layout of the motion+coverage buffer (RGBA16F vs RG16F plus a separate coverage target). Owned by the MSAA note.
4. Shading model in stencil only (ours) vs the video's G-buffer-to-stencil "resolve" step. Does any later stage need it in a texture?
5. Does the dual-normal buffer need a clear if every read is stencil-masked to skin samples?
6. Occlusion culling from pre-pass depth (D3D11 predication or a Hi-Z downsample like AC Unity). The video calls it under-researched.
7. Precision of decal normal blend weights on `R10G10B10A2`, and whether decals need soft-blended roughness (that would mean a layout change).
8. NVIDIA's real cost for D32S8 vs D24S8, and whether `DiscardView` does anything in NVIDIA's D3D11 driver.
9. Calibrating the heuristic constants (radius, px/tri, occluder cap) to our content. Add hysteresis?
10. Skinned occluders are skinned twice (pre-pass + base pass). Is a once-per-frame pre-skin (stream-out or CS) worth its complexity for a proof of concept?
11. Our scenes need an asset whose alpha-tested close foliage is representative of KCD2 / Silent Hill 2 to reproduce the video's 13–40% claim.

---

## Sources
- Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering" (2026-08-01), https://www.youtube.com/watch?v=SnNm7rSSvlg. The claims marked [video], transcript 8:00–39:00.
- NVIDIA (I. Belyavtsev), "Advanced API Performance: Clears" (2022-05-11), https://developer.nvidia.com/blog/advanced-api-performance-clears/. Use API clears; never clear shaders; limited clear colours per frame (25 Ampere / 10 Turing); depth clear values for GREATER; batch clears.
- Khronos Vulkan-ValidationLayers, NVIDIA best-practice checks, `layers/best_practices/bp_cmd_buffer_nv.cpp` and `bp_constants.h`, https://github.com/KhronosGroup/Vulkan-ValidationLayers. 0/1 clears always compressed; custom-colour compressible formats; 16-colour limit; Z-cull disabled for the less-used compare direction until a clear.
- AMD GPUOpen, "RDNA Performance Guide", https://gpuopen.com/learn/rdna-performance-guide/. Fast clears about 100x faster; the 4 fast-clear colours; depth 0/1 with stencil 0; full-image clears only; 24-bit depth costs the same as 32-bit; reversed depth; EQUAL instead of discard after a pre-pass; position-only stream; write all channels.
- C. Brennan (AMD), "Delta Color Compression Overview" (2016-03-14), https://gpuopen.com/learn/dcc-overview/. Clear only when needed; 0/1 clears are best; masked writes to compressed data cause read-modify-write; MSAA depth with SRV flagging.
- E. Persson (ATI/AMD), "Depth In-depth" (2007), http://developer.amd.com/wordpress/media/2012/10/Depth_in-depth.pdf (fetched via web.archive.org). Hi-Z min/max modes and direction changes; early-Z vs alpha test table; EQUAL; stencil ops and Hi-Z; clear depth and stencil together; why skipping depth clears hurts; the 100/1000-pixel pre-Z rule; Tootle; sky last.
- Microsoft, Direct3D 11.3 Functional Specification, https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm. Input coverage is 0 for helpers and ignores depth (§16.3.2); forced early depth/stencil (§16.12); helper/discard behaviour for UAVs (§16.13); `PSInvocations` may include helpers; write masks and undefined outputs (§17.15); blending precision (§17.5).
- Microsoft Learn: `ID3D11DeviceContext1::DiscardView` (https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-discardview), `ClearView` (…-clearview), `ID3D12Device::SetStablePowerState` (https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-setstablepowerstate), FL 11.0 format support (https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware). API semantics; Developer Mode requirement and system-wide clock effect; IA vertex and MSAA RT support for RGB10A2, R11G11B10F, RGBA16 SNORM/UNORM/FLOAT, RGBA8 SNORM/UNORM, D32S8.
- N. Reed (NVIDIA), "Depth Precision Visualized" (2015), https://developer.nvidia.com/content/depth-precision-visualized. Float reverse-Z is near ideal; the far plane doesn't matter; int24 gains little.
- gamedev.net thread "24bit depthbuffer is a sub optimal format" (2017), https://gamedev.net/forums/topic/691579-24bit-depthbuffer-is-a-sub-optimal-format/. Separate 32-bit depth and 8-bit stencil planes on GCN (Microsoft's A. Miles).
- Epic Games, Unreal Engine Console Variables Reference, https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-console-variables-reference. `r.EarlyZPass` modes, `r.MinScreenRadiusForDepthPrepass` 0.03, `r.EarlyZSortMasked`, `r.VelocityOutputPass`, `r.StencilForLODDither`, `r.ClearSceneMethod`, `r.SelectiveBasePassOutputs`, `r.GBufferFormat`, `r.DBuffer`, `r.LODFadeTime`.
- G. Wihlidal (Frostbite), "Optimizing the Graphics Pipeline with Compute", GDC 2016, https://media.gdcvault.com/gdc2016/Presentations/Wihlidal_Graham_OptimizingTheGraphics.pdf. Rasterizer and quad efficiency; helper-lane cost; small-primitive test; de-interleaved streams and a separate depth-only index buffer; partial pre-passes are common; culling timings (443k triangles @ 1080p).
- U. Haar, S. Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015 Advances, https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf. 300 best occluders about 600 µs (PS4); Hi-Z downsample and reprojection timings.
- M. Pettineo (MJP), "Bindless Texturing for Deferred Rendering and Decals" (2016), https://therealmjp.github.io/posts/bindless-texturing-for-deferred-rendering-and-decals/. A deferred pre-pass was never faster with a cheap G-buffer PS (GTX 970, 1080p, Sponza); G-buffer timings.
- S. Hill, "Counting Quads" (2012-11-12), https://blog.selfshadow.com/2012/11/12/counting-quads/. UAV quad-lock counting technique, live-lane histogram, iteration-bound caveat (basis of §5.2).
- E. Persson (Humus), "Triangulation" (2009-01-14), http://www.humus.name/index.php?page=News&ID=228. Max-area vs fan and strip; quad overshading at edges; MSAA amplifies it.
- zeux, meshoptimizer README, https://github.com/zeux/meshoptimizer. Optimisation pipeline order; vertex cache, overdraw (threshold 1.05), fetch; quantization advice; shadow index buffer; position-only stream; analyzers.
- Z. Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors", JCGT 3(2) 2014, https://jcgt.org/published/0003/02/01/paper.pdf. Error table (oct20, oct24, snorm8×3, snorm10×3, Crytek BFN).
- S. Lagarde, C. de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0", SIGGRAPH 2014 course notes, https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf. Frostbite G-buffer tables; blendability constraints; decal pitfalls (MaterialId in alpha, indirect lighting, emissive).
- T. Sousa et al. (Crytek), "The Rendering Technologies of Crysis 3", GDC 2013, https://www.slideshare.net/slideshow/rendering-technologies-from-crysis-3-gdc-2013/25052434. Thin G-buffer 2.0 layout; D24S8 and stencil-bit masking for the MSAA sample mask; "fast double-Z pre-pass" for close geometry.
- A. Courrèges, "Metal Gear Solid V: Graphics Study" (2017), https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/. Fox G-buffer, reversed D32F, terrain-only pre-pass, decal placement.
- A. Courrèges, "DOOM (2016): Graphics Study" (2016), https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/. Full pre-pass with velocity for dynamic meshes; EQUAL decals; normal/specular formats.
- J. Jimenez, J. Petersen et al., "The Rendering of The Callisto Protocol", SIGGRAPH 2023 Advances, https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf. Dual normals (specular vs diffuse) for skin and sweat; modified UE4 base.
- Guru3D, "GeForce RTX 5060 8GB review", https://www.guru3d.com/review/geforce-rtx-5060-8gb-review/, and pcspecchart listings. 3,840 cores, 48 ROPs, about 2.5 GHz, 128-bit GDDR7 at 28 Gbps = 448 GB/s, used for back-of-envelope costs.
- Background, named but not consulted directly: T. Forsyth, "Linear-Speed Vertex Cache Optimisation" (2006); P. Sander, D. Nehab, J. Barczak, "Fast Triangle Reordering for Vertex Locality and Reduced Overdraw" (SIGGRAPH 2007); Nehab, Barczak, Sander, "Triangle Order Optimization for Graphics Hardware Computation Culling" (I3D 2006, cited by Persson 2007).
