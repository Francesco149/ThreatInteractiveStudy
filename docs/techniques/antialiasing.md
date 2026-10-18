# Anti-aliasing: SMAA 4x (S2x + two-frame T2x), modernized

Video chapters: 1:19:38 "AA (Non Temporal Aspect)", 1:24:25 "AA (Temporal Aspect)".
Research: [../research/07-antialiasing-smaa.md](../research/07-antialiasing-smaa.md) (sample
positions, subsample indices, sign conventions, prior art). Code: `src/r_post.cpp`,
`shaders/tonemap.hlsl`, `shaders/smaa.hlsl` (wrapping `third_party/smaa/SMAA.hlsl`, MIT),
`shaders/temporal.hlsl`.

TI's position: MSAA is not there to resolve jaggies (morphological AA does that better in a single
frame) but to supersample temporally unstable edges; SMAA is the only viable morphological option;
the temporal part must blend only **two** frames, the current SMAA output and the previous frame's
SMAA output, never an accumulation buffer, with a narrow resolve, velocity weighting, a responsive
mask for VFX, and no texture blur.

## Pipeline

1. **Tone map + separate** (`tonemap.hlsl`, `LAYERS=2`): each of the two MSAA samples is tone
   mapped into its own 8-bit layer (`R8G8B8A8_TYPELESS` with UNORM and UNORM_SRGB views), i.e. the
   MSAA resolve happens *after* tone mapping, per sample. The same pass writes the 1x velocity (the
   average of the samples' G-buffer motion, camera-only reprojection for sky samples) and the AA
   mask (r = the two samples differ visibly = MSAA edge, g = responsive mask from the HDR alpha).
2. **SMAA S2x**: the full SMAA (colour edge detection on the gamma view, blending weights with the
   stencil optimization, neighbourhood blending on the sRGB view) runs on each layer with that
   layer's subsample indices; layer 1 is averaged into layer 0's result by hardware blending with
   blend factor 0.5 into an sRGB target (a linear average). The neighbourhood pass also stores
   SMAA's velocity length in alpha and an "SMAA touched this pixel" flag in the AA mask.
3. **Temporal (two frames)**: `lerp(current, previous_smaa_output(p - v), w)` with
   `w = 0.5 * saturate(1 - 30 * sqrt(| |v_cur| - |v_prev| |))` (SMAA's velocity weighting), point
   sampled (narrow resolve), multiplied by `1 - responsive`, and restricted to pixels that are an
   MSAA edge or SMAA-modified in the current or previous frame (edge-only blending, TSCMAA-style:
   interior texture detail is *never* temporally blended, so it can't blur). A motion-gated 3x3
   colour clamp of the previous value kicks in only for moving pixels (an always-on clamp makes
   static sub-pixel features flicker at 30 Hz under jitter).
4. Previous/current SMAA outputs ping-pong; the temporal output is never fed back.

The camera jitters between (-0.125, -0.125) and (+0.125, +0.125) px (SMAA 4x), so with the 2x MSAA
standard pattern the two frames give the 4-rook pattern of SMAA 4x; subsample indices per frame and
layer come from the SMAA table (`(5,3,1,3)`, `(4,6,2,3)`, `(3,5,1,4)`, `(6,4,2,4)`). MSAA targets
are created with `D3D11_STANDARD_MULTISAMPLE_PATTERN` so the sample positions are known.

Modes for the A/B: none, MSAA box resolve (after tone mapping), SMAA 1x, S2x, T2x (1x MSAA +
(-0.25, 0.25)/(0.25, -0.25) jitter), 4x.

## What isn't possible in D3D11

TI's preferred fix for jittered texture/lighting is **programmable sample locations** (match 4x MSAA
or a Decima-like pattern without jittering the camera). Core D3D11 doesn't expose them (only NVAPI,
which this project excludes; AMD AGS has nothing for D3D11). Texture jitter compensation via
`EvaluateAttributeSnapped` is described in the research note and not implemented yet; the edge-only
temporal mask already keeps interior texels exactly the current frame's.

## Costs

SMAA 4x at 1080p on the RTX 5060 (clock locked at 1485 MHz; 0.35-0.38 ms at stock clocks) costs
0.63 ms on the courtyard (mean over the
benchmark views: 2 x (edges 0.05 + weights 0.10 + blend 0.11) + 0.10 temporal), exactly TI's
0.38 + 0.25 ms; S2x alone 0.56, T2x 0.34, SMAA 1x 0.29 ms. On the simple test scene it was about
0.28 ms: the weight pass scales with the number of edges. The SMAA paper measured 2.34 ms for 4x on
a GTX 470 in 2012. Per mode and view: [../PERF.md](../PERF.md), [../perf-tables.md](../perf-tables.md).
