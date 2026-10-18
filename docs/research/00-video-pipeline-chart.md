# 00 - Video pipeline chart: full transcription and budget extraction

Source: Threat Interactive, "Tutorial: How To Optimize Almost Every Step In Modern Game Rendering"
(YouTube `SnNm7rSSvlg`, 2h01m). The chart file itself is Patreon-exclusive; everything below is read
off the video frames (2560x1440), zoomed and upscaled where needed.

**Citation conventions**

- `f_NNNN` is a 15 s keyframe in `scratchpad/yt/frames/`, at time (N-1)*15 s.
- `t_SSSSS` is an extra frame pulled with ffmpeg at second SSSSS (`scratchpad/yt/extra/*/`).
- `[mm:ss]` / `[h:mm:ss]` are video timestamps.
- "[illegible]" means the text was too small or blurred to read even at 4-5x upscale. "[cut off]" means it
  ran off-screen in every frame we checked.
- The chart's own typos are kept as written and marked `[sic]`. Common ones: "retrospective" for
  "respective", "D32S28" for D32S8, "RGBA6" for RGBA16, and "Crisis 3" for Crysis 3.

**Chart legend** (from the narration at [3:30]-[6:00] and [13:50])

- Top layer: "Engine Features & Workflows (General Studio Access)". Grey note boxes hang off a green
  dashed line and connect to a pass with a green line.
- Middle layer: "Rendering Pipeline (Graphic API Engineers)". A blue 3D box is one pass. The lower-right
  number is the **maximum** cost studios should aim for. Light-grey boxes inside are steps.
  - **Red/maroon** step boxes mean "more custom in-engine tests are needed" (tentative).
  - A **grey** pass boundary means the pass could move or be removed after more testing.
- Bottom layer: "Reasoning Data (Viewer)".
  - "Learned from:" boxes use **green text** for positive attributes taken from a game and
    **red/pink text** for issues being avoided. White text is a neutral note.
  - Yellow dashed **"???"** tags mean the authors have thoughts to share but need to make a related video
    first, or that more custom in-engine research is needed.
- The chart flows left to right.

---

## 1. Frame-budget / assumption slides

### 1.1 "Diagram assumes 1080p 60fps / desktop RTX 3060" ([3:21], `extra/t_201.jpg`; also [3:05]-[3:25])

- Header text, verbatim: **"Diagram assumes 1080p 60fps / desktop RTX 3060"**.
- The slide shows thumbnails of four earlier TI videos that define the standards:
  1. "Fake Optimization in Modern Graphics (And How We Hope To Save It)" defines "2-Frame AA / AA
     Independent Graphics".
  2. "Dynamic Lighting Was Better Nine Years Ago | A Warning About 9TH Gen's Neglect." defines the
     scenario table below.
  3. "How Incompetent Graphics Create Fake Realism & Why Your Brain Hates It." defines "Standards In
     Realism".
  4. "Why MSAA Should Be In EVERY Deferred Renderer | The Incompetence Of Crysis 3's ..." contributes
     an "Equal &/or similar rendering density" chart:
     - 720p rows: No AA / 2xMSAA / 4xMSAA / 8xMSAA.
     - 1080p rows: No AA / 4xMSAA.
     - Higher rows: 1440p No AA, 4K No AA, 1440p 2xMSAA.
     - Cost deltas shown: +50% / +100% / +100%.
- Scenario table: "THESE CONTAIN THE MOST COMMON SCENARIO ATTRIBUTES FOUND IN MODERN TITLES OR
  UNIVERSALLY BENEFICIAL TO ANY GAME". Each row is game aspect, then technical challenge and/or needs:
  - FAST PACED: 60FPS (PRE-FRAMEGEN) AND RESPONSIVE VISUALS.
  - REALISTIC: ACCURATE PBR, SHADERS, LIGHTING, IMAGE STABILITY.
  - 3RD PERSON: PRONE TO TEMPORAL DISOCCLUSION AND GHOSTING ERRORS.
  - INDOOR & OUTDOOR TRAVERSAL: ACCURATE LIGHT BLENDING & LEAKING CANNOT BE PRESENT.
  - 75% STATIC ENVIRONMENT: RELIABLE LIGHTING INFORMATION.
  - 25% OF SCENE IS DYNAMIC (characters, breakable objects, vehicles, moving doors/windows): SCENARIO
    DEPENDENT, LITTLE IMPACT ON MAIN LIGHTING.
  - DYNAMIC LIGHTS (sun/moon and many gameplay-toggleable light sources): SCENARIO DEPENDENT, RANDOM,
    AND UNPREDICTABLE MOVEMENT.
- Narration on the totals ([1:01:00]-[1:01:20]): "unlike other timings, if you add them up, they'll end
  up matching a 59 FPS cost. Because this does assume high action worst-case scenario rendering. But
  you shouldn't add this cloud timing without understanding its context."
  - Our sum of every legible chart timing **excluding the 2.90 ms clouds** is about **16.4 ms**, or
    **17.1 ms** if the tone-map upper bound (0.93) is used. That is about 59-61 fps, which is consistent.
- The 1080p+8% shadow-density rule (see 2.10) also assumes a 1080p player resolution.

### 1.2 Graphics API slide ([5:00], `f_0021`)

The slide has columns: API, platform, "forces budget spending on alternative API support", and a tools
verdict.

| API | Platform | Tools verdict on slide |
|---|---|---|
| OpenGL | OS agnostic + mass hardware support | tools limited / untested performance |
| DX11 | Windows & Xbox only | "Great Tools! 2x faster shading than DX12" |
| DX12 Ultimate | Windows & Xbox only | "Okay'ish Tools, Slow?" |
| Vulkan | OS agnostic + mass hardware support | "Still catching up to DX11" |
| Proprietary console APIs | related console only | "We've heard good things" |

The narration ([4:30]) says that in their UE testing the hardware shades pixels about 2x faster with
DX11 than DX12. This is relevant to us: **DX11 is the right choice**.

---

## 2. Middle-layer passes (left to right) with workflow and reasoning notes

### 2.0 Start-of-frame Compute Passes (grey boundary, no timing) - best `f_0026` [6:15], `extra/comp/t_00460`/`t_00470` [7:40]-[7:50]

- Steps: one **red** box, "Particles, Clouds, & Fog Compute Shaders?".
- Workflow (top), verbatim: "Modern visual scripting is far behind C++ standards. This is just a matter
  of attention from development & needs to be better. Classes need to be simplified. So simplified the
  user should be able to swap from a graph view to traditionally text based source code within
  secounds [sic]."
- Reasoning ("???" plus a box), verbatim: "Avoid GPU skinning as many titles (Alan Wake 2, DX11 Callisto
  Protocol, UE5 Games) lose up to around 0.20ms-1.00ms. Titles like KCD2, Days Gone, & MGSV titles run
  just fine without this approch [sic]. Run as many features on CPU & save performance by not using
  butchered (slowed) coding languages such as the stock UE blueprint system. KDC2 [sic] uses compute
  shaders for cloud rendering & Dead Space Remake spends around 60 us for particle logic. You wouldn't
  want these passes superseding a 0.15ms budget".
- Takeaway: keep start-of-frame compute work **at or below 0.15 ms**, and do skinning on the CPU.

### 2.1 Minimal Hardware Clear Pass - **0.15 ms** - `f_0033` [8:00], `f_0040` [9:45], `f_0044` [10:45]

Resources listed in the box (all hardware-cleared):

| Resource | Format |
|---|---|
| Depth Stencil | **D32S28 [sic = D32S8] (2xMSAA)** |
| Dual Normal & Skin Mask | **RGB10A2_UNORM (2xMSAA)** |
| Motion Vectors/MSAA Data | **RGBA16_FLOAT (2xMSAA)** |
| Emissive/Diffuse | **RGBA16_FLOAT (2xMSAA)** |
| Shadow Map(s)/Atlases (red, tentative) | **D16 (2048x2048)** |
| Visual Effect Resources (red, tentative) | - |

- Not cleared:
  - G-buffers fully overwritten by geometry (normals, albedo/roughness); a depth clear suffices.
  - The specular RGBA16, which is recycled later.
- Workflow (`extra/hij/t_00640` [10:40]): "These resources are always cleared by the engine but the engine
  settings must provide a list of render targets that allow artist/studio members to include in this
  stage if they want the ability to use that render target in a custom material passes. *Include in
  this clear stage or forced permutation within select non-bandwidth bound full screen passes (hijack
  clears)".
- Reasoning ("Learned from", `f_0044`, `extra/clear_r/t_00854` [14:14]):
  - green, NFS2015, Crysis 3 2013, (Future TI analysis investigations): "These titles save performance
    by minimizing hardware clears by taking advantage hardware depth testing."
  - green, KCD2: "This title uses some type of trick to speed up hardware clears. RGAB8 [sic] cost around
    2-3 us instead of 8-10 us. This needs to be adopted."
  - red, MGSV:TPP: "This title underestimates the long term cost of unneeded hardware clears. Up to
    5.0ms in lost in this title due to poor management." The zoomed-out [7:40] view of the same box
    reads "1.0ms".
  - red, Days Gone, (Future TI...): "NEVER use quads or triangles to clear render target(s). Days Gone
    loses up to .12ms doing this for example."
- Related slides:
  - [8:15]-[10:35]: "previous frame ending draws vs current frame beginning draws" (1080p RGBA8, VRAM
    8.29 MB); "DO overwrite / DON'T clear+overwrite"; a hijack-clear diagram; a tweet "Do non bandwidth
    bound work and clear at same time".
  - [11:30]-[12:15]: see section 3.1 for timings.
- Marketing template (`f_0055` [13:45]): "...We've optimized our hardware clears beyond typical DX11
  performance (around 2us for 32bit resources, x us for nxMSAA resources); .15ms Total cost for all
  hardware clears; XX hardware clears skipped with fullscreen overwriting; XX cleared through hijack
  clears. All testing performed with a RTX Desktop 3060 at native 1080p."

### 2.2 Non-Alpha-Tested Partial Prepass (For Deferred Rendering) - **0.30 ms** - `extra/prepass_r2/t_01646` [27:26], `t_01700` [28:20]

Steps:

1. **Step 1/3, Large Non-Alpha Tested Content**: "Draw z-sorted (front to back) contents to get best
   possible occlusion. Depth only output ensures fastest rendering speed."
   - Output: Depth Stencil **D32S28 [sic] (2xMSAA)**.
2. **Step 2/3, Dual Normal Contents**: "Draw all z-sorted (front to back) contents with dual normal
   flagged materials & write motion vectors along the with depth".
   - Outputs: DS **D32S8 (2xMSAA)**, Motion Vectors/MSAA Data **RGBA16_FLOAT (2xMSAA)**.
3. **Step 3/3, Emissive Contents**: "Draw all z-sorted (front to back) contents with emissive flagged
   materials & write motion vectors along the with depth".
   - Outputs: same two as step 2.

- Workflow (`t_01673` [27:53]):
  - Step 1: "Requires max-area generative LODs. Artist need a feature that allows them to force certain
    meshes to "always prepass"."
  - Pass: "Prepass quad+overdraw view mode. Geometry preprocessing workflows (that are final material
    aware) for vertex compression (overdraw-reorder, indexing, cache efficiency)."
  - Steps 2/3: "Materials must be flagged with "Allow Dual Normal" and/or "Allow Emissive" by artist."
- Reasoning (`f_0045` [11:00] zoomed-out):
  - green, MGSV:TPP, Half-Life: Alyx, Jedi Survivor: "These titles prevent alpha [tested] drawing in the
    prepass".
  - red, Days Gone (Non-Remastered), The Callisto Protocol, KCD2, Crysis 3 2013, Silent Hill 2 Remake:
    "These titles lose up to sev[eral ...]" [rest illegible].
- Narration targets: "It should remain under 0.3 milliseconds as that's what Half-Life: Alyx is able to
  achieve while processing a full non-alpha tested pre-pass in heavily urban scenes" ([27:00]).
  - Only large/close geometry is prepassed.
  - No per-pixel texture sampling is allowed (no alpha test, no per-pixel displacement).
- Marketing template ([26:34]): "...Alpha-tested content & other per-pixel sampling techniques like
  per-pixel displacement is actively excluded to keep our prepass performing fast... Our prepass
  typically cost 10-15% of the basepass in high action scenes on X generation hardware."
- Slides:
  - [26:04]: prepass variants (Depth Only / Depth + Motion Vector / Depth + V/UV Buffer). Performance
    weaknesses: "1. per-pixel texture sampling like alpha testing 2. dense geometry (quad overdraw)".
  - [25:40]: quad-leakage debug view, "50-70%+ surface area here is leaking 75% GPU performance via
    quad overdraw".

### 2.3 Software Mesh Occlusion Pass? - **0.25 ms** (red/grey, tentative) - `extra/prepass_r/t_01738` [28:58]

- Sits between the prepass and the base pass. A "???" is attached.
- Narration ([28:55]): "it would be nice to take advantage of [the prepass occluders] with some sort of
  occlusion approach... but I haven't found this to be a well-researched topic."

### 2.4 Alternated Z-Sorted Basepass (For Deferred Rendering) - **2.80 ms** - `f_0122` [30:15]; reasoning `extra/bpr/t_02059` [34:19]; workflow `t_02068` [34:28]

Outputs for steps 1-4 (all **2xMSAA**):

| Target | Format |
|---|---|
| Depth Stencil | D32S28 [sic] |
| Motion Vectors/MSAA Data | RGBA16_FLOAT |
| Normals & Custom Stencil | RGB10A2_UNORM |
| Albedo & Roughness | RGBA8_UNORM_SRGB |

A red box adds "(2xMSAA) UE4 G-Buffer Layout+ 13 or 16 Bit BRDF ID+ 12+Bits for Depth Bias+". The
remaining G-buffer content is undecided (see 3.2).

Steps:

1. **Large Alpha Tested Content**: "Draw the relevant z-sorted (front to back) contents into the main
   buffers".
2. **Small Non-Alpha Tested Content**: "Draw non-prepassed content with z-sorting (front to back) into
   the main g-buffers below".
3. **Small Alpha Tested Content**: "Draw the remaining alpha tested content with z-sorting (front to
   back) & output to the the main g-buffers below".
4. **Depth-Only Prepassed Content**: "Draw the prepassed content & output result to the main g-buffers
   below" (depth test EQUAL; no overdraw).
5. **Motion Vector Prepassed Content**: "Output the previous basepass render targets but swap the motion
   vector output depending on Prepass Stage".
   - Extra outputs: Emissive/Diffuse **RGBA16_FLOAT (2xMSAA)**, Dual Normal & Skin Mask **RGB10A2_UNORM
     (2xMSAA)**.

Reasoning:

- Step 1 ("???" and Learned from Callisto Protocol, NFS2015): "The Callisto Protocol's results show the
  importance of 10 bit normals & the 2-bit alpha can probably be used for lighting channel separation.
  Callisto Protocol also makes a good case for SRGB albedo data. Need For Speed 2015 stores the
  roughness with the albedo because the stochastic SSR pass needs both during the variance evaluation
  stage."
- Step 2: "Requires optimized LODs. These objects are usually geometry bound so we gain performance by
  only drawing them once. When the pass is completed, the collective surface area for depth occlusion
  will be massive. This will help the next two passes which tend to be the most expensive."
- Step 3:
  - "Requires optimized LODs that are less geometrically dense as this will be the main bottleneck
    instead of pixel invocations or overdraw."
  - Learned from Days Gone (Non-Remastered): "The optimal foliage LOD model is thoroughly explain[ed] in
    the Threat Interactive analysis."
- Step 4: "The most expensive materials are now shaded with maximum occlusion."
- Step 5: "This is a cheap way to keep g-buffer outputs to a minimum".
  - red, NFS2015, Dead Space Remake, MGSV:TPP, (Future TI...): "These engines spend a minimum of 0.20ms
    re-rendering textured meshes with additional emissive data in a 2nd or even 3rd rendering pass."
  - green, UE4/UE5: "This engine ensures emissive data can be written along with general g-buffer
    processing".

Workflow (top), verbatim list:

- "Prepass aware quad+overdraw viewmode."
- "ALU driven 2x2 (2 position diagonally jittered) bayer matrix LOD fade with 500 millisecond
  transition. Shadow draws need to be synced."
- "Up to date specular anti-aliasing."
- "Low roughness fresnel anti-aliasing (Johan Hammes) & related vertex normal prebake workflow (for
  static meshes)."
- "Pre-bake workflow for static meshes (or maybe min-max animation posing) for SV_Coverage minimizing."
- "Modern updates to the texture workflows discussed by EA but with Days Gone depth bias & mip-map
  filtering support (build on-top of proxima BRDF?)."
- "Global vertex foliage movement."
- "Extensive BRDF library created from pre-existing photographic validation for each relevant shading
  model. This is stored in a large look-up-table similar to FOX engine with empty profiles artist can
  tweak/load for production."
- "Alpha tested content needs to utilize alpha-to-coverage mipmaps & the material options need to
  provide the appropriate alpha test SSAA code (reference crysis 3 rendering resources)."
- "Occupancy friendly/aware material graph workflows."

Narration details ([30:50]-[36:00]):

- 32-bit depth with **reverse-Z**.
- 10-bit normals (RG16 normals not needed). RGB10A2 alpha holds the anisotropic flag and one
  lighting-channel stencil bit.
- The ID channel becomes a "dual material profile ID", with up to an 8,000-slot profile LUT.
- SV_Coverage plus vertex-baked "shared flat discontinuity" bits trim MSAA coverage.
- Material table slide at [32:15] (`f_0129`).

### 2.5 Pre-Lighting G-Buffer Resolve: Depth Bias | SV_Coverage | Stencil Data - **0.50 ms** - `extra/resolve/t_02305` [38:25]

- **Merged Pass (Sample Frequency Draw 1)**: "Use the D32S8, shading model, depth bias, & motion
  vector/msaa g-buffers in a sample frequency pixel shader.
  - Output 1 (Sample frequency 32-bit detail-biased depth): Store result by overwriting an existing
    2xMSAA Specular RGBA16.
  - Output 2 is more complex. It's a sample frequency shading model mask with an extra value
    representing sky pixels/sample 0-only pixels. SV_Coverage pixels that are affected by the
    pre-baked vertex data are marked as sample 0 data to prevent redundant shading in pixel frequency
    lighting passes."
  - red, "Output MSAA resources for later stages?": "If a later stage needs a native MSAA resource (even
    a clear) output the result here."
- **Remaining Sample Frequency Draws**: "By any means necessary: Transfer the output 1 data from the
  RGBA16 back into the depth channel of the original D32S8. Then (or simultaneously) transfer the
  output 2 data into the stencil channel of the original D32S8. Clear the specular RGBA16 afterwards
  (hijack/hardware)".
  - red, "Complex Stencil Approach (Depth Testing Abuse?)": "If output 2 data is stored as values in a
    2xMSAA D16; a clever approach where both data can be written to the D32S8 in the same draw might be
    viable using full screen triangles drawn with sizes/distance to the value in the D16 (sample
    frequency shading). This might not work if something is being overlooked..."
- red, "Output relevant non-MSAA resources for later stages?": "For example; later stages like SSGI or
  half-resolution VFX might need a min-max depth RG16 resource. Wetness passes?"
- Reasoning ("???" plus Learned from Crysis 3 2013, Days Gone 2019, UE5 forced RGBA16 lighting inputs):
  "Days Gone uses the stencil buffer for shading models but outputs the result per-draw. This pipeline
  allows for more complex materials & we need to take advantage of the stencil buffer for redundant
  SV_Coverage samples. This will speed up sample frequency passes & prevent alternating passes between
  pixel & sample frequency seen in Crysis 3."
- Learned from (red), MGSV:TPP: "This title has a great rain system but it has to copy the render
  target views which is slow. If roughness & albedo are stored in a singular buffer: the rain pass can
  just [use] the ID LUTs, use early stencil testing per shading model & blend the new roughness & albedo
  darkening."
- Narration ([37:00]): no non-MSAA G-buffer copies are made. Empty/disabled sub-samples and sky pixels
  share one stencil value.

### 2.6 Occlusion Queries - **0.30 ms** - `extra/resolve/t_02305`

- Step: "Test static light bounds, eye meshes, and other related info".
- Learned from, Callisto Protocol: "This game saves several milliseconds by baking tight geometry bounds
  that represent stationary light reach. This prevents light overdraw in the lighting pass & related
  shadow map processing."
- Workflow: "...[b]aking geometric light volumes [in the en]gine for stationary/static lights" [partly
  cut off].

### 2.7 Direct G-Buffer Decal Pass - **0.30 ms** - `extra/resolve/t_02305`

- Step: "Output to select basepass render targets using vertex/pixel shader pipeline. **Avoid d-buffer at
  all cost!**"
- red: "Copy scene normals in dual normal (non-skin pixels) for higher quality results?"
- Learned from:
  - green, MGSV:TPP, Tesseract Engine, KCD2: "These titles use minimal inputs (scene depth) to project
    decals on the g-buffers. They are already extreme fast."
  - red, Days Gone (Non-Remastered), Callisto Protocol, Crysis 3 2013, Silent Hill 2 Remake, NFS2015:
    "These titles either waste memory with d-buffer decals or lose some performance creating new
    resources containing non-msaa normal data".
- Workflow: 'Decals materials need a "Write new BRDF ID" flag for artist.'
- Narration: Fox Engine decals write every G-buffer channel except the shading model ID.

### 2.8 Density Synced Perspective Shadow Maps (e.g. Non-Cascaded Spot/Point Light) - **1.00 ms** - `f_0158` [39:15], `extra/resolve2/t_02375` [39:35]

- Step, "Per-visible shadowed light": "Render the relevant shadow map data at a maximum resolution of
  **2048x2048** with perspective aligned (player's viewport) FOV/zoom. The shadow map density needs to
  match **1080p+8%**. Quad-clears/copies need to be banned from the pipeline & objects that cannot cast
  into the players viewport need to be culled from the shadow pass".
- red, "Several 2048x2048 formats or Atlas?": "**D16** with guaranteed density has shown great quality
  with native 24 bit viewport depth but needs more testing with native 32 bit native viewport depth.
  D32 is the alternative. We also need a solution that is friendly to forward rendering."
- red, "PSL MSAA shadow maps & biasing research": "Avelina9X on r/GraphicsProgramming made a post about
  "Bias Free Shadow Mapping" which looks promising. Shadow mapping on large triangles could potentially
  be far more efficient if a smaller resolution was used with MSAA. PSL can force the original non-msaa
  positions."
- Reasoning ("???" plus Learned from):
  - green, Days Gone 2019: "This title showcases how important distance ratios are & perspective shadow
    maps can help with this." Image labels: 4096x4096; "1080p ... 1166p is 8%".
  - green, MGSV:TPP: "This title uses backface culling for shadows which helps floor meshes remained
    shadowed without major cost problems".
  - red, Callisto Protocol, MGSV:TPP: "...shadows draw so many objects inside the shadow maps even though
    the mesh bounds would never cast into the players viewport..."
  - red, Callisto Protocol, Crisis 3 [sic] 2013, Dead Space Remake: "These titles waste so much
    performance using quads to clear/copy the shadow map/atlas..."
- Workflow:
  - "The engine must render objects with backface culling by default but offer artist with a front face
    culling option in the mesh editor/inspector..."
  - "The engine must provide a quad+overdraw viewmode for each shadow-casting light..."
  - "Foliage shadows via shadow map rendering can be expensive & hardly worth-it with screen space
    shadows. The mesh editor/inspector must offer artist with a shadow scalability setting..."
- Slide [41:20]: the 2002 Perspective Shadow Maps paper's downsides are answered with:
  - "All fixed with the 1080p+8% density rule & 8th generation games already render 15-30 shadow maps
    per-frame".
  - "1080p+8% density rule (explained in Video 19): The shadow map objects must always be rendered at an
    8% higher density than the player's set resolution which is assumed as 1080p".
  - "The player's game resolution can be increased without increasing the shadow map density".

### 2.9 Deferred Lighting Structure for Non-Cascaded Lights (Shadowed & Non-Shadowed) - **1.50 ms** - `f_0168` [41:45], `extra/litr2/t_02492`

The pass is an "Iterate per visible light" loop:

- **First Step: Stencil Volume (Sample Frequency)**: "Render the retrospective dynamic or baked
  mesh-volume to increase/decrease the current stencil values by 1. Use early stencil testing with the
  sky value to prevent changes/invocations on redundant SV_Coverage pixels."
  - red, "Lighting Channel Stencil Pass": "If the light has artist enabled "channel restrictions" such as
    a "character only light"; the channel mask can be referenced from the Normal RGB10A2 Alpha. It would
    be great if this could be referenced in the initial stencil volume."
- **Remaining Sample Frequency Draws (Stenciled Fullscreen Triangles Draws Based On Shading Model
  Stencil)**, verbatim:
  - "Avoiding fullscreen quads: Draw a single fullscreen triangle with early stencil testing to shade
    the retrospective shading model of said stencil value. Only the relevant basepass g-buffers, shadow
    map (if shadowed), and retrospective BRDF ID map should be used as the pixel shader inputs. The
    diffuse & specular output will be stored in the Emissive RGBA16 & recently cleared specular RGBA16.
    The Emissive RGBA16 alpha will store the Luminance. Each draw reverts the stencil to it's original
    unlit value."
  - "We need to keep the shading model options as small as possible as this will induce an additional
    draw per light. The main 5 models will be: Lit (new metallic workflow?), Full Callisto Dual Normal,
    Foliage, Subsurface, & Cloth. Up to 7 models is manageable for a system that doesn't track existing
    model types within light boundaries."
  - "Avoiding shadow projection masks if the light is shadowed: A minimal PCF algorithm & screen space
    shadow method will be processed in the same direct lighting pixel shader. Thanks to the minimized
    complexity through stenciled separation; this should make room for occupancy optimizations."
  - "To avoid shadow acne, peter panning, & jagged projection; A minimal PCF & shadow map anti-aliasing
    method that compliments the targeted shadow rendering density needs to be implemented. Shadow
    sharpening needs to be forced to a certain degree in-order to prevent common leaking caused by
    overly soft-shadows."
  - Outputs: Emissive/Diffuse **RGBA16_FLOAT (2xMSAA)**, Specular **RGBA16_FLOAT (2xMSAA)**.
- red, "4x Non-MSAA RGBA16s?": "Sample frequency shading increases the quad cost by N samples because
  quad shading does not work in parallel outside of the sample index (samples 0 & 1). We need extensive
  testing across vendors to see if forcing pixel frequency draws to treat subsample results as another
  render target output increases performance..."
- Learned from: Days Gone (Non-Remastered), Crysis 3 2013, MGSV:TPP: "These games use stencil based
  lighting & maintain the best performance."
- Fox Engine captures at [42:00]-[42:24] show the stencil volume plus lighting draw, labelled "(With
  lighting draw stencil testing disabled!)" and "Wireframe of lighting draw mesh=quad overdraw!".
  - This is why the chart lights with a fullscreen triangle per shading model (the Days Gone approach)
    instead of the light mesh.
- Narration ([44:00]): the timing assumes high-action content. At a distance the lighting shader swaps
  shadow-map projection for noiseless, high-sample-count screen-space shadows. Artists are restricted
  to "barely softened shadows".

### 2.10 Directional Light (Sun/Moon) - **0.60 + 0.32 + 0.18 + 0.36 ms (= 1.46 ms)** - `f_0181` [45:00]

Steps:

- **Rendering The Shadow Maps**: "Using cascade receiver culling: Render 3 cascaded shadow maps with an
  8% higher density & distance ration seen in Days Gone (around **1300x1300**)".
  - red: "Several resources or Atlas?"
  - red: "PSL MSAA shadow maps & biasing research?"
- red, "Create 8x smaller min-max RG16?"
- red, "Far Field Cascade (FF CSM) Resolution?": "This might be a crutial [sic] place to implement
  scalability. Dead Space Remake uses a 4k by 4k FF CSM where as Days Gone (an open world) uses a 2k by
  2k FF CSM."
- red, "SDF/Days Gone Tree Rendering?": "Alpha testing is extremely expensive & Days Gone's pipeline gets
  around this with a 0.3ms pass that renders specialized atlases that end up projected into a shadow
  mask."
- **Compute Shader Screen Space Shadows**: "To take advantage of groupshared memory: Use a compute shader
  to process a 32 bit depth resource (with the imprinted micro detail) to output a noiseless non-MSAA 8
  bit output."
- **Project Shadow Data On Native Resolution Non-MSAA RG8_UINT**: "It might require multiple draws as it
  does in Days Gone, but the R channel holds the general shadow map data where as the G channel holds
  related subsurface properties."
- **Remaining Sample Frequency Draws (Separated Draws Based On Shading Model Stencil)**: "It might be
  helpful to use the RG16 shadow mask as a full screen pass on top of the stencil to prevent shading on
  pixels with full shadowing. Shade the directional light with various early stencil lighting draws
  using the RG16 shadow mask. The Crysis 3 cascaded shadow mask optimization should be used since no
  downside as been found. The subsurface data may need some extra R&D."

Learned from:

- Days Gone: "This title culls redundant objects from re-drawing in the cascade & uses the most optimized
  density. The closest cascade is almost the same density as players 1080p viewport."
- Days Gone: "This title uses a compute shader to create this RG16 for the projection stage. We need
  testing to see if it's worth the performance gain".
- Crysis 3 2013: slide "Deferred MSAA: performance shortcuts".
- KCD2: "It will cost another 0.30ms to project cloud shadows on the shadow-mask."

Narration ([45:30]-[47:00]):

- Crysis 3's six cascades cost about **1.2 ms**; the target is half that.
- KCD2 renders 5 cascades but updates the far ones across frames.
- Far-field shadows should use approximations (Days Gone foliage atlas, SDF shadows).
- Sample-frequency shadow masking (the Crysis 3 trick).

### 2.11 (Sample Frequency) Forward Rendered or Pixel Shader Tiled Deferred Character Materials - **Eyes: 0.15 ms, Hair: ???/0.3 ms** - `f_0193` [48:00]

- red **Eye Meshes**: "Forward render eye meshes on diffuse & specular RGBA16s. The internal iris
  shadowing will be determined by an artistic recreation driven by values that track the corresponding
  positions of the eyelid bones (tracked by CPU)."
- red **Hair**: "Hair should look far better than most games thanks to 2x2 bayer matrix+alpha to
  coverage there but should we take the quad overdraw cost to incorporate more features through extra
  textures?"
- Workflow (`extra/gir3/t_02980` [49:40]): "The engine must provide easy workflows to link the various
  properties. The bone editor will need to offer artist with options & workflows that enable the CPU to
  track eye/lid animation positions so it can alter values in the retrospective eye material."
- Reasoning ("???" plus Learned from): green, "The Callisto Protocol provides iris shadows but only with
  RT shadows. This is why we need a new approach/standardized workflow that links eye meshes with
  animation & material data." The image is captioned "IRIS DUAL RAYTRACED SHADOWS".
- Narration: Days Gone and Callisto forward-render their extra eye layers for **< 0.2 ms**.

### 2.12 Global Illumination: Cascaded Probe Lighting - **1.0 ms (World Space GI) & 1.0 ms for Screen Space Techniques** - `f_0200` [49:45], `f_0201` [50:00]

Steps (grey = tentative structure):

1. **Stream In/Out GI data & update resources accordingly**.
2. **Determine Visible Radiance Probes Within Player Viewport**: "Depth buffer information is relevant
   here but maybe not..." [rest illegible].
3. **Determine Propagation Paths**: "Using the previously computed list of visible probes within the
   player's viewport bounds; determine which offscreen radiance probes can propagate the brightness of
   direct lighting from their p[osition to the visible] radiance pr[obes]" [partly cut].
4. **Shadow Map Tracing**:
   - "For each relevant radiance probe; update it's list of local irradiance sensors."
   - "To avoid harsh popping (aliasing) as direct lights change; sample multiple positions around the
     sensor to determine how much geometry is in-between the sensor & the shadow's retrospective light.
     Store the result for later."
   - "For each sensor that is determined outside the view within a shadow map, mark the sensor for world
     space software RT tracing."
5. **Fallback For Irradiance Sensor Updates (Temporal Software Ray-Tracing)**:
   - "Using on whatever worldspace representation ends up being the most optimal (SDFs, Voxel, BVH,
     Micro Shadow Maps w/ Proxy Data?): Send an initial ray from one of the sample positions near the
     marked irradiance sensor to determine it's occlusion status from said lights. Store the result in
     the same place as the shadow map traces."
   - "More than one ray per sensor would be preferred but if one ray is deemed too expensive: change the
     initial ray position every frame as done with the shadow map tracing."
6. **Feed Irradiance Sensor Data Into Radiance Probes**:
   - "Update each relevant radiance probe with it's retrospective sensor data."
   - "Baked sensor data contains: a most unoccluded direction and an average albedo value of local static
     geometry facing said direction. Dynamic sensor data contains: direct lighting occlusion. Other
     dynamic data like retrospective light color, & brightness values can be reference in memory
     elsewhere."
7. **Propagate Lighting Across Radiance Probes (Temporal Software Ray-Tracing)**: "...Send rays from
   multiple positions near the radiance probes to determine their occlusion status from one another.
   Combine the hit result, accumulated colored lighting data & directions to determine the how the
   visible probes will output the spherical gaussian output. More than one ray per radiance probe would
   be preferred but if one ray is deemed too expensive: change the initial ray position every frame as
   done with sensor traces".
8. red **Update World Space Reflection Environment?**
9. red **Output Simplified Spherical Map On Pre-Computed Atlas?** [body illegible].
10. **(Pixel Frequency Shading) 0.55ms SSGI / 0.40ms Radiance Probe Guided SSAO** [body illegible; it
    mentions 1080p Days Gone timings and avoiding GTAO].
11. **(Pixel Frequency Shading) 1st Step Separable Denoising** [body illegible; roughly "denoise with
    one separable pass, then merge the second separable pass with the composite stage"].
12. **Shade Indirect Lighting On Previous Diffuse 2xMSAA RGBA16 (Sample Frequency)** [body illegible].
    - Inner red box: **Indirect SDF shadows** [body illegible].

Workflow (top, `f_0201`):

- "The engine must provide a workflow that allows artist quickly view their level with artist selected
  static geometry flagged for global illumination baking."
- "The engine must automatically place radiance probes (and retrospective irradiance sensors) in
  optimized areas."
- "The engine must provide a separate version of the GI system that constantly runs the placement logic
  & gathers the baked sensor data with raytracing. This way artist don't have to constantly bake
  information & when they are done, they only need to bake once. Consumers are left with a system that
  reacts to dynamic lighting while remaining optimized thanks to caching local/relevant data."

Reasoning:

- Learned from:
  - green, Days Gone: "This title is able to update baked local probes just using shadow-map data with
    compute shaders. This only take .3ms."
  - red, Crisis 3 [sic] 2013, Tesseract Engine: "These titles using reflective shadow maps which cost up
    to .3ms to process before another .3ms pass manages the data. The result in the end is visually
    poor."
- Learned from, green, MGSV:TPP: "This title makes use of small atlases for GI. If micro (30x30)
  orthographics shadow-map views are used (from direct light to marked irradiance sensor). The earlier
  D16 may might have good use here in terms of recycling. Every other frame can jitter the view on a
  subpixel level".
- "???" note (white): "Simply reference the grand dynamic & automated placement scale of the GI in The
  Division, the simplicity & effectiveness of the GI in Metal Gear Solid V, & the clean/reactive results
  achieved in Days Gone. World Space GI does not need to be noisy or slow. We simply need a robust
  system."
- Learned from (red, Crysis 3 2013, MGSV:TPP): roughly "Crysis 3 shades indirect lighting in a way that
  harms MSAA... MGSV suffers from pixelation from upscaled direct lighting. It was cheap on modern
  hardware so native resolution shading should not be too expensive." [low resolution]
- Layer slide ([50:25]), nested boxes from outer to inner:
  - World Space Large Scale Occlusion (Needs R&D).
  - Shadow Map & Baked Static-World-Hint Driven World Space 3x+ Bounce Indirect Lighting (Needs R&D).
  - Indirect Lighting Driven Noiseless & Non-Haloing Screen Space Occlusion (should NOT be visible where
    direct lighting is present).
  - Screen Space Global Illumination Driven By Final Lit Scene (NOT ALBEDO Buffer Driven!) (Needs R&D;
    "If composited with SSAO, quarter resolution?").
- Narration: reflective shadow maps cost 0.3-0.6 ms per frame, so do not implement them. "If these
  layers are provided under a 2-ms budget with noiseless results..."

### 2.13 Reflections - **0.5 ms + 1.0 ms + 0.30 ms (= 1.8 ms)** - `extra/reflr/t_03216` [53:36]

1. red **Overwrite 2 different RGB10A2 with resolved normal data**: "The pipeline already contains 2
   non-MSAA RGB10A2 resources which might do well as pixel frequency placeholders for normals &
   roughness/variance. Remember that a very basic SSR should have ran during the resolve stage. It may
   have been good to store something that prevents extra traces."
2. **Compute Shader Stochastic SSR Classifications**: "Determine relevant variance using the 2xMSAA RGBA8
   albedo-roughness G-Buffer & output the various resources for compute shader tracing. Might need an
   native resolution (later mipmapped) non-MSAA **R8_UINT** as a trace mask".
3. **Pixel Frequency Stochastic SSR (Computer Shader)**: "A mipmapped (convoluted) **R11G11B10** of the
   last frame's post VFX image should be traced with blue noise sampling in about **0.40ms**. The output
   should provide **RG16** specular motion vectors, a noisy native R11G11B10."
4. **Generate A World Space Tracing Mask**: "It would probably be best to use the scene roughness & ID
   map (sky values), RG16, & noisy R11G11B10 to determine which pixels should be skipped for RT tracing.
   The R8_UINT could be recycled."
5. **Pixel Frequency (Compute Shader) World Space RT Reflections**: "Trace the allowed pixels (tracing
   mask) with blue noise sampling & output the results on a 75% smaller buffers (**720p RG16 &
   R11G11B10**)".
6. **Merged Temporal Reprojection & Composite Draw**:
   - Inputs:
     1. Native Temporal SSR History (R11G11B10)
     2. Stochastic SSR Traces (R11G11B10)
     3. Stochastic SSR Specular Motion Vectors (RG16F)
     4. 720p World Space RT Traces (R11G11B10)
     5. 720p World Space RT Specular Motion Vectors (RG16F)
     6. 2xMSAA Base Pass Motion Vectors (RGBA16)
     7. 2xMSAA D32S28 [sic] Or D16 Stencil (Early Testing with sky value if PS)
     8. 2xMSAA Normal Buffer (RGB10A2)
   - Outputs:
     1. 2xMSAA Specular Composited Frame (RGBA16)
     2. New (Next Frame) Temporal SSR History (R11G11B10)
   - red **Merge Temporal SSGI/SSAO Resolve?**: additional inputs are horizontally blurred (depth-aware)
     SSGI and SSAO (Format?); outputs are the next-frame temporal SSGI and SSAO histories (Format?).
7. red **Real Time Cubemaps? (+ new topic/video)**: "Days Gone rendering already showcased how fast this
   can be."

- Workflow (top): "The engine must provide a workflow to allow artist to bake large scale environmental
  proxies for software RT based on static geometry." and "The outputs most important aspect is direct
  lighting. The other aspects can be approximated as long as the ambient lighting isn't a disgusting
  bland "sky-derived blue"."
- Learned from (stochastic SSR):
  - green, NFS2015: "This title showcases a fair cost (**0.30ms**) for native SSR tracing".
  - green, Detroit: Become Human: "This title has a great resolve thanks to blue noise sampling & great
    motion results thanks to 2x the motion vector data (RG16_Float)".
  - red, Alan Wake 2 2013 [sic], NFS2015, Dead Space Remake, UE5 SSR, Silent Hill 2 Remake: "These titles
    either output no specular motion vectors or an inadequate R16_Float".
- Learned from (composite), green, Crysis 3 Remastered: "This title showcases a clean result from
  blending pixel frequency SSR data in a multi-sample image. It also optimizes SSR by merging it's
  denoising step with the composite step (where it's added to the specular buffer)".
- Narration: the world-space RT representation should be "near a voxel simplified"; dynamic cubemaps
  remain an option.

### 2.14 (Pixel Frequency?) Diffuse Channel Only Native Resolution Screen Space SSS - **0.3 ms** - `extra/sssr/t_03236` [53:56]

- Step: "Overwrite the upcoming light prepass resolve RGBA16 with the first separable pass & write to the
  previous input with second pass. Only 2 Draws should be dispatched."
- Reasoning: "???" only.
- Narration ([56:30]-[57:00]):
  - Model it on Crysis 3's stencil-tested pixel-shader SSS with an ID buffer for per-profile colour.
  - "aim for a 1.4 ms full-screen cost modeled after Callisto Protocol's ... or look into something more
    conservative", then "in general third-person gameplay, this should only cost around 0.3 ms."
  - Use a 2-position 2x2 Bayer jitter if banding appears.

### 2.15 Composite Diffuse & Specular Lighting With Fog Shading - **0.12 ms** - `extra/sssr/t_03236`, `f_0246` [1:01:15]

- Step: "Using the diffuse & specular RGBA16 resources & whatever relevant g-buffers/ID required:
  overwrite a **2xMSAA RGBA16** with the composited lighting result. Preventing processing on sky pixels
  would be beneficial. It may be best to implement a simple SSR in this stage for fresnel anti-aliased
  edges."
- red, "Stencil Test The Resolves Based On Shading Model?": "It may not be worth it (removing the branch)
  but a gaussian smoothing pass needs to be applied to any specular skin data. Callisto Protocol's
  pipeline never had a dedicated draw for this action despite a mention in the presentations. So it
  must have been merged with the composite stage?"
- red, "2xMSAA RGB10A2 Fox Engine Resolve? (Early Tonemapping)": "The benefits of this approach should be
  investigated. (See early tone mapping in Fox Engine)".
- Learned from, MGSV:TPP, Crysis 3: "These titles merge the specular & diffuse lighting resolve with
  either tone mapping+fog or directional lighting. This might be worth looking into."
- Narration ([58:00]-[1:01:00]):
  - Lighting must be **RGBA16** (KCD2, Dead Space Remake and Phantom Pain do pure RGBA16 lighting).
  - Fog shading belongs here. NFS2015 and Phantom Pain use fog atlases; an ALU approach may be better.

### 2.16 Sky & Cloud Rendering - **0.18 ms (Sky) + 2.90 ms (Clouds)** - `f_0246`

- red step: "Overwrite the current sky data with relevant sky shading & create a new value in the stencil
  for the sky value. It may be important for SSR reflections if we do post-tonemapping SSR."
- "???" plus Learned from KCD2: "This is a mix of timings based on Fox Engine, UE, & KCD2's sky cost.
  KCD2 clouds are expensive but the cost is fine since clouds are only visible when cost of reflections,
  direct lighting, base/prepass, & other passes are reduced. Simply matching the worse case scenario
  basepass timing is enough to keep performance stable."
- Narration: replicate the GT7 sky presentation (photographically validated).

### 2.17 Transparent Draws (Enhanced With Fog Atlas) - **0.15 ms** - `f_0246`

1. **Full Expression Decals**: "This decal should be able to express basic colors & emissive properties
   using the scene depth & artist selected meshes. Useful for shadow decals which can makes a huge
   different for feet & vehicles".
2. **Water Mesh Draws**: "Water tends to be close to surface depth & sometimes even writes depth. It's
   just good draw it first."
3. **Forward Lit Decals**: "Like many of these transparent draws, it does not need to affect luminance".
4. **Back To Front Forward Lit Transparents**: "Sort the draws from back to front".

- Learned from:
  - NFS2015, MGSV:TPP: "These titles use this approach for shadow decals, grime decals, & blood."
  - Crysis 3, MGSV:TPP: "These titles handle water efficiently thanks to drawing water early."

### 2.18 VFX Draws (Artist Created Sprites & Particles) - **1.00 ms** - `f_0246`

- red **Min-Max Half Res Shading (RGBA16)**: "An optimized approach for sorting artist flagged effects for
  half-res upscaling (min-max depth) need to be explored with native resolution particle effects. A
  composite mask in alpha needs to be stored to help TAA from blending particles."
- red **Forward Lit Sprites?**: "Light the contents of the sprite based on direct lighting & compute
  lighting at a vertex level."
- Learned from:
  - MGSV:TPP: "Half-Res shading for many sprite/low frequency effects can be a massive boost for
    performance without any visual consequence."
  - Crysis 3, CryEngine: "This is an extremely interesting app[roach]. We simply haven't seen it with
    min-ma[x half] resolution shading."
- Narration: every VFX/transparent writes a **responsive mask** in alpha, which TAA uses later.

### 2.19 DOF? - **0.30 ms (?)** - `f_0260` [1:04:45]

- red: "Needs better references."
- "???" attached.

### 2.20 DOF Blend, Physically Based Tone Mapping & MSAA Separation - **0.93 (TCP no bloom cost) - 0.25 ms (?)** - `f_0260`

- Step **2x RGB10A2 (Non-MSAA)**:
  - "Separate the 2xMSAA RGBA16 into two separate 10bit tone mapped resources with each containing the
    retrospective MSAA sample index (sample 0 & 1 data)."
  - "We know without a doubt we need linear midtones, comparable uchimura contrast toe, and hue
    preservation. The smooth shoulder is still a working progress. The GT7 tonemap curve introduces
    clipping faster but this mimics naked eye perception... Then there also properly exposing which was
    stressed in the original GT7 tone mapping presentation."
- red **Auto-Exposure & Auto-White Balance System**: "It might be beneficial to utilize the diffuse
  lighting only RGBA16 and downsample this for both white balance & auto-exposure. We need to figure out
  how to exclude emissives & SSS."
- red **Implement LUT: but what format?**: "First draw for LUT update (The Callisto Protocol uses a
  **64x64x64 RGBA6 [sic = RGBA16]**) then second draw for merged compositing & tone mapping. There might
  be room for scalability (**RGB10A2 32x32x32** like Days Gone) but the "power-of-4 nonlinearity
  anti-banding" from GT7 needs to be implemented regardless since it just makes sense."
- red **Re-use SSR RG16_Float For Temporal Responsive Mask**.
- Learned from:
  - Crysis 3: "This sets up everything for a clean SMAA 4x pass. It should only cost around **.20ms** +
    and extra **0.05ms** for the 2nd tone mapped image."
  - Days Gone (Non-Remastered), KCD2: [cut off].
- Narration ([1:05:00]-[1:19:30]):
  - The GT/GT7 tone curve with a contrast toe is preferred (Callisto Protocol shipped the GT curve).
  - The Fox Engine computes exposure from a downsampled diffuse-only buffer, excluding emissives.

### 2.21 Compute Shader SMAA S2X Passes - **0.38 ms** - `f_0260`

- Step **2x RGB10A2 (Non-MSAA)**: "Run a compute shader SMAA implementation on both tone mapped RGB10A2
  resources then combine the results in another non MSAA RGB10A2. The SMAA S2X resolve pass should
  utilize additional logic (& inputs) that blend the subsamples (and any custom sample locations),
  modified SV_Coverage mask & current frame GBAA (alike tech) with linear blending."
- red **Use Diffuse L-Buffer For Predication?**: "Again, for scalability & it's supposed to use a diffuse
  lighting buffer which our pipeline supports."
- Learned from (red), KCD2, CryEngine 5.6, UE5.7: "It could possibly relate to the RGB10A2/RGBA16 inputs,
  but modern SMAA implementations have been extremely poor lately in terms of catching edges. By any
  means necessary make sure SMAA is catching edges properly".
- Narration ([1:21:00]): the Crysis 3 worst case on dense grass reaches nearly 1 ms (see 3.6). This is
  covered because heavy foliage scenes under-use the VFX budget. The Jimenez optimizations give about
  40% more speed.

### 2.22 TSSMAA 4x Pass (Compute Shader?) - **0.25 ms** - `f_0260`

- Step **2x RGB10A2 (Non-MSAA)**:
  - "Use motion vector 2xMSAA RGBA16, tone mapped SMAA S2X RGB10A2, responsive masks, & linear depth
    (2xMSAA R16), from current & previous frame."
  - "Re-project data with no wide resolve (similar to persistence biased UE5 TAA). Only reproject data if
    the previous history is marked with relevant SV_Coverage & save responsive mask & depth in another
    output (R8 & D32/D16?)."
  - "The result should be stored on a non-MSAA RGB10A2".
- red **Merge With Previous Pass?**: "The reference pipeline blends composite data & bloom."
- red **PSL Decima Resolve?**: "We might just get rid of the whole temporal blending aspect if this
  sampling is sufficient".
- Learned from:
  - Crysis 3, Days Gone: "The first offers the reference SMAA 4x pipeline & both use tonemapped SDR
    history & have a good (still) resolve."
  - UE5/UE4: "Reference appearance without wide resolve (also the cheapest/**0.28ms**)", with cvars:
    - `r.TemporalAA.Algorithm 0`
    - `r.TemporalAA.HistoryScreenPercentage 100`
    - `r.TemporalAA.Upsampling 0`
    - `r.TemporalAACatmullRom 0`
    - `r.TemporalAAFilterSize 0.09`
    - `r.TemporalAASamples 2 (MSAA x2)`
    - `r.TemporalAACurrentFrameWeight 0.6`
- Narration ([1:25:00]-[1:42:00]):
  - Use 2-frame blending (current plus previous SMAA output, not an accumulation buffer) with a narrow
    resolve.
  - Use velocity weighting plus colour clamping, and a responsive mask for VFX.
  - Blend temporally only on MSAA-flagged edges.
  - Texture jitter compensation, or **programmable sample locations** with a Decima-like 2x pattern, so
    no camera jitter is needed (slides at [1:40:00]-[1:42:10]).
  - The AA marketing template is at [1:42:22] (`extra/velr/t_06142`).

### 2.23 Motion Blur / Velocity Compensation (Pixel Frequency) - **0.30 ms** - `f_0412` [1:42:45]

1. **Pixel Frequency Velocity Packing Via Compute Shader**: "Use the basepass motion vector 2xMSAA
   RGBA16 & 2xMSAA DS288 [sic = D32S8] to output specialized motion vectors in the native resolution
   stochastic SSR **R11G11B10** while simultaneously outputting an **120x68 RGBA16** (tiled velocity). If
   the pixel isn't animated: reduce velocity (blur) based on depth so that only close or animated objects
   receive blur. We might want to re-use the responsive mask RG16 to reduce animated velocity behind
   sprite effects."
   - red **Drop Tiled Motion Blur? (And Related Compute Passes?)**: "We've been dealing with tile
     artifacts for more than a decade. A modern approach should not have this issue."
2. **Native Resolution Motion Blur (Pixel Frequency)**: "Using the R11G11B10 velocity map, 120x68
   RGBA16, & SMAA 4x RGB10A2: overwrite the SMAA S2X RGB10A2 with the motion blur output. Utilize a
   noiseless sampling pass along with logic based on the "Improved sample contribution" discussed in:
   Next-Gen Post Processing in CoD: AW. Blue noise & IGN trigger pattern recognition & should not be
   used. White noise & checkerboarded bayer may work but it looks worse than uniform sampling."
3. **Native Resolution Separable Pass (Pixel Frequency)**: "Using the R11G11B10 velocity map, 120x68
   RGBA16, & previous motion blur output: Overwrite the pre-motion blur RGBA10A2 [sic] with a smoothed
   motion blur output."
   - red **Composite Half Resolution Motion Blur With (Bilinear)**: "It may serve as a nice scalability
     option."

- Workflow: "The engine needs to provide the depth limit to artist. Some may want to change it's
  behavior during gameplay & cutscenes."
- Learned from:
  - NFS2015: the only title that shades motion blur without tiled artifacts. It does not generate a
    separate R11G11B10 or other velocity mipmaps like UE, Fox Engine or Northlight.
  - Days Gone (UE4.11): only **~0.13 ms** to consolidate depth and motion into one velocity map, writing
    the first 120x68 RGBA16 at the same time.
  - red, stock UE4 (later)/UE5, Alan Wake 2: noticeable noise.
  - green, NFS2015 (white-noise sampling), MGSV (uniform sampling).
  - green, MGSV: 2x smoothed motion blur. It shades motion blur at half resolution and then smooths it
    with the same motion vectors, costing another **0.12 ms** at 1080p.
  - red, Days Gone (stock UE4.11), UE5: noise and/or poor separable passes.
- Narration ([1:47:00]-[1:57:00]):
  - Blur in a single direction only (trail behind, like NFS2015).
  - Depth-limited camera blur in a U-shape.
  - Use a uniform-sampled plus separable pass (CryEngine sample code, Fox Engine).

### 2.24 Artist Velocity VFX (Pixel Frequency) - **(VFX Budget)** - `f_0412`

- Workflow: "Another debug view needs to allow artist to view their effect in a stand alone view mode
  with interchange background & test meshes. A few defaults like a checkerboarded background should
  already exist. It may be good to offer vertex lighting as an option for artist to control the strength
  of the RGB values."
- Learned from, Alan Wake 2: it renders velocity VFX (RG16_F) after the motion blur pass without a
  separable pass. That pass only does chromatic tints with an extra texture.

### 2.25 Downscaled Bloom Resource - **0.38 ms** - `f_0412`

- Step references "Next Generation Post Processing in Call of Duty: Advanced Warfare".
- red **Generate Stochastic SSR Resources**: "It's important to include the anti-aliased/post motion blur
  image as the convoluted SSR input."
- Learned from, NFS2015: it uses the post-motion-blur image for the stochastic SSR tracing pass. Because
  the mipmapped frame has to be convoluted anyway, that data can be recycled for bloom, which is what
  Frostbite does.
- Learned from, GT7 sky (s2025 PBS course slides): "These results need to be replicated. It's all
  photographically validated work & digital skies tend to look terrible."
- Narration ([1:57:40]): bloom should react to the motion-blurred result.

### 2.26 Full & Half Resolution Composite Effects (Pixel Frequency) - **0.15 ms** - `extra/endr/t_07100` [1:58:20]

1. **Recycle Half Resolution RGBA8 (or higher precision)**: "Use a hardware clear on any half resolution
   pixel frequency resource with at least 8 bits per RGB channel. We do not need an alpha for the
   compositing stage".
2. **Downscale Current Frame**: "Use whatever non-MSAA resource that can store a downsampled image of the
   current frame & overwrite the upper corner with a downsampled image (540p is preferable). This is for
   transparent composite materials".
3. **Render Half Resolution Composite Draws**: "...The engine should sort non-transparent material draws
   first followed by transparent draws afterwards".
4. **Full Resolution (Pixel Frequency) Merged Post Processing Pass**: "Effects like film grain,
   vignetting, & chromatic aberration are usually computed in a single draw. Merge more functionality by
   having it also upscale the half resolution composite effects & bloom resources. (The camera effects
   should appear underneath the artist composited effects)".
5. **Recycle The Half Resolution Composite & Bloom Resources**: "Overwrite the half resolution composite
   resource with a downsampled version of post-processed frame so artist can use it for the next pass."
6. **Full Resolution (Pixel Frequency) Post Processing Pass**: "Render additional artist controlled
   content/sprites on top of the scene. This gives artist control up until the last moments in the
   frame..."

- Learned from:
  - NFS2015, Crysis 3 (Non-Remastered): reuse an **RGBA16** half-resolution composite stage; Crysis 3
    does this after tone mapping.
  - MGSV: uses only an **RGBA8** and processes this stage after tone mapping with great results.
  - "We can probably just recycle the half resolution VFX RGBA16."
- Learned from, MGSV, NFS2015: both merge several effects into a single draw, and the Fox Engine also
  provides a full-resolution pass. This helps artistically placed brightness/darkness sprites and
  water droplets.
- Narration: Crysis 3's native-resolution composite buffer "completely chokes performance". NFS2015 uses
  a half-resolution **RG11B10**.

### 2.27 UI - **0.1 ms** - `extra/endr/t_07100`

- Step **UI Draws**: "Draw UI elements according to developer logic".
- red **Gamma Correct?**: "If we use compositing, this is where Fox Engine does gamma correction."
- Narration: render the UI to its own target and apply gamma during the overlay (Days Gone). Callisto
  and Alan Wake 2 offer multiple gamma settings.

### 2.28 Totals

| # | Pass | Budget (ms) |
|---|---|---|
| 0 | Start-of-frame compute | (keep <= 0.15) |
| 1 | Hardware clears | 0.15 |
| 2 | Non-alpha-tested partial prepass | 0.30 |
| 3 | Software mesh occlusion? | 0.25 (tentative) |
| 4 | Alternated Z-sorted basepass | 2.80 |
| 5 | Pre-lighting G-buffer resolve | 0.50 |
| 6 | Occlusion queries | 0.30 |
| 7 | G-buffer decals | 0.30 |
| 8 | Perspective shadow maps (local lights) | 1.00 |
| 9 | Stencil-volume deferred lighting (local) | 1.50 |
| 10 | Directional light (CSM 0.60 + SS shadows 0.32 + projection 0.18 + shading 0.36) | 1.46 |
| 11 | Eyes / hair | 0.15 / 0.30 |
| 12 | GI (world 1.0 + screen 1.0) | 2.00 |
| 13 | Reflections (0.5 + 1.0 + 0.3) | 1.80 |
| 14 | Screen-space SSS | 0.30 |
| 15 | Diffuse+specular composite + fog | 0.12 |
| 16 | Sky / clouds | 0.18 / 2.90 |
| 17 | Transparents | 0.15 |
| 18 | VFX | 1.00 |
| 19 | DOF | 0.30 (?) |
| 20 | Tone map + MSAA separation | 0.25-0.93 |
| 21 | SMAA S2X | 0.38 |
| 22 | TSSMAA 4x | 0.25 |
| 23 | Motion blur | 0.30 |
| 24 | Velocity VFX | (VFX budget) |
| 25 | Bloom downscale | 0.38 |
| 26 | Composite effects | 0.15 |
| 27 | UI | 0.10 |
| | **Sum without clouds and without the occlusion pass** | **~16.4 (tone map 0.25) to ~17.1 (tone map 0.93)** |

---

## 3. Analysed-game slides: timings and formats

### 3.1 Clears ([11:15]-[12:30], `f_0046`-`f_0052`; `extra/prer3`)

- **UE5** (DX12 shipping demo, 1080p):
  - Tone-mapping draw (#168, low PP) = **145.4 us**.
  - ClearRenderTargetView #74 = **5.1 us**. "Both 1920x1080 RGB10A2 clears"; total clear cost about
    21 us, so 145 + 21 = **166 us**.
- **Crysis 3**: non-MSAA "Tone Mapping+" draw #7859 = **202 us**. It also merges filmic curve, bloom
  resolve and upscale of half-resolution volumetric fog, and needs no clear (full-screen overwrite).
- **NFS2015**:
  - HBAO pass writes an R8_UNORM by full-screen overwrite, no clear.
  - Tone Mapping + Bloom Resolve writes RGBA8_UNORM, also no clear.
- **KCD2**: RGBA8 clears cost 2-3 us versus the usual 8-10 us.
- **MGSV**: up to 1.0-5.0 ms lost to unneeded clears.
- **Days Gone**: up to 0.12 ms lost to quad/triangle clears.

### 3.2 G-buffer data budget slide ([37:53], `extra/resolve/t_02273`)

Status columns are "Decided", "Mostly Undecided Render Target Format" and "Very Temporary Data". The
fourth column records the effect on the pixel-shader output count.

| Bits | Content | PS outputs | Status |
|---|---|---|---|
| 64-bit | Emissive (48) + Luminance (16) | +1 (if emissive alpha testing) | Decided |
| 32-bit | Dual Normal (30) + Skin Stencil (2) (skin lighting only) | +1 (replaced with motion vectors) | Decided |
| 64-bit | Depth (32) + Stencil (8) + Unusable (24) | Free | Decided |
| 32-bit | Normal (30) + Lighting Channel (2) | +1 | Decided |
| - | "3 out of 8 pixel shader outputs" | - | - |
| 24 | Albedo/Base Color | lighting-stage input | Undecided |
| 8 | Material AO | lighting-stage input | Undecided |
| 8 | Roughness (1st anisotropy axis?) | lighting-stage input | Undecided |
| 8 (?) | 2nd anisotropy axis? | lighting-stage input | Undecided |
| 8 | Specular (reworked) | lighting-stage input | Undecided |
| 8 | Metal (reworked) | lighting-stage input | Undecided |
| 8 | Opacity/Cloth | lighting-stage input | Undecided |
| 13/16 | Material Expression ID | lighting-stage input | Undecided |
| 12 | Depth Bias | "RGBA16 is the best fit" | Very temporary |
| 48 | 3D Motion Vectors | "RGBA16 is the best fit" | Very temporary |
| 4/2 | 2xMSAA Coverage Mask | "RGBA16 is the best fit" | Very temporary |
| 3 | Shading Model | "RGBA16 is the best fit" | Very temporary |

The 3-bit shading model enumerates Cloth, Dual Normal Skin, Lit, Two-Sided Foliage, Subsurface, and
Other (unlit/forward-rendered).

### 3.3 Prepass experiments ([19:00]-[23:00])

- **Crysis 3**, alpha-tested tree:
  - **0.13 ms** with its prepass counterpart.
  - Without the prepass and with depth writes disabled: at least **13% faster**.
  - With depth writes enabled: about equal to the prepass cost alone, so "**40%**" more efficient.
- **Silent Hill 2 Remake** asset (about 250k triangles, massive G-buffer): **0.60 ms** with prepass
  versus **0.44 ms** without.
- **KCD2**: gained performance when its partial alpha-tested prepass was disabled.
- **Half-Life: Alyx**: full non-alpha-tested prepass **< 0.3 ms** in dense urban scenes.

### 3.4 Shadows ([39:00]-[47:00])

- **Crysis 3**: six cascades about **1.2 ms**; the far cascades are the cost drivers (static high-poly
  and alpha-tested content). The target is half.
- **KCD2**: 5 cascades, far ones updated across frames. Cloud-shadow projection is an extra **0.30 ms**.
- **Days Gone**:
  - Closest cascade at about 1080p density; FF CSM 2k x 2k.
  - 0.3 ms tree-atlas shadow pass.
  - Compute-generated RG16 shadow mask.
  - 4096 x 4096 example in the 1080p+8% figure.
- **Dead Space Remake**: 4k x 4k FF CSM.
- **Callisto Protocol, Crysis 3, Dead Space Remake**: quad-based shadow clears/copies (avoid them).

### 3.5 GI / SSS / lighting formats ([50:00]-[58:00])

- **Reflective shadow maps**: 0.3-0.6 ms per frame (Crysis 3, Tesseract); avoid them. Days Gone probe
  update from shadow maps costs **0.3 ms**.
- **Eyes**: Days Gone and Callisto extra eye layers cost **< 0.2 ms**.
- **SSS** (`extra/sssr/t_03332` [55:32], `t_03348` [55:48], `t_03404` [56:44]):

| Title / config | Format | Cost |
|---|---|---|
| Callisto Protocol (UE cmds) | native RGBA16 SSS | **1.40 ms** ("unnecessary copy for resolve pass instead of immediate work") |
| Callisto, `r.SSS.SampleSet` 2 to 0 | native RGBA16 | **0.78 ms** (banding, weak scattering) |
| Days Gone | native RG11B10 | **1.00 ms** ("unnecessary clear") |
| Days Gone, inputs forced to RGBA16 | native RGBA16 | **1.35 ms** |
| Crysis 3 2013, inputs forced to RGBA16 | native RGBA16 | **0.62 ms** |
| Crysis 3 2013, default | native RG11B10 (32-bit inputs) | **0.49 ms** |
| SSS sample set 0 / 1 / 2 | - | **0.23 / 0.31 / 0.39 ms** |

- Callisto's half-resolution SSS blended at 80% with the native non-blurred diffuse is about 3x faster.
  It is not recommended without an ID- and depth-aware upscale.

**"Number & Formats of Lighting Outputs"** (`extra/sssr/t_03460` [57:40]):

| Title | Lighting outputs | Order after lighting |
|---|---|---|
| Alan Wake II (first TI analysis version) | R11G11B10 diffuse + R11G11B10 specular | SS SSS, HDR R11G11B10 diff+spec resolve, transparents+VFX, SDR RGB10A2 tone map |
| Callisto Protocol (`r.SceneFormatColor X`) | diffuse + specular | SS SSS, HDR resolve, transparents+VFX, SDR tone map |
| Crysis 3 | R11G11B10 diffuse + R11G11B10 specular | HDR **RGBA16** resolve, SS SSS, transparents+VFX, two RGBA8 tone-mapped images (sample 0, sample 1) |
| Days Gone / NFS2015 | one R11G11B10 (diff+spec) | SS SSS, transparents+VFX, SDR RGB10A2 or RGBA8 tone map |
| Silent Hill 2 / Jedi Survivor (`r.SceneFormatColor X`) | one diff+spec | SS SSS, transparents+VFX, SDR tone map |
| Dead Space Remake | **RGBA16** diff+spec + **RGBA16** skin diffuse | SS SSS, transparents+VFX, SDR tone map |
| MGSV: Phantom Pain | **RGBA16** diffuse + **RGBA16** specular | SS SSS, SDR RGBA8 tone map, then transparents+VFX |
| KCD2 | **RGBA16** diff+spec + R11G11B10 skin diffuse | SS SSS, transparents+VFX, SDR RGB10A2 tone map |

- UE checkerboards diffuse and specular into one 32-bit R11G11B10 ([57:51]).

### 3.6 AA ([1:19:30]-[1:42:30])

- **Crysis 3 SMAA S2X** worst case on dense grass (1080p, Nsight `f_0326`, 13 draws selected): **990
  us**, PS invocations 10.2M, 7970k pixels rendered. It is usually far lower.
- Jimenez's SMAA optimizations give **~40%** more speed.
- **UE5.8 TSR Low, 1080p, history 200%**: **2.20 ms**, which the narration calls "more than 18 times more
  expensive" than the original temporal SMAA reprojection (so about 0.12 ms).
- **UE TAA**: about **0.3 ms** ("28% slower"); the no-wide-resolve reference configuration is **0.28 ms**.
- **Final Fantasy XIV TSCMAA** (CMAA2 + temporal): fattest formats. Resources are Current Frame Depth,
  RG16 static & dynamic velocity, Post-CMAA frame, CMAA edge/temporal blend mask, infinite accumulation
  buffer, previous-frame depth, TSCMAA output ([1:39:45], `f_0400`).
- Sample-pattern slides ([1:40:30]-[1:42:10]): "SMAA 4x (2xMSAA)" vs 4xMSAA; "SMAA 4x (2xMSAA)+PSL";
  "Decima 2x Sampling" (pixel's own custom sample locations plus neighbouring-pixel samples) vs 4xMSAA.

### 3.7 Motion blur ([1:43:00]-[1:56:00])

- **Days Gone, UE4.11 motion blur**:
  - Native **RG11B10** packed velocity plus a **120x68 RGBA16** max-tile written in the same draw
    (about 0.13 ms).
  - Dilation pass.
  - Unneeded clear of the motion-blur output.
- **MGSV**:
  - Half resolution (1080p from 4K), **RGBA8** velocity+neighbour-tile pack (RG = velocity with near
    precision, BA = bilinear max tiles), 120x68 neighbourhood tiles.
  - Uniform 3 samples per direction plus a separable pass (draws 5717 and 1718).
  - The separable pass costs about 40% more than UE's separable pass (draw 9208), but looks better.
- **NFS2015**: white-noise single-direction blur; no tiles.

### 3.8 Other numbers quoted

- UE Blueprints are up to 10x slower than C++.
- Shadow map ceiling: 2048x2048; local lights render at 1080p+8% density.
- Composite/VFX resources are half resolution (540p preferred for the downscaled frame).

---

## 4. Implications for our engine (D3D11, 1080p, 60 fps, RTX 3060-class)

Budget philosophy:

- Every pass has a hard ceiling taken from the chart.
- The ceilings sum to about 16.4 ms worst case without clouds. Our engine has no clouds yet, so we have
  about 0.5 ms of headroom.
- Each pass is timed with D3D11 timestamp queries and reported against its ceiling.

### 4.1 Resource set (persistent; MSAA 2x unless stated)

| Name | Format | Notes |
|---|---|---|
| `DepthStencil` | D32_FLOAT_S8X24 (D32S8), 2xMSAA | Reverse-Z. Stencil holds shading model, sky/sample-0 value, light-volume counter, lighting channel. |
| `GB_MotionMsaa` | R16G16B16A16_FLOAT, 2xMSAA | 3D motion vectors + depth bias + coverage mask + shading model; recycled later. |
| `GB_Normal` | R10G10B10A2_UNORM, 2xMSAA | Normal + 2-bit lighting channel/aniso flag. |
| `GB_AlbedoRough` | R8G8B8A8_UNORM_SRGB, 2xMSAA | Albedo + roughness (SSR variance, rain). |
| `GB_Material` | R8G8B8A8_UNORM (provisional) | Metal/spec/AO/material-ID. The chart leaves this undecided. |
| `L_EmissiveDiffuse` | R16G16B16A16_FLOAT, 2xMSAA | Emissive in base pass, then the diffuse light accumulator; A = luminance. Hardware cleared. |
| `L_Specular` | R16G16B16A16_FLOAT, 2xMSAA | Temp for resolve, then specular accumulator. Not cleared (overwritten). |
| `GB_DualNormalSkin` | R10G10B10A2_UNORM, 2xMSAA | Only if we implement skin; cleared. |
| `ShadowLocal` | D16_UNORM, <= 2048^2 per light (or atlas) | Perspective-fitted. |
| `ShadowCSM` | D16/D32, 3 cascades ~1300^2 | Receiver-culled. |
| `ShadowMask` | R8G8_UNORM, native non-MSAA | Sun shadow + SSS term. |
| `LDR_S0`, `LDR_S1` | R10G10B10A2_UNORM, non-MSAA | Tone-mapped sample 0 / sample 1. |
| `AA_Out` / `AA_History` | R10G10B10A2_UNORM | SMAA S2X result and previous frame. |
| `Velocity` | R11G11B10_FLOAT native + 120x68 RGBA16 tiles | Motion blur. |
| `HalfRes_*` | R16G16B16A16_FLOAT half-res | VFX, composite, bloom (recycled). |
| `ToneLUT` | 3D 32^3 RGB10A2 (scalable) or 64^3 RGBA16 | GT7-style curve. |

### 4.2 Pass list with budgets (implementation order and frame order)

| # | Pass | Budget | Key rules |
|---|---|---|---|
| 1 | Start-of-frame compute (only if needed) | <= 0.15 | No GPU skinning; CPU does simulation and culling. |
| 2 | Hardware clears | **0.15** | Use `ClearDepthStencilView` and clear only emissive/diffuse, dual-normal and motion/MSAA. Never clear with quads or triangles. Hijack-clear where a fullscreen pass already writes. |
| 3 | Partial depth prepass (large, close, opaque, non-alpha-tested; front-to-back) | **0.30** | Depth only, no texture sampling. Prepass emissive and dual-normal objects with motion vectors. |
| 4 | Base pass, alternated Z-sorted (large alpha-tested, small opaque, small alpha-tested, then prepassed with EQUAL) | **2.80** | 2xMSAA. Alpha-to-coverage for cutouts. Dithered LOD fade (2x2 Bayer, 500 ms). Emissive written in the same pass. |
| 5 | G-buffer resolve (sample frequency): shading model, sky and sample-0 stencil, depth bias | **0.50** | Classify redundant SV_Coverage samples so lighting shades once per pixel where possible. |
| 6 | Occlusion queries / light-bounds tests | **0.30** | Baked light volumes for static lights. |
| 7 | Deferred decals (no D-buffer) | **0.30** | Write the G-buffer directly. |
| 8 | Local-light shadow maps (perspective-fitted, 1080p+8% density, <= 2048^2, D16) | **1.00** | Cull casters that cannot affect the view; back-face culling by default. |
| 9 | Local lights: stencil volume, then one fullscreen triangle per shading model with early stencil | **1.50** | PCF and screen-space shadows in the lighting shader; outputs to L_Diffuse/L_Specular RGBA16. |
| 10 | Sun: 3 CSM (~1300^2, receiver-culled) 0.60 + CS screen-space shadows 0.32 + RG8 mask 0.18 + per-model shading 0.36 | **1.46** | Crysis 3 sample-frequency mask trick. |
| 11 | Eyes / hair (forward) | **0.15 / 0.30** | Optional. |
| 12 | GI: probe/irradiance-sensor GI from shadow maps (world) + SSGI/SSAO (screen) | **1.0 + 1.0** | Start with baked probes + SSAO. No RSMs. |
| 13 | Reflections: stochastic SSR (CS) + optional world-space RT + temporal composite | **0.5 + 1.0 + 0.3** | Start with SSR only (~0.4 ms target). |
| 14 | Screen-space SSS, diffuse only, 2 separable draws, stencil-tested | **0.30** | |
| 15 | Composite diffuse+specular+fog into the 2xMSAA RGBA16 | **0.12** | Skip sky pixels. |
| 16 | Sky (stencil sky value) | **0.18** | Clouds 2.90 are outside the core budget. |
| 17 | Transparents (decals, water first, forward-lit, back-to-front) | **0.15** | |
| 18 | VFX (half-res min-max depth shading, writes responsive mask) | **1.00** | |
| 19 | DOF (optional) | 0.30 | |
| 20 | Tone map + MSAA separation into 2x RGB10A2 (sample 0/1), LUT, auto exposure from diffuse-only buffer | **0.25** (target; 0.93 upper bound) | GT7-style curve. |
| 21 | SMAA S2X (compute) on both samples, combined | **0.38** | |
| 22 | Temporal SMAA 4x (2-frame, narrow resolve, velocity weighting + responsive mask), or PSL Decima pattern | **0.25** | |
| 23 | Motion blur: velocity pack + uniform blur + separable pass, single direction, depth-limited | **0.30** | |
| 24 | Bloom downscale (reuse the convolved frame for SSR next frame) | **0.38** | |
| 25 | Half/full-res composite effects + merged post (grain, vignette, CA) | **0.15** | |
| 26 | UI + gamma | **0.10** | |
| | **Total (no clouds)** | **~16.4 ms** | Matches the chart's "59 FPS worst case". |

### 4.3 Concrete engineering consequences

1. **D3D11 only.** The chart reasons from DX11 tooling, and the narration reports 2x faster pixel
   shading in DX11 than DX12 in their UE tests.
2. **2xMSAA deferred** throughout until tone mapping:
   - Lighting runs at sample frequency only where the resolve-stage stencil marks real edges.
   - Plan on per-sample stencil (`SV_SampleIndex`), `SV_Coverage` output, and alpha-to-coverage.
3. **Stencil is a first-class resource**: shading-model IDs, sky/sample-0 marker, light-volume
   increment/decrement, and lighting channels. Budget at most 5-7 shading models, since each costs one
   draw per light.
4. **No D-buffer, no quad clears, no RSMs, no GPU skinning, no accumulation-buffer TAA, no blue-noise or
   IGN motion blur.** These are the chart's explicit red items.
5. **Lighting buffers are RGBA16F.** Do not use R11G11B10 for diffuse/specular; R11G11B10 is only for
   velocity, SSR and bloom intermediates.
6. **Shadow density rule**: fit each local-light shadow frustum to the view (perspective shadow maps),
   cap at 2048^2 D16, and keep sun cascades at about 1300^2 with receiver culling. Target 0.6 ms for the
   sun shadows versus Crysis 3's 1.2 ms.
7. **Debug views are part of the deliverable**:
   - Quad-overdraw view (per pass, and prepass-aware).
   - Per-light shadow overdraw view.
   - Per-pass GPU timings shown against the budget table above.
8. **Open/tentative items to prototype later**, matching the chart's red boxes:
   - Software mesh occlusion (0.25 ms).
   - PSL Decima resolve replacing temporal SMAA.
   - Per-sample non-MSAA lighting targets.
   - Min-max half-res VFX.
   - Real-time cubemaps.
   - Early (Fox Engine-style) tone mapping in the composite.
