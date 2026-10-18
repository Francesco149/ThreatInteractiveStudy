# Geometry stage: clears, partial pre-pass, base pass order, G-buffer, LODs, decals

Video chapters: 8:00 "The Importance Of Resource Clearing Management", 14:21 "How To Properly
Prepass Content", 28:53 "How To Properly Sort Deferred Basepass Content", 36:44 "Post Basepass
Resource Management & Decal Notes". Research: [../research/01-clears-prepass-basepass.md](../research/01-clears-prepass-basepass.md).
Code: `src/r_geometry.cpp`, `src/r_decals.cpp`, `src/scene.cpp`, `shaders/gbuffer.hlsl`,
`shaders/depth.hlsl`, `shaders/geometry.hlsli`, `shaders/msaa_edges.hlsl`, `shaders/overdraw.hlsl`.
Measurements: [../PERF.md](../PERF.md).

## Draw submission (the CPU side, before any of the techniques)

Every frame, `build_draw_lists()` frustum-culls the instances, picks an LOD per primitive, sorts
the visible items into buckets, and batches identical consecutive items (same primitive, LOD,
material and dither flags) into one `DrawIndexedInstanced`. Per-draw data (3x4 world matrix, last
frame's matrix, material index, LOD fade, wind bend) for *all* passes of the frame is written into one
dynamic `StructuredBuffer<DrawData>` with a single `Map(WRITE_DISCARD)`. The vertex shader finds its
entry through a per-instance vertex stream that holds 0, 1, 2, ... and `StartInstanceLocation` = the
batch's first entry. There are no per-draw constant buffer updates at all, which is what lets a
1500-instance scene with 4 shadow cascades submit in well under a millisecond of CPU time.

Vertex data is split into two streams: positions (12 bytes) and everything else (12 bytes: normal
and tangent as `R10G10B10A2_UNORM`, UV as `R16G16_FLOAT`). Depth-only passes (pre-pass, shadows)
bind only the position stream: TI's "minimize the amount of bits per vertex" for pre-pass content.
On load, meshoptimizer reorders triangles for the post-transform vertex cache and then for
overdraw (`meshopt_optimizeOverdraw`, a Tipsify-style ordering that puts outward-facing clusters
first) and builds up to 3 simplified LODs (`meshopt_simplify`). `--no-mesh-opt` disables the
reordering for the A/B.

## Clears (minimal, hardware, never with a draw)

`pass_clear()` clears only what isn't fully overwritten:

| target | cleared? | why |
|---|---|---|
| depth/stencil (D32F S8) | yes, to 0 (reversed Z) and stencil 0 | everything depth-tests against it; stencil 0 = sky |
| G-buffer albedo/normal/material/motion | no | the base pass overwrites every geometry sample; every reader masks sky/empty samples with stencil (or depth == 0) |
| emissive (R11G11B10F) | yes | only emissive materials write it (other draws use a blend state with write mask 0 on that target: in D3D11 a bound target the shader doesn't write gets undefined data otherwise) |
| diffuse/specular lighting | no | the sun + ambient pass *overwrites* them for every geometry sample (blend off), the sky pass overwrites the sky samples: "full-screen overwrite instead of clear" |
| SMAA edges/weights, UI, half-res VFX | yes | partially written by design; the half-res VFX clear is replaced by a *hijack clear* (below) |

**Hijack clear** (TI 10:32): "while a shader writes its main output, simultaneously overwrite
additional outputs with the data equivalent to a clear". Implemented where it's natural: the
half-resolution depth downsample for the particles (`ps_depth_downsample`, `shaders/vfx.hlsl`)
writes `SV_Depth` *and* zero into the half-res particle colour target bound as RT0, so the
particle buffer's `ClearRenderTargetView` disappears (`Settings::hijack_clear`, A/B).

**Clearing with a full-screen triangle** is implemented only as the anti-pattern for the A/B
(`CLEAR_ALL_FULLSCREEN`, `fullscreen.hlsl:ps_clear`). On the RTX 5060 it costs about 15x the
hardware clears of the same targets (see PERF.md), because hardware clears of fresh targets are
metadata-only "fast clears" while the draw writes every sample of 6 MSAA targets.

## Reversed depth

`D32_FLOAT_S8X24_UINT`, reversed Z with an infinite far plane (`perspective_reverse_z_inf` in
`src/vmath.h`): near = 1, infinity = 0, cleared to 0, every main-view test is `GREATER` or
`GREATER_EQUAL`. The comparison direction never flips within the frame: on NVIDIA and AMD a flip
disables the hierarchical depth (Z-cull/Hi-Z) until the next clear (research note 01), which is
also why the stencil light volumes and the decals use no reversed tests (see lighting.md).

The float format has a measured price: with a stencil, D3D11 only offers `D32_FLOAT_S8X24` (64 bits
per sample), and on the courtyard at 2x MSAA the base pass takes 1.22 ms with it vs 1.05 ms with
`D24_UNORM_S8_UINT` (`--bench depth`, repeated in both orders; -0.19 ms per frame). Reversed Z
needs the float format for its precision, so the default stays D32F.

## Partial, non-alpha-tested depth pre-pass

TI: pre-pass only large/close occluders, never alpha-tested content (an opacity texture fetch per
pixel makes the pre-pass draw as expensive as its base-pass counterpart), no per-pixel texture
sampling at all, < 0.3 ms.

- Selection (`build_draw_lists`): non-alpha-tested, not in an LOD transition, and projected
  bounding-sphere size >= `prepass_min_size` (default 12% of the screen height).
- Depth-only: position stream, **no pixel shader bound**, front-to-back.
- A/B modes: none / full opaque / full including alpha-tested (with the alpha-tested depth shader)
  / partial (TI). With the alpha-tested full pre-pass, the base pass draws pre-passed alpha-tested
  objects with depth EQUAL and the *non-discarding* shader: texels that failed the alpha test left
  no depth to be equal to, which is the classic argument for alpha pre-passes. TI argues the total
  is still more expensive; the benchmark measures exactly that trade.
- The pre-pass and the base pass compute positions with the same `precise` code
  (`geometry.hlsli:world_position`), so `GREATER_EQUAL` on pre-passed draws behaves as EQUAL.

## Base pass order (TI)

Four buckets (`Bucket` in `r_geometry.cpp`), executed in TI's order:

1. close alpha-tested objects (closer than `close_alpha_distance`): they give the rest of the pass
   the depth an alpha-tested pre-pass would have given, without the pre-pass cost;
2. opaque objects that weren't pre-passed (front to back);
3. the remaining alpha-tested objects, which now benefit from almost complete scene depth;
4. the pre-passed objects, depth `GREATER_EQUAL` (= EQUAL), no depth writes.

Within a bucket, instances of the same mesh/material are grouped (instancing) and groups are sorted
by their closest member. A/B: "by material" (no distance sorting) and "front to back" (pre-passed
first, opaque front-to-back, alpha-tested last).

The base pass writes the shading model into the stencil (`StencilRef` = shading model + 1, op
REPLACE, write mask 0x07), per draw, which is per *sample* in an MSAA depth-stencil buffer: TI's
sample-frequency shading-model stencil without any extra pass (`SV_StencilRef` from the shader
isn't available on NVIDIA in D3D11).

## G-buffer layout (2x MSAA)

| target | format | contents |
|---|---|---|
| RT0 | RGBA8 sRGB | albedo, a = roughness (after specular AA) |
| RT1 | RGB10A2 | octahedral normal (10:10), specular scale (10 bits), 2 flag bits (anisotropy / lighting channel, reserved) |
| RT2 | RGBA8 | material profile pair id, profile blend (the old "metallic"), AO, shading model |
| RT3 | RG16F | motion vector (uv delta, unjittered) for every object, incl. wind-animated foliage |
| RT4 | R11G11B10F | emissive (cleared; only emissive draws write it) |
| DS | D32F S8 | reversed depth; stencil: shading model (bits 0-2), MSAA edge (bit 3), light volume mark (bit 7) |

The shading model is also in RT2.a so that the MSAA edge pass (which writes stencil, so can't read
it) can compare it between samples. TI's "dual material profile" is explained in lighting.md.

## Specular anti-aliasing

`gbuffer.hlsl:specular_aa_roughness`: Tokuyoshi & Kaplanyan, "Improved Geometric Specular
Antialiasing" (I3D 2019): the NDF is widened by the screen-space variance of the normal,
`a2' = a2 + min(2 * (1/2pi) * (|ddx n|^2 + |ddy n|^2), 0.18)`. It replaces Valve's 2015 version
that UE still uses (TI 35:00). It runs in the base pass and modifies the roughness written to the
G-buffer, so every light benefits.

## LOD transitions

Stateless dithered cross-fades: LOD `i` is chosen from the projected size; in a band of 15% below
the switch point both LODs are drawn, the coarse one with `DRAW_FADE_OUT` and the fine one with
`DRAW_FADE_IN`, complementary in a **2x2 Bayer pattern shifted by one pixel every other frame**
(`common.hlsli:bayer2x2_dither`): TI's "ALU-based two-position jittered Bayer matrix", which the
2-frame temporal AA averages back into a smooth blend. Objects in a transition are excluded from
the pre-pass (the dithered shader discards, which would make it a texture-free but discarding
draw). The dither costs one `discard` permutation (`ps_main` with `DITHER=1`); non-transition draws
keep the discard-free shader and early depth testing.

## MSAA complex-pixel stencil ("G-buffer resolve")

After the base pass and decals, `msaa_edges.hlsl` runs once per pixel, reads depth, normal,
material pair and shading model of every sample, and marks pixels whose samples would shade
differently with the stencil EDGE bit (via `discard` + stencil REPLACE; no colour output: a render
target would have to be multisampled like the depth). Every later full-screen pass draws twice:
once at pixel frequency for simple pixels (stencil EDGE = 0) and once at sample frequency
(`SV_SampleIndex`) for edge pixels. Samples of coplanar triangles of one flat surface (same normal,
continuous depth, same material) are *not* edges, which is what TI's baked "flat discontinuity"
vertex flags aim for (35:40), done here from the G-buffer itself with no asset processing.

## Decals

`r_decals.cpp`, `decal.hlsl`: box-projected decals written straight into the G-buffer albedo
(no decal buffer: TI, "decal buffers need to be avoided since those will require unperformed
resolve steps and memory we can't spare"; Fox Engine's decals take on the shading properties of
the base object). They never touch the shading model (stencil) or the material id. The box's faces
are rasterized without depth test (the camera can be inside), and the pixel shader rejects G-buffer
positions outside the box. Albedo only: normals are stored octahedrally, which doesn't blend
linearly. Textures are generated procedurally at startup (stain, moss, cracks).

## Quad overdraw view

GPUs shade 2x2 quads; a triangle covering one pixel of a quad still pays for 4 lanes, and TI's
point (24:00) is that quad overdraw, not triangle count, is what artists should manage.
`overdraw.hlsl` re-renders the base pass (the pre-pass depth-only first, so the counts include its
rejections) with a pixel shader that finds how many lanes of its quad are live (`SV_Coverage != 0`,
helper lanes have 0) with fine derivatives, and adds `12 / live` to the quad's counter, so every
shaded quad adds exactly 12. The debug view shows quad invocations per 2x2 quad as a heat map
(blue = 1, red = 8+). Triangle edges show up as thin lines of 2, dense foliage as red blotches.
