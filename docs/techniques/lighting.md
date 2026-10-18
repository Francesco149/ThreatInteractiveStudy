# Deferred lighting: stencil volumes, per-shading-model draws, dual material profiles, SSS, composition

Video chapters: 39:00 "Perspective Shadow Maps & Deferred Stencil Volume Lighting", 53:48
"Screen Space Subsurface Scattering", 57:21 "Direct Lighting Compositing & Effects Rendering".
Research: [../research/04-stencil-lighting-brdf.md](../research/04-stencil-lighting-brdf.md),
[../research/02-msaa-deferred-specular-aa.md](../research/02-msaa-deferred-specular-aa.md),
[../research/05-sss-gi-reflections-sky.md](../research/05-sss-gi-reflections-sky.md).
Code: `src/r_lighting.cpp`, `src/r_sss.cpp`, `shaders/lighting.hlsl`, `shaders/brdf.hlsli`,
`shaders/compose.hlsl`, `shaders/sss.hlsl`. Shadows: [shadows.md](shadows.md); GI: [gi-ao.md](gi-ao.md).

## Buffers

Lighting is accumulated into two 2x MSAA RGBA16F targets (TI wants RGBA16 lighting everywhere):

- **diffuse**: lighting *without albedo* (irradiance x diffuse weight / pi). It's what the skin SSS
  blurs, and what auto exposure meters (Fox Engine: dark albedo and emissives can't fool it).
- **specular**: reflected radiance. After the compose pass this buffer *is* the HDR scene colour,
  recycling the target instead of allocating a third one (TI: "the specular RGBA16 is
  overwritten / recycled in a later pass").

## Sun + ambient: one full-screen draw per shading model

`pass_sun` overwrites both buffers (blend off), so they never need a clear. For each shading model
present in the frame (tracked while building the draw lists) it draws a full-screen triangle with
stencil EQUAL (shading model + 1); the shader permutation for that model contains only its code
(skin: wrap lighting before the SSS blur; foliage: back-lit transmission). Under MSAA each model is
two draws: pixel frequency where the EDGE bit is clear, sample frequency where it is set.

## Local lights: stencil volumes + full-screen triangles per shading model

Per visible light (`pass_local_lights`):

1. **Mark** (no colour, no pixel shader): the light's low-poly volume (an icosphere scaled so its
   *faces* enclose the range, or a cone whose base polygon circumscribes the circle) is drawn with
   both faces, depth test `GREATER` (reversed Z, so the test never flips direction), no depth
   writes, depth clip off, stencil op **INVERT on depth fail** for bit 7. A face behind the visible
   surface fails the depth test; a surface inside a convex volume has exactly one such face (the
   back face), so its bit flips once; surfaces in front of or behind the volume flip zero or two
   times. This handles the camera inside the volume without any CPU test, and the stencil test of
   the same draw rejects sky samples (`(stencil & 7) != 0`) so they're never marked.
2. **Light**: for each shading model, a full-screen triangle clipped by the light's screen-space
   scissor rectangle, stencil `EQUAL (0x80 | model+1)` under mask `0x80 | 0x07 | EDGE`, with
   pass op **ZERO on bit 7**: the draw lights its samples and clears the mark for the next light,
   so no stencil clear is ever needed (TI: "the lighting draw simultaneously reverts the stencil").
   Blend ONE+ONE into both buffers. Again two draws per model under MSAA (pixel / sample frequency).

Light attenuation is UE4's windowed inverse square (Karis 2013):
`saturate(1 - (d/r)^4)^2 / (d^2 + 1)`, spot cones `saturate((cos - cos_outer) / (cos_inner -
cos_outer))^2`.

A/B variants (`LightCullMode` and flags, all in the benchmark):

| variant | what it does |
|---|---|
| stencil volume + scissor (TI) | the default above |
| no scissor | full-screen triangles cover the whole screen; only the stencil rejects |
| scissor only | no volume pass, the scissored triangle lights everything in the rectangle |
| full-screen | no culling at all |
| volume mesh (Fox Engine) | marked as above, but lit by drawing the volume's back faces instead of the full-screen triangle (quad overdraw along the mesh's triangle edges) |
| uber shader | one lighting draw per light (per edge class) that branches on the shading model per pixel, UE style |
| separate unmark | lighting draws only *test* stencil, a separate colourless draw clears bit 7 (does writing stencil in the lighting draw disable NVIDIA's coarse stencil culling?) |

Measured on the courtyard (`--bench lights`, mean over views, [../PERF.md](../PERF.md)): the
default 0.76 ms; no scissor 0.93; scissor only 0.89; full-screen without culling 1.15; separate
unmark 0.82; Fox Engine volume mesh 0.45; **uber shader 0.40**. Culling and the scissor pay off, but
with these shading models one draw per light beats one draw per shading model: the per-draw cost of
rasterizing the scissor rectangle dominates the per-pixel savings of specialised shaders.

## BRDF and dual material profiles

`brdf.hlsli`: GGX NDF, height-correlated Smith visibility (Heitz 2014), Schlick Fresnel, Lambert
diffuse with optional wrap (foliage, cloth), plus optional clearcoat and sheen lobes.

TI proposes replacing the metallic and specular PBR channels with a **dual material profile**: a
per-pixel id that selects a pre-made *pair* of BRDF profiles, with the old metallic channel
becoming the blend between them, so e.g. paint thinning over iron blends two complete BRDFs instead
of producing an in-between "half metal" that exists in neither. Implementation:

- `MaterialProfile` (`scene.h`): F0 (from measured data / IOR), metal flag, clearcoat amount and
  roughness, sheen, diffuse wrap. Built-in table: stone, brick, plaster, wood, bark, ceramic,
  cloth, paint, iron, gold, brass, copper, glass, skin, foliage, rubber.
- glTF materials name their profile in an extra (`"profile": "paint_iron"` = pair (paint, iron));
  the metallic texture channel becomes the per-pixel blend.
- The G-buffer stores the pair id (8 bits here, 256 pairs; TI's chart mentions ~8000 slots) and the
  blend. The lighting shader evaluates **both profiles and blends the results** (only when the
  blend is > 0, a uniform-per-region branch).
- The specular channel scales the profile's dielectric F0 (0.5 = nominal), so cavity/specular maps
  can still darken reflections without storing IOR in 8 bits.

## Screen-space subsurface scattering

`r_sss.cpp`, `sss.hlsl`: Jimenez et al. 2015 separable SSS, with the kernel generated on the CPU
from the d'Eon & Luebke sum-of-Gaussians skin profile (17 taps, centre weight
0.5363/0.6246/0.7489, identical to the reference preset; checked at startup in the log). TI wants
a Crysis 3-like pixel-shader implementation with stencil testing, full resolution, ~0.3 ms:

- two draws (horizontal then vertical), ping-ponging between the diffuse buffer and an MSAA temp,
  stencil EQUAL "skin" so no non-skin pixel ever runs the shader;
- physical footprint: taps are in millimetres, converted to pixels from the view depth, clamped;
  sub-pixel footprints skip the blur entirely (distant characters are free);
- follow-surface: taps further than 1.5 cm in depth fall back to the centre; non-skin taps are
  rejected through the stencil SRV (no bleeding of eyebrows/collars/background);
- MSAA: pixel frequency, centre from the first skin sample, taps read sample 0 interpolated by hand
  (MSAA can't be filtered), writes land only on skin samples (per-sample stencil);
- the 2-position 2x2 Bayer jitter of the kernel width (TI's suggestion against banding).

## Composition with fog (dual-source blending)

`compose.hlsl` computes `diffuse * albedo + emissive`, applies analytic exponential height fog (the
closed-form integral along the view ray, as UE's ExponentialHeightFog), and adds it onto the
specular buffer. The fog must also attenuate the specular already *in* the buffer, so the pass uses
**dual-source blending**: output 0 is the new colour, output 1 the fog transmittance, and the blend
unit computes `scene = src0 + dest * src1`: composition and fog in one draw (TI: "fog shading makes
the most sense at this point"). Sky samples are then overwritten by the sky pass (stencil 0).
