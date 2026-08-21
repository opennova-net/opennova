# docs/render/ — rendering records and evidence

The historical render visual-parity track ([maturity-program.md](../maturity-program.md)
REN; [ADR 0023](../adr/0023-render-visual-parity.md)) established the decoded
material, texture, order and lighting records kept here.
[ADR 0036](../adr/0036-hard-cut-rendering-facelift.md) now governs the shipping
presentation: one hard-cut Godot-native relight, no render profiles or inferred
PBR maps, and unchanged source assets and decoded semantics. The 18 registered
fixtures remain descriptive retail comparisons rather than a fixed-function
pixel target. The live device uses a native shadow-casting directional sun,
pooled clustered effect lights, and fixed Filmic exposure; the retail
render-slot planner remains only a portable oracle, with no Godot
`SlotShadow`/drape or fullscreen sun-veil device.

| Record | Lands at | Catalog | Covers |
|---|---|---|---|
| [`render-material-re.md`](render-material-re.md) | **landed at REN-2** | D-RMAT | the runtime flag/tag→state path down to the device boundary (registry @ 0x5af790/0x5ae690, resolution @ 0x5b03c0, state application @ 0x5d9f50; the planning anchor "Entity_UpdateRenderState @ 0x5d6a30" resolved at REN-5 to the render-slot light updater, renamed `RenderSlot_UpdateEntityLight`) |
| [`render-order-re.md`](render-order-re.md) | **landed at REN-3** | D-RORD | batching, sort keys, technique-class selection, the render-state stack, and the frame pass sequence (Render_SubmitEntity @ 0x5dad80, CRenderBatchQueue_SortAndFlush @ 0x5dae40, Terrain_RenderSceneWithReflection @ 0x5c93a0) |
| [`render-lighting-re.md`](render-lighting-re.md) | **landed at REN-5** | D-RLIT | the iris/modulator chain (env #17), the world lighting block + per-entity uniforms and hemisphere D3D lights, dynamic point lights + group culling, terrain/foliage c0/c1, lighting textures, the cubemap sources (CubeRotSpecular = D-RORD-5's answer), the render-slot shadow lighting |
| [`render-occlusion-re.md`](render-occlusion-re.md) | **landed 2026-07-16** (outside the original three REN slices) | D-OCC | blink-box visibility: section masks, portal traversal, occluder culling, indoor frame gates, the GPM `OVRT`/`OPLN`/`OFAC`/`OOBJ` occlusion chunks, and the sound-occlusion witness (which closed D-SND-7). Sound occlusion (2026-07-16, `CollisionWorld` + `engine/runtime/terrain_query`), the indoor frame gates (2026-07-16, `OcclusionFramePass`), and the section-mask/portal engine (init, mask build, traversal, occluder culling — 2026-07-17, `engine/runtime/world/src/occlusion.cpp`) are all ported; residuals ride the D-OCC rows |
| [`render-lighting-parity-2026-08-15.md`](render-lighting-parity-2026-08-15.md) | **evidence session 2026-08-15; facelift publication refreshed 2026-08-20** | render fixture evidence | exact-pose catalogs, maximum-video frame-correlated capture tooling, [18 current comparisons](../../screenshots/facelift/render-lighting-2026-08/registered-2026-08-20/README.md), shadow lifecycle repair, live retail IDA bounds, and comparison limitations |

Terrain TSS findings grow [terrain/terrain-re.md](../terrain/terrain-re.md);
sky/water shader gaps grow [env/env-tod-re.md](../env/env-tod-re.md) — in
place, never forked.

## The retained rendering instrument

ADR 0023's semantic vectors and capture machinery remain in service. Under ADR
0036, semantic-vector drift still requires explanation, while swatches and retail
pixels describe or regress the current presentation rather than requiring the old
fixed-function look.

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

**T3 — registered retail side-by-side.** Registration replaces REN-7's earlier
unregistered captures with cryptographically bound evidence. ADR 0036 retains all
18 catalog-v4 scenes and recaptures the OpenNova leg after the hard cut. The
scene-by-scene review checks preserved content semantics and a coherent restrained
relight, not similarity to the fixed-function pixels. MAE/RMS numbers are
descriptive measurements, never the gate.
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
[`render-fixtures-retail-v4.json`](render-fixtures-retail-v4.json), SHA-256
`e230836ea42fe563e24d16eec3a0d95137bb3c7138c711a9e04b8cc4a30fd1b1` — minted by
[`mint_retail_catalog.py`](../../scripts/render/mint_retail_catalog.py) from
the 2026-08-20 verbatim retail sessions: every applied pose kept exactly, each
fixture's `camera_bms` recalibrated from its own registered inverse view
matrix. It extends v3 with two snapshot-derived fixtures
(`00tra-armory-lght-retail`, `03tr-sun-sky-retail`) and adds `03TR.bms`
(authored start minute 390) to the mission set. The final CP01/CP12 anchors are `cp01-water-oblique-retail`,
`cp12-yard-road-retail`, and `cp12-yard-tanks-retail`; rejected unregistered
context frames are not substitutes for these fixtures.

The current 2026-08-20 facelift publication contains
[all 18 registered pairs and 90 OpenNova diagnostic variants](../../screenshots/facelift/render-lighting-2026-08/registered-2026-08-20/README.md),
captured from frozen source
`e59dc041e78509abed3b4110db95b729a6312125`. Every row binds Godot executable
SHA-256 `1e5efe381f62ee1cea6bc18caac71c0c74bd6e68e6af5c6efb3dbe76628f61c7`
and GDExtension SHA-256
`129bb0cd93c082337206b6d57bcd3b5a1646dd64eb83ac6cc5eb81e739d578d5`.
The fixture index links every
side-by-side, overlay, difference image, comparison manifest, and OpenNova
variant manifest, retail registration, and sanitized staging record. Every row
uses fixture catalog v4,
stage/restore v3 (tool 3.0.0), a raw capture bundle v4 from bridge 1.4/onHook 0.5.0,
registered capture v5 (tool 4.0.0), and comparison v6 (tool 4.0.0). Retail
images come from the pre-HUD backbuffer snapshot; the ordinary retail HUD/FPS
path remains active on screen. Validators reject every legacy schema rather
than silently upgrading it.

Three superseded sets are retained only in repository history and are not
current facelift evidence: the pre-facelift catalog-v4 publication (frozen
source `30f3e194b9f7aaea6053492f7797c9940f8039ed`; its prior frozen sources
`3ada00e96…` and `3232a5c86…` were orphaned from master ancestry by a rebase
merge), the 2026-08-17 set (catalog v1, frozen source
`b476b1ef8acd65d18f7149f744e72d48f086e506`, `object_texdetail=1`,
process-scoped HUD suppression) and the 2026-08-16/18 set (catalog v2, frozen
source `2cde75ad029be14d1c6e0dd8b034efc5fef017b5` — a pre-squash PR SHA that is
not an ancestor of master, which is what prompted this refresh). Catalogs v2 and v3
([`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json),
[`render-fixtures-retail-v3.json`](render-fixtures-retail-v3.json)) are
kept, like v1, as the record of the poses this revision inherits; they are not
re-shootable — its `camera_bms` values bind its own capture session's physics
(the [runbook](render-parity-runbook.md) section 3b carries the full
constraint table).

Across the 18 current comparison-v6 manifests, full-frame MAE spans
`8.948040`–`32.961275`, `world_center` MAE spans
`5.394610`–`35.765520`, and `viewmodel_arms` MAE spans
`5.756973`–`44.881361`. These are descriptive deltas, not parity thresholds.
Water reflection/noise, fire particles and spill, vegetation and live actors,
night exposure, residual CP12 tile-marking contrast, and viewmodel pose/light
differences remain visible. CP04 is the clearest night-exposure outlier: an
8-pixel-stride full-frame Rec. 709 luma sample measures OpenNova mean `0.036`
versus retail `0.123`, with `97.3%` of OpenNova samples below `0.10`; its
`shadows_off` diagnostic stays dark, locating the residual in ambient/exposure
rather than cast shadows. The `00tra-tire-marks-retail` fixture (added 2026-08-20
from a debug snapshot) measures the ordered `.til` overlay tire-mark composition
under the new relight; the remaining data/composition questions stay in
D-TERRAIN-7. The
`00tra-armory-lght-retail` fixture (added 2026-08-20 from a debug snapshot)
measures model-authored `LGHT` lamp delivery inside the armory. The portable
pool retains decoded identity, transform, animated color, range, lifetime and
participation flags; `EffectLightDirector` realizes each active row as a pooled
Godot `OmniLight3D` or `SpotLight3D`, selected from the decoded target bit, and
keeps the witnessed corona walk live. Illumination is now native clustered and
spatial rather than retail's nearest-four per-draw delivery. Native-versus-retail
attenuation, specular and shadow response still diverge, along with batched
static subobject ownership and the remaining D-RLIT-4 delivery tail. The
`03tr-sun-sky-retail` fixture (same session) measures the low-sun divergence in
iris/ambient sampling (D-RLIT-2), sun-glint/reflection stand-ins (D-RLIT-5),
and the deferred overcast/TOD first-pass table (env #16).

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
[`render-fixtures-retail-v4.json`](render-fixtures-retail-v4.json) is the
registered publication catalog at 2000x1200 / vertical FOV 53.4468. The
diagnostic workflow is never a substitute for a registered HUD-hidden retail
comparison, and the legacy `retail-parity` comparison mode is not sanctioned
for cross-engine evidence.

Raw capture bundles, retail tool transcripts, and full baseline sets stay under
`.scratch` per the
[asset-gated evidence policy](../asset-gated-tests.md). The capture probe never
writes directly to a tracked or external evidence root; publication happens
only after the scratch bundle passes the registered validation workflow.
