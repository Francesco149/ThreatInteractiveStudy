# Performance measurements

All numbers on this page were measured on:

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 5060 8 GB (Blackwell), driver 610.88 |
| CPU | AMD Ryzen 9 5900X (12 cores), 64 GB RAM |
| OS | Windows 11 Pro 26200 |
| API | Direct3D 11.1, feature level 11_1 |
| Resolution | 1920x1080 internal (TI's target), 2x MSAA unless stated |
| GPU clock, pass 1 ("locked") | graphics clock locked by the owner at the card's second-lowest V/F point: a flat 1485 MHz under load, memory 14001 MHz (GDDR7, 448 GB/s), ~50 W. At this clock the card has about 11.4 TFLOPS FP32, close to a stock RTX 3060's 12.7 (the GPU TI's chart budgets are for), with more memory bandwidth (448 vs 360 GB/s). |
| GPU clock, pass 2 ("stock") | unlocked: boosts to ~2.8 GHz, hits the 145 W limit and then 87-90 C within a minute, after which it holds the temperature at 2355-2790 MHz (mean 2507 MHz, ~128 W). Times depend on how long the GPU has been loaded (see the noise floor). |

**Suspected PCIe flakiness.** This RTX 5060 is suspected of an unstable PCIe link: on 2026-10-18
it crashed the display driver twice during development, once with bugcheck 0x119
(VIDEO_SCHEDULER_INTERNAL_ERROR) while several GPU jobs ran at once, and once with a "BusReset TDR"
(nvlddmkm event 153) under a single engine instance. Every benchmark here ran afterwards without a
driver event, one engine instance at a time, and repeated runs agree within the noise floor below.
Still, a marginal link can add stalls and run-to-run variance that a healthy card would not show,
so read the numbers as indicative for this machine rather than as reference values for the GPU.

## Method

- GPU time comes from D3D11 timestamp queries around every pass (`profiler_push/pop` in
  `src/gpu.cpp`), resolved four frames later; nested scopes give per-sub-pass costs.
- The benchmark (`--bench NAME`, `src/bench.cpp`) runs each configuration on each view: the named
  viewpoints of `assets/scene/bench_path.json` (static camera) and the 40 s camera path (compressed
  into the measured frames). Each run: 90 warm-up frames (query latency, clocks, exposure), then 300
  measured frames. Time and animation advance in fixed 1/60 s steps by frame count, so every
  configuration renders exactly the same frames, particles included. VSync off, tearing allowed.
- Reported: mean, **median** (the headline number: robust to the odd spike), p95 of the whole-frame
  GPU time, mean CPU submit time, and the mean of every pass.
- Pixel-shader invocation counts come from `D3D11_QUERY_PIPELINE_STATISTICS` per scope (`--stats`),
  measured in separate benchmark runs (the queries themselves cost a little); with `--stats` the
  benchmark CSVs carry them as `<pass> [PS invocations]`.
- Each pass is one `--bench all` run (every configuration of every benchmark, each applied on top
  of the defaults), standalone `gi`, `motionblur`, `sss`, `vfx` and `lod` runs (see the noise floor),
  three `--bench smoke` runs of the default configuration (the frame vs the chart budgets), `--stats`
  runs of the benchmarks where pixel work is the question, and `--no-bc` / `--no-mesh-opt` smoke runs
  for the load-time A/Bs. `tools/perf_report.py` turns the CSVs into tables; it splits the `all` run
  back into its benchmarks.
- The same set was run twice: at the locked 1485 MHz clock (14:44-15:58) and at stock clocks
  (16:07-16:44). **A/B verdicts use the locked pass**: its clock is constant, so differences between
  configurations are not mixed with thermal drift, and its shader throughput is close to the chart's
  RTX 3060. The stock pass gives this card's real speed; every verdict holds there too (every pass
  is 1.4-1.8x faster, no ordering between configurations changes).
- Runs were done with nothing else using the GPU: while background jobs (Blender, other engine
  instances) were running, p95 values were 3-5x the medians, which is why those exploratory runs are
  not reported.

Raw results: `bench/locked-1485mhz/` (pass 1) and `bench/*.csv` (pass 2), one `.csv` + `.md` pair
per benchmark run. The later shadow optimization pass is in `bench/shadow-pass-2026-10-18/`
(section "Shadow optimization pass"). Exploratory runs taken earlier under GPU contention are kept
out of the report in `bench/exploratory/` (not committed).

## Results

Measured 2026-10-18. Full tables (every configuration on every view, the focus passes,
pixel-shader counts): [perf-tables.md](perf-tables.md) (locked pass) and
[perf-tables-stock.md](perf-tables-stock.md) (stock pass). Screenshots of the views: [images/](images/).
Numbers below are from the locked pass unless they say "stock".

### Noise floor

Read every difference below against these two effects:

- **Between processes, about 3% (~0.2 ms).** Three `--bench smoke` runs of the same default
  configuration measured 7.44, 7.25 and 7.25 ms (mean of the view medians). The difference is spread
  evenly over most passes (3-5% each) at the same 1485 MHz clock. The default row of each standalone
  benchmark lies in the same band (7.24-7.46 ms). So only differences larger than ~0.2 ms in the frame
  are meaningful between separate runs. Inside one run, configurations are compared at the same state.
- **Re-created resources, up to ~0.15 ms per pass.** Toggling a setting that re-creates GPU resources
  can change the cost of *other* passes that read them for the rest of the process. Switching GTAO
  from half to full resolution re-creates its targets: GTAO went from 0.64 to 0.74 ms for the same
  configuration (one fresh process also started at 0.72 ms). Toggling the probe visibility test
  re-lights the probe grid, and "Sun + ambient" then measured 0.94 instead of 0.79 ms. This is memory
  placement or compression of the new textures, which D3D11 does not expose. The `gi` benchmark
  therefore runs its default first and the visibility toggle last, and `gi`, `motionblur`, `sss`, `vfx`
  and `lod` were run as separate processes: in the single `--bench all` run they followed the GI
  toggles and all read ~0.25 ms high (the reason this section uses their standalone runs).
- **At stock clocks, thermal drift of about +-5%.** The card reaches its temperature limit within a
  minute and then lowers its clock, so the same default configuration measured 4.32-4.34 ms in the
  short smoke runs (each starts after a cooler loading phase) and 4.38 -> 4.65 -> 4.79 ms as the long
  `all` run went on. That is larger than many A/B differences, hence the locked pass for verdicts.
- Formats were double-checked where this could mislead: D24 vs D32F depth was repeated in one
  process in the order D24, D32F, D24, D32F, and gave the same result both times (below).

### The frame vs TI's chart budgets

Default configuration (`--bench smoke`, 1920x1080, 2x MSAA, SMAA 4x, everything on), median GPU
frame time per view:

| view | square overview | foliage dense | busts close-up | lamps | arcade | skyline | cafe | market | camera path | **mean** |
|---|---|---|---|---|---|---|---|---|---|---|
| locked 1485 MHz (ms) | 10.59 | 12.02 | 6.39 | 5.69 | 4.08 | 8.67 | 5.55 | 7.24 | 6.72 | **7.44** |
| stock clocks (ms) | 6.05 | 6.94 | 3.80 | 3.40 | 2.50 | 4.89 | 3.36 | 4.23 | 3.92 | **4.34** |

At stock clocks the worst view runs at 144 fps and the mean at 230 fps; after a long sustained load
(thermally limited, ~2.45 GHz) the mean rises to ~4.8 ms.

TI's chart adds up to about 16.4 ms for a worst-case 1080p frame on an RTX 3060 (without clouds), and
the passes this engine maps onto the chart have 13.25 ms of budget. The locked pass, with about an
RTX 3060's shader throughput, is the fairer comparison; its worst view is 12.0 ms. Per pass (mean
over views, worst view, budget):

| pass | locked mean | locked worst | stock mean | TI budget | locked vs budget |
|---|---|---|---|---|---|
| Shadows (sun cascades + local maps) | 2.40 (1.13 + 1.27); after the shadow pass 1.96 (1.09 + 0.86) | 4.89 (foliage dense); after: 3.81 | 1.31 (0.61 + 0.70); local maps after: 0.51 | 1.60 (0.60 + 1.00) | **over** (the cascades; the local maps now fit on average) |
| Base pass | 1.22 | 3.19 (foliage dense) | 0.72 | 2.80 | within on average; the foliage view is over |
| Sun + ambient (incl. GI probe lookups) | 0.81 | 0.92 | 0.50 | 0.54 | over; the probe lookups (0.50) belong to GI's 1.00 world-space share, without them 0.30 |
| Local lights | 0.76 | 1.21 (skyline) | 0.42 | 1.50 | within |
| GTAO | 0.64 | 0.72 | 0.39 | 1.00 (GI's screen-space share) | within |
| Anti-aliasing (SMAA 4x) | 0.63 | 0.79 (market) | 0.35 | 0.63 | at the budget |
| Motion blur | 0.24 | 0.31 | 0.17 | 0.30 | within (worst view at it) |
| Bloom | 0.22 | 0.22 | 0.12 | 0.38 | within |
| Compose (+ height fog) | 0.17 | 0.20 | 0.12 | 0.12 | over by 0.05 |
| Tone map (+ exposure 0.04) | 0.13 | 0.14 | 0.09 | 0.25 | within |
| Transparents + VFX | 0.13 | 0.17 | 0.08 | 1.15 | within |
| Depth pre-pass | 0.08 | 0.10 | 0.04 | 0.30 | within |
| MSAA edge stencil | 0.07 | 0.08 | 0.05 | 0.50 | within |
| Final output + UI composite | 0.05 | 0.05 | 0.03 | 0.25 | within |
| Clears, decals, SSS, sky | 0.02, 0.005, 0.02, 0.02 | | 0.01, 0.003, 0.01, 0.01 | 0.15, 0.30, 0.30, 0.18 | within |

The frame is dominated by **shadows** (32% of the mean frame, 41% in the foliage view). In the locked
pass the cascades are about twice their budget on average and the local maps 27% over (2.2-2.8 ms,
up to 2.8x, in the three wide views); at stock clocks both fit on average, but the foliage view still
needs 2.68 ms. The cause is the same for both: depth fill through dense, wind-swayed foliage canopies
(see "Shadows: where the time goes"). The later shadow optimization pass clipped the local views'
casters at the far plane: local maps 0.86 ms (within budget on average), shadows 1.96 ms, frame
-0.30 ms (section "Shadow optimization pass").

### Clock scaling: which passes are bound by what

The memory clock was the same in both passes (14001 MHz) and the graphics clock ~1.69x higher at
stock (2507 vs 1485 MHz mean). A pass that speeds up by about that much is limited by the core
(shaders, rasterization, ROPs); one that speeds up much less is limited by memory bandwidth.
Default configuration, mean over views (locked / stock):

| speed-up | passes |
|---|---|
| 1.7-1.8x (core-bound) | shadows 1.83, depth pre-pass 1.84, clears 1.80, local lights 1.79, anti-aliasing 1.79, bloom 1.77, SSS 1.74, base pass 1.71, final output 1.71, transparents + VFX 1.70 |
| 1.6-1.7x | GTAO 1.66, sun + ambient 1.63 |
| 1.4-1.5x (largely memory-bound) | exposure 1.52, motion blur 1.43, MSAA edge stencil 1.43, tone map 1.42, compose 1.38 |

The memory-bound group is the passes that read every MSAA sample of the full-screen HDR buffers
(compose, tone map, the edge stencil) or gather many full-screen taps (motion blur): on a GPU with
less bandwidth per FLOP they would weigh more.

### TI's claims, tested

Locked-pass numbers. The stock pass reaches the same verdicts with every number 1.4-1.8x smaller
([perf-tables-stock.md](perf-tables-stock.md)).

| # | claim (video) | test | result | verdict |
|---|---|---|---|---|
| 1 | Clear only what needs clearing; never clear with full-screen draws (8:00) | `clears` | minimal 0.022 ms; every target with hardware clears 0.053 ms; with full-screen draws 0.257 ms (+0.23 ms frame) | **confirmed**: the full-screen-draw anti-pattern costs 12x minimal clears |
| 2 | A partial pre-pass of large, non-alpha-tested occluders (14:21) | `prepass`, `prepass-stats` | base-pass pixel shading 20.3 M -> 18.2 M (-11%); pre-pass + base 1.31 ms vs 1.38 ms without; same as a full opaque pre-pass (1.30 ms); alpha-tested casters in the pre-pass +0.17 ms frame | **confirmed, small**: right choice, the gain is ~0.07 ms; pre-passing alpha-tested geometry is a loss |
| 3 | TI's base-pass order (28:53) | `order`, `order-stats` | base pass 1.23 ms vs 1.28 (front to back) and 1.33 (by material); fewest shaded pixels (18.2 vs 18.7-18.8 M) | **confirmed**: -0.10 ms vs material order |
| 4 | Stencil-volume local lights with a scissor, per shading model (41:00) | `lights` | 0.76 ms; no scissor 0.93; scissor only 0.89; full-screen without culling 1.15; **one uber-shader draw per light 0.40**; Fox Engine volume mesh 0.45 | **partly**: culling and scissor pay off, but per-shading-model draws lose to one draw per light here (per-draw rasterization of the scissor rectangle dominates with these light shading models) |
| 5 | 2x MSAA with edge-detected per-sample shading (36:44) | `msaa` | +1.24 ms over 1x; per-sample everywhere +1.64; 4x +3.11. Sun + ambient 0.80 vs 1.30 ms, compose 0.17 vs 0.30 | **confirmed**: edge detection saves 0.40 ms. (Local lights alone are cheaper per-sample everywhere, 0.57 vs 0.75 ms, for the same draw-count reason as #4) |
| 6 | Reversed-Z float depth (D32F S8) | `depth` + repeat | D24 S8 is **faster**: base pass 1.05 vs 1.22 ms (-14%), frame -0.19 ms, same in both orders | **cost measured**: D32_FLOAT_S8X24 is 64 bits per sample; float precision with a stencil costs ~0.17 ms here |
| 7 | SMAA 4x = S2x + 2-frame T2x within 0.38 + 0.25 ms (1:19:38) | `aa` | AA pass 0.63 ms (S2x 0.56, T2x 0.34, 1x 0.29); +0.57 ms frame vs no AA | **confirmed**: exactly at the budget |
| 8 | Sun cascades within 0.60 ms; D16; tight caster culling; amortize far cascades (39:00) | `shadows`, `shadows-stats` | cascades 1.13 ms (4x2048 D16); D32F +0.51 ms cascades and +54% pixel work; slice culling saves 0.61 ms (1.73 -> 1.13); KCD2 amortization -0.08 ms; 9-tap PCF +0.02 ms in the lookup; 4x1024 -0.55 ms total | **over budget**; D16 and slice culling are clear wins, amortization small |
| 9 | Distant foliage as solid cards in shadow maps (Days Gone, 46:40) | `shadows`, `localshadows` | main pass: cascade pixel work 81.6 -> 40.3 M, but cascades only -0.11 ms, local maps +0.18 ms; all foliage as cards: local-map pixel work 55 -> 0.3 M for -0.01 ms. After the far clip: distant cards -0.21 ms frame (cascades -0.11, local maps -0.12); all foliage as cards: local maps 0.86 -> 0.55 ms | **confirmed once the local maps clip at the far plane** (before, clamped geometry beyond it hid the gain); a visible trade-off (blob shadows), so an option |
| 10 | Density-synced local-light shadow maps ("1080p + 8%") within 1.00 ms (39:15) | `localshadows` | main pass: fitted 1.27 ms (worst view 2.84 ms, foliage dense); fixed 1024 per face 0.92 ms; fitted at half density 0.89 ms; off saves 1.22 ms frame. With the far clip: fitted 0.86 ms (worst view 1.84 ms); clipped at the receivers 0.77 ms (worst view 1.56 ms) | **within budget on average** after the shadow pass; the three wide views still 1.6-1.8x |
| 11 | Tone map within 0.25 ms; GT7 curve; bake to a LUT (1:04:45) | `post` | GT7 through the 64^3 LUT 0.13 ms; evaluated per sample 0.40 ms; other curves cost the same through the LUT | **confirmed**: the LUT saves 0.26 ms and makes the curve's cost irrelevant |
| 12 | Bloom within 0.38 ms (CoD:AW) | `post` | 0.22 ms (bloom off -0.22 ms frame) | **confirmed** |
| 13 | Baked probe GI + screen-space AO within 1.0 + 1.0 ms, no RSMs (49:45) | `gi` | probe lookups +0.50 ms in "Sun + ambient" (visibility test 0.18 of it); GTAO 0.65 ms full res, 0.22 half; GI + AO together +1.12 ms frame vs sky ambient only; relighting every frame 6.6 ms | **confirmed for a static sun**; a per-frame relight is 6.6x the world-space share (on demand only: ~11 ms once when the sun moves) |
| 14 | Trailing, separable motion blur within 0.30 ms (1:42:40) | `motionblur` | 0.23 ms; one pass 0.17; centered 0.15 | **confirmed**: the separable second pass costs 0.06 ms |
| 15 | Stencil-masked SSS within 0.30 ms (53:48) | `sss` | 0.02 ms (two busts on screen) | **confirmed** (light scene) |
| 16 | Half-resolution particles with min/max depth and a hijack clear (1:02:00) | `vfx` | half res 0.13 ms vs **full res 0.06 ms**; hijack vs separate clear 0.128 vs 0.118 ms | **not in this scene**: 11 small emitters cover too few pixels; the min/max depth downsample (0.03 ms) and the upsample (0.07 ms) cost more than half-res shading saves |
| 17 | Dithered LOD cross-fades | `lod` | LODs -0.05 ms vs none; dithering costs 0.04 ms vs hard switches | small either way in this scene |
| 18 | BC7 textures | `smoke` / `smoke-nobc` | runtime 7.41 vs 7.44 ms (noise); scene load 3.08 s -> 0.36 s; 1/4 texture memory | **load time and memory only** |
| 19 | Vertex-cache / overdraw triangle order (meshoptimizer) | `smoke-nomeshopt` | runtime 7.39 vs 7.44 ms (noise); base pass 1.24 vs 1.22 ms | **no measurable effect** on this GPU and scene |
| 20 | Texture + lighting jitter compensation | `frame_diff.py`, earlier session | static-scene frame difference 3.0 -> 0.49 -> 0.19 (8-bit), changed pixels 37% -> 5.7% -> 2.0% | **confirmed** (deterministic) |
| 21 | Don't copy cached static shadow maps; re-rendering is faster on current hardware (R3 4.8) | `localshadows` (shadow pass) | a copy-free cache (tiles sampled in place, second atlas slice): 0.86 ms with and without; every expensive view has wind-swayed casters; static casters alone cost 0.05-0.11 ms (stock) | **consistent**: even without copies, caching has nothing to save while foliage sways (wind off: -16%) |
| 22 | Fade distant lights' shadows (R3 4.9: below ~96 px; TI on UE's missing fade, Days Gone 13:56) | shadow pass | implemented (fade to unshadowed); the smallest whole-view size in the courtyard is ~385 texels | **not exercised**: the courtyard is too small for it to trigger |

### Shadows: where the time goes

The local maps were investigated with per-projector GPU timers at the overview viewpoint (23
projectors, 17.6 M atlas texels):

- One lamp face inside a tree canopy took 1.1 ms on its own (39.5 M pixel-shader invocations in 3 M
  texels: about 13 layers of leaf cards); six more faces took 0.2-0.34 ms; the other 16 were
  negligible.
- With all foliage as solid cards (no pixel shader at all) the same projectors still cost 2.4 ms: the
  canopies' depth complexity at the resolution the farther receivers demand.
- Alpha-tested shadow casters now sample with plain trilinear instead of the 16x anisotropic sampler
  (the research note's "opacity-only, cheap sampling" advice): local maps -13 to -17% in the wide
  views. An opacity-only BC4 texture is the next step.
- The density rule does what TI describes (every visible receiver gets >= 1.08 texels per 1080p
  pixel). A fixed 1024 per face is cheaper on average (0.92 vs 1.27 ms) but undersamples receivers
  near the camera; a fixed setup with the same worst-case density (2048 per face) does not fit an
  8192^2 atlas. Halving the density brings the fitted maps to 0.89 ms.
- For the sun, slice culling is worth 0.6 ms and D16 0.5 ms; the remaining 1.13 ms (vs 0.60) is
  again foliage depth fill in cascades 0-1.

### Shadow optimization pass

Done after the main passes, with the GPU locked again (it read 1520-1540 MHz under load this time).
Raw files: `bench/shadow-pass-2026-10-18/`. Details: [shadows.md](techniques/shadows.md) 2.5, 2.9,
2.10 and 3.

**Result.** Same-session before/after, `--bench smoke` in the order old build (62a8c03), new, old,
new. The two runs of each build agree to within 0.01-0.04 ms per pass. Mean over the 9 views:

| | before | after | TI budget |
|---|---|---|---|
| Local shadow maps (atlas pass) | 1.24 | **0.87** (-30%) | 1.00 |
| Sun cascades | 1.10 | 1.10 | 0.60 |
| Shadows total | 2.34 | **1.97** | 1.60 |
| Local-light lookups (lighting pass) | 0.74 | 0.74 | |
| GPU frame (mean of the view medians) | 7.30 | **7.00** | |

Local maps per view: foliage dense 2.75 -> 1.84 ms, square overview 2.48 -> 1.69, skyline
2.18 -> 1.56, camera path 1.41 -> 0.90 (locked `localshadows`, with and without the clip in one run).
At stock clocks, 0.72 -> 0.51 ms on average. The "before" values are ~3% below the main pass
(1.27 / 2.40 / 7.44), which is the noise floor between processes.

**What was measured and what helped:**

1. **Where the time goes.** Per-projector GPU timers plus in-process A/Bs that drop one class of
   casters showed that wind-swayed foliage is 77-94% of the local-map cost wherever there is any.
   Without it the pass costs 0.05-0.11 ms in every view (stock). Drawing *only* the swaying casters
   is slower than drawing everything (1.67 vs 1.38 ms in the overview, stock): the static casters,
   drawn first, occlude foliage fill. Reversing the caster order costs +45%.
2. **Clip casters at the far plane: kept, -30%.** The local views render with depth clip off so
   that casters in front of the near plane are pancaked. That also clamps everything beyond the
   far plane to depth 0, where it was still rasterized and alpha-tested only to fail the depth test.
   A clip distance in the shadow vertex shader (`SV_ClipDistance0 = z`) removes it before
   rasterization. The maps are unchanged: screenshots differ by no more than two runs of the same
   build do. Full depth clipping would also drop casters between the light and the near plane: it
   is wrong, and in some views slower.
3. **Cache static local maps: implemented as an A/B (off), no gain here.** Tiles are kept in a
   second atlas slice, sampled in place, never copied or quad-cleared.
   - With the courtyard's wind, 0.86 ms with and without the cache: every expensive view contains
     swaying casters, and LOD cross-fades and the animated props add more dynamic views.
   - Re-rendering the moving views only every 2nd frame gives 0.70 ms (-19%), with a one-frame lag
     in their shadows.
   - With wind off the cache saves 16% (0.85 -> 0.72 ms).
   - A single-slice first version thrashed: the views that render every frame used up the atlas
     rows that were still clear every few frames.
4. **Per-light distance fade (96 px): implemented, never triggers.** The smallest whole-view size
   at any viewpoint is ~385 texels; a lamp would have to be ~170 m away.
5. **Opacity-only BC4 for alpha-tested casters: not built, bound measured.** A diagnostic build
   took the alpha test's texture fetch from a coarse mip, which leaves almost no texture traffic.
   - That saved 0.05 ms on the local maps and 0.14 ms on the cascades (locked, one process).
   - A BC4 opacity texture halves the bytes of the BC7 albedo, so it would recover less than that
     bound, ~0.1 ms at best.
   - Solid cards (no pixel shader) saved 0.32 + 0.15 ms in the same run. The rest of the alpha
     test's cost is the discard and the late depth writes it forces, which BC4 would not change.

**Revised understanding.** The main pass concluded "depth fill, not the alpha test": all foliage
as solid cards removed the pixel work but not the time. Part of that fill was the clamped
geometry beyond the far plane, which costs the same with or without the alpha test. With the clip,
solid cards save 36% of the local maps, and distant foliage as cards (20 m) now wins overall
(-0.21 ms frame). Both change the look, so they stay options.

**Left over budget:** the sun cascades (1.09 vs 0.60 ms), again foliage in cascades 0-1, and the
local maps of the three wide views (1.6-1.8 ms).
- Next levers: proxy or impostor casters for tree crowns (Days Gone); coarser foliage shadow LODs
  (a visible trade-off against TI's main-view LOD rule).
- Done since: bounding the wind sway, then clipping the local casters at the receivers' exact
  depth (next section).

### Wind sway bound and receiver-depth clip

Done after the shadow pass, same locked clock (1507-1537 MHz under load). Raw files:
`bench/wind-pass-2026-10-18/`. Details: [shadows.md](techniques/shadows.md) 2.5, 3.2 and section 5.

**The bug.** The vertex wind sway grew with the square of the object-space height, which moved the
top of the 11 m linden trees by up to 4.4 m and the wall ivy by up to 2 m. The crowns leaned and
sheared ([images/wind_sway_before_after.jpg](images/wind_sway_before_after.jpg)), and nothing
accounted for it: the instance bounds ignored the sway and the local shadow fit padded receivers by
10 cm. Swaying receivers could leave their shadow tile or go past its far plane, swaying casters
could leave the caster volume, and the main view could cull a visible treetop.

**The fix.** Meshes taller than 2.5 m get a smaller bend so that their top sways at most 25 cm
(peak 21 cm); the small plants keep their old motion. The bounds of wind-swayed instances include
the sway (28 cm for the trees). That makes B's depth range, the deepest visible receiver point, a
safe place to clip local shadow casters, tighter than the quarter-octave-rounded far plane.

**Result.** `--bench smoke` in the order old build (471567f), new, old, new; mean over the 9 views:

| | before | after | TI budget |
|---|---|---|---|
| Local shadow maps (atlas pass) | 0.88 / 0.85 | **0.77** / 0.77 (-11%) | 1.00 |
| Sun cascades | 1.12 / 1.08 | 1.10 / 1.10 | 0.60 |
| Shadows total | 2.00 / 1.93 | **1.88** / 1.87 | 1.60 |
| Local-light lookups (lighting pass) | 0.75 / 0.72 | 0.72 / 0.72 | |
| GPU frame (mean of the view medians) | 7.07 / 6.81 | 6.82 / 6.74 | |

In one process (`localshadows`), the receiver clip against the far-plane clip: 0.77 vs 0.84 ms
(-8%), up to -11% in the foliage and overview views (1.76 -> 1.56, 1.64 -> 1.46 ms). The maps are
unchanged: with the sun off, screenshots at all 8 viewpoints differ between the two clips no more
than two runs with the same clip do. The bounded sway on its own is within the noise between
processes, except in the cafe view (0.61 -> 0.45 ms). The first baseline run was ~0.26 ms slower
over the whole frame than the second, so the frame line is noise-dominated; the shadow lines
repeat to within 0.03-0.07 ms.

The cache (2.10) clips at the far plane, because a kept tile is reused while the receivers change.
Against the new default it loses 8% with wind on (0.83 ms) and saves 6% with wind off.
