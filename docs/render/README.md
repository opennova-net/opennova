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

## Reproducible three-mission fixture capture

The exact-pose harness is the
[`render_fixture_capture_probe.tscn`](../../godot/tests/render_fixture_capture_probe.tscn)
scene driven by the versioned
[`render-fixtures-v1.json`](render-fixtures-v1.json) catalog. It boots the
production `MainGame` world, dismisses the start-mission splash, and then
fail-closes until the single-player auto-spawn has produced a local player,
gameplay camera, and active world input with no spawn/menu shell remaining.
Only then does it wait the declared settle frames, realize the catalog camera
and minute, freeze simulation and weather, and capture the diagnostic variants
in their declared order.

Run it **windowed** from the repository root. The verified entry point is the
`.tscn` as the positional scene, with the canonical resolution on the command
line. Do not pass the `.gd` file to `-s`, and do not add `--headless`:

```powershell
$env:GODOT_BIN = "C:\path\to\godot.windows.editor.x86_64.exe"
$env:NOVA_RENDER_FIXTURE_ID = "00tra-fire-barrel"
$env:NOVA_MISSION_RESOURCE_DIR = "C:\path\to\loose-missions"
$env:NOVA_RUNTIME_RESOURCE_DIR = "C:\path\to\packed-runtime-root"
$env:NOVA_EXPANSION = "jox01"
$env:NOVA_RENDER_FIXTURE_OUTPUT = (Join-Path (Resolve-Path ".").Path ".scratch\golden\render\fixtures\00tra-fire-barrel")
$env:NOVA_RENDER_FIXTURE_MINUTE = "1320"
$env:NOVA_RENDER_CAPTURE_MODE = "world_only"
$env:NOVA_EVIDENCE_SOURCE_COMMIT = (git rev-parse HEAD).Trim()
$env:NOVA_GDEXTENSION_BINARY = (Resolve-Path "godot\bin\libopennova.windows.template_debug.x86_64.dll").Path

& $env:GODOT_BIN --path godot --resolution 1600x900 res://tests/render_fixture_capture_probe.tscn
```

Only the path values are installation-specific. The invocation and environment
contract are:

| Variable | Requirement |
|---|---|
| `GODOT_BIN` | A Godot 4.6.1 editor binary capable of windowed rendering. This is the shell launcher; the probe does not read it. |
| `NOVA_RENDER_FIXTURE_ID` | **Required.** One exact `id` from the catalog, for example `00tra-fire-barrel`. |
| `NOVA_MISSION_RESOURCE_DIR` | **Required for reproducible evidence.** A valid loose-mission root containing the catalog mission as a loose `.bms`. The probe has a persisted-setting fallback for interactive convenience; evidence runs must set this explicitly. |
| `NOVA_RUNTIME_RESOURCE_DIR` | **Required.** A valid packed runtime resource root. |
| `NOVA_EXPANSION` | Optional; defaults to `jox01`. |
| `NOVA_RENDER_FIXTURE_CATALOG` | Optional; defaults to `res://../docs/render/render-fixtures-v1.json`. An override remains subject to the same schema, resolution, mission-hash, and variant-order checks. |
| `NOVA_RENDER_FIXTURE_OUTPUT` | Optional; defaults to `res://../.scratch/golden/render/fixtures/<fixture-id>`. An override must be a dedicated strict descendant of this repository's `.scratch` directory; the probe rejects `.scratch` itself, paths outside it, and any linked/reparse ancestor. |
| `NOVA_RENDER_FIXTURE_MINUTE` | Optional strict integer selector. It must be one of the selected fixture's declared `minutes_of_day`; empty captures all declared minutes. Values such as `720.0` and undeclared minutes are rejected. |
| `NOVA_RENDER_CAPTURE_MODE` | Optional verifier. If set, it must equal the selected fixture's declared `world_only`, `full_frame`, or `hud_hidden` mode; it cannot relabel a fixture. |
| `NOVA_EVIDENCE_SOURCE_COMMIT` | **Required.** The lowercase full 40-character commit that produced the capture. Capture only after that commit is frozen; do not identify a dirty or subsequently rebuilt tree with its current `HEAD`. |
| `NOVA_GDEXTENSION_BINARY` | **Required.** Absolute path to the exact GDExtension binary loaded by this Godot build. The manifest hashes it together with the Godot executable. |

The catalog fixes the viewport at 1600×900, vertical FOV at 50.534 degrees,
mission hashes, pose, settle counts, and the exact variant order: `beauty`,
`shadows_off`, `lighting_only`, `unshaded`, and
`directional_shadow_atlas`. The probe rejects a catalog/driver mismatch, a
mission hash mismatch, a missing production gameplay camera, a viewport-size
mismatch, a source/build provenance failure, a capture-mode mismatch, or any
drift from the requested integer minute/fixed24 state.

Fixture anchors record both serialized and runtime identity. The nested
`entity.bms_record` distinguishes the BMS `write_order_index` from the
`mission_kind` pool (`item`, `building`, `marker`, or `organic`) and its
kind-local `kind_index`; `bms_id` remains the stable authored identity check.
Do not interpret the write-order index as an item-pool index.

Each selected fixture/minute emits five lossless PNGs and five `.state.json`
sidecars; each fixture invocation also emits one
`<fixture-id>-manifest.json`. Sidecars are sampled in the same completed draw
callback as their PNG and carry the realized camera, environment, water/sky,
active lights, shadows, pass counts, and renderer state.

Raw capture bundles, retail tool transcripts, and full baseline sets remain
under `.scratch` according to the
[asset-gated evidence policy](../asset-gated-tests.md). Published captures and
generated comparison sheets live under `screenshots/parity/` with their
portable manifests and metric policy intact. Use
[`build_render_comparison.ps1`](../../scripts/render/build_render_comparison.ps1)
with `-ComparisonMode subsystem-ab` for honest within-run comparisons such as
`beauty` / `shadows_off` / `lighting_only`. Cross-engine comparisons use the
registered workflow below, not the legacy `retail-parity` mode or free-form
side-by-side script. The capture probe never writes directly to a tracked or
external evidence root; those are publication targets only after the scratch
bundle passes the registered validation workflow.

## Registered retail/OpenNova comparison capture

The registered comparison catalog is
[`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json), canonical
SHA-256
`607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8`.
The final CP01/CP12 review poses are `cp01-water-oblique-retail`,
`cp12-yard-road-retail`, and `cp12-yard-tanks-retail`. Use the catalog payload
verbatim; an old screenshot, pose sidecar, or renamed fixture is not evidence
for one of these rows.

### 1. Freeze and capture OpenNova

Capture from a clean, immutable commit and use that same full SHA at retail
registration. The manifest also hashes the exact Godot executable and loaded
GDExtension, so rebuilding either binary requires a new OpenNova capture.

```powershell
$sourceCommit = (git rev-parse HEAD).Trim()
if (git status --porcelain) { throw "capture worktree is not source-frozen" }
$env:NOVA_EVIDENCE_SOURCE_COMMIT = $sourceCommit
$env:NOVA_GDEXTENSION_BINARY = (Resolve-Path "godot\bin\libopennova.windows.template_debug.x86_64.dll").Path
$env:NOVA_RENDER_FIXTURE_CATALOG = "res://../docs/render/render-fixtures-retail-v2.json"
$env:NOVA_RENDER_FIXTURE_ID = "cp01-water-oblique-retail"
$env:NOVA_RENDER_CAPTURE_MODE = "hud_hidden"
$env:NOVA_RENDER_FIXTURE_OUTPUT = (Join-Path (Resolve-Path ".").Path ".scratch\golden\render\fixtures\cp01-water-oblique-retail")

& $env:GODOT_BIN --path godot --resolution 2000x1200 res://tests/render_fixture_capture_probe.tscn
```

The probe must produce one raw 2000x1200 beauty artifact and a manifest whose
catalog hash, mission hash, fixture payload, capture mode, camera, source
commit, Godot hash, and GDExtension hash all survive the comparison preflight.
For this catalog, preflight also requires the catalog comparison contract
verbatim and a runtime witness sampled after pose settle and before fixture
freeze. The witness must report `WPN_M16BURST`, 30 rounds loaded and 270 in
reserve, character `0x0402`, bare `IndoArms.3di` arms with camo `[1,0,0]`,
`gameplay_hud_visible=false` while the HUD `CanvasLayer` remains active and
visible, active player-view effects and viewmodel, available and visible
terrain, hipfire/no ADS, no big map, and the requested catalog player pose.
Every emitted state sidecar must carry that same witness.

### 2. Stage highest-quality retail and capture the exact process

Before launching retail, stage the sole publishable
`retail_reference_highest_retail_selectable_v2` profile and bind the active
slot-0 profile. Staging never changes `hud_detail`, device identity, or
keyboard/gameplay tip preferences. The sanitized manifest must be a portable
sibling of the later registration; its derived restore token and caller-owned
backup are local-only.

```powershell
uv run python scripts/render/stage_retail_presentation.py stage `
  --game-cfg C:\GAMES\JOTAC\Game\JO\game.cfg `
  --weapon-sav C:\GAMES\JOTAC\Game\JO\expansion\revx02\weapon.sav `
  --backup C:\evidence-local\game.cfg.original `
  --manifest C:\evidence\retail\retail-presentation-stage.json
```

The stage record must contain this exact effective map (there are no lower
publication profiles):

```text
windowed=0                 video_res=1920x1200       gamma=0.8
terrain_polydetail=3       terrain_texdetail=3       object_polydetail=3
object_texdetail=3         display_16x9=1            water_quality=3
shadow_quality=3           particle_density=2        antialias_mode=2
texfilter_level=3          fbeffects_level=3         shader_usage_level=2
texcompression_level=2     lock_framerate=0          force_vsync=0
reduce_mouselag=1          NoBlood=0                 NoCasings=0
NoSmoke=0                  no_anim=0
```

On the authoritative capture machine, token `antialias_mode=2` is a one-based
ordinal into RevX02's runtime AA list, not a sample count. It must prove
effective `D3DMULTISAMPLE_4_SAMPLES` at quality 0; retail's selectable list is
exactly `[2,4]`, with 4x selected as its highest entry. The separate D3D9
color/depth intersection must prove hardware-usable maskable samples `[2,4,8]`
and a hardware maximum of 8x. Hardware-only 8x is not a retail-selectable
setting and is therefore not admissible as the reference path. The stage
manifest records original/effective typed maps, exact changed keys, preserved
adapter/tip/HUD values, and the video-option catalog hash. The approved active
`weapon.sav` SHA-256 is
`f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623`.
That profile decodes slot 0 as blue class/avatar `9/2/0/0x0402`, red
`9/7/0/0x8207`, with `WPN_M16BURST` in the selected blue kit.

Launch the freshly built external onHook MCP and keep all artifacts under one
create-new evidence directory. The MCP interaction is deliberately explicit:

The authoritative session must be launched operationally as
`C:\GAMES\JOTAC\Game\JO\Jointops.exe /exp revx02 /w`. Registration does not
mislabel command-line arguments as native runtime telemetry: the same-frame D3D
witness proves effective windowed mode, while the selected RevX02 archives and
profile are corroborated by engine-VFS resolution plus the exact-PID mission
log. Preserve the owned runner's recorded launch argv with the local raw
session, but do not publish it as a frame-correlated engine witness.

1. Launch the exact argv above through the owned mission session; retain its
   exact `instance_id` and PID.
2. Save `onhook_instances()` structured content and require that the same
   instance/PID reports proxy mode, `capture_bundle_supported=true`,
   `render_fixture_supported=true`, a ready 1920x1200 backbuffer, and available
   render state.
3. If the deployment/control-point overlay is present, activate the window
   owned by that exact PID and press SPACE once. Never send SPACE to a process
   selected only by executable name or to whichever retail window happens to
   be foreground. Re-read `onhook_instances()` and continue only with the
   original instance/PID after the deployment shell is visibly gone and
   gameplay is capture-ready.
4. Call `onhook_apply_render_fixture` for that same `instance_id`, using the
   catalog `retail_player_bms.applied`, yaw, pitch, `vertical_fov_degrees`,
   the sole `minutes_of_day` value multiplied by 60 as
   `time_of_day_seconds`, exact `mission_file` and catalog `mission_sha256`,
   and `camera_mode: "first_person"`; save the exact result and require
   `exact=true`. All of those fields are mandatory on every fixture call and
   must be reapplied when fixtures run sequentially; a value inherited from a
   prior fixture is not evidence. This is the shared requested teleport source
   named by `comparison_contract.player_pose_source`; it is not a claim that retail's
   later observed body position remains byte-for-byte equal after physics
   settles.
5. Call `onhook_capture_retail_reference({instance_id, path, fixture_id})`
   immediately.
   It arms at Present N and snapshots exactly frame N+1 at the exact RevX02
   `world_labels` (`0x5CAB26`) or `pre_feed` (`0x5CAB34`) callsite, performs one
   `EndScene`/`BeginScene` split around that snapshot, and then executes the
   original retail UI call exactly once without modification. It saves nothing
   unless the whole transaction succeeds; if neither cutpoint executes, capture
   fails closed. Retain its create-new PNG,
   `.state.json`, and same-process log.

Capture `cp01-waterline-below-retail` in an isolated fresh retail process.
Dismiss deployment for that exact PID, apply the underwater pose, and capture
its bundle immediately before breath expiry; do not queue another fixture or
reuse a surfaced/damaged process. Registration still decides validity from
the correlated observed state and camera matrices, not from equality between
the later player position and the requested teleport.

The capture is pre-onHook-overlay, retains the viewmodel, terrain,
post-processing, and all live effects, and has no gameplay HUD. Bundle v4 binds
the image hash and frame/QPC to the exact 21 quality/effect engine runtime
globals, adapter/device and backbuffer formats, the effective highest-retail-
selectable MSAA mode plus the distinct hardware-usable domain, and the
pre-HUD snapshot proof. The D3D frame identity equals the root snapshot
cutpoint; the presentation proof requires armed N, target N+1, and
`armed_qpc < frame_qpc <= target_qpc`, one scene split, and one unmodified UI
call. `windowed` and
`video_res` remain mandatory members of the 23-key staged profile; their
effective values are proven by that same-frame D3D windowed/backbuffer witness,
not mislabeled as engine runtime globals. Missing, extra, lower, unavailable,
stale, post-HUD, repeated-snapshot, failed-scene-restoration, or modified-UI
witnesses fail closed. Ordinary retail HUD and ImGui FPS remain visible throughout.
Only bridge protocol 1.4 with onHook 0.5.0 can supply a registerable raw-v4
bundle; raw v3, policy v1, and late `main_hud_fallback` evidence are rejected.

### 3. Register the retail bundle

Save the exact same-instance tool results, then run:

```powershell
uv run python scripts/render/register_retail_capture.py `
  --catalog docs/render/render-fixtures-retail-v2.json `
  --fixture-id cp01-water-oblique-retail `
  --raw-state C:\evidence\retail\frame.state.json `
  --raw-image C:\evidence\retail\frame.png `
  --instance-status C:\evidence\retail\instances.json `
  --fixture-result C:\evidence\retail\fixture-result.json `
  --capture-result C:\evidence\retail\capture-result.json `
  --onhook-log C:\evidence\retail\onhook.log `
  --game-dir C:\GAMES\JOTAC\Game\JO --expansion revx02 `
  --retail-executable C:\GAMES\JOTAC\Game\JO\Jointops.exe `
  --onhook-mcp C:\path\to\fresh\onhook-mcp.exe `
  --onhook-proxy C:\GAMES\JOTAC\Game\JO\binkw32.dll `
  --onhook-forwarder C:\GAMES\JOTAC\Game\JO\binkw32_.dll `
  --opennova-source-commit $sourceCommit `
  --retail-stage-manifest C:\evidence\retail\retail-presentation-stage.json `
  --confirm-retail-presentation-contract `
  --output C:\evidence\retail\registered.json
```

Registration fail-closes on a catalog, video profile, effective D3D mode,
presentation transaction, fixture, process, frame, camera, projection, mission,
source, or build mismatch. The saved capture result must
carry the exact v2 highest-retail-selectable profile ID and selected catalog
fixture ID, then bind the selected colocated PNG and state paths, byte counts,
exact process-start instance, frame serial, and QPC. That frame must follow the
exact fixture-application frame by no more than 120 frames. The catalog camera
is recovered from the inverse correlated retail view matrix, never from the
legacy eye-offset lead. Registration resolves the logical BMS through the
engine VFS, verifies its catalog hash and exact-instance launch log, and
records hashes for the ordered retail archives/version marker, retail
executable, external MCP, proxy, and forwarder. The executable, proxy, and
forwarder must resolve to `Jointops.exe`, `binkw32.dll`, and `binkw32_.dll`
inside the selected game directory; the explicitly selected MCP remains an
external retained build. A caller-only fixture label or legacy uncorrelated
sidecar cannot be registered by itself.

Registration must run before restoration. It independently hashes the
still-staged live `game.cfg` and active expansion `weapon.sav`, requires exact
agreement with the sanitized stage record and catalog profile contracts, and
embeds the stage and runtime witnesses. After successful registration, restore
the exact original config bytes:

```powershell
uv run python scripts/render/stage_retail_presentation.py restore `
  --game-cfg C:\GAMES\JOTAC\Game\JO\game.cfg `
  --backup C:\evidence-local\game.cfg.original `
  --manifest C:\evidence\retail\retail-presentation-stage.json
```

At contract definition time the authoritative original `game.cfg` SHA-256 is
`556880e9ec85d60021f2ce17d584bde30702a6ae3ecfba6a1e6eb54402c8cad3`;
applying the maximum profile produces
`e7bd7d27d6dcb22c8b58e3daa6ef543e2489f0600ffee28ffcb9a53d79806ed3`.
That authoritative original already has `antialias_mode=2`; staging changes
only `object_texdetail` from 1 to 3 and preserves ordinary `hud_detail`.
The new capture must record and verify its own hashes. Do not publish the
backup, absolute-path restore token, or restore receipt.

### 4. Build the registered comparison

```powershell
uv run python scripts/render/build_retail_side_by_side.py `
  --catalog docs/render/render-fixtures-retail-v2.json `
  --fixture-id cp01-water-oblique-retail `
  --opennova-manifest .scratch\golden\render\fixtures\cp01-water-oblique-retail\cp01-water-oblique-retail-manifest.json `
  --retail-bundle C:\evidence\retail\registered.json `
  --output-dir C:\evidence\published\cp01-water-oblique-retail `
  --opennova-caption "HUD hidden - bare arms - M16 Burst 30/270 - terrain" `
  --retail-caption "Pre-HUD snapshot - bare arms - M16 Burst - terrain - frame-correlated" `
  --roi world_center=240,180,1320,420 `
  --roi viewmodel_arms=850,700,900,500
```

The builder accepts only a raw 2000x1200 OpenNova image and raw 1920x1200
retail image. It fail-closes unless the catalog, OpenNova manifest and selected
OpenNova state carry the same matched presentation contract and runtime
witness, and unless retail registration carries the operator-observed contract
bound to the selected image hash. It performs exactly one full-source
HighQualityBicubic horizontal normalization from 2000x1200 to 1920x1200: no
crop, translation, vertical scale, or second normalization. It writes
create-new normalized, side-by-side, 50/50 overlay, absolute-difference, and
manifest artifacts with hashes, dimensions, captions, tool version, transform
metadata, and the matched-presentation provenance.

Every full-frame cross-engine metric is labeled
`qualitative_only_matched_hud_hidden_cross_engine`. The production catalog
requires both `world_center=[240,180,1320,420]` and
`viewmodel_arms=[850,700,900,500]`; ROI and mask metrics are labeled
`descriptive_explicit_presentation_regions` and are not pixel-parity gates.
The independent `world_only` diagnostic workflow above remains valid for
same-run subsystem isolation, but it is not a substitute for a registered
HUD-hidden retail comparison.
