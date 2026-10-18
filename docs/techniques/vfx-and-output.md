# Transparents, half-resolution VFX, final composite and UI

Video chapters: 57:21 "Direct Lighting Compositing & Effects Rendering" (half-res min/max depth
effects, responsive mask), 1:57:29 "Motion Effects, Composite Effects, & UI/Gamma". Research:
[../research/08-velocity-motionblur-vfx.md](../research/08-velocity-motionblur-vfx.md) section 11,
[../research/06-tonemap-exposure-bloom.md](../research/06-tonemap-exposure-bloom.md).
Code: `src/r_vfx.cpp`, `shaders/vfx.hlsl`, `shaders/final.hlsl`, `src/r_post.cpp`
(`renderer_final_to_backbuffer`).

## Glass

Forward-lit (sun specular + sky reflection with Fresnel), premultiplied alpha, sorted back to front,
depth-tested against the MSAA depth without writing it, drawn into the HDR scene colour after the
sky.

## Particles at half resolution (The Phantom Pain)

TI: "the biggest performance optimization we can use in this section is the Phantom Pain's
half-resolution min-max depth effect rendering for low-frequency effects and sprites".

1. **Half-res depth** (`ps_depth_downsample`): each half-res texel reads its 2x2 full-res pixels
   (sample 0) and keeps the farthest depth on even texels and the nearest on odd ones (checkerboard
   min/max, UE's CBMINMAX), so both sides of every depth edge are represented; written with
   `SV_Depth` into a real half-res depth buffer so particles get hardware depth testing.
2. **Particles** (camera-facing quads expanded in the vertex shader from a structured buffer) into
   a half-res RGBA16F with premultiplied alpha, soft-particle fade against the half-res depth.
   The buffer's clear is done by step 1 as a **hijack clear** (it binds the colour target too and
   writes zero).
3. **Composite** into the 2x MSAA scene colour: nearest-depth upsample (Jansen & Bavoil 2011): if
   one of the 4 surrounding half-res depths matches the full-res depth much better than the others,
   take that texel, otherwise bilinear.

Particles are a pure function of time (each slot's age = (time + offset) mod lifetime), so every
benchmark configuration renders exactly the same particles. Emitters (smoke, fire, spray) come from
glTF node extras (`"vfx": "smoke"`).

## Responsive mask

Every transparent writes its opacity into the scene colour's alpha with a `MAX` blend op; the tone
mapping pass carries it into the AA mask and the two-frame temporal resolve multiplies its blend
weight by `1 - max(responsive_now, responsive_then)`. Particles have no motion vectors, so without
this they would ghost for one frame. TI's point: with only two frames blended, a responsive value of
1 simply shows the current 2x-SMAA'd frame; there's no accumulation to protect.

## Final composite and UI

ImGui renders into its own target at window resolution (cleared each frame). One final draw then:

- scales the render-resolution image to the window;
- applies lateral chromatic aberration, vignette and film grain (camera effects TI lists for the
  composite stage);
- applies the display calibration in display space: sRGB encoding and a **brightness curve with
  fixed black and white points** (`pow(x, 2^-brightness)`), which scales the image rather than
  crushing shadows or clipping highlights (TI on Callisto Protocol vs Dead Space Remake);
- adds an 8-bit dither against banding;
- overlays the UI (premultiplied over transparent black, in gamma space) in the same draw (TI: the
  Days Gone approach, "as we overlay that UI, we can apply gamma correction in the same draw").

The back buffer is written through a plain UNORM view because the shader does the encoding itself.
