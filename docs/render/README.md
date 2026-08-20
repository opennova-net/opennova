# docs/render/ — the REN track's RE-record home

The render visual-parity track ([maturity-program.md](../maturity-program.md)
REN; standing rules [ADR 0023](../adr/0023-render-visual-parity.md)). Records
land here as the grill slices convert the three `UNAUDITED` render systems
([divergence-ledger.md](../divergence-ledger.md) audit track):

| Record | Lands at | Catalog | Covers |
|---|---|---|---|
| [`render-material-re.md`](render-material-re.md) | **landed at REN-2** | D-RMAT | the runtime flag/tag→state path down to the device boundary (registry @ 0x5af790/0x5ae690, resolution @ 0x5b03c0, state application @ 0x5d9f50; the planning anchor "Entity_UpdateRenderState @ 0x5d6a30" resolved at REN-5 to the render-slot light updater, renamed `RenderSlot_UpdateEntityLight`) |
| [`render-order-re.md`](render-order-re.md) | **landed at REN-3** | D-RORD | batching, sort keys, technique-class selection, the render-state stack, and the frame pass sequence (Render_SubmitEntity @ 0x5dad80, CRenderBatchQueue_SortAndFlush @ 0x5dae40, Terrain_RenderSceneWithReflection @ 0x5c93a0) |
| [`render-lighting-re.md`](render-lighting-re.md) | **landed at REN-5** | D-RLIT | the iris/modulator chain (env #17), the world lighting block + per-entity uniforms and hemisphere D3D lights, dynamic point lights + group culling, terrain/foliage c0/c1, lighting textures, the cubemap sources (CubeRotSpecular = D-RORD-5's answer), the render-slot shadow lighting |
| [`render-occlusion-re.md`](render-occlusion-re.md) | **landed 2026-07-16** (outside the original three REN slices) | D-OCC | blink-box visibility: section masks, portal traversal, occluder culling, indoor frame gates, the GPM `OVRT`/`OPLN`/`OFAC`/`OOBJ` occlusion chunks, and the sound-occlusion witness (which closed D-SND-7). Sound occlusion (2026-07-16, `CollisionWorld` + `engine/runtime/terrain_query`), the indoor frame gates (2026-07-16, `OcclusionFramePass`), and the section-mask/portal engine (init, mask build, traversal, occluder culling — 2026-07-17, `engine/runtime/world/src/occlusion.cpp`) are all ported; residuals ride the D-OCC rows |
| [`render-lighting-parity-2026-08-15.md`](render-lighting-parity-2026-08-15.md) | **evidence session 2026-08-15; max-quality publication refreshed 2026-08-18** | render fixture evidence | exact-pose catalogs, maximum-video frame-correlated capture tooling, [15 current comparisons](../../screenshots/parity/render-lighting-2026-08/registered-2026-08-16/README.md), shadow lifecycle repair, live retail IDA bounds, and comparison limitations |

Terrain TSS findings grow [terrain/terrain-re.md](../terrain/terrain-re.md);
sky/water shader gaps grow [env/env-tod-re.md](../env/env-tod-re.md) — in
place, never forked.

## The parity instrument (REN-1)

Three tiers; tolerances never widen — a divergence triggers a re-grill
(ADR 0023 §4).

**T1 — render-state vectors (the CI gate).**
`tests/renderer/state_vectors_test.cpp` (ctest `renderer_state_vectors`)
walks the full material input matrix through
`classify_object_material` → `build_object_shader_key` →
`describe_object_shader_pipeline` against the committed golden
`tests/renderer/render_state_vectors.golden` (2112 input rows + 630 typed
pipeline descriptors). The engine returns renderer-neutral technique, blend,
depth, cull, coverage, environment-source, and specular-source data; Godot
maps that descriptor to one of the finite checked-in resources under
`godot/shaders/object/`. No runtime shader source is assembled. Dump mode
(`OPENNOVA_RENDER_VECTORS_DUMP=1`) rewrites the
golden and deliberately fails. Re-dumps carry witness citations in the same
commit. A thin GUT leg (`godot/tests/render_shader_cache_handoff_test.gd`)
pins the GDScript→native binding, while
`tests/test_object_shader_resources.py` pins the complete resource manifest
and rejects runtime topology switches or source-generation paths.
`tests/object_shader_resource_hashes.golden.json` pins a normalized SHA-256 of
each of the 104 checked-in wrappers plus its transitive include closure,
preserving the former composed-source regression sensitivity. A deliberate,
witnessed shader change re-dumps with
`OPENNOVA_OBJECT_SHADER_HASHES_DUMP=1`; like the state-vector dump, that run
rewrites the golden and intentionally fails.

**T2 — swatch A/B (local, mandatory per REN slice).**
`godot/tests/render_swatch_probe.gd` renders one cell per unique object-shader
key (sphere + quad, code-generated textures, orthogonal camera — asset-free,
deterministic) and diffs captures exactly (`compare` mode,
`Image.compute_image_metrics`, max-delta 0). World-level baselines ride the
asset-gated `godot/tests/env_visual_baseline_probe.gd` (ENG-2's driver).
The `composite` mode (REN-3) renders the draw-order scenes — overlapping
translucent layers with depths arranged AGAINST the witnessed order, so only
the ported priority ladder composes them correctly (the water bracket and the
sky ladder; [render-order-re.md](render-order-re.md)).

```
# capture (windowed, never --headless):
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- capture .scratch/golden/render/<label>
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- composite .scratch/golden/render/<label>
"$GODOT_BIN" --path godot -s res://tests/env_visual_baseline_probe.gd -- <JOX_dir> .scratch/golden/render/<label> world
# compare two captures (swatch or composite):
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- compare a_grid.png b_grid.png
```

Baselines live under `.scratch/golden/render/` (machine-local, never
committed — [asset-gated-tests.md](../asset-gated-tests.md) policy). The
pre-change baseline set is captured before the first REN behavior change and
each slice's PR attests its A/B.

**T3 — registered retail side-by-side (the headline gate, attested at
REN-7).** Registration replaces REN-7's earlier unregistered captures with
cryptographically bound evidence, but the instrument is unchanged: the by-eye
retail pass over each sheet is the maintainer's attestation, recorded
scene-by-scene in the slice PR. MAE/RMS numbers are descriptive
measurements, never the gate.
The named scene list is captured in retail JO and OpenNova from a catalog pose
and authored start minute. Registration binds the exact retail process,
frame-correlated state, packed mission, binaries, and frozen OpenNova source.
Every publishable row uses the catalog's matched `hud_hidden` presentation:
the M16 Burst, bare `IndoArms.3di` arms, first-person viewmodel, and terrain
remain active in both engines while gameplay HUD text is absent. Retail stages
the exhaustive `retail_reference_highest_retail_selectable_v2` video profile,
then the bridge snapshots the composed backbuffer at a known pre-retail-HUD
boundary. It restores the D3D scene and lets retail execute its ordinary UI
call unmodified, so HUD/FPS remain visible to the player. OpenNova suppresses
HUD/FPS only for its capture frame.
Cross-engine metrics remain qualitative because the engines do not render
identical viewmodel lighting/placement, post-processing, or animated phases.
The explicit `world_center` and `viewmodel_arms` ROIs are descriptive only.
The review remains scene-by-scene:

1. Water horizon at TOD 550 / 1200 / 1845 / 2200 (sun-facing).
2. Alpha-tested foliage/fence close-up (scissor edges, two-sided).
3. Glass / env-mapped vehicle (reflection, fresnel, specular).
4. Transparents composite: smoke or glass over water over sky.
5. Night terrain with lightmapped emplacements (lightmap + modulator scale).
6. First-person viewmodel over the world (draw-order: viewmodel pass).

The current publication catalog is
[`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json), SHA-256
`607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8`.
The final CP01/CP12 anchors are `cp01-water-oblique-retail`,
`cp12-yard-road-retail`, and `cp12-yard-tanks-retail`; rejected unregistered
context frames are not substitutes for these fixtures.

The current 2026-08-18 max-quality publication contains
[all 15 registered pairs and 75 OpenNova diagnostic variants](../../screenshots/parity/render-lighting-2026-08/registered-2026-08-16/README.md),
captured from frozen source
`c06778f55d7ef660d49f2d2678e9ab9ae05aa607`. That source includes the retail-shaped
128-page terrain composition cache, exact detail-density UV scale and 16×
anisotropy, ordered `.til` RGBA composition, and static-model silhouettes in
terrain-page alpha. Terrain and detail foliage consume the same current-frame
page. The fixture index links every
side-by-side, overlay, difference image, comparison manifest, and OpenNova
variant manifest, retail registration, and sanitized staging record. Every row
uses fixture catalog v2 at SHA-256
`607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8`,
stage/restore v3 (tool 3.0.0), a raw capture bundle v4 from bridge 1.4/onHook 0.5.0,
registered capture v5 (tool 4.0.0), and comparison v6 (tool 4.0.0). Retail
images come from the pre-HUD backbuffer snapshot; the ordinary retail HUD/FPS
path remains active on screen. Validators reject every legacy schema rather
than silently upgrading it.

The superseded 2026-08-17 set used catalog v1 and frozen source
`b476b1ef8acd65d18f7149f744e72d48f086e506`. It previously occupied this
publication slot, but is retained only in repository history: its shared
staging hash proves `object_texdetail=1` (Normal), and its HUD suppression was
process-scoped. It is not admissible parity evidence and must not be restored
over the current path.

Across the 15 replacement comparison-v6 manifests, full-frame MAE spans
`6.020889`–`21.961355`, `world_center` MAE spans
`3.870290`–`32.547584`, and `viewmodel_arms` MAE spans
`4.133595`–`26.845487`. These are descriptive deltas, not parity thresholds.
For historical comparison only, the superseded set's full-frame MAE spanned
`5.459962`–`27.712417`,
`world_center` MAE spanned `4.432509`–`32.137639`, and `viewmodel_arms` MAE
spanned `4.462837`–`23.340581`. The below-water CP01 row was the lowest
full-frame pair at `5.459962` MAE / `9.306184` RMS and also had the lowest arms
result at `4.462837` / `6.976587`; courtyard had the lowest world result at
`4.432509` / `11.090609`. Water reflection/noise, fire particles and spill,
vegetation and live actors, night exposure, residual CP12 tile-marking
contrast, and viewmodel pose/light differences remain visible. Those
historical values are
descriptive measurements, not parity thresholds.

## Capture procedures

The operator procedure for both tiers lives in
[render-parity-runbook.md](render-parity-runbook.md): worktree preflight, the
frozen-commit rule, the OpenNova probe and its environment contract, retail
staging/registration (including re-registering retained bundles against a new
commit), the comparison build, publication assembly, and failure triage. This
page states what the evidence must prove; the runbook states how to produce it.

The exact-pose harness both tiers drive is
[`render_fixture_capture_probe.tscn`](../../godot/tests/render_fixture_capture_probe.tscn).
It boots the production `MainGame` world, dismisses the start-mission splash,
and fail-closes until single-player auto-spawn has produced a local player,
gameplay camera, and active world input with no spawn/menu shell remaining.
Only then does it wait the declared settle frames, realize the catalog camera
and minute, freeze simulation and weather, and capture the declared variants in
order. Each selected fixture/minute emits five lossless PNGs and five
`.state.json` sidecars sampled in the same completed draw callback as their
PNG, plus one `<fixture-id>-manifest.json` per invocation.

Two catalogs drive it. [`render-fixtures-v1.json`](render-fixtures-v1.json) is
the `world_only` diagnostic catalog at 1600x900 / vertical FOV 50.534, for
within-run subsystem isolation via
[`build_render_comparison.ps1`](../../scripts/render/build_render_comparison.ps1)
`-ComparisonMode subsystem-ab`.
[`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json) is the
registered publication catalog at 2000x1200 / vertical FOV 53.4468. The
diagnostic workflow is never a substitute for a registered HUD-hidden retail
comparison, and the legacy `retail-parity` comparison mode is not sanctioned
for cross-engine evidence.

Raw capture bundles, retail tool transcripts, and full baseline sets stay under
`.scratch` per the
[asset-gated evidence policy](../asset-gated-tests.md). The capture probe never
writes directly to a tracked or external evidence root; publication happens
only after the scratch bundle passes the registered validation workflow.
