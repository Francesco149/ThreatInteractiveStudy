# Wind sway bound and receiver-depth caster clip (2026-10-18, late evening)

Raw benchmark output behind `docs/PERF.md` "Wind sway bound and receiver-depth clip" and
`docs/techniques/shadows.md` 2.5, 3.2 and section 5. RTX 5060, 1920x1080, 2x MSAA, SMAA 4x,
courtyard. GPU clock locked by the user at its 1485 MHz V/F point (read 1507-1537 MHz, 50-63 C
under load).

| File | Build | What |
|---|---|---|
| `185020_smoke-baseline`, `185145_smoke-baseline` | 471567f (before this pass: unbounded sway, far-plane clip) | default frame, runs 1 and 3 of old/new/old/new |
| `185103_smoke`, `185226_smoke` | this pass | default frame, runs 2 and 4 |
| `185307_localshadows` | this pass | local shadow A/Bs in one process, incl. the far-plane clip against the receiver clip |

The first baseline run is ~0.26 ms slower over the whole frame than the second (base pass and
lookups too), the usual between-process noise; the passes this pass touches are compared run by
run in PERF.md.
