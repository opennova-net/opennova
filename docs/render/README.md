# docs/render/ — the REN track's RE-record home

The render visual-parity track ([maturity-program.md](../maturity-program.md)
REN; standing rules [ADR 0023](../adr/0023-render-visual-parity.md)). Records
land here as the grill slices convert the three `UNAUDITED` render systems
([divergence-ledger.md](../divergence-ledger.md) audit track):

| Record | Lands at | Catalog | Covers |
|---|---|---|---|
| [`render-material-re.md`](render-material-re.md) | **landed at REN-2** | D-RMAT | the runtime flag/tag→state path down to the device boundary (registry @ 0x5af790/0x5ae690, resolution @ 0x5b03c0, state application @ 0x5d9f50; the planning anchor "Entity_UpdateRenderState @ 0x5d6a30" resolved at REN-5 to the render-slot light updater, renamed `RenderSlot_UpdateEntityLight`) |
| [`render-order-re.md`](render-order-re.md) | **landed at REN-3** | D-RORD | batching, sort keys, technique-class selection, the render-state stack, and the frame pass sequence (Render_SubmitEntity @ 0x5dad80, CRenderBatchQueue_SortAndFlush @ 0x5dae40, Terrain_RenderWorldScene @ 0x5c93a0); since the 2026-09-24 rendering parity pass also the post-particle overlay stage and the FrameFX screen effects |
| [`render-lighting-re.md`](render-lighting-re.md) | **landed at REN-5** | D-RLIT | the iris/modulator chain (env #17), the world lighting block + per-entity uniforms and hemisphere D3D lights, dynamic point lights + group culling, terrain/foliage c0/c1, lighting textures, the cubemap sources (CubeRotSpecular = D-RORD-5's answer), the render-slot shadow lighting |
| [`render-occlusion-re.md`](render-occlusion-re.md) | **landed 2026-07-16** (outside the original three REN slices) | D-OCC | blink-box visibility: section masks, portal traversal, occluder culling, indoor frame gates, the GPM `OVRT`/`OPLN`/`OFAC`/`OOBJ` occlusion chunks, and the sound-occlusion witness (which closed D-SND-7). Sound occlusion (2026-07-16, `CollisionWorld` + `engine/runtime/terrain_query`), the indoor frame gates (2026-07-16, `OcclusionFramePass`), and the section-mask/portal engine (init, mask build, traversal, occluder culling — 2026-07-17, `engine/runtime/world/occlusion.cpp`) are all ported; residuals ride the D-OCC rows |
| [`shader-validation.md`](shader-validation.md) | **landed with the FrameFx slice (2026-08-23)** | shader provenance | the checked-in shader inventory, its citation coverage, the bounded parity statuses, and the light-response validation procedure; the hand-maintained object wrappers and what `object/pipeline_manifest.json` does and does not describe |
| [`render-lighting-parity-2026-08-15.md`](render-lighting-parity-2026-08-15.md) | **evidence session 2026-08-15; settled max-quality publication refreshed 2026-08-22** | render fixture evidence | exact-pose catalogs, maximum-video frame-correlated capture tooling, [18 current comparisons](https://github.com/opennova-net/opennova/blob/8881f61d7cdf7f848cea85393a3858f6e2866dbd/screenshots/parity/render-lighting-2026-08/registered-2026-08-22/README.md), shadow lifecycle repair, live retail IDA bounds, and comparison limitations |

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
(`renderer_state_vectors_test --dump`) rewrites the
golden and deliberately fails. Re-dumps carry witness citations in the same
commit. A thin GUT leg (`godot/tests/render_shader_cache_handoff_test.gd`)
pins the GDScript→native binding. The former
`shader_resource_contract_test.gd` manifest pin and the transitive-source
hash golden over the 132 object wrappers were retired on 2026-09-21 with the
other textual source pins: a shader change is
reviewed as a diff and validated by loading every resource
(`shader_resource_validation_test.gd`) and by the swatch A/B below.

**T2 — swatch A/B (local, mandatory per REN slice).**
The `render_swatch` probe (`godot/probes/render/render_swatch_probe.gd`, run
through the game MCP's `game_probe` tool) renders one cell per unique object-shader
key (sphere + quad, code-generated textures, orthogonal camera — asset-free,
deterministic) and diffs captures exactly (`compare` mode,
`Image.compute_image_metrics`, max-delta 0). World-level baselines ride the
asset-gated time-of-day recipe (`docs/mcp.md`: `environment_time_of_day` +
`game_capture_bundle` per minute, compared with `render_swatch` `compare`;
formerly `env_visual_baseline_probe.gd`, ENG-2's driver).
The `composite` mode (REN-3) renders the draw-order scenes — overlapping
translucent layers with depths arranged AGAINST the witnessed order, so only
the ported priority ladder composes them correctly (the water bracket and the
sky ladder; [render-order-re.md](render-order-re.md)).

```
# capture (windowed, never --headless):
python scripts/mcp/game_mcp.py launch --windowed --resource-dir "$OPENNOVA_JO_DIR"
python scripts/mcp/game_mcp.py probe run render_swatch '{"mode":"capture","output_dir":"<baseline-dir>/<label>"}' --wait
python scripts/mcp/game_mcp.py probe run render_swatch '{"mode":"composite","output_dir":"<baseline-dir>/<label>"}' --wait
# world baselines: per minute, game_debug set environment_time_of_day then game_capture_bundle world_only
# compare two captures (swatch or composite):
python scripts/mcp/game_mcp.py probe run render_swatch '{"mode":"compare","a":"a_grid.png","b":"b_grid.png"}' --wait
```

Baselines live in a gitignored local directory (machine-local, never
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
identical viewmodel lighting, post-processing, or animated phases, and the
registered retail frames hold the viewmodel inside a teleport transient
(runbook section 8).
The explicit `world_center` and `viewmodel_arms` ROIs are descriptive only.
The review remains scene-by-scene:

1. Water horizon at TOD 550 / 1200 / 1845 / 2200 (sun-facing).
2. Alpha-tested foliage/fence close-up (scissor edges, two-sided).
3. Glass / env-mapped vehicle (reflection, fresnel, specular).
4. Transparents composite: smoke or glass over water over sky.
5. Night terrain with lightmapped emplacements (lightmap + modulator scale).
6. First-person viewmodel over the world (draw-order: viewmodel pass).

The current publication catalog is
[`render-fixtures-retail-v5.json`](render-fixtures-retail-v5.json), SHA-256
`d5ea3d0548f9854e38f8d30ecf3c0119053edcf4496260b27b611e35a39c8f3e` — minted from
the 2026-08-22 SETTLED retail sessions: every fixture re-applied and left to
settle before capture, so motion-lead saturation and the deterministic ground
snap finish first, then each `camera_bms` recalibrated from its own registered
inverse view matrix. The id set, missions, minutes, FOV, and comparison
contracts are v4's unchanged; only the poses moved (courtyard 19.4 cm in z and
10.8 cm in x, fire-barrel-close 0.99 m — the teleport transient every v4 land
camera sampled; net-re section 5.40 ninth pass). Deep-water CP01 rows keep the
fast capture because float physics pins the pose (D-INF-3). The final
CP01/CP12 anchors are `cp01-water-oblique-retail`,
`cp12-yard-road-retail`, and `cp12-yard-tanks-retail`; rejected unregistered
context frames are not substitutes for these fixtures.

The `screenshots/` tree was removed on 2026-09-26; its links here point at commit
`8881f61d7`, the last that carried it.

The current 2026-08-22 settled max-quality publication contains
[all 18 registered pairs and 90 OpenNova diagnostic variants](https://github.com/opennova-net/opennova/blob/8881f61d7cdf7f848cea85393a3858f6e2866dbd/screenshots/parity/render-lighting-2026-08/registered-2026-08-22/README.md),
captured from frozen source
`269c1c8e727a16074f13e7b2dfdc4ba9ff3af42c` (the viewmodel-alignment PR #562
branch commit — the counter-gated first-person clip advance and the bone-exact
one-rig viewmodel placement, D-INF-14 — kept an ancestor of master by that
PR's merge commit). The fixture index links every
side-by-side, overlay, difference image, comparison manifest, and OpenNova
variant manifest, retail registration, and sanitized staging record. Every row
uses fixture catalog v5,
stage/restore v3 (tool 3.0.0), a raw capture bundle v4 from bridge 1.5/onHook
0.6.0 — whose capture-frame fixture-binding witness proves the capture frame
against the fixture apply (serial, pinned clock, held lease) —
registered capture v5 (tool 4.1.0, settle floor 1.0 s with the documented
deep-water exception), and comparison v6 (tool 4.0.0). Retail
images come from the pre-HUD backbuffer snapshot; the ordinary retail HUD/FPS
path remains active on screen. Validators reject every legacy schema rather
than silently upgrading it.

Three superseded sets are retained only in repository history and are not
admissible parity evidence: the 2026-08-17 set (catalog v1, frozen source
`b476b1ef8acd65d18f7149f744e72d48f086e506`, `object_texdetail=1`,
process-scoped HUD suppression), the 2026-08-16/18 set (catalog v2, frozen
source `2cde75ad029be14d1c6e0dd8b034efc5fef017b5` — a pre-squash PR SHA that
is not an ancestor of master), and the 2026-08-20 set (catalog v4, frozen
source `58ea3e5ff61b9aef5841a8601b7d9b86817ebde1` — every land capture was
taken the same frame as its fixture apply, so the cameras sample the teleport
transient the settled 2026-08-22 refresh removed). Catalogs v2, v3, and v4
([`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json),
[`render-fixtures-retail-v3.json`](render-fixtures-retail-v3.json),
[`render-fixtures-retail-v4.json`](render-fixtures-retail-v4.json)) are
kept, like v1, as the record of the poses this revision inherits; they are not
re-shootable — a catalog's `camera_bms` values bind its own capture session's
physics.

Across the 18 current comparison-v6 manifests, full-frame MAE spans
`5.165582`–`25.923306`, `world_center` MAE spans
`2.439046`–`15.728697`, and `viewmodel_arms` MAE spans
`5.036941`–`32.155453`. These are descriptive deltas, not parity thresholds.
Water reflection/noise, fire particles and spill, vegetation and live actors,
night exposure, and residual CP12 tile-marking contrast remain visible, as do
viewmodel lighting, material response, and animation-phase differences — the
viewmodel identity and placement themselves are matched (D-INF-14 FIXED; the
deep-water CP01 rows still show retail's D-INF-3 raised swimming hold, so
they are not placement evidence). The `00tra-tire-marks-retail` fixture (added
2026-08-20 from a debug snapshot) measures the ordered `.til` overlay
tire-mark composition; the corrected celestial axis map (#525) now lays the static tree
silhouettes through this camera's view, so its full-frame MAE (`10.191796`)
measured the low-sun silhouette density together with the tile-composition
items (D-TERRAIN-7, closed 2026-09-26). The
`00tra-armory-lght-retail` fixture (added 2026-08-20 from a debug snapshot)
measures model-authored `LGHT` lamp delivery inside the armory - the
2026-08-20 slice landed vertex-rate point shading, static-source owner
scope, and corona billboards, and refuted the Target/spot-cone premise
(the spot spawner is caller-less dead code); interior groups, batch draw
contexts, and the foliage leg ride the open D-RLIT-4 residual
tail (the per-light TERRAIN projected pass is ported 2026-08-21 end to end —
`light_terrain_pass.h` rows, the procedural falloff textures, the additive
two-stage fold in `terrain_lighting.gdshaderinc`; a dusk pool on the ground is
the next fixture to register). The `03tr-sun-sky-retail` fixture (same session)
measures low-sun sky-dome/sun/ambient response on the 03TR airfield; its
deltas then rode D-RLIT-2, D-RLIT-5 (both since closed), and the deferred
env #16 first-pass TOD table. The 2026-09-24 rendering parity pass used its
pose for the sun (placement, the body and bloom colour, the dome order;
env-tod-re.md §Celestial bodies) and found one global gain behind a ~15 %
uniform lit-ground gap against the superseded 2026-08-20 frame: every iris
sample at that pose lies inside the hangar's blink volume (bms 71), so all
three take the indoor ceiling/floor branch `[orig:
Terrain_SectorComputeLighting @ 0x5c7660..0x5c76fe]` and the settled
modulator is 76/64 = 1.1875, while that frame was captured three frames
after its fixture apply, before the modulator had chased its target
(`ColorBlock_SetStepDeltas(62) @ 0x57e538`); no engine change followed.
The capture rule under "Capture procedures" covers
it.

## Capture procedures

The former registered-capture publication pipeline (the render-parity runbook,
`docs/render/render-parity-runbook.md` until 5820432c1, and the
`scripts/render/*.py` tools) was retired with the Python FFI (ADR 0038). The
retained catalogs and screenshots are historical evidence carrying their own
provenance, not a currently reproducible release gate. The remaining PowerShell
helpers under `scripts/render/` support local OpenNova capture and comparison
work, in pipeline order:

- [`capture_opennova_fixtures.ps1`](../../scripts/render/capture_opennova_fixtures.ps1)
  launches one windowed OpenNova with its MCP endpoint and runs the
  `render_fixture_capture` probe once per catalog id (the raw 2000x1200
  bundles).
- [`normalize_opennova_for_retail.ps1`](../../scripts/render/normalize_opennova_for_retail.ps1)
  draws one raw 2000x1200 OpenNova capture into the 1920x1200 retail frame
  size (bicubic, never a crop or a vertical change) so the two sides compare
  pixel for pixel.
- [`build_render_comparison.ps1`](../../scripts/render/build_render_comparison.ps1)
  assembles the side-by-side, overlay and difference images plus the
  comparison manifest (`-ComparisonMode subsystem-ab` for the within-run
  diagnostic catalog).
- [`build_model_lighting_comparison.ps1`](../../scripts/render/build_model_lighting_comparison.ps1)
  and [`test_model_lighting_parity.ps1`](../../scripts/render/test_model_lighting_parity.ps1)
  are the model-lighting sheet's own pair
  ([screenshots/parity/model-lighting](https://github.com/opennova-net/opennova/blob/8881f61d7cdf7f848cea85393a3858f6e2866dbd/screenshots/parity/model-lighting/README.md)).

The exact-pose harness both tiers drive is
the `render_fixture_capture` probe
([`render_fixture_capture_probe.gd`](../../godot/probes/render/render_fixture_capture_probe.gd),
a `game_probe` tool).
It boots the production `MainGame` world, dismisses the start-mission splash,
and fail-closes until single-player auto-spawn has produced a local player,
gameplay camera, and active world input with no spawn/menu shell remaining.
Only then does it wait the declared settle frames, realize the catalog camera
and minute, freeze simulation and weather, and capture the declared variants in
order. Each selected fixture/minute emits five lossless PNGs and five
`.state.json` sidecars sampled in the same completed draw callback as their
PNG, plus one `<fixture-id>-manifest.json` per invocation. Since 2026-09-24
the probe no longer forces the 0.05 near plane: the world's scene-environment
leg pins retail's 0.2 world near plane on the render camera every frame
(render-order-re.md, the scene projection near plane). The fixture contract's
water-mirror check (`realized_reflection_pose_matches`,
`godot/probes/render/render_fixture_contract.gd`) expects the reflection
camera to mirror the eye only at or above the water plane; below it the
reflected pass keeps the live eye, as `Render_MainScene` copies the camera
block unchanged there (env-tod-re.md #30).

Retail references carry the same settle obligation as OpenNova captures: a
retail frame must be taken after the iris modulator has settled at the pose,
or the whole frame carries a stale global gain. The modulator chases its
target over 62 ticks (`ColorBlock_SetStepDeltas(62) @ 0x57e538`), and before
the local player exists `Environment_ApplyFogAndAmbient`
`@ 0x57e50b..0x57e512` skips the retarget and leaves the 0x40 identity; the
2026-09-24 pass used about 300 ticks after the local player exists at the
pose as its margin. The superseded 2026-08-20 `03tr-sun-sky-retail` frame,
three frames after its apply, read ~64-66 against the settled 76; the
registered 2026-08-22 frame was taken 4.03 s (530 frames) after its apply.

Two catalogs drive it. [`render-fixtures-v1.json`](render-fixtures-v1.json) is
the `world_only` diagnostic catalog at 1600x900 / vertical FOV 50.534, for
within-run subsystem isolation via
[`build_render_comparison.ps1`](../../scripts/render/build_render_comparison.ps1)
`-ComparisonMode subsystem-ab`.
[`render-fixtures-retail-v5.json`](render-fixtures-retail-v5.json) is the
registered publication catalog at 2000x1200 / vertical FOV 53.4468. The
diagnostic workflow is never a substitute for a registered HUD-hidden retail
comparison, and the legacy `retail-parity` comparison mode is not sanctioned
for cross-engine evidence.

Raw capture bundles, retail tool transcripts, and full baseline sets stay machine-local per the
[asset-gated evidence policy](../asset-gated-tests.md). The capture probe never
writes directly to a tracked or external evidence root; publication happens
only after the scratch bundle passes the registered validation workflow.
