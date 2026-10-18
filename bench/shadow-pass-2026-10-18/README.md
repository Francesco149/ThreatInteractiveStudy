# Shadow optimization pass (2026-10-18, evening)

Raw benchmark output behind `docs/PERF.md` "Shadow optimization pass" and
`docs/techniques/shadows.md` 2.5, 2.9, 2.10 and 3. RTX 5060, 1920x1080, 2x MSAA, SMAA 4x, courtyard.

| File | Clock | Build | What |
|---|---|---|---|
| `175833_smoke-baseline`, `175958_smoke-baseline` | locked (1520-1540 MHz under load) | 62a8c03 (before this pass) | default frame, runs 1 and 3 of old/new/old/new |
| `175916_smoke`, `180041_smoke` | locked | this pass | default frame, runs 2 and 4 |
| `180149_localshadows` | locked | this pass | local shadow A/Bs: far clip, cache, wind off, density, cards |
| `181034_localshadows-stats` | locked | this pass | the same with `--stats` (pixel-shader invocations) |
| `180608_shadows` | locked | this pass | cascade A/Bs with the new local maps |
| `175353_localshadows-stock` | stock (87-90 C) | this pass | the local shadow A/Bs at stock clocks |
| `170146_localdiag` | stock | temporary diagnostic build | default / no wind-swayed casters in the local maps / only wind-swayed casters / default |
| `170543_localdiag` | stock | temporary diagnostic build | as above plus back-to-front caster order and depth clip on (`--stats`) |
| `171350_localdiag` | stock | temporary diagnostic build | depth clip on / far-plane clip distance / near-plane clip distance (`--stats`) |
| `172150_localdiag` | stock | temporary diagnostic build | far-plane clip / clip at B's exact maximum depth (+2%), the tighter variant not kept then (`--stats`); it became the default once the wind sway was bounded, `bench/wind-pass-2026-10-18/` |
| `181550_alphadiag` | locked | temporary diagnostic build | alpha test from a coarse mip (upper bound for an opacity-only BC4 texture) vs solid cards |

The diagnostic builds added a settings field and benchmark entries that were removed again; the
far-plane clip of `171350`/`172150` ("far clip distance") is the one this pass keeps. Every file
compares its configurations within one process, the default first and (where present) repeated
last to show the drift.
