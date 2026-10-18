# R3: Shadows (density-synced local shadow maps, cascaded sun shadows, filtering, MSAA shadow mask)

Research note for the D3D11 educational engine: FL 11_0/11_1, SM 5.0, no vendor SDKs, deferred,
2x MSAA, 1080p60, test machine RTX 5060 8 GB + Ryzen 9 5900X. It covers the shadow stages of
Threat Interactive's (TI) "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering"
(2026-08-01). The relevant parts are 39:00-47:40 (local shadows, lighting structure, sun) and
34:40-35:00 (dithered shadow LODs).

How this was checked:
- Primary papers: PSM 2002, LiSPSM 2004, TSM 2004, Kozlov (GPU Gems 2004) and Zhang (GPU Gems 3).
- MS docs: CSM, shadow-depth techniques, the D3D11 depth-bias formula.
- MJP's MIT-licensed Shadows sample source.
- Castaño's The Witness post.
- Talks: Crytek's CryENGINE 3 Graphics Gems 2013 deck, Kasyan's CryEngine "Playing with Real-Time
  Shadows" 2013, Cyberpunk 2077 shadows 2021, Insomniac CSM scrolling 2012.
- The Bend Studio screen-space-shadow code (Apache 2.0) and its slides.
- The tutorial's own video frames (the pipeline chart is shown on screen), plus auto-captions of
  every TI video.

Anything that is my own derivation or inference is marked **[derived]**. Code is my own
formulation of published math, not copied source.

---

## 0. TL;DR: decisions for our engine

1. **Local (spot/point) shadows use "focused" density-synced frusta. We do not warp in camera
   post-perspective space.**
   - Each shadowed light keeps its own view orientation (the spot axis, or one of 6 cube axes for a
     point light). Only the off-center *window* of its perspective projection changes.
   - The window is fitted to the light-space projection of body **B** = camera frustum ∩ light
     frustum (∩ scene/receiver bounds).
   - The angular texel size is set by the 1080p+8% rule (section 2) and snapped to its own grid, so
     edges are stable while the camera moves. Tile size = window / texel size, capped at 2048.
   - This is the practical form of the video's "perspective shadow maps": the shadow FOV and zoom
     are scaled to the player's view. It has no PSM singularities.
2. **Local-shadow storage:** one D16_UNORM atlas, default 8192² (128 MiB; A/B 4096²).
   - Rectangular tiles per projector, packed by a shelf packer every frame.
   - One full `ClearDepthStencilView` per frame. No quad clears or copies, which the video bans.
3. **Local-shadow lookup is inline in the stenciled lighting draws.** There is no per-light mask
   pass. We use the 4-tap optimized 3x3 PCF (Castaño), plus slope-scaled raster bias and a
   receiver normal offset that scales with texel size.
   - Distant lights whose tile would drop below ~96 px fade to screen-space shadows (phase 2: a Bend
     SSS dispatch per light), then to no shadow map.
4. **Sun uses 3 near cascades and 1 far-field cascade.**
   - The near cascades are a `Texture2DArray` with 3 slices of 1296² D16 (the video says "around
     1300"; 1296 = 1.2 x 1080 and divides by 8 and 16). They are stabilized (bounding sphere +
     texel snapping) and rendered every frame.
   - The far-field cascade is a separate 2048² D16 slice, refreshed every 3rd frame.
   - On a camera cut, the far field is rendered in the same frame into a 1024² sub-rectangle.
   - Splits use a UE-style exponent of 3 over 72 m, which gives about 5.5 / 22 / 72 m. The far field
     covers 72-400 m. All values are tunable, and a density heat-map checks them.
5. **Caster culling everywhere.**
   - Spot/point: the fitted frustum *is* the extruded caster volume.
   - Sun: a light-space box test, then the camera slice extruded toward the light (Cyberpunk 2077
     style, up to ~8 planes).
   - Also: small-caster culling (less than ~1-2 texels), and an artist flag `castsShadow = false` for
     floors, terrain and ceilings that cannot shadow anything visible.
6. **The sun shadow is pre-projected into a native-resolution, non-MSAA `R8G8_UNORM` mask** with a
   full-screen pass.
   - The pass uses sample-0 depth and normal, interval cascade selection with a 10% blend band, and
     3x3 optimized PCF.
   - The result is `min`-combined with Bend screen-space shadows (compute, R8). G is reserved for
     subsurface/transmission data.
   - In the sample-frequency (edge) sun-lighting path, sample 0 reads the mask. Other samples reuse
     the 3x3 neighbour with the closest depth (Crysis 3's "bilateral upscale"), with an inline PCF
     fallback when no neighbour is within 2% of the depth.
7. **Shadow-pass hygiene.**
   - LODs are chosen from the *main camera* and cross-faded with the same ALU dither as the main view.
   - Alpha-tested casters get separate draws with an opacity-only (BC4) texture.
   - Back-face culling is on. We do not rely on front-face culling.
8. **Budgets to measure against (from the video's chart):** about 1.00 ms for all non-cascaded
   shadow maps, and 0.60 + 0.32 + 0.18 + 0.36 ms for the sun chain. Crysis 3's ~1.2 ms for 6 cascades
   is the "halve this" reference.

---

## 1. How the video positions shadows (paraphrased, with timestamps)

**39:00-41:20, non-cascaded (local) shadow maps.**
- TI objects to games that render large amounts of shadow depth where the on-screen light can never
  evaluate it.
- Raising shadow resolution hides leaking and jaggies, but costs 4x-8x more depth surface area.
- Instead, use the player's and the light's view positions to render the shadow map tightly inside
  the relevant bounds, and scale the shadow map's field of view with the player's perspective. The
  example is a pond whose shadow map becomes much denser.
- This is presented as Perspective Shadow Maps (Stamminger & Drettakis, 2002). TI says the paper's
  downsides are now irrelevant or solved.
- The density target is the "1080p + 8% rule" from their Days Gone analysis, capped at 2048². The
  shadow map should shrink as the light gets farther away within the camera frustum.

**On-screen chart box "Density Synced Perspective Shadow Maps (e.g. non-cascaded spot/point light)"
(39:05), paraphrased:**
- For each visible shadowed light, render only the relevant shadow data, at most 2048x2048, with
  FOV/zoom aligned to the player's viewport. Density should match 1080p+8%.
- Ban quad clears and copies. Cull objects that cannot cast into the player's viewport.
- Open questions in the box:
  - Several 2048² textures or an atlas? D16 looked great, provided the density is guaranteed, against a 24-bit
    main depth buffer. It still needs testing against a 32-bit main depth, and D32 is the
    alternative. The format must also suit forward rendering.
  - MSAA shadow maps with programmable sample locations (PSL), plus a Reddit post on "bias free
    shadow mapping".
- Budget: **1.00 ms**.
- "Learned from" notes:
  - Days Gone shows how much distance ratios matter, and that PSM helps.
  - MGSV culls one triangle orientation in its shadow passes, so single-sided floors never enter the
    map.
  - The Callisto Protocol and MGSV draw casters whose bounds can never shadow the viewport.
  - The Callisto Protocol, Crysis 3 and Dead Space Remake waste time on quad clears and copies of
    shadow maps.

**The PSM slide (41:05), paraphrased:**
- The paper's 2002 downsides, as listed: per-frame recomputation, temporal instability from that
  recomputation, remaining projection aliasing, and shadow bias artifacts.
- TI's claim: the first three are fixed by the 1080p+8% density rule and by 8th-gen games already
  rendering 15-30 shadow maps per frame. Bias artifacts have 20+ years of newer algorithms.
- Definition box: shadow-map *objects* must always be rendered at 8% higher density than the
  player's resolution, which is assumed to be 1080p. A higher game resolution must *not* raise
  shadow density.

**42:40-44:40, lighting structure.**
- Each shading model gets its own stenciled full-screen triangle draw. That leaves complexity budget
  for more advanced PCF / anti-aliased shadow-map projection.
- Artists stay restricted to *barely softened* shadows, because sharp shadows can be biased almost perfectly.
- At a distance, the light shader switches to a permutation that replaces shadow-map projection with
  noiseless, high-sample-count screen-space shadows.
- Chart box (44:40):
  - Shadowed local lights do not use shadow projection masks. Minimal PCF and screen-space shadows
    run in the same lighting pixel shader.
  - A minimal PCF and shadow-map AA method must complement the targeted density.
  - Shadow sharpening is forced to a degree, to stop the leaking that over-soft shadows cause.
  - Lighting budget: 1.50 ms.

**44:40-47:00, sun/moon.**
- PSM was originally designed for the sun. It would need testing to prove it beats orthographic CSM
  with well-chosen distance ratios (Days Gone). Far-field cascades are still a work in progress.
- **KCD2** uses 5 cascades but refreshes the farther ones over several frames. The slide shows each
  frame rendering cascades 1 and 2 plus one of 3/4/5, so all 5 are updated across 3 frames. TI is
  open to this, *if* casters that cannot shadow the view are culled.
- This needs tests with racing-speed motion and camera cuts. Proposed cut fix: render the late
  cascades in the cut frame at reduced resolution, then return to the 3-frame alternation.
- Prefer approximated distance shadowing: the Days Gone foliage shadow atlas, or SDF shadows with
  native-resolution projection.
- Crysis 3's 6 cascades cost about 1.2 ms. The target is half that. Most of Crysis 3's cost came from
  the last cascades, which drew static high-poly or alpha-tested content.
- Shadows should be pre-projected into a native-resolution 8-bit buffer channel. Crysis 3's
  sample-frequency shadow masking saves cost.

**Chart row "Directional Light (Sun/Moon)" (46:40), paraphrased:**
1. Rendering the shadow maps: cascade receiver culling, 3 cascades at 8% higher density with Days
   Gone distance ratios, around 1300².
2. Optional: create an 8x smaller min-max RG16. Days Gone builds one in a compute shader for the
   projection stage. Its value needs testing.
3. Far-field cascade resolution: this is the scalability knob. Dead Space Remake uses 4k x 4k,
   while Days Gone, an open world, uses 2k x 2k.
4. Optional SDF / Days Gone tree rendering: alpha testing is expensive. Days Gone uses a ~0.3 ms pass
   that renders specialized atlases, later projected into the shadow mask.
5. Compute-shader screen-space shadows: groupshared memory, a 32-bit depth input that includes the
   micro-detail depth bias, and noiseless non-MSAA 8-bit output.
6. Project the shadow data into a native-resolution non-MSAA RG8. R holds the shadow map result and
   G holds subsurface properties. It may take multiple draws, as in Days Gone.
7. Remaining sample-frequency draws, separated per shading-model stencil.
   - The mask could drive a full-screen stencil pass so that fully shadowed pixels skip sun shading.
   - Use Crysis 3's cascaded shadow mask optimization.
   - Subsurface needs R&D.

The row's timing labels are **0.60 + 0.32 + 0.18 + 0.36 ms**. KCD2's note says cloud shadows add
about 0.30 ms on the mask. **[derived]** My reading of the four labels: 0.60 = cascades (half of
Crysis 3), 0.18 = compute SSS (Bend's PS5 figure is 0.19 ms), 0.36 = projection/mask. 0.32 is
probably far field plus tree atlas. The chart does not label them.

**47:00-47:40.** The marketing focus is shadows that are noiseless, artifact-free and faster than
RT. New games lack shadows because of unoptimized geometry, missing approximated distance shadowing
and *shadowing floors*. As the video puts it: "Shadows are not expensive if you properly optimize
them."

**34:40.** Shadow-map LOD draws must use the same dithered LOD transitions as the main view. TI says
undithered shadow LODs caused severe popping in KCD2.

---

## 2. The "1080p + 8%" density rule: what it means, precisely

### 2.1 Where it comes from

- **Days Gone frame analysis** (TI, `2IeYOECebTA`, 7:53):
  - About 600 draws go into the sun cascades, each cascade 1152² (RTX 3060 at 1080p).
  - Foliage casts through sprite atlases instead of meshes.
- **"Top 10 Most Neglected Areas" (TI Video 19, `N6sLJhI_Fmg`, about 9:13-12:17).** This is the
  video the tutorial's PSM slide points to as Video 19.
  - Days Gone's low preset uses 3x 576² cascades, and the medium preset about 1152². The highest
    preset keeps 1152² but adds cheap shadow AA and very conservative softening.
  - TI says roughly 30% more *squared* resolution is still needed (1152² x 1.3 ≈ 1313²), because
    the goal is to render shadow objects at a slightly higher density than the same objects in the
    main camera.
  - That is the origin of the chart's ~1300² figure.
- **"When Butchered Rasterization Leads To Raytracing" (TI Video 21, `PhEIa5RtKVA`, 9:47-10:17).**
  - RT shadows mostly look better because the ray-traced result matches on-screen density.
  - A shadow map rendered at 8% above 1080p density would give RT-like results.
  - Resizing the map alone is not enough. Its zoom/FOV must also follow the main camera.
- **Tutorial chart image (39:05):** the Days Gone thumbnail labels the screen region **1080P** and
  the shadow-map region **1166P = 8%**. 1166/1080 = 1.0796, so **the 8% is linear, not area**.
- **Tutorial slide (41:05):** "objects" must be rendered at 8% higher density than the player's
  resolution, assumed 1080p. The real output resolution can go higher without raising shadow
  density.

### 2.2 Definition used in this engine

> At every visible shadow receiver, one shadow-map texel must project to at most 1/1.08 of a
> *1080p-reference* screen pixel, measured linearly. Equivalently: shadow texels per 1080p pixel ≥
> 1.08 along each screen axis. The reference is 1080 lines regardless of the actual output height.

The rule is capped at 2048² per local shadow map. **[derived]** 2048 ≈ 1.07 x 1920, so a light
whose visible region spans the whole 1080p screen width hits the cap at almost exactly the rule.

### 2.3 Shadow-map aliasing formula (Stamminger & Drettakis 2002, section 1.1)

A shadow-map texel of size `ds` (on the shadow image plane, at unit distance) projects onto the
final image with size

```
d  =  ds * (r_s / r_i) * (cos(beta) / cos(alpha))
```

- `r_s` = distance from the light to the surface.
- `r_i` = distance from the camera to the surface.
- `alpha` = angle between the light ray and the surface normal.
- `beta` = angle between the view ray and the surface normal.

Aliasing appears when `d` exceeds the image pixel size `di`:
- `ds*r_s/r_i` is **perspective aliasing**. Reparameterization (PSM etc.) and resolution choice fix
  this term.
- `cos(beta)/cos(alpha)` is **projection aliasing**: light rays grazing the surface. Nothing short
  of more resolution fixes this term.

### 2.4 Required resolution for a local light **[derived from 2.3]**

Ignore the projection term. Let the camera have vertical FOV `theta_y`. A 1080p-reference pixel
spans `p = 2*tan(theta_y/2)/1080` radians, which is `p * z_c` metres at view depth `z_c`.

A perspective shadow map with angular texel size `delta` (tan-space units per texel) spans
`delta * z_L` metres at light-space depth `z_L`. The rule `delta*z_L <= p*z_c/1.08` gives

```
delta_req = (2*tan(theta_y/2) / (1.08*1080)) * min over receivers of (z_c / z_L)
tile size  N_x = ceil(window_width_tan / delta),  N_y = ceil(window_height_tan / delta),  each <= 2048
```

- Evaluate the `min` over the vertices of body B (section 4.2).
- Clamp `z_c >= z_focus_min` (about 0.5-1 m). Otherwise points on the camera near plane demand
  infinite resolution.
- This is the literal rule. A cheaper approximation is to project B to the screen and take
  `N ≈ 1.08 x (B's bounding-rectangle extent in 1080p pixels)`. It agrees when the light and camera
  view B from similar directions. Keep it as a debug overlay or sanity check.
- CryEngine does essentially this for point-light projectors. Kasyan 2013 scales each of the 6
  projectors separately according to how much of the view its projection covers.

### 2.5 What the rule implies for cascades **[derived]**

Take a stabilized cascade (bounding sphere of radius `R` around a frustum slice that ends at
`d_far`) with resolution `N`.
- Texel size: `2R/N`.
- Pixel size at `d_far` (1080p): `2*d_far*tan(theta_y/2)/1080`.
- For slices with near/far ratio ≥ 2, `R ≈ k*d_far*tan(theta_y/2)*sqrt(1+a²)`, where `a` = aspect
  and `k ≈ 1.04` for a ratio of 3.

So, at the slice's far edge:

```
texel/pixel  ≈  1080*sqrt(1+a²)*k / N      (independent of the split distances!)
```

- At 16:9 this is about `2290/N`.
- N = 1296 gives about 1.77: texels are 1.77x the pixel size, so density is about 0.56.
- N = 2048 gives about 1.12.
- **Meeting 1.08x density at the far edge needs N ≈ 2470** (about 1.08 x the 1080p screen diagonal).
- At the slice's near edge the ratio is worse by `d_far/d_near` (the "distance ratio"): about 4x for
  a 5.5→22 m cascade.
- A tight light-space fit with light perpendicular to the view needs ≥ 1.08 x 1920 ≈ 2074 texels
  across the cascade at its far edge.

Consequence: full-frustum cascades of ~1300² cannot satisfy the literal rule everywhere. TI's chart
note that the closest Days Gone cascade nearly matches the density of the 1080p viewport can only
hold for particular content or distances. We treat the cascade rule as a *target measured with the
heat-map* (2.6). It is tuned with cascade count, split ratio, resolution, optional depth-bounds
tightening (SDSM) and the A/B tests in section 11. This is open question Q1.

### 2.6 Debug view: shadow texels per 1080p-reference pixel

Use this in the mask pass or the light shader. The input `atlasUV` is the shadow UV of the receiver,
in [0,1] of the *whole* shadow texture.

```hlsl
// Returns how many shadow texels one 1080p-reference pixel spans along its worse screen axis.
float ShadowTexelsPerRefPixel(float2 shadowUV, float2 shadowTexSize, float outputHeight)
{
    float2 t  = shadowUV * shadowTexSize;          // texel coordinates
    float2 dx = ddx(t), dy = ddy(t);               // texels per actual pixel step in x / y
    float  s  = sqrt(min(dot(dx, dx), dot(dy, dy)));
    return s * (outputHeight / 1080.0);            // per 1080p-reference pixel
}
// Colour: < 1.0 red (undersampled), 1.0..1.08 yellow, 1.08..2.0 green (rule met), > 2.0 blue (waste)
```

Derivatives break at depth discontinuities and cascade or tile seams. Ignore those pixels or show
them grey.

---

## 3. PSM, LiSPSM and TSM: what they do, their problems, and the verdict

### 3.1 Perspective Shadow Maps (Stamminger & Drettakis, SIGGRAPH 2002)

**Idea.** Render the shadow map in the camera's **post-perspective space** (after the camera
projection, the view frustum becomes the unit cube). The final image is then an orthographic view of
that cube, so the `r_s/r_i` term of 2.3 becomes constant and perspective aliasing disappears.

**Light types after the transform:**
- Directional lights become point lights on the infinity plane. A light from behind the camera
  becomes an "inverted" point light, where the hit order along each ray reverses.
- Point lights in front of the viewer stay point lights. Lights behind the viewer become inverted.
- Lights in the camera plane become directional. This is the ideal "miner's lamp" case.
- Benefit is largest for a light perpendicular to the view direction. For a light parallel to the
  view (dueling frusta), the two projections cancel and PSM degenerates to a uniform shadow map.

**Problems:**
1. **Casters behind the camera.** They map beyond the infinity plane. The paper's fix is a
   *virtual camera shift* backwards until the caster set H = hull(V ∪ light) ∩ scene ∩ light frustum
   lies in front. This degrades toward a uniform shadow map.
2. **Bias.** Depth is non-uniformly scaled in post-perspective space. The paper used a constant
   offset that needs per-scene tuning.
3. **Near-plane sensitivity.** Quality needs the camera near plane as far out as possible. The
   paper read back the depth buffer every frame for this, which cost 10 ms at 640x480 with 16-bit
   depth on a GeForce3.
4. **Implementation.** It needs CGAL convex-hull and intersection code. The case analysis is
   involved.
5. **Uneven resolution.** It over-allocates near the viewer and starves distant shadows.

### 3.2 PSM fixes (Kozlov, "Perspective Shadow Maps: Care and Feeding", GPU Gems 2004)

- **Inverse light projection** with a *negative* near plane, so the light camera "sees beyond
  infinity". No virtual camera shift is needed. It needs about 24-bit depth precision.
- **Unit-cube clipping.** Aim the light camera only at receivers' bounding volumes. This works well
  for directional lights in large outdoor scenes, but not for point lights.
- **Bias in world space.** Move the receiver along the light direction by `a + b*texelWorldSize`,
  then transform. Kozlov calls it empirical.
- **Cube maps in post-perspective space.** 3-5 faces for directional lights, all 6 for a point light
  inside the cube.
- Tested on a GeForce4 Ti 4600 at 1600x1200 with 100k-500k polygons: above 30 fps.

### 3.3 Light Space Perspective Shadow Maps (Wimmer, Scherzer, Purgathofer, EGSR 2004)

**Steps:**
1. **Focus** on the convex body B: the convex hull of the view frustum and the light position,
   intersected with the light frustum and the scene bounds.
2. Build light space: y = light vector, z ⟂ light vector in the plane of the view and light vectors,
   x completes the frame.
3. Enclose B with a perspective frustum P whose view direction is *parallel to the shadow-map
   plane*. Its near and far planes are at B's min/max light-space z.
4. Choose the free parameter `n`, the distance from the projection reference point to P's near
   plane.

**Optimal n for a view perpendicular to the light:** `n_opt = z_n + sqrt(z_f*z_n)`, where `z_n` and
`z_f` are the camera near/far planes.

**General case:** `n_opt' = n_opt / sin(gamma)`, where gamma is the angle between view and light
vectors.
- At gamma → 0, n → ∞ and LiSPSM becomes uniform shadow mapping, which is optimal for dueling
  frusta.
- The paper also suggests replacing `z_f` by `z_n + Δz` with `Δz = (f - n)*sin(gamma)`.

**Why it is safer than PSM:**
- Light directions do not change.
- There are no relevant singularities, because the projection plane is parallel to the light.
- Point lights are handled by applying their own perspective first and then treating them as
  directional.

**Caveat in the paper itself:** nearby, strongly perspective point lights (e.g. indoors) suffer
less from perspective aliasing, so *uniform (focused) shadow maps may be enough*.

**Performance:** LiSPSM runs as fast as standard shadow mapping (at least 75 fps on a P4 2.4 GHz +
GeForce FX 5900, 1024² map).

### 3.4 Trapezoidal Shadow Maps (Martin & Tan, EGSR 2004)

- Approximate the camera frustum, as seen from the light (in light post-perspective space), by a
  **trapezoid**. Map it to the unit square with a projective "trapezoidal transformation" `N_T`.
- **80% rule:** choose a focus distance `d` from the near plane. Pick the trapezoid's side edges so
  that the point at distance `d` maps to the 80% line of the shadow map.
- **Continuity:** the two parallel edges give smooth frame-to-frame resolution changes.
- **Depth:** `N_T` would warp z per vertex and break constant bias. TSM therefore transforms only x,
  y and w, and keeps light-space z by interpolating it and writing it per fragment, which costs
  early-z.
- **Limits stated by the authors:** dueling frusta give no advantage, and flicker still happens
  occasionally.
- Reported results: 100k triangles at about 28 fps (1024² map, Pentium 4 1.8 GHz + FX5900 Ultra).

### 3.5 Verdict per light type

| Light | Recommended | Why |
|---|---|---|
| **Spot** | **Focused uniform perspective** (window fitted to B, own axis kept), resolution from 2.4, window snapped to the texel grid | No camera post-perspective transform, so no singularities and no lights-behind-camera case. Bias stays standard. LiSPSM's authors say near local lights do not need warping. Fitting gives most of the density gain the video shows. Snapping gives stability. |
| **Point** | **6 independent focused cube-face projectors** in the same atlas (CryEngine does this) | Each face is a 90° spot. Faces whose B is empty are skipped. Dual-paraboloid maps need finely tessellated casters, because the projection is non-linear and straight edges rasterize wrong. They also cannot be window-fitted. |
| **Sun** | **Stabilized orthographic CSM** (+ far field), no warping | Warping breaks texel snapping, so shadows shimmer on camera rotation. It degenerates in dueling frusta. MS docs: CSM can be combined with PSM/LiSPSM, but the docs call that unnecessary. The video also says PSM-for-sun is unproven against good distance ratios. LiSPSM on cascade 0 only is an optional experiment (A/B 11.6). |

PSM's 2002 "downsides" in our design:
- *Recomputation every frame*: we re-render every frame anyway; that is the video's point.
- *Temporal instability*: solved by keeping the light orientation fixed and snapping the window to a
  fixed angular texel grid (4.3). Changes happen only when the quantized texel level changes, with
  hysteresis.
- *Projection aliasing*: inherent. It is mitigated by density and PCF.
- *Bias*: handled with slope bias + normal offset (6.3), because the depth is a normal perspective z.

---

## 4. Local lights: density-synced focused shadow frusta

### 4.1 Per-frame algorithm, for each visible shadowed light (or each cube face)

1. Build the fixed light view `V_L`: eye = light position, look = spot axis, up = any non-parallel
   axis that stays constant over time.
2. Compute body **B** = camera frustum ∩ light pyramid (∩ optional receiver depth bounds). If B is
   empty, the light lights nothing visible: skip the light completely.
3. Transform B's vertices into light view space and compute the tan-space window `[l,r]x[b,t]` =
   min/max of `x/z` and `y/z`, clamped to the cone.
4. Compute `delta_req` (2.4). Quantize to `delta = delta0 * 2^(k/4)` (quarter-octave levels) with
   hysteresis. Compute the tile size and cap it at 2048 by coarsening `delta`.
5. Snap the window to multiples of `delta` and pad it by the PCF radius + 1 texel.
6. Choose near/far from B (4.4) and build an off-center perspective projection.
7. Allocate the tile in the atlas, then set viewport + scissor.
8. Cull casters against the fitted frustum (4.6) and draw them depth-only.
9. Upload `WorldToAtlas`, `delta`, the tile rectangle and the light parameters for the lighting
   shader.

### 4.2 Computing B (C++, symmetric convex clipping)

The vertex set of F ∩ G (two convex frusta; here F = camera frustum, G = light pyramid, and the
result is body B) is the union of:
- F's faces clipped by G's planes,
- G's faces clipped by F's planes.

That covers vertices on F's faces, vertices on G's faces, and corners of one frustum lying inside
the other.

```cpp
struct Plane { XMFLOAT3 n; float d; };                       // inside: dot(n,p)+d >= 0
using Poly = std::vector<XMFLOAT3>;
struct Frustum { XMFLOAT3 c[8]; Poly faces[6]; Plane planes[6]; };

// Build a frustum from 8 corners: 0-3 near (TL,TR,BR,BL), 4-7 far (same order).
Frustum MakeFrustum(const XMFLOAT3 c[8]) {
    static const int F[6][4] = {{0,1,2,3},{4,5,6,7},{0,3,7,4},{1,2,6,5},{0,1,5,4},{3,2,6,7}};
    Frustum f; XMVECTOR centre = XMVectorZero();
    for (int i = 0; i < 8; ++i) { f.c[i] = c[i]; centre += XMLoadFloat3(&c[i]) * 0.125f; }
    for (int k = 0; k < 6; ++k) {
        f.faces[k] = { c[F[k][0]], c[F[k][1]], c[F[k][2]], c[F[k][3]] };
        XMVECTOR a = XMLoadFloat3(&c[F[k][0]]), b = XMLoadFloat3(&c[F[k][1]]), d = XMLoadFloat3(&c[F[k][3]]);
        XMVECTOR n = XMVector3Normalize(XMVector3Cross(b - a, d - a));
        float dist = -XMVectorGetX(XMVector3Dot(n, a));
        if (XMVectorGetX(XMVector3Dot(n, centre)) + dist < 0) { n = -n; dist = -dist; } // orient inward
        XMStoreFloat3(&f.planes[k].n, n); f.planes[k].d = dist;
    }
    return f;
}

Poly ClipPoly(const Poly& in, const Plane& p) {
    Poly out; size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        XMVECTOR a = XMLoadFloat3(&in[i]), b = XMLoadFloat3(&in[(i + 1) % n]), N = XMLoadFloat3(&p.n);
        float da = XMVectorGetX(XMVector3Dot(N, a)) + p.d, db = XMVectorGetX(XMVector3Dot(N, b)) + p.d;
        if (da >= 0) out.push_back(in[i]);
        if ((da >= 0) != (db >= 0)) { XMFLOAT3 q; XMStoreFloat3(&q, a + (b - a) * (da / (da - db))); out.push_back(q); }
    }
    return out;
}

std::vector<XMFLOAT3> IntersectionVertices(const Frustum& F, const Frustum& G) {
    std::vector<XMFLOAT3> pts;
    auto clipAll = [&](const Frustum& X, const Frustum& Y) {
        for (const Poly& face : X.faces) {
            Poly p = face;
            for (const Plane& pl : Y.planes) { p = ClipPoly(p, pl); if (p.empty()) break; }
            pts.insert(pts.end(), p.begin(), p.end());
        }
    };
    clipAll(F, G); clipAll(G, F);
    return pts;                                  // empty => no overlap => skip the light
}
```

**Inputs:**
- *Camera frustum*: corners from the camera parameters. The far plane is clamped to
  `min(camera far, max view distance, distance to light + light range)`.
  - Optional: clamp near/far to the visible depth range from a GPU min/max reduction read back 2
    frames late. That is the "receiver depth bounds" step, SDSM-like (Lauritzen et al. 2010/2011).
- *Light frustum*: the square pyramid that circumscribes the cone:
  - corners at `z = n_min` and `z = range`,
  - half-extent `z*tan(outer_half_angle)`.
  - For cube faces: half-angle 45°, plus padding (4.7).

**Pitfalls:**
- Use the *outer* cone angle.
- Very wide spots (≥ ~150°) are better handled as point lights.
- Add a small epsilon (`da >= -1e-5f`) for stability.
- 6x6 plane clips per face are cheap: under 1 µs per light.

### 4.3 Window, quantized texel size and snapping

```cpp
// ptsWorld: vertices of B (4.2). Each is transformed by V_L (light view space, z > 0 along the axis).
float l=+FLT_MAX, r=-FLT_MAX, b=+FLT_MAX, t=-FLT_MAX, zMin=FLT_MAX, zMax=0, ratioMin=FLT_MAX;
for (const XMFLOAT3& pw : ptsWorld) {
    XMFLOAT3 p = TransformPoint(pw, V_L);
    float z = std::max(p.z, nMin);
    l = std::min(l, p.x / z); r = std::max(r, p.x / z);
    b = std::min(b, p.y / z); t = std::max(t, p.y / z);
    zMin = std::min(zMin, z); zMax = std::max(zMax, z);
    float zc = std::max(CameraViewDepth(pw), zFocusMin);            // same point, camera view depth
    ratioMin = std::min(ratioMin, zc / z);
}
float tanCone = tanf(outerHalfAngle);
l = std::max(l, -tanCone); r = std::min(r, tanCone); b = std::max(b, -tanCone); t = std::min(t, tanCone);

float deltaReq = (2.0f * tanf(camFovY * 0.5f) / (1.08f * 1080.0f)) * ratioMin;
int   k        = (int)floorf(4.0f * log2f(deltaReq / delta0));      // finer-or-equal level
k = ApplyHysteresis(light, k);    // go finer immediately; go coarser only after ~30 frames stable
float delta = delta0 * exp2f(k / 4.0f);

const int pad = 3;                                                   // PCF radius (2) + 1
auto snapLo = [&](float v){ return (floorf(v / delta) - pad) * delta; };
auto snapHi = [&](float v){ return (ceilf (v / delta) + pad) * delta; };
l = snapLo(l); b = snapLo(b); r = snapHi(r); t = snapHi(t);
int Nx = (int)lroundf((r - l) / delta), Ny = (int)lroundf((t - b) / delta);
while (std::max(Nx, Ny) > 2048) { ++k; /* recompute delta, re-snap, Nx, Ny */ }
```

**Why this is stable:**
- `V_L` never changes while the light is static.
- Texel boundaries lie on the fixed grid `i*delta` in tan-space. As the camera moves, the window
  slides by whole texels and the rasterized depth of every texel stays the same. This is the
  perspective analogue of CSM texel snapping.
- Only a change of `k` re-grids the texels. Hysteresis keeps that rare.
- `delta0` is any constant base (for example `2*tanCone/4096`).

### 4.4 Near/far planes and D16 precision

**Planes:**
- `far = zMax` (≤ light range).
- `near = max(n_min, zMin)`. zMin is the closest *receiver* in B.
- Casters between the light and `near` are kept by **depth clamping**
  (`DepthClipEnable = FALSE`). Their z clamps to 0, so they still occlude every receiver behind the
  near plane (pancaking). Their x/y still project correctly, because only points with w > 0 can
  occlude.

**Precision** for `depth = f/(f-n)*(1 - n/z)` stored in D16:

```
Δz_world ≈ (1/65536) * (f - n) * z² / (f * n)
```

Example values:
- n = 0.1, f = 10 m: 3.8 mm at z = 5 m, 15 mm at z = 10 m.
- n = 0.5, f = 10 m: 2.9 mm at z = 10 m.

So pushing `near` out to zMin matters more than D16 vs D32.
- Reverse-Z with D32_FLOAT (near=1, far=0, `GREATER_EQUAL`) makes precision almost uniform. It is the
  A/B alternative, per the video's open "D16 vs D32" question.
- Reverse-Z gives **no benefit with D16 UNORM**.

### 4.5 Projection and lookup matrices

```cpp
XMMATRIX P = XMMatrixPerspectiveOffCenterLH(l*nearZ, r*nearZ, b*nearZ, t*nearZ, nearZ, farZ);
// NDC -> atlas UV for tile (x0,y0,w,h) in an atlas of size A (row-vector convention, z untouched):
float sx = w / A, sy = h / A, ox = x0 / A, oy = y0 / A;
XMMATRIX T( 0.5f*sx, 0,        0, 0,
            0,      -0.5f*sy,  0, 0,
            0,       0,        1, 0,
            0.5f*sx + ox, 0.5f*sy + oy, 0, 1);
XMMATRIX WorldToAtlas = V_L * P * T;     // HLSL: h = mul(float4(P,1), WorldToAtlas); uvz = h.xyz / h.w
```

Upload `XMMatrixTranspose(WorldToAtlas)`, because HLSL cbuffers are column-major by default. Or
declare the member `row_major`.

**Render pass state:**
- Viewport `{x0, y0, w, h, 0, 1}` plus a scissor rectangle with the same bounds
  (`ScissorEnable = TRUE`). Set both. D3D11 docs do not promise that the viewport alone scissors.
- PCF taps stay inside the tile thanks to the 3-texel pad.

### 4.6 Caster culling for local lights

**The fitted frustum is the exact caster volume.** Every ray from the light to a point of B lies
inside the pyramid (apex = light, window = projection of B, depth up to zMax). So the objects that
cannot cast into the player's viewport are exactly the objects outside its 4 side planes and its
far plane. The near plane is *not* tested, because of pancaking.

Further culls:
- **Small casters:** cull if the bounding diameter is less than `cullTexels * delta * z_L(center)`.
  Use cullTexels ≈ 1 to 2.
- **Floors and non-shadowing geometry:** an artist flag `castsShadow = false`, which TI insists on.
  MGSV gets a similar effect for free by culling one face orientation in shadow passes. TI calls
  front-face culling an outdated trick because it causes self-shadow moiré. Our default stays back-face
  culling.
- **Optional:** test the main-view visibility of the caster's light-extruded bounding box against
  the CPU/GPU occlusion data (Cyberpunk 2077 does this for cascades).

### 4.7 Point lights

- Treat each point light as 6 projectors: ±X, ±Y, ±Z, each with a 90° pyramid.
  - Compute B per face. Faces with empty B are skipped.
  - Each face gets its own `delta`, tile and matrix. Kasyan 2013 describes CryEngine always splitting
    omni lights into six projectors, each scaled separately from its projected coverage.
- **Face selection in the shader:** take the major axis of `P - lightPos` and use that face's
  matrix. Store 6 matrices per light, with a validity bit.
- **Pitfall:** PCF taps near a face edge.
  - Extend each face pyramid by the pad, i.e. `tanCone = 1 + pad*delta`, *before* clamping. The
    fitted window can then hold the extra texels.
  - Make sure the selected face is never an empty one. A visible pixel always lies in its own face's
    B, so this holds.
- **Dual-paraboloid (Brabec et al. 2002), not used:** the projection is non-linear, so casters need
  dense tessellation, and the window cannot be fitted.

### 4.8 The atlas

**Resource:**
- `Texture2D` 8192x8192, `DXGI_FORMAT_R16_TYPELESS`, `BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE`.
- DSV `D16_UNORM`, SRV `R16_UNORM`.
- Size: 128 MiB. A 4096² A/B is 32 MiB and holds four 2048² tiles.

**Packing** (shelf packer, every frame):

```cpp
// Sort projector requests by height (descending), then fill left-to-right shelves.
bool Pack(std::vector<Req>& q, int A) {
    std::sort(q.begin(), q.end(), [](auto& a, auto& b){ return a.h > b.h; });
    int x = 0, y = 0, shelf = 0;
    for (auto& r : q) {
        if (x + r.w > A) { y += shelf; x = 0; shelf = 0; }
        if (y + r.h > A) return false;          // overflow
        r.x = x; r.y = y; x += r.w; shelf = std::max(shelf, r.h);
    }
    return true;
}
// On overflow: coarsen the least important projectors (farthest / smallest screen size) by one
// delta level (x2^(1/4) texel size) and retry; log it, it means the budget (Q4) is exceeded.
```

**Clearing:** one `ClearDepthStencilView(atlasDSV, D3D11_CLEAR_DEPTH, 1.0f, 0)` per frame.
- A full-resource clear is a metadata fast clear on modern GPUs. AMD's RDNA guide puts fast clears at
  roughly 100x the speed of normal clears, and requires a full-image clear with value 0 or 1.
- **Never quad-clear** sub-rectangles. TI measured Crysis 3's quad clear of its 4k atlas at about
  10x the cost of a proper clear. Callisto Protocol and Dead Space Remake do the same.
- **Never copy cached static maps.** TI found such static-map copies very costly in one analysis. The
  video's stance is that re-rendering is faster on current hardware.

**Format note (AMD RDNA guide):**
- 24-bit depth costs the same as 32-bit.
- 16-bit depth is not guaranteed to compress better than 32-bit.
- D16's real wins are half the memory and half the PCF fetch bandwidth.

### 4.9 Distance swap: screen-space shadows

- If a projector's `max(Nx, Ny)` falls below ~128, fade its shadow-map term toward the screen-space
  term over the band 128 → 96 px, then stop rendering its map.
  - This is the video's distance-based shader permutation. TI also criticises Unreal for having no
    light-shadow fade-out with distance (Days Gone video, 13:56).
- **Phase 1:** fade to unshadowed, plus the contact SSS from the sun pass (no extra cost).
- **Phase 2:**
  - Dispatch Bend SSS for that light with `inLightProjection = (lightPos, 1) * ViewProj`. Bound it by
    the light's screen rectangle and write to a scratch R8.
  - The light's draw reads the scratch target. Bend writes every pixel in its bounds (plus a margin
    of 2 x WAVE_SIZE), so no clear is needed.
  - Cost scales with the light's on-screen area.

### 4.10 Pitfalls (local lights)

- `z_focus_min` clamp: without it, a camera standing inside a spot cone near the apex always gets
  2048².
- Lights or camera teleporting: reset the hysteresis.
- **Moving lights** re-grid every frame and will shimmer. This is inherent; keep density ≥ 1.08 and
  PCF on.
- **Atlas UV vs tile:** the PCF math (6.2) must use the *atlas* texel grid (atlas size), not the tile
  size.
- **Light leaking:** if the frustum is fitted to the camera frustum but casters lie outside B in x/y,
  they are correctly ignored because their shadows cannot land in view. Check that the pad covers
  the PCF kernel.

---

## 5. Sun: cascaded shadow maps

### 5.1 Layout and formats

| Resource | Size | Format | Update |
|---|---|---|---|
| Near cascades 0-2 | `Texture2DArray` 1296² x 3 | R16_TYPELESS / D16 DSV per slice / R16_UNORM SRV | every frame |
| Far-field (FF) cascade 3 | `Texture2D` 2048² (or slice 3 of a 2048 array) | D16 | every 3rd frame; on a camera cut, the same frame at 1024² |
| Shadow mask | 1920x1080 | `R8G8_UNORM` RTV + SRV (non-MSAA) | every frame |
| SSS | 1920x1080 | `R8_UNORM` UAV + SRV | every frame |

Comparison sampling on texture arrays needs D3D10.1+, so FL11 is fine. Use one DSV per array slice:
`FirstArraySlice = i, ArraySize = 1`. Clear each slice fully before rendering it.

### 5.2 Split schemes

**Practical Split Scheme** (Zhang et al., PSSM, 2006; GPU Gems 3 ch. 10), for split `i` of `m`:

```
C_i = lambda * n*(f/n)^(i/m) + (1 - lambda) * (n + (f - n)*i/m)          (paper default lambda = 0.5)
```

- `lambda = 1` gives logarithmic splits, which keep the far-edge/near-edge density ratio constant.
- MJP's sample implements exactly this (`minZ*pow(maxZ/minZ, p)` blended with uniform by lambda).

**UE4-style exponent** (UE `CascadeDistributionExponent`): each cascade is `E` times longer than the
previous one:

```
split_i = near + (D - near) * (1 + E + ... + E^(i-1)) / (1 + E + ... + E^(m-1))
```

- Defaults: `E = 3`, `m = 3`, `D = 72 m`, which gives ≈ 5.5 / 22 / 72 m.
- Days Gone runs on UE4, but its actual ratios are *not public*. Tune with the heat-map.
- Kasyan (CryEngine) uses an approximately logarithmic distribution, with the closest splits
  adjusted by hand. Wider FOV and a closer near plane increase cascade overlap and waste.

**Far field:** 72 → 400 m (tunable). Cyberpunk 2077's "distant shadows" go much further: 4 levels,
each 4x larger, the last covering 256 km².

### 5.3 Stabilization (Valient, ShaderX6, 2008; as in MJP's sample)

1. Per slice, take the 8 corners and their centroid `C`. The radius is `R = max |corner - C|`,
   rounded up to 1/16 m, so `R` is rotation-invariant.
2. Orthographic projection: extents ±R in x and y, z range `[0, 2R]`. The light camera sits at
   `C - L*R` looking along L (L = direction the light travels), with a constant world up vector.
3. **Texel snap:** project the world origin with `ViewProj`. Scale by `N/2`, round, and convert the
   rounding delta back to NDC (`*2/N`). Add it to the x/y translation of the projection matrix.
   Then every texel stays fixed in world space under camera translation.
4. Optional: the MS docs variant snaps the light-space min/max to `worldUnitsPerTexel` and keeps the
   map 1 texel larger.

**Trade-off:** sphere fitting wastes resolution (a sphere around a frustum slice), but it does not
shimmer. A tight fit is denser but changes size with camera rotation, so it shimmers below 1x
density. TI's claim that density fixes instability is testable as A/B 11.5.

### 5.4 Depth range

- Use the sphere's `[C - R, C + R]` along L, rasterized with `DepthClipEnable = FALSE` (pancaking).
  Casters in front of the near plane clamp to depth 0 and still shadow.
- D16 step = `2R/65536`. For example R = 30 m gives 0.9 mm and R = 200 m (far field) gives 6 mm.
  Both are fine with slope bias.
- Optional tightening, as in MS docs "light frustum intersected with scene": clip the scene AABB to
  the cascade's x/y planes and take its z range.

### 5.5 Caster culling for cascades

1. **Light-space box test**, with the caster bounding sphere `(c, r)` in cascade light space:
   cull if `|x| > R + r` or `|y| > R + r` or `z > 2R + r` (beyond far). There is *no* near test,
   because of pancaking.
2. **Extruded-slice test** (Cyberpunk 2077, Dybisz & Witanowski 2021). The sphere box is much larger
   than the slice. Cyberpunk switched from map-based to interval-based cascade selection precisely
   so that only casters whose shadows can hit *that slice* need drawing.
   - Build a convex volume = slice swept toward the light (up to ~8 planes). The original planes are
     the slice planes; the extra ones go through silhouette edges and are parallel to L:

```cpp
// slice: 8 corners, 6 inward planes, 12 edges {v0, v1, faceA, faceB}; L = direction light travels.
std::vector<Plane> CasterVolume(const Slice& s, XMVECTOR L) {
    std::vector<Plane> out; bool keep[6];
    for (int f = 0; f < 6; ++f) {                  // moving toward the light (-L) keeps you inside
        keep[f] = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&s.planes[f].n), L)) <= 0.0f;
        if (keep[f]) out.push_back(s.planes[f]);
    }
    for (const Edge& e : s.edges) if (keep[e.faceA] != keep[e.faceB]) {   // silhouette edge
        XMVECTOR a = XMLoadFloat3(&s.c[e.v0]), b = XMLoadFloat3(&s.c[e.v1]);
        XMVECTOR n = XMVector3Normalize(XMVector3Cross(b - a, L));
        float d = -XMVectorGetX(XMVector3Dot(n, a));
        if (XMVectorGetX(XMVector3Dot(n, XMLoadFloat3(&s.centre))) + d < 0) { n = -n; d = -d; }
        Plane p; XMStoreFloat3(&p.n, n); p.d = d; out.push_back(p);
    }
    return out;     // cull sphere if dot(n,c)+d < -r for any plane
}
```

   - For a *point* light, keep the planes with `dot(n, P_light) + d >= 0`. Silhouette planes then go
     through edge `(a, b)` and the light position: `n = cross(b - a, P_light - a)`.
3. **Small casters:** cull if `2r < 1-2 texels` of that cascade (`2R/N`). In the far field, this
   removes most props. TI blames KCD2's third cascade (about 5,000 small draws) and Crysis 3's last
   cascades.
4. **Far field:** draw only large static casters, at their lowest LOD. Exclude alpha-tested foliage,
   or draw it as an opaque proxy, until an approximation (5.8) exists.

### 5.6 Cascade selection and blending

**Interval-based selection** (MS docs; Cyberpunk switched to it):

```
c = (viewZ > split0) + (viewZ > split1) + (viewZ > split2)
```

It is cheaper and it allows slice culling. Map-based selection (is the UV inside the cascade?) uses
the maps better, but needs every cascade to contain every caster for its whole box.

**Blend band** (MJP; MS docs), with `fade = (split_c - viewZ)/(split_c - split_{c-1})`:
- If `fade < 0.1`, sample cascade `c+1` too and `lerp(next, cur, smoothstep(0, 0.1, fade))`.
- Use a dynamic branch, so most pixels sample once.
- **Pitfall:** derivative-based features (RPDB, the density view) break when quads straddle a cascade
  boundary. Compute derivatives on cascade-independent coordinates (MS docs) or skip them at
  boundaries.
- **Pitfall:** the PCF kernel and normal offset are in texels, so their world size jumps between
  cascades. The blend band hides it. Castaño also interpolates the bias magnitude across cascades.

### 5.7 Far field and amortized updates

**Sources:**
- **CryEngine** (Kasyan 2013): not all cascades are updated each frame. Distant cascades update less
  often, so more cascades are affordable. Cached cascades are sampled but not redrawn, and the last
  cascade uses VSM.
- **KCD2** (TI slide): cascades 1 and 2 every frame, plus one of 3/4/5 in rotation.
- **Cyberpunk 2077:** "distant shadows" are refreshed over several frames, with a per-frame budget.
  Culling cannot be spread over frames.
- **Insomniac** (Day/Hastings/Acton 2012): "CSM scrolling". Cache the static content, scroll the map
  by whole texels, render only the newly exposed strips, then render dynamic objects on a copy. The
  video argues against copies, so this is an optional later experiment.

**Our scheme:**

```
frame % 3 == 0 : render FF (2048²) in addition to cascades 0-2
store per cascade: ViewProjTex used at render time, sphere (C,R), texel size, frameRendered
lookup always uses the *stored* matrix of the map being sampled
camera cut (|Δpos| > 10 m or |Δyaw| > 45° in one frame, or explicit flag):
    render FF this frame at 1024² (viewport = top-left quarter of the slice, UV scale 0.5),
    next scheduled update returns it to 2048²  (the video's proposal)
```

**Pitfalls:**
- **Stale coverage:** the FF map was fitted to an older camera position. Pad the FF sphere by
  `vmax * 3 frames * dt`, and treat UVs outside [0,1] as lit (or SSS-only).
- **Dynamic casters** in FF lag by up to 2 frames. This is acceptable at 72 m+, and TI also tolerates
  it at distance.
- **Racing speeds:** test explicitly (11.4).

### 5.8 Approximated distance shadowing (later phase)

- **Days Gone:** foliage and trees cast through specialized sprite atlases that hold depth plus other
  data, in a ~0.3 ms pass. The result is projected into the mask. Bend has not published the
  method. TI's frame captures are the only source (Q6).
- **SDF shadows** (UE4-style "distance field shadows"): the video suggests them for far-field fill
  ("native resolution projection"). They are out of scope for the first version.

### 5.9 Published sun-shadow costs

See the table in section 9.

---

## 6. Filtering and biasing

### 6.1 D3D11 states

```cpp
// Comparison sampler (standard Z: lit when receiver <= stored)
D3D11_SAMPLER_DESC s = {};
s.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;    // 2x2 bilinear PCF per tap
s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
s.BorderColor[0] = 1.0f;                                        // outside the map = far = lit
s.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;                 // reverse-Z D32F variant: GREATER_EQUAL
s.MaxLOD = 0.0f;

// Shadow-pass rasterizer
D3D11_RASTERIZER_DESC r = {};
r.FillMode = D3D11_FILL_SOLID;
r.CullMode = D3D11_CULL_BACK;          // TI rejects front-face-culling tricks; floors are flagged instead
r.DepthBias = 0;                       // D16 UNORM: units of 1/65536 of the depth range
r.SlopeScaledDepthBias = 1.5f;         // tune 1.0-2.5 (covers the 3x3 kernel footprint)
r.DepthBiasClamp = 0.0f;               // or e.g. 0.005 to stop huge bias on grazing triangles
r.DepthClipEnable = FALSE;             // pancaking (CSM and local lights)
r.ScissorEnable = TRUE;                // atlas tiles
// Depth-stencil: DepthEnable TRUE, WriteMask ALL, Func LESS; no pixel shader for opaque casters.
```

**D3D11 bias formula** (MS docs "Depth Bias"):
- UNORM: `Bias = DepthBias*r + SlopeScaledDepthBias*MaxDepthSlope`, where `r` = the smallest
  representable value > 0 (1/2^16 for D16).
- Float: `Bias = DepthBias*2^(exponent(max z in primitive) - 23) + SlopeScaledDepthBias*MaxDepthSlope`.
- The clamp then applies (`DepthBiasClamp`), and bias is added after clipping.
- In **reverse-Z**, all three bias values must be **negative** to push casters away from the light.

### 6.2 Optimized PCF (Castaño, The Witness, 2013; also in MJP's sample)

**Principle.** A separable kernel is built from **bilinear PCF taps** (each `SampleCmpLevelZero`
with a LINEAR comparison filter returns a bilinearly weighted 2x2 comparison). Solve for tap offsets
and weights so the sum reproduces the target kernel.

Castaño reports this is 10-20% faster than a GatherCmp approach on a GTX 480 and an Intel HD 4000.
It also needs no Gather.

**The two sizes we use** (per axis, `s` = sub-texel fraction):

| Kernel | Taps | Footprint | Per-axis weights `w` | Per-axis offsets `o` (texels from `base`) | Normalizer |
|---|---|---|---|---|---|
| "3x3" (tent) | 2x2 = 4 | 4x4 texels | `3-2s`, `1+2s` | `(2-s)/w0 - 1`, `s/w1 + 1` | 1/16 |
| "5x5" (~Gaussian 1,3,4) | 3x3 = 9 | 6x6 texels | `4-3s`, `7`, `1+3s` | `(3-2s)/w0 - 2`, `(3+s)/7`, `s/w2 + 2` | 1/144 |

```hlsl
// uvz.xy in [0,1] of the WHOLE texture (atlas or array slice), uvz.z = biased receiver depth.
float PCF3x3(Texture2D<float> map, SamplerComparisonState cmp, float3 uvz, float2 size, float2 invSize)
{
    float2 tc   = uvz.xy * size;                  // texel space
    float2 base = floor(tc + 0.5);
    float2 s    = tc + 0.5 - base;                // sub-texel position in [0,1)
    base        = (base - 0.5) * invSize;

    float2 w0 = 3.0 - 2.0 * s,  w1 = 1.0 + 2.0 * s;
    float2 o0 = (2.0 - s) / w0 - 1.0,  o1 = s / w1 + 1.0;

    float sum = w0.x * w0.y * map.SampleCmpLevelZero(cmp, base + float2(o0.x, o0.y) * invSize, uvz.z)
              + w1.x * w0.y * map.SampleCmpLevelZero(cmp, base + float2(o1.x, o0.y) * invSize, uvz.z)
              + w0.x * w1.y * map.SampleCmpLevelZero(cmp, base + float2(o0.x, o1.y) * invSize, uvz.z)
              + w1.x * w1.y * map.SampleCmpLevelZero(cmp, base + float2(o1.x, o1.y) * invSize, uvz.z);
    return sum * (1.0 / 16.0);
}
// Texture2DArray version: identical, with float3(uv, slice) coordinates.
```

**Recommendation for "barely softened" shadows: the 4-tap "3x3".**
- Its penumbra is about 2-3 texels, which at ≥ 1.08 texels/px is 2-3 screen pixels. That is just
  enough to anti-alias the projected edge, which is the shadow-map anti-aliasing the video asks for.
- A larger kernel needs proportionally more bias, so more Peter-panning and leaking. That is TI's
  argument against softening.
- The 9-tap "5x5" is the A/B option (and a candidate for the far field only).
- MJP's measurements: going from 2x2 to 7x7 PCF costs about +0.4 ms (AMD HD 7950, 1920x1080, 4
  cascades).
- Kasyan's randomized Poisson PCF is noisy, which the video rejects ("noiseless").

### 6.3 Bias toolbox

1. **Raster slope-scaled bias:** see 6.1. It is the cheapest way to cover the texel-quantization
   error. Kasyan uses it for the sun, with front faces.
2. **Receiver normal offset** (Holbert, GDC 2011 "normal offset shadows"; MJP, Castaño): move the
   *receiver* position along its normal by a world distance that scales with the shadow texel size
   at that point, before projecting.

   ```hlsl
   float  NdotL      = saturate(dot(N, Ldir));                  // Ldir = towards the light
   float  texelWorld = isSpot ? delta * zLight : cascadeTexelWorld[c];   // 2R/N for cascades
   float3 Pbiased    = P + N * (kNormalTexels * texelWorld * (1.0 - NdotL));  // kNormalTexels ~ 1.0-1.5
   ```

   Castaño's variant scales by `sin(acos(NdotL))` for the normal offset and by `min(2, tan)` for a
   slope term.
   **Pitfall:** G-buffer normals are *shading* normals, normal-mapped. Offsetting along them adds
   noise. Prefer a geometric normal: an extra encoded G-buffer channel, or `cross(ddx(P), ddy(P))`
   in the pixel-frequency mask pass (unreliable at MSAA edges and depth discontinuities).
3. **Receiver plane depth bias** (Isidoro, GDC 2006; MJP). Use screen derivatives of
   `(u, v, z)` to get `dz/du` and `dz/dv`, and offset the compare depth per PCF tap by
   `dot(tapOffsetUV, dzduv)`:

   ```hlsl
   float2 RPDB(float3 dx, float3 dy) {          // dx = ddx(uvz), dy = ddy(uvz)
       float det = dx.x * dy.y - dx.y * dy.x;
       return float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;
   }
   // per tap: z_tap = z + dot(offsetUV, rpdb); plus a static term ~ 2*dot(texelSize, abs(rpdb)), clamped 0.01
   ```

   It is accurate on planes, but it fails at quad boundaries (cascade/tile seams, MSAA edges) and in
   degenerate cases. Castaño disabled it for bilinear-tap PCF because each tap's 2x2 footprint
   cannot be biased. **Off by default, A/B only.**
4. **Constant bias:** keep it near zero. With perspective D16 it is non-uniform in world space.

**Starting values:** SlopeScaled 1.5, DepthBias 0, kNormalTexels 1.0.
- Tune on a test scene with 2 m and 20 m casters, grazing light angles and thin walls.
- MJP's starting point for comparison: manual depth bias ≈ 0.0015 (normalized), offset scale 1-2.
- **Geometry rules** (MS docs): give thin walls back faces; avoid single-sided casters.

### 6.4 Formats, reverse-Z and MSAA shadow maps

- **D16** is the default, as the video suggests (guaranteed density + tight near/far). It uses
  half the memory and half the PCF bandwidth of D32.
- **D32_FLOAT** is the A/B. With reverse-Z it suits perspective (spot) maps. It gives no gain for
  orthographic cascades, where z is linear.
- **MSAA shadow maps with programmable sample positions:** core D3D11 has no programmable sample
  positions (NVAPI only). That is excluded by "no vendor SDKs", so this is out of scope (Q7).
  - The "bias free shadow mapping" Reddit post the video cites could not be located.
  - A related academic line: Dou et al. 2014 adaptive depth bias, and a WSCG 2015 follow-up that
    extends it to PCF and PCSS.

---

## 7. The screen-space shadow mask with 2x MSAA, and screen-space shadows

### 7.1 What CryEngine / Crysis 3 do

- **Kasyan 2013:** the sun gets a "shadow mask" render target that accumulates several techniques
  before shading: cascades, VSM far cascade, per-object shadows and cloud shadows. Per-object maps
  are combined with `max()`. Point-light shadows go *directly* into the light buffer.
  - Deferred shadow passes tag receiving areas in stencil by rendering cascade frustum volumes, and
    pick the highest-resolution cascade where they overlap.
- **Crytek, CryENGINE 3 Graphics Gems, SIGGRAPH 2013:**
  - Deferred MSAA uses a custom per-sample mask built while pre-resolving sample 0. Similar samples
    are compared, instead of using `SV_Coverage`, which over-marks.
  - It is stored as a reserved stencil bit (0x80), so pixel-frequency and sample-frequency passes
    are separated by stencil read masks.
  - **Performance shortcut:** deferred cascaded sun shadows are rendered normally at pixel
    frequency, then bilaterally upscaled to the samples during the deferred-shading composite pass.
  - That is the "sample-frequency shadow masking" the video refers to.
- **TI's Crysis 3 analysis:** the alpha channel is cleared with a quad, cascade bounds are written to
  the non-MSAA stencil, and cascades plus cloud shadows are projected per stencil region into that
  channel.

### 7.2 Our mask pass (pixel frequency, non-MSAA)

**Pass setup:**
- Full-screen triangle.
- RTV `R8G8_UNORM` 1920x1080. The chart says "RG8_UINT", but a UNORM channel is what the lighting
  shader filters and samples.
- Optional stencil skip of sky pixels, which needs a non-MSAA stencil copy. Or branch on depth =
  far.

```hlsl
Texture2DMS<float>       DepthMS    : register(t0);   // main depth, sample 0 used
Texture2DMS<float4>      NormalMS   : register(t1);   // G-buffer normal (decode as the engine does)
Texture2DArray<float>    Cascades   : register(t2);   // R16_UNORM SRV
Texture2D<float>         FarField   : register(t3);
Texture2D<float>         SSS        : register(t4);   // Bend SSS result (R8)
SamplerComparisonState   CmpSampler : register(s0);

float2 SunMaskPS(float4 pos : SV_Position) : SV_Target
{
    int2  pix = int2(pos.xy);
    float z   = DepthMS.Load(pix, 0);
    if (z == FAR_DEPTH) return float2(1, 1);
    float3 P  = ReconstructWorldPos(pix, z, DepthMS.GetSamplePosition(0)); // sample-0 offset (+0.25,+0.25 px in std 2x)
    float3 N  = DecodeNormal(NormalMS.Load(pix, 0));
    float  sh = SunShadow(P, N);                       // cascade select + blend band + PCF3x3 + normal offset
    sh = min(sh, SSS.Load(int3(pix, 0)));              // contact detail (Days Gone combines the same way)
    return float2(sh, 1.0);                            // G: reserved (subsurface / transmission thickness, Q8)
}
```

`SunShadow` follows 5.6 with `PCF3x3` (6.2) and the normal offset (6.3).
- Skip the shadow work when `NdotL <= 0` for opaque Lit pixels. The light is zero anyway.
- Keep the work for foliage and subsurface models, which use back-lighting.

### 7.3 Using the mask in the sun lighting draws (pixel vs sample frequency)

The lighting draws run per shading model, with stencil: non-edge pixels at pixel frequency, edge
pixels at sample frequency (`SV_SampleIndex`).
- **Non-edge pixels:** `shadow = Mask.Load(pix).r`. This is exact, because all samples share one
  surface.
- **Edge pixels, sample 0:** `Mask.Load(pix).r`. This is exact.
- **Edge pixels, sample s > 0** (Crysis 3's bilateral idea, my concrete form):

```hlsl
float SunShadowSample(int2 pix, uint s, float viewZs, float3 Ps, float3 Ns)
{
    if (s == 0) return Mask.Load(int3(pix, 0)).r;
    float best = 1e30, val = 1.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        int2  q  = pix + int2(x, y);
        float dz = abs(ViewZ0.Load(int3(q, 0)) - viewZs);   // linear depth of each pixel's sample 0
        if (dz < best) { best = dz; val = Mask.Load(int3(q, 0)).r; }
    }
    [branch] if (best > 0.02 * viewZs)                       // no neighbour lies on this sample's surface
        val = SunShadow(Ps, Ns);                             // inline PCF fallback (rare)
    return val;
}
```

- `ViewZ0` is a non-MSAA linear-depth copy of sample 0. It is usually produced anyway for SSAO and
  SSS. If it is missing, linearize `DepthMS.Load(q, 0)`.
- **A/B "inline-on-edges":** skip the neighbour search and always call `SunShadow` for s > 0. Edge
  pixels are a few percent of the screen, so this may be just as cheap and is simpler.
- **Optional stencil trick** from the chart: after the mask pass, a full-screen pass sets a stencil
  bit where `mask.r == 0` and the shading model has no transmission. Sun-lighting draws then skip
  those pixels. A/B it, since the cost of the extra pass must be won back.

### 7.4 Local lights: inline, no mask

This follows the chart. The light's stenciled full-screen draws (per shading model, pixel or sample
frequency) call `PCF3x3` on the atlas directly:

```hlsl
float LocalShadow(LightShadow L, float3 P, float3 N, float3 toLight)
{
    float  zL  = dot(P - L.Pos, L.Axis);                                 // light view depth (per face for point lights)
    float  NdL = saturate(dot(N, toLight));
    float3 Pb  = P + N * (L.NormalOffsetTexels * L.Delta * zL * (1.0 - NdL));
    float4 h   = mul(float4(Pb, 1.0), L.WorldToAtlas);
    return PCF3x3(Atlas, CmpSampler, h.xyz / h.w, AtlasSize, InvAtlasSize);
}
```

At MSAA edges, this runs per sample automatically, because the light draw is already at sample
frequency there.
- This matches the video's goal of avoiding separate (slow) shadow projection passes.
- The A/B is a per-light mask pass (11.7).

### 7.5 Screen-space shadows: Bend Studio SSS (Aldridge, 2023, Apache 2.0)

**Algorithm** (from the slides and the shipped headers `bend_sss_cpu.h` / `bend_sss_gpu.h`):
- Rays from all pixels toward the projected light position converge in screen space. Shifting each
  ray origin within its pixel makes neighbouring rays share samples exactly.
- Work is mapped to **1D 64-thread groups along the ray**. Each thread does one manual, edge-aware
  bilinear depth fetch into groupshared memory, then iterates the shared samples. That is about 2
  instructions per sample, under 5 instructions per sample overall, and about 300 instructions in
  total for 60 samples.
- The output is noiseless and temporally stable. It uses no dithering and no TAA.
- The CPU side (`BuildDispatchList`) builds 1-8 dispatches per light: 1-2 when the light is off
  screen, 4-6 when it is on screen.
- Inputs: the light's homogeneous position, `(dir, 0)` for the sun or `(pos, 1)` for local lights,
  times ViewProj, without the w divide.

**Parameters and published defaults:**
- `SAMPLE_COUNT` 60 (the shadow length in pixels). `WAVE_SIZE` 64. `HARD_SHADOW_SAMPLES` 4.
  `FADE_OUT_SAMPLES` 8.
- `SurfaceThickness` 0.005, `BilinearThreshold` 0.02, `ShadowContrast` 2-4.
- `FarDepthValue` 0 and `NearDepthValue` 1 for reverse-Z.
- Sampler: point, clamp-to-border, with border = far depth.

**Cost:** **0.19 ms** on PS5 at 1440p, 60 samples, full-screen coverage, with wave32 scheduler and
occupancy tweaks.

**Role:**
- Bend's slides: SSS cannot replace shadow maps (disocclusion), only complement them.
- Days Gone added a ±8 cm per-material "depth bias" micro-displacement (12 bits of G-buffer) to the
  depth used by SSS, AO and decals. It is out of scope here; the micro-detail topic is another note.

**D3D11 / FXC port notes:**
- The code targets DXC. Its early-out path uses `WaveActiveAnyTrue` / `WaveGetLaneCount` (SM6).
  Delete it or set `UseEarlyOut = false`.
- `DispatchParameters` contains resource members. Move `DepthTexture`, `OutputTexture` and
  `PointBorderSampler` to globals for FXC `cs_5_0`.
- Groupshared memory and barriers are SM5-legal.
- The input is the non-MSAA sample-0 depth copy. The output is an R8_UNORM UAV. Typed UAV stores
  of R8_UNORM are supported at FL11_0, but check `D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW`.

### 7.6 Optional: Days Gone-style 8x min/max RG16 early-out

- A compute shader reduces each cascade to `R16G16_UNORM` at N/8, holding the min and max depth per
  8x8 block.
- In the mask pass, `Gather` the 2x2 blocks around the PCF footprint, which covers ≤ 4x4 texels.
  - If the biased receiver z ≤ min of the mins, it is fully lit: skip PCF.
  - If it is > max of the maxes, it is fully shadowed.
  - Otherwise run PCF.
- TI marks it as needing tests. It is an A/B in phase 3.

---

## 8. Shadow-pass draw hygiene

- **LOD selection follows the main camera**, not the light. The shadow then matches the visible
  mesh and there is no self-shadow mismatch.
  - During a cross-fade, draw both LODs into the shadow map with complementary dither masks.
  - Use the same ALU dither as the main view, indexed by shadow-map pixel (`SV_Position.xy`).
  - Only those draws get a pixel shader with `clip()`, which costs early-z only during the fade.
  - The dither function is defined in the LOD/prepass note. Standard 4x4 Bayer value:
    `((x^y)&1)<<3 | (y&1)<<2 | ((x^y)&2) | ((y&2)>>1)`, divided by 16.
- **Alpha-tested casters:** separate draws, with a PS that samples only an opacity texture (BC4). TI
  says sampling RGBA albedo just for alpha slows depth passes.
  - In the far field, exclude them or use proxies.
  - Crysis 3's and KCD2's far-cascade costs come mostly from such content.
- **No floors in shadow maps:** add a per-mesh flag, plus a validation view that tints
  shadow-casting floors.
- **No pixel shader** for opaque casters: depth-only.

---

## 9. Published performance numbers

| What | Number | Hardware / resolution | Source |
|---|---|---|---|
| Crysis 3: 6 sun cascades (32-bit) | < 1.3 ms ("~1.2 ms" in the tutorial) | RTX 3060 12 GB, 1080p | TI Crysis 3 analysis (`ElBUUMi_L5c`, 0:34); tutorial 46:10 |
| Days Gone: sun CSM | ~600 draws, 1152² cascades; 4 cascades incl. 2k far field | RTX 3060, 1080p | TI Days Gone (`2IeYOECebTA`, 7:53); TI Crysis 3 video (10:18); chart |
| Days Gone: foliage shadow atlas pass | ~0.3 ms | (TI chart) | tutorial chart, 46:40 |
| KCD2: 3 cascades ~1600² (captured), ~5,000 draws | all shadow passes 4.6 ms; cascade 1 ≈ 0.5 ms (0.4 foliage), cascade 2 ≈ 0.5 ms | RTX 3060, 1080p | TI KCD2 analysis (`SYUL1tt_olk`, 7:08-8:40) |
| KCD2: cloud shadows on the mask | ~0.30 ms | (TI chart) | chart, 46:40 |
| MGSV (Fox Engine): 16 local shadow maps at 4096² D24S8 | about 1 ms; per-light clears were the biggest cost; +0.24 ms from two alpha-tested trees | TI capture PC (hardware not restated in that segment) | TI Fox Engine analysis (`aB5qxp6SPPQ`, 9:50-10:51) |
| NFS 2015 (Frostbite): 24 shadow maps at 512² | 1.95 ms | RTX 3060, 1080p | TI NFS 2015 analysis (`Te9xUNuR-U0`, ~9:00) |
| Video's targets | local shadow maps 1.00 ms; sun 0.60 + 0.32 + 0.18 + 0.36 ms; lighting of non-cascaded lights 1.50 ms | (target) | tutorial chart |
| PCF kernel size | 2x2 → 7x7 ≈ +0.4 ms | AMD HD 7950, 1920x1080, 4 cascades | MJP, "A Sampling of Shadow Techniques" (2013) |
| EVSM4 (2048², 4xMSAA, mips, 8xAF) | ≈ +11.5 ms vs 7x7 PCF | (same blog) | MJP 2013 |
| Optimized bilinear-tap PCF | 10-20% faster than GatherCmp | GTX 480, Intel HD 4000 | Castaño 2013 |
| Bend SSS (60 samples, full screen) | 0.19 ms ± 0.01 | PS5, 1440p | Bend Studio SIGGRAPH 2023 slides |
| CryEngine (2013): typical shadow budget | ~5-7 ms of a 33 ms frame; point-light atlas 1024² (4 MB) on consoles | X360/PS3-era consoles | Kasyan 2013 |
| PSM 2002: depth read-back for the near plane | 10 ms (640x480, 16-bit) | dual P3 1 GHz, GeForce3 | Stamminger & Drettakis 2002 |
| LiSPSM 2004 | same speed as standard SM (≥ 75 fps) | P4 2.4 GHz, FX 5900, 1024² map | Wimmer et al. 2004 |
| TSM 2004 | ~28 fps, 100k triangles, 1024²; <10 fps at 1.4M triangles, 2048² | P4 1.8 GHz, FX 5900 Ultra | Martin & Tan 2004 |
| PSSM(3)/(4), 1K maps | >31 / 27 fps | Athlon64 X2 3800, 7800 GTX, 1280x1024 | Zhang et al., GPU Gems 3 |
| Cyberpunk 2077 | up to 4 cascades + 4 distant levels (each 4x larger; last 256 km²) refreshed over several frames; 8-plane caster volume | PS4/PC era | Dybisz & Witanowski 2021 |

---

## 10. Recommended implementation (phased)

**Phase 1: baseline that can measure everything**
1. Atlas 8192² D16 with a per-frame full clear, shelf packer, spot lights only.
   - Start with **fixed** per-light resolution (1024²) and the **unfitted** full-cone frustum. This is
     the "brute force" baseline the video criticises.
   - Then add focusing (4.2-4.3) and density-based sizing (2.4) behind runtime toggles.
2. Inline `PCF3x3` in the light draws, plus slope bias and normal offset. Add the density heat-map
   (2.6) and the debug views:
   - tile rectangles in the atlas,
   - B outline in world space,
   - texel/pixel ratio,
   - "atlas texels written vs texels sampled" (11.2).
3. Sun: 3 stabilized cascades at 1296² D16 (UE-exponent splits, 72 m), interval selection, a 10%
   blend band, and the R8G8 mask pass. Sample-frequency reuse (7.3) with a toggle for
   inline-on-edges.
4. Caster culling: light-space box test for cascades, fitted frustum for spots, small-caster cull,
   and the `castsShadow` flag.
5. GPU timestamps around each shadow-map render (per cascade, per light), the mask pass and the
   lighting.

**Phase 2: the video's remaining asks**
6. Point lights as 6 projectors, distance fade, and Bend SSS for the sun.
7. Far-field 2048² cascade with every-3rd-frame updates and camera-cut handling. Extruded-slice
   culling (5.5).
8. Dithered shadow LOD transitions and alpha-tested caster draws with BC4 opacity.

**Phase 3: experiments**
9. Min/max RG16 early-out. The fully-shadowed stencil skip. LiSPSM on cascade 0.
10. D32F reverse-Z vs D16. Tight-fit cascades. Per-light SSS for distant lights. Foliage shadow
    atlas or SDF far shadows.

---

## 11. A/B test plan and how to measure

**Tools:**
- **GPU time:** `D3D11_QUERY_TIMESTAMP_DISJOINT` + `D3D11_QUERY_TIMESTAMP` pairs around every shadow
  render and pass. Read back 3 frames later. Report mean and 99th percentile over 600 frames.
- **"Depth surface area":** `D3D11_QUERY_OCCLUSION` around each shadow-map render. It counts depth
  samples that pass, which is a proxy for rasterized texels. Depth-only draws have no PS
  invocations, so this replaces `PSInvocations`.
- **Geometry:** `D3D11_QUERY_PIPELINE_STATISTICS` (`IAPrimitives`, `CPrimitives`), plus CPU counts of
  draws per shadow view.
- **Utilization:** a debug UAV `RWTexture2D<uint>` at 1/4 atlas resolution. In the lighting shaders,
  mark the texel block each PCF footprint touches. Compute **texels sampled / texels rendered** per
  tile. This directly measures the video's complaint about depth rendered where the light can never be evaluated.
- **Density:** the % of shadowed-receiver pixels with ≥ 1.08 texels/ref-pixel (2.6). Report the
  median ratio too.
- **Quality:**
  - Image difference against a reference (each spot at 8192² with a fitted window, cascades replaced
    by a single 16384² map for a small scene). Count pixels with |Δshadow| > 0.1.
  - Temporal flicker: mean |Δ| of the shadow term between consecutive frames, on a slow camera
    dolly with static lights.

**Scenes** (fixed camera paths, recorded inputs):
- S1: 1 spot far away (small on screen).
- S2: camera inside a spot cone, close to a lit wall.
- S3: spot behind the camera lighting the view (PSM's hard case).
- S4: spot facing the camera (dueling).
- S5: corridor with 24 shadowed spots/points (the video's "15-30 shadow maps").
- S6: open terrain with the sun at noon and at a 10° elevation, a 120 km/h fly-through, and a
  hard camera cut.

| # | Comparison | Expectation to confirm or refute |
|---|---|---|
| 11.1 | Fixed 1024² (and 2048²) per light **vs** density-sized (2.4), both fitted | Similar or better density with far fewer texels for distant lights, and capped cost for close ones. Report the GPU ms of all local shadow maps vs the 1.00 ms target. |
| 11.2 | Unfitted full-cone frustum **vs** fitted window (same texel angle) | Large reduction in rendered texels and draws when only part of the cone is visible. Utilization ratio up. Quality equal. |
| 11.3 | Fitted window **with vs without** texel snapping and hysteresis | Flicker metric on S1/S2 dolly paths. |
| 11.4 | Sun: 3 cascades every frame **vs** + FF every 3rd frame **vs** KCD2-style (0,1 every frame + {2, FF} round-robin) | Mean and peak sun-shadow ms vs the 0.60 ms target. Visible lag at 120 km/h. Behaviour on the camera cut (with and without the 1024² same-frame FF). |
| 11.5 | Sphere-stabilized **vs** tight-fit cascades (texel-snapped translation only) | Density heat-map vs flicker under camera rotation. Tests TI's claim that density fixes instability. |
| 11.6 | Cascades 1296² **vs** 2048²; λ / exponent sweeps; optional LiSPSM on cascade 0 | % pixels meeting 1.08; ms. |
| 11.7 | Sun mask + reuse **vs** inline PCF in the sun lighting draws; for local lights, inline (default) **vs** a per-light mask pass | ms of mask + lighting combined. Edge-sample error vs the per-sample inline reference. |
| 11.8 | Edge samples: bilateral reuse **vs** always-inline | ms; image difference at MSAA edges. |
| 11.9 | PCF 3x3 (4 taps) **vs** 5x5 (9 taps); RPDB on/off; kNormalTexels sweep | ms; acne and Peter-panning counts on the bias test scene; leak test (thin walls). |
| 11.10 | D16 **vs** D32F reverse-Z (spots), D16 vs D32F (cascades) | ms (render + lighting); acne at minimum bias. |
| 11.11 | Culling: none **vs** light-space box **vs** extruded slice **vs** + small-caster cull; floors flagged vs not | Draws, `CPrimitives`, occlusion-query samples, ms per cascade. |
| 11.12 | Min/max RG16 early-out on/off; fully-shadowed stencil skip on/off | Net ms of the mask and sun lighting. |

---

## 12. Open questions

- **Q1.** Under the literal definition (2.5), the "8% higher density" claim for ~1300² cascades is
  geometrically impossible across full-frustum cascades. Which measure did TI use for Days Gone?
  Measure with the heat-map and decide the cascade count and resolution from data.
- **Q2.** Days Gone's actual split ratios are unpublished. Is UE's exponent 3 a reasonable proxy?
- **Q3.** Does `delta` quantization with quarter-octave levels + hysteresis keep spots stable enough,
  or are coarser (power-of-two) levels needed?
- **Q4.** Atlas budget: is 8192² D16 (128 MiB) right for 15-30 shadowed lights, plus up to 6
  projectors per point light, on 8 GB? What should the coarsening policy be on overflow?
- **Q5.** On the RTX 5060, which is cheaper: the mask + bilateral reuse, or inline PCF everywhere,
  given the ≤ 1.5 ms lighting budget?
- **Q6.** Approximated far shadowing: the Days Gone foliage atlas details are unpublished. Should SDF
  far shadows be attempted in D3D11 (3D textures plus a compute march)?
- **Q7.** Programmable sample positions and MSAA shadow maps cannot be done in core D3D11. Is a
  non-PSL MSAA shadow-map experiment (D16 2x MSAA, resolve by min) worth anything?
- **Q8.** What should mask channel G hold for subsurface (transmission thickness from the
  shadow-map depth difference)? This needs the SSS/skin note.
- **Q9.** Where does the geometric normal for the normal offset come from: an extra G-buffer channel,
  or derivatives?

---

## Sources

**Threat Interactive (video frames and auto-captions, fetched with yt-dlp):**
- Tutorial `SnNm7rSSvlg` (2026-08-01), https://www.youtube.com/watch?v=SnNm7rSSvlg: transcript
  39:00-47:40 and 34:40. Chart frames at 39:05, 40:20, 41:05, 44:40, 46:02 and 46:40 (local-shadow
  box, PSM downsides slide with the 1080p+8% definition, lighting box, KCD2 cascade schedule, sun
  row with budgets) and 47:24 (marketing example). Pinned comment: the chart is Patreon-only.
- "When Sony Made Optimized Realistic Graphics By Fixing UE4" (Days Gone),
  https://www.youtube.com/watch?v=2IeYOECebTA: 1152² cascades, ~600 draws, foliage sprite atlases,
  SSS, RTX 3060 at 1080p.
- "The Top 10 Most Neglected Areas in Modern Graphics" (Video 19),
  https://www.youtube.com/watch?v=N6sLJhI_Fmg: Days Gone 576²/1152² cascades, "~30% more squared
  resolution", density reasoning.
- "When Butchered Rasterization Leads To Raytracing" (Video 21),
  https://www.youtube.com/watch?v=PhEIa5RtKVA: the "8% higher density than 1080p" phrasing; scale the
  zoom/FOV with the main camera.
- "Understanding Crysis 3's Rendering…", https://www.youtube.com/watch?v=ElBUUMi_L5c: 6x 32-bit
  cascades < 1.3 ms; quad atlas clear; stencil-projected mask; Days Gone uses 4 cascades.
- "What Modern CryEngine Does To Your GPU" (KCD2), https://www.youtube.com/watch?v=SYUL1tt_olk:
  3x ~1600² cascades, 5,000 draws, 4.6 ms.
- "Took For Granted: Why Fox Engine Is So Crazy Optimized",
  https://www.youtube.com/watch?v=aB5qxp6SPPQ: 16x 4096² local maps in ~1 ms, face culling excludes
  floors.
- NFS 2015 analysis, https://www.youtube.com/watch?v=Te9xUNuR-U0: 24x 512² in 1.95 ms.

**Papers and talks:**
- Stamminger & Drettakis, "Perspective Shadow Maps", SIGGRAPH 2002,
  https://www.cse.msu.edu/~cse872/papers_files/PerspectiveShadowMaps.pdf: aliasing formula, light
  type mapping, caster hull H, virtual camera shift, read-back cost.
- Kozlov, "Perspective Shadow Maps: Care and Feeding", GPU Gems ch. 14 (2004),
  https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-14-perspective-shadow-maps-care-and-feeding:
  inverse projection, unit-cube clipping, world-space bias, cube maps.
- Wimmer, Scherzer & Purgathofer, "Light Space Perspective Shadow Maps", EGSR 2004 (rev. 2005),
  https://www.cg.tuwien.ac.at/research/vr/lispsm/shadows_egsr2004_revised.pdf: body B focusing,
  light-space frame, n_opt and its gamma adjustment, point-light remarks, performance.
- Martin & Tan, "Anti-aliasing and Continuity with Trapezoidal Shadow Maps", EGSR 2004,
  https://www.comp.nus.edu.sg/~tants/tsm/tsm.pdf (overview at
  https://www.comp.nus.edu.sg/~tants/tsm.html): trapezoid, 80% rule, z handling, results.
- Zhang et al., "Parallel-Split Shadow Maps on Programmable GPUs", GPU Gems 3 ch. 10,
  https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus:
  practical split scheme, crop matrix, caster/receiver sets, results.
- Microsoft, "Common Techniques to Improve Shadow Depth Maps",
  https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps:
  tight fit, near/far from scene clipping, texel-increment snapping, face-culling discussion,
  PSM/LiSPSM vs CSM.
- Microsoft, "Cascaded Shadow Maps",
  https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps: interval vs
  map-based selection, fit to cascade vs scene, blend band, derivative-based bias, padding.
- Microsoft, "Depth Bias",
  https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias:
  exact UNORM and float bias formulas and clamp.
- MJP, "A Sampling of Shadow Techniques" (2013), https://therealmjp.github.io/posts/shadow-maps/,
  and the Shadows sample (MIT), https://github.com/TheRealMJP/Shadows (`Mesh.hlsl`,
  `MeshRenderer.cpp`): stabilization and snapping code, PSSM code, normal offset, RPDB, optimized
  PCF, blend band, depth-clip-off, numbers.
- Castaño, "Shadow Mapping Summary – Part 1" (2013), https://www.ludicon.com/castano/blog/?p=901:
  The Witness PCF derivation, kernel constants, bias combination, performance vs Gather.
- Kasyan, "Playing with Real-Time Shadows" (CryEngine, SIGGRAPH 2013 course),
  http://www.realtimeshadows.com/sites/default/files/Playing%20with%20Real-Time%20Shadows_0.pdf:
  sun shadow mask, deferred stencil-tagged shadow passes, amortized and cached cascades, 6 scaled
  projectors per point light in an atlas, bias practices, budgets.
- Sousa, Kasyan & Schulz, "CryENGINE 3: Graphics Gems" (SIGGRAPH 2013),
  https://advances.realtimerendering.com/s2013/Sousa_Graphics_Gems_CryENGINE3.pptx: deferred MSAA
  per-sample mask and stencil bit; shadows at pixel frequency, bilateral upscale in the composite.
- Dybisz & Witanowski, "Shadows Optimizations in Cyberpunk 2077" (SIGGRAPH 2021 talk),
  https://history.siggraph.org/wp-content/uploads/2022/06/2021-Talks-Dybisz_Shadows-Optimizations-in-Cyberpunk-2077.pdf:
  interval selection for culling, 8-plane extruded caster volume, distant shadows over several
  frames, local-shadow throttling.
- Day, Hastings & Acton (Insomniac), "CSM Scrolling" (SIGGRAPH 2012),
  https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf:
  cached and scrolled cascades.
- Bend Studio, "Inside Bend: Screen Space Shadows",
  https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/. Code:
  https://www.bendstudio.com/assets/cms/downloads/code_final_candidate.zip (Apache 2.0). Slides:
  https://www.bendstudio.com/assets/cms/downloads/screen_space_shadows.zip. Algorithm, parameters,
  DX11 caveats, 0.19 ms on PS5 at 1440p, Days Gone "depth bias".
- AMD GPUOpen, "RDNA Performance Guide", https://gpuopen.com/learn/rdna-performance-guide/: depth
  format costs, D16 compression caveat, fast clear rules.
- Flax Engine docs, "Shadows", https://docs.flaxengine.com/manual/graphics/lighting/shadows.html:
  another engine sizing atlas slots from the light's screen size; a point light uses 6 slots.
- Courrèges, "DOOM (2016) – Graphics Study",
  https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/: a shipped 8k x 8k
  shadow atlas with distance-based tile sizes and cached static maps.

**Referenced, not fetched in full** (cited through the sources above):
- Valient, "Stable Rendering of Cascaded Shadow Maps", ShaderX6 (2008).
- Isidoro, "Shadow Mapping: GPU-based Tips and Techniques", GDC 2006 (receiver plane depth bias).
- Holbert, "Normal Offset Shadows", GDC 2011 poster.
- Lauritzen et al., "Sample Distribution Shadow Maps", I3D 2011.
- Brabec, Annen & Seidel, dual-paraboloid shadow maps, 2002.
- Dou et al., adaptive depth bias, 2014.
- The "Bias Free Shadow Mapping" Reddit post the video cites could not be located.
