# Motion blur ("velocity compensation")

Video chapter: 1:42:40 "Velocity Compensation". Research:
[../research/08-velocity-motionblur-vfx.md](../research/08-velocity-motionblur-vfx.md) (sections
5-8 derive the filter). Code: `src/r_motion.cpp`, `shaders/motion_blur.hlsl`.

TI's requirements: motion vectors for everything (cheap if written in the base pass), velocity
packing (UE4/Days Gone style) where the camera's contribution is limited by depth (near content
blurs with camera motion like peripheral vision, the focus distance stays sharp, far content only
under extreme rotation: a "U" shape), blur that **trails only behind** the motion like the eye sees
it (NFS 2015) instead of the camera-footage look centred on the object, no tile-max outline
artifacts, no noise patterns (interleaved gradient noise, blue noise), uniform sampling with a
**separable** second pass (Crytek 2008, Fox Engine), and a trail length that is a constant time,
independent of the frame rate.

## Passes (after anti-aliasing, on the tone-mapped image)

SMAA S2x runs per MSAA sample, so the first single-sample image is the AA output; Fox Engine also
blurs there. Bright highlights streak a little dimmer than they would in HDR (LDR input).

1. **Velocity pack + tile max** (compute, one dispatch): per pixel, the front-most MSAA sample's
   depth and motion; `v_cam` = where a static point at that depth was last frame
   (`prev_view_proj * inverse(view_proj)`, unjittered), `v_obj = v_total - v_cam`, and
   `v = v_obj + w(z) * v_cam` with `w(z) = max(1 - smoothstep(1.5 m, 3 m, z),
   smoothstep(20 m, 60 m, z) * far_amount)`, where `far_amount` rises with the camera's angular
   speed (2-6 rad/s). The trail is `v * trail_time / frame_time` pixels (8 ms default = a 180°
   shutter at 60 fps; frame-rate independent), clamped to 32 px = the tile size. The 32x32 tile max
   is found with one groupshared `InterlockedMax` per thread. Each thread packs 4 pixels 16 apart
   so warps store contiguous rows: with a 2x2 quad per thread the same shader cost twice as much
   (0.12 vs 0.06 ms).
2. **One-sided neighbour max** over the 60x34 tile grid: a neighbour tile counts only if its trail
   points towards the centre tile (trails extend backwards from moving objects), which removes half
   of the usual dilation that causes the thick blocky outlines TI points out.
3. **Pass 1, trailing gather** (8 uniform taps, no noise): layer A samples ahead of the pixel along
   its own velocity and accepts samples whose own trail covers the pixel and that are in front or
   moving the same way; the pixel's own colour fills the unclaimed weight, so the leading edge
   stays opaque and sharp. Layer B samples along the neighbour-max velocity and accepts only
   *strictly closer, differently moving* samples: trails of foreground objects over the
   background. Background trails never cover the foreground (NFS 2015's barrel).
4. **Pass 2, separable** (8 taps): the same directional filter iterated over one pass-1 stride
   (`L/N`), centred, giving 8 x 8 = 64 evenly spaced virtual taps over the trail.

A/B: off; trailing 1 pass; trailing + separable (default); centred (classic per-pixel blur along the
pixel's velocity, the camera-footage look); depth-limited camera blur on/off.
