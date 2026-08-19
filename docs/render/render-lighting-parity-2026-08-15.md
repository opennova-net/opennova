# Render/lighting parity evidence — 2026-08-15

*(2026-08-16 review correction: the water admission model this session landed
— "all pool-2 buildings reflect" — was refuted against the binary and shipped
mission data before merge. The corrected witness is vehicles plus records
whose BMS attribute authors `Reflective` (`0x800000`); see the water section
below and env-tod-re.md #30. The shadow, shader-pipeline, and capture-harness
content of this record is unaffected.)*

This record covers two OpenNova rendering repairs, one renderer-boundary
refactor, and the evidence plumbing used to compare them across three missions:

- production directional-shadow orientation and caster admission;
- reflected-world admission for water (vehicles + authored-Reflective
  records);
- replacement of runtime-composed object shader source with typed pipeline
  descriptors and finite checked-in technique resources; and
- frame-correlated diagnostics, capture bundles, exact post-spawn fixtures,
  and the [current 15-pair max-quality registered review set](../../screenshots/parity/render-lighting-2026-08/registered-2026-08-16/README.md),
  refreshed 2026-08-18.

It does **not** claim that glass/environment cubemaps, sky-dome rendering,
EffectWorld point lights, or every material technique now match retail. Those
systems remain represented in the fixture catalog so their divergences can be
measured without being mistaken for closures.

The retail facts below were checked live on 2026-08-15 in IDB
`Jointops.exe.kong.i64`, module `Jointops.exe`, imagebase `0x400000`, with
Hex-Rays available. They bound what the retail comparison is expected to
exercise. The immediate `SunShadow` synchronization described next is
OpenNova/Godot lifecycle plumbing; it is not inferred retail lifecycle
behavior.

## OpenNova behavior repairs

### Directional-shadow lifecycle and production admission

`SunShadow` could retain its default or previous transform until the next
process tick even after a `MissionEnvironment` was already available. The fix
publishes `_update_direction()` synchronously from both `_ready()` and
`set_environment_node()`. Rebinding also invalidates the cached emission
direction before the update. Consequently, the live dynamic directional light
is aligned when production `GameWorld` composition returns, without waiting for
an extra frame.

Focused coverage in
[`nova_sun_shadow_test.gd`](../../godot/tests/nova_sun_shadow_test.gd) pins:

- a preassigned environment direction at `_ready()`;
- a warm light's environment rebind before another process frame; and
- immediate alignment of `GameWorld`'s live `SunShadow`; and
- absence of the retired `NovaStaticSunShadow` whole-scene light.

That lifecycle repair only removes stale OpenNova light orientation; it does
not assert that retail performs the same calls or has the same scene lifecycle.
Static terrain shadows now follow the separate retail-shaped path documented
below: eligible static-model silhouettes are rasterized into terrain-page alpha
using the current environment projection vector, while the shared quantized
terrain-light epoch prevents sub-quantum time-of-day changes from invalidating
every resident page.

The second shadow failure was production-only. The gameplay camera cleared the
dedicated static/dynamic caster layers (`0x2000` and `0x4000`) to hide the
first-person body. Godot's directional-shadow admission intersects the current
camera mask, instance layer, and light caster mask, so the production camera
rejected every dedicated caster before shadow submission. The synthetic shadow
probe retained Camera3D's default all-layer mask and could not reproduce it.

The gameplay camera now admits both caster layers (`cull_mask = 0xFE7FF`) while
still hiding first-person-body and viewmodel visual layers. In first person the
avatar and held world weapon use `SHADOWS_ONLY`; third-person/debug views restore
normal casting, the dedicated viewmodel never casts, and teardown restores the
exact prior camera mask. Focused presenter coverage pins all transitions and
the production fixture diagnostics verify that beauty frames submit nonzero
shadow objects/draws while the paired `shadows_off` frames submit exactly zero.

### Water reflected-world admission (corrected 2026-08-16)

The earlier port treated the reflected world as a single vehicle-filtered
entity collection, which starved retail scenes like CP01 of their visible
building reflections. This session's first reading — "the sector-model pass
reflects all pool-2 buildings, independent of the entity filter" — was
refuted at review: the sector-model pass (`Terrain_RenderSectorModels
@ 0x5C5D30`, called at `0x5C8576`) renders `g_VisibleBuildingBatch`, and that
list is populated by `collect_visible_sector_userpoints @ 0x5C6B60` under the
same reflection filterMask as every entity wave (`(flagMatch & entity+36) ==
flagMatch` at `0x5C6C32..0x5C6C39`; mask = `camera_below_water ? 0 : 0x400`
from `Terrain_CollectVisibleEntitiesForReflection @ 0x5C90A0`).

Flag `0x400` has exactly two writers: `Entity_InitFromModel
@ 0x40E208..0x40E20A` sets it for `ItemDefType == 1` (vehicles), and
`Entity_SpawnFromBMSRecord @ 0x40ED1D..0x40ED2B` maps the mission-authored
BMS attribute `0x800000` (`bms::BmsiAttributeFlags::Reflective`) into it for
every spawned pool. Shipped data authors it per record: CP01 flags 217/956
buildings and 5/14 pool-1 items Reflective (with the same building graphic
placed both flagged and unflagged), 00TRa flags 63/926 buildings, CP12 none.

The mission placer now applies exactly that admission: `vehicle || (BMS
attribute & Reflective)` mirror-visible, everything else excluded above
water. Same-graphic populations with different authored policies are split
into separate batches, destruction routing carries the split key so hiding
one population cannot carve the other, and the husk graft keeps the carved
slot's reflect policy (retail's husk swap never clears flag `0x400`).
Cross-run pixel sheets remain a **non-gating visual diagnostic** because
water/particle phases are not canonical across launches. Semantic unit tests
and same-run fixture state are the gate. A remaining Godot-side performance
divergence stands: Godot batches per graphic, whereas retail culls sector
cells spatially.

### Typed, checked-in object shader pipelines

The object-material boundary no longer builds Godot shader source strings.
`describe_object_shader_pipeline` returns a renderer-neutral technique plus
blend, depth, cull, coverage, normal, environment, and specular policies. The
Godot cache fail-closes to a finite manifest of checked-in technique resources;
texture stages, normal source, self-lighting, glass/environment behavior, and
output policy are compile-time includes rather than `u_object_family` or
`u_cap_*` fragment switches. This is an architecture correction that preserves
the currently witnessed material math; it does not claim the remaining
cubemap/specular stand-ins are retail-equivalent. A committed golden pins the
normalized SHA-256 of every wrapper plus its transitive include closure, so a
shared FF/fog/glass/tracer formula edit cannot bypass CI merely because the
typed descriptor stayed unchanged.

## Evidence plumbing

The new seam is intentionally narrow:

- `game_render_diagnostics` returns one typed snapshot of the exact gameplay
  camera/projection, environment and weather, sky and water shader inputs,
  directional-shadow state, bounded visible active lights, root/reflection
  pass counts, renderer configuration, and runtime counters.
- `game_capture_bundle` writes the gameplay viewport without resampling as a
  lossless full-resolution PNG plus JSON state sidecar and SHA-256 metadata.
  Diagnostics are sampled inside the same `frame_post_draw` callback as the
  texture readback; persistence rejects a draw/diagnostic process-frame
  mismatch.
- Capture rejects a loading/start-splash state or an unavailable current
  gameplay camera. `world_only` capture reversibly hides the OpenNova HUD,
  menu composites, and viewmodel pass, then restores their exact prior
  visibility.
- Full-depth `describe_asset` for 3DI files returns bounded material summaries
  (technique, shadow inputs, glass/emissive interpretation, and authored
  textures/flags) plus bounded authored `LGHT` summaries. This is inspection
  data, not a renderer behavior change.

The versioned [`fixture catalog`](render-fixtures-v1.json) and production
[`capture scene`](../../godot/tests/render_fixture_capture_probe.tscn) add the
repeatable driver. The scene validates the catalog variant order and mission
SHA-256 before loading, realizes an exact catalog camera/TOD, freezes the
production shell after settling, and records five variants per selection:
`beauty`, `shadows_off`, `lighting_only`, `unshaded`, and
`directional_shadow_atlas`. See the
[render evidence README](README.md#reproducible-three-mission-fixture-capture)
for the exact invocation and environment contract.

## Three-mission fixture scope

| Mission | Fixture poses | Minute selections | Comparison questions |
|---|---:|---:|---|
| `00TRa.bms` | `00tra-courtyard`, `00tra-armory-glass`, `00tra-fire-barrel` | 7 | outdoor directional shadows/exposure, building and armory glass, and a fire-barrel identity anchor with one authored `LGHT` record |
| `CP01.bms` | `cp01-water-wide`, `cp01-waterline-above`, `cp01-waterline-below`, `cp01-suv-glass` | 7 | water/reflection admission, above/below-water branches, shoreline ordering, vehicle glass and shadow context |
| `CP12.bms` | `cp12-night-wreck`, `cp12-night-sky-up`, `cp12-night-sky-ground`, `cp12-truck-material` | 7 | night lighting and shadows, sky/cloud/horizon context, wreck/truck glass and environment-phong materials |

The full catalog is 11 poses and 21 pose/minute selections. At five diagnostic
variants each, a complete catalog sweep (one probe launch per fixture) produces
105 PNG/sidecar pairs. Coverage labels identify the question a pose is meant to
reveal; they are not parity verdicts.

In particular, `00tra-fire-barrel` records `FireBrl3.3di`'s authored `LGHT`
identity and attenuation fields. OpenNova now instantiates that record through
EffectWorld, but the existing sheet alone does not prove per-draw selection,
target flags, transient delivery, or teardown behavior; those require the
registered dedicated-light fixtures in the follow-up capture set.

## Selected final evidence and provenance

The final capture set was produced after the production single-player shell
auto-spawned the player. Every row below passed the same manifest gate:
`entry=single_player_auto_spawn_after_splash`,
`observed_at=before_fixture_freeze`, and
`world_loaded/runtime_playing/local_player_spawned/gameplay_input_active/gameplay_camera_current=true`
while `world_loading/spawn_or_menu_active=false`. Every bundle is a 1600x900
`world_only` capture, contains the five catalog variants, preserves the listed
minute and fixed24 value across all variants, and has
`capture.process_frame == diagnostics.frame.process` for every PNG/state pair.
All six manifests carry catalog SHA-256
`b16a00b8b1f998c43dcdf28438a8c58d31fc2e77f461d29c221812e159e4b458`.

The shadow counter tuple below is `objects / draw calls / primitives` for the
root pass. Raw bundles and manifests remain machine-local under `.scratch/` capture
roots; the listed hashes make their provenance explicit. The
`00tra-courtyard`, `00tra-fire-barrel`, `cp01-suv-glass`, and
`cp01-water-wide` rows were RECAPTURED 2026-08-16 on the corrected
reflection-admission build (their earlier frames contained water rendered
under the refuted policy); the CP12 rows kept their original bundles because
CP12's water is inactive at those poses.

| Mission / selection | Minute / fixed24 | Beauty shadow tuple -> `shadows_off` | Beauty PNG / state SHA-256 | Manifest SHA-256 |
|---|---:|---:|---|---|
| `00TRa.bms` / `00tra-courtyard` | 1125 / 314572800 | 998 / 998 / 2,553,528 -> 0 / 0 / 0 | `b0d6b15a1e1e839cb7ba50c0c77becb3562695780cd693d87503effa17e906f0` / `9043ae8c92e6d33815562aa68b7b4c70558650c78873262dd44e24d0ec81799b` | `22a7a4e81da649960846bacb57262b802b5097cee5fa34c686864c7f9e93099c` |
| `00TRa.bms` / `00tra-fire-barrel` | 1320 / 369098752 | 476 / 476 / 2,345,698 -> 0 / 0 / 0 | `201a51870a5059b40cd46b7af6b4f0f6a5c91b296f18114c77223448e33581a4` / `195544f0b9175fc8e0e2a37320968d813bed4ff1e14ecb3f0979eb2a86dbda83` | `0799da384f8e903c1bcdd0ee53f10944c7770e958ed9fb6dadc35a9a01cbed0e` |
| `CP01.bms` / `cp01-water-wide` | 720 / 201326592 | 714 / 714 / 4,097,173 -> 0 / 0 / 0 | `9035192f4048e8a635d1f8b8e9738bd0d432b2634c0cc7e886ea86893a36dceb` / `2ca4d8f67dcd02b5a56d223af6ade2f9862a28f7f0daa9b372d92be624a506e4` | `70f301ab8262dc5151f4fdf0ddc16b88715defe5f8ae08f87cf4086b579bd3b7` |
| `CP01.bms` / `cp01-suv-glass` | 720 / 201326592 | 83 / 83 / 1,388,884 -> 0 / 0 / 0 | `efc82ca18be7e502861567d692e6c8c35d0c66c1636bb82a90bc9f09988ffd99` / `f366aa5b2a3ea723ff7b64064dc75d4d3cfd5540d709877ef046dba793753348` | `16f0fdb95323681fae7d9926bee96bba9ba03a46a610f9174235ebba3c4225fd` |
| `CP12.bms` / `cp12-truck-material` | 1260 / 352321536 | 488 / 488 / 1,701,720 -> 0 / 0 / 0 | `d19ecfd250a80aa17281f2088a7c393466ab4a8a732ab4c25daff80e29edad1c` / `fea79aba4358c9161fb1f6e654cec7a15a92ce297d500fe9b3885486ee94d07d` | `b3d4967f6efa2a7f4fbc4b9eaaadb3157620e631cd5abe222671a2846d97ceb9` |
| `CP12.bms` / `cp12-night-wreck` | 1260 / 352321536 | 43 / 43 / 922,326 -> 0 / 0 / 0 | `f0f88691805ab2356789cecd0923da4e1d63051fa50f884167de2f2c02b3377b` / `010854e87ccade1ec9076ae5640248613e453e0e3bade5c622811a60682c6e81` | `aae0a976eef45f4d9595ee00b8e63459004bbb4ea7c4dafa6b804db3c2a1f8e7` |

The selected same-run shadow sheets compare `beauty` directly with
`shadows_off`; `lighting_only` is included as a third diagnostic panel. These
sources share one frozen presentation phase, so their full-resolution metrics
are the gate. `cp12-night-wreck` remains in the capture/context set but is not
presented as a shadow-delta sheet because its beauty and shadow-off PNGs are
pixel-identical despite the nonzero submission counters.

| Selected evidence | Result | Committed artifacts |
|---|---|---|
| `00TRa.bms` / `00tra-courtyard` m1125 | mean 1.398601, max 60, 137,924 changed pixels | [sheet](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/00tra-courtyard-m1125.png), [heatmap](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/00tra-courtyard-m1125-heatmap.png), [metrics](evidence/render-lighting-2026-08/00tra-courtyard-m1125.json) |
| `00TRa.bms` / `00tra-fire-barrel` m1320 | mean 1.128846, max 104, 107,363 changed pixels | [sheet](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/00tra-fire-barrel-m1320.png), [heatmap](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/00tra-fire-barrel-m1320-heatmap.png), [metrics](evidence/render-lighting-2026-08/00tra-fire-barrel-m1320.json) |
| `CP01.bms` / `cp01-suv-glass` m0720 | mean 2.405642, max 82, 160,037 changed pixels | [sheet](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp01-suv-glass-m0720.png), [heatmap](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp01-suv-glass-m0720-heatmap.png), [metrics](evidence/render-lighting-2026-08/cp01-suv-glass-m0720.json) |
| `CP12.bms` / `cp12-truck-material` m1260 | mean 1.118113, max 58, 196,288 changed pixels | [sheet](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp12-truck-material-m1260.png), [heatmap](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp12-truck-material-m1260-heatmap.png), [metrics](evidence/render-lighting-2026-08/cp12-truck-material-m1260.json) |
| `CP01.bms` / `cp01-water-wide` m0720 | **non-gating cross-run** pre-fix/corrected localization: mean 31.276250, max 241, 362,458 changed pixels | [sheet](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp01-water-wide-m0720.png), [heatmap](../../screenshots/parity/render-lighting-2026-08/subsystem-ab/cp01-water-wide-m0720-heatmap.png), [metrics](evidence/render-lighting-2026-08/cp01-water-wide-m0720.json) |

Exact source/output SHA-256 values (reference / diagnostic / final, then sheet /
heatmap / metrics JSON) are:

- `00tra-courtyard-m1125`: `135e862b6a97522d232d330f88f5a5fbda81a08dd791480ec7313b387bb2b3f2` / `ee9117cb33dc4135c4690476239cd81690971734fb72f7ba2448cddf0065c2d4` / `3756894738619ef748f0a756e5ab0b811398c829bcf4c1bc1b531428d5291a87`; `e07b44d3f9ae0661491c7527405d1f50eb11dae75697e2d43c51341858596fd9` / `15e2e243454fe8f469def6175638e22a38ee61f22f9361cfb9b07a645ea1a6c6` / `f832214e85b4040673b4a877818fad466e9fef8bb07790d9df3c0cdac5a5cffb`.
- `00tra-fire-barrel-m1320`: `8f44e47d60346a2da1a3ea9bb4322a5fe923e616de8c17666915704badd9b9ec` / `a8d91fe4f341940a2f3a286593eb388aebc8f2380d21cd77b52ad95fdad54752` / `fb6b91535bc9927f21922007a7b4e58d573d7d59d7cf0f058d85ab3f2c1b6138`; `ab78928c3debe4404bf7409ed7460b676dcdc561bafaeb7d31c930ffbacd1351` / `c96e248ae1712a80ecaf660087fd67747daa3e4db879e736539e8449e89b17ef` / `3adb393bfd840811e60d3738542e3a2ec6e3fc1152d6db39b94ea9512e08a9ec`.
- `cp01-suv-glass-m0720`: `149298d8e51c42bc0bef67d62edd10069d2c61b8e5ed46659d31d0851adefeb4` / `88419980acaff21884e8c39fe6a7044791d95ec08d88bf2ee415df84f8b68f2f` / `588e9accbe6ca01fa66c4785535d94daa48064c3125a531ee29d310b3c129081`; `4e2c5e3923b957b2eb4cf38088b419c3ac7bc64f79633517cddaebb00f0c55e7` / `43f10a15b1fa6c95f9cd9dc78ec7e80d44942f50a1257f05b16f23b97ac38130` / `ae8671614da838f9e16970aabcc12e95942fb200e7d0fe1851912a2d62fbe19b`.
- `cp12-truck-material-m1260`: `654551276aa1fe72f1f7ce7be76a6a60e2d4c1218a87c54029495e3f46ed06d1` / `5f20d30b360dcdbca0241683acb8960fef6ab9ffeb5d3f7ee1ef323e86aef7dd` / `d19ecfd250a80aa17281f2088a7c393466ab4a8a732ab4c25daff80e29edad1c`; `f14f298ad4526910a8bc8a83ce848aa946dbabc9e1044caef06c1c6df5fef0e8` / `23a6a393938f019249b2dc504f1c7c4ab4fff8438adf3273a4eaea5f47006eee` / `d5c273860671c0cef196cc6c7dfa51d60710f623752409d3a36de5b8ee209f62`.
- `cp01-water-wide-m0720` (pre / repeated pre panel / final): `15acb4cdd023f3ef939c87b4b1febffb1574ef66850b080d8cfc92fdb36eace9` / `15acb4cdd023f3ef939c87b4b1febffb1574ef66850b080d8cfc92fdb36eace9` / `e2e6347f82b72bc97e1aaa1e1db1a6f4bfddd841267137c6181abd93a49621fa`; `559db4f867a99400e4c0332267a388cacd88917120e6e964f53670c8e95accab` / `9f4b967f7325917402bdcc6f79f2dd039d887713179768e833a92a5b9e9c48ef` / `7b7ed3b944260220822f1cb31dd201b3b35afe8467e1f8240e3281567190f1a1`.

The water sheet deliberately repeats the pre-fix image in its second panel so
the four-panel builder can carry only the two available cross-run beauty
sources without inventing another baseline. Its panel labels, heatmap label,
and metrics all say **non-gating**: water and particle phases differ between
launches, no source was resampled, and semantic admission tests plus the final
same-run manifest are the acceptance gate.

The barrel fixture retains `FireBrl3.3di`'s single authored `LGHT` identity
(`lght_count=1`, style 113, attenuation 0..8), while final diagnostics report
`omni=0`. Its selected sheet proves directional-shadow behavior at the requested
location; it explicitly exposes rather than closes the unhosted EffectWorld
point-light gap.

### 2026-08-18 maximum-video publication contract

New evidence uses
[`render-fixtures-retail-v2.json`](render-fixtures-retail-v2.json) and the sole
profile `retail_reference_highest_retail_selectable_v2`. All terrain/object
geometry and texture controls are Highest; water, shadows, particles, framebuffer
effects, and shaders are at their top menu values; filtering is Anisotropic
High; compression is Minimal; and effective antialiasing must prove the
highest RevX02-retail-selectable mode. Token 2 is a one-based ordinal into the
runtime list `[2,4]` and yields 4x at quality 0. The separately witnessed D3D9
color/depth intersection is `[2,4,8]`, so 8x is hardware-usable but not exposed
by retail and is not the reference path. Resolution,
aspect, gamma, frame controls, mouse-lag mitigation, and all visual-effect
suppressors are also pinned by the catalog.

Stage v3 never changes `hud_detail`. On the authoritative config it changes
only `object_texdetail` from 1 to 3, producing effective SHA-256
`e7bd7d27d6dcb22c8b58e3daa6ef543e2489f0600ffee28ffcb9a53d79806ed3`.
Capture bundle v4 proves the full live scalar key set, D3D
adapter/backbuffer/depth/MSAA facts, and one
`retail_pre_hud_backbuffer_snapshot.v2` transaction. The root and D3D frame
identities name the pre-HUD cutpoint; armed N targets N+1, with
`armed_qpc < frame_qpc <= target_qpc`. The exact RevX02 `world_labels`
(`0x5CAB26`) and `pre_feed` (`0x5CAB34`) callsites are the only admissible
cutpoints; capture fails closed if neither executes. Exactly one pre-HUD boundary,
one restored `EndScene`/`BeginScene` split, and one subsequent unmodified retail
UI call are required. Registered capture v5/tool 4.0.0 and comparison v6/tool 4.0.0
reject missing, lower, type-coerced, stale, post-HUD, repeated-snapshot,
modified-UI, non-highest-retail-selectable, failed-restoration, and legacy
evidence. Raw v4 is accepted only from bridge protocol 1.4/onHook 0.5.0, and the
late `main_hud_fallback` boundary is not admissible.
Every exact fixture application also reapplies the catalog vertical FOV, the
sole authored minute multiplied by 60 as seconds, and the catalog mission
filename/SHA pair; sequential capture never treats a prior fixture's FOV or
clock as evidence for the next one.

### Current 2026-08-18 max-quality registered publication

The current publication is indexed at
[`screenshots/parity/render-lighting-2026-08/registered-2026-08-16/`](../../screenshots/parity/render-lighting-2026-08/registered-2026-08-16/README.md).
It replaces the lower-quality material that formerly occupied that path and
contains all 15 registered pairs, 15 overlays, 15 absolute-difference images,
15 normalized OpenNova images, and 75 raw OpenNova diagnostic images with
portable evidence records. All rows bind frozen source
`2cde75ad029be14d1c6e0dd8b034efc5fef017b5`, fixture catalog v2 at SHA-256
`607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8`,
stage/restore v3 (tool 3.0.0), raw capture bundle v4 from bridge 1.4/onHook 0.5.0,
registered capture v5 (tool 4.0.0), and comparison v6 (tool 4.0.0).

Retail frames are copied at the certified pre-HUD backbuffer boundary. The
capture transaction restores the D3D scene before retail's unmodified UI call,
so ordinary HUD/FPS remain visible during play while the registered screenshot
contains the matched HUD-hidden game composite. Across its 15 comparison-v6
manifests, full-frame MAE spans `6.089059`–`21.948388`, `world_center` MAE spans
`3.870290`–`32.554461`, and `viewmodel_arms` MAE spans
`4.142536`–`26.971500`. These are descriptive deltas rather than parity
thresholds; no metric from the superseded set is carried forward as if it
described these images.

### Archived 2026-08-17 HUD-hidden registered side-by-side workflow

> **Invalidated 2026-08-18.** The 15-row v1/v2/v3 set below is retained only
> as a historical diagnostic archive. Its common staged `game.cfg` used
> `object_texdetail=1` (Normal), and HUD detail 3 was staged for the process
> lifetime. Current validators reject its catalog, registered-capture, and
> comparison schemas.

The earlier orientation and “matched” context sheets were rejected by the
adversarial review. They mixed clocks, poses, aspect treatments, and UI, and
some retail frames lacked frame-correlated or verified fixture provenance.
Their screenshots and hashes are intentionally no longer listed as parity
evidence. Findings they helped localize remain governed by their IDA,
semantic, and same-run OpenNova tests; they are not rescued by relabeling the
old images.

The archived workflow used
[`render-fixtures-retail-v1.json`](render-fixtures-retail-v1.json), canonical
SHA-256
`6948682efc5e58d0ea887728e1318dd3c618b286a322b1bce430e9fbfff6ebf2`.
The final CP01/CP12 anchors are:

| Fixture | Authored minute | Review target |
|---|---:|---|
| `cp01-water-oblique-retail` | 930 | oblique water/reflection registration and shoreline materials |
| `cp12-yard-road-retail` | 1260 | night ground lighting, road context, and vehicle contact shadows |
| `cp12-yard-tanks-retail` | 1260 | tank materials, night sky, and directional-shadow context |

Every registered pair must satisfy all of the following:

- **Source freeze.** OpenNova capture occurs at a clean full 40-character
  commit and records that commit plus hashes of the exact Godot executable and
  loaded GDExtension. Retail registration receives the same commit; rebuilding
  or changing source requires recapture rather than a manifest edit.
- **Exact retail process.** `onhook_host_lan` returns one `instance_id` and
  PID. `onhook_instances`, the exact fixture result, CAP bundle, and
  `onhook.log` must all bind to that same instance/PID. If a deployment or
  control-point overlay appears, activate the window owned by that exact PID
  and press SPACE once; never broadcast SPACE by executable name or send it to
  whichever retail window is foreground. Continue only after that PID's
  deployment shell is visibly gone and the same instance is capture-ready.
- **Isolated underwater timing.** Capture `cp01-waterline-below-retail` in a
  fresh process by itself. Dismiss deployment for its exact PID, apply the
  underwater pose, and take the CAP bundle immediately before breath expiry;
  do not queue another fixture or reuse a surfaced/damaged instance.
- **Observed pose and frame.** The fixture application must report
  `exact=true`. The saved MCP capture result must bind the selected PNG and
  sidecar paths, byte counts, exact process-start instance, frame serial, and
  QPC, and the capture must follow the apply result by no more than 120 frames.
  Registration checks the 1920x1200 backbuffer/viewport, observed player pose
  and first-person BAM angles, finite rigid view/projection matrices, and the
  camera recovered by inverting the correlated retail view matrix against the
  catalog camera. The legacy eye-offset model is never accepted as observed
  camera state.
- **Mission and build identity.** Registration resolves the logical mission
  through the engine VFS, compares its SHA-256 to the catalog, checks the
  exact-instance launch log, and records the ordered archive/version-marker,
  game-directory retail executable/proxy/forwarder, and external MCP hashes.
  `/exp revx02` is an operational launch prerequisite corroborated by the
  selected expansion VFS/profile and same-PID mission log, not a claimed native
  argv witness; `/w` is independently proven by the correlated D3D device.
  Retail runtime telemetry still does not claim TOD, weather, water, light,
  material, or pass state.
- **Retail staging and restoration.** Before launch, the staging helper makes a
  byte backup and changes only `game.cfg`'s `hud_detail` token to 3. Its
  sanitized stage manifest binds the original/effective config hashes and
  decodes the approved active `weapon.sav` slot 0. After retail exits and
  before restoration, registrar 1.2.0 independently hashes the live staged
  config and profile, then binds the colocated sanitized manifest to the
  registered frame. The original config was restored byte-for-byte afterward:
  original/current SHA-256
  `556880e9ec85d60021f2ce17d584bde30702a6ae3ecfba6a1e6eb54402c8cad3`
  versus staged SHA-256
  `a5d3a6df6dbf49bb342f8201fe8e717a5c8685e623b6c6c82c3b8bd7eb831ca5`.
  The absolute-path restore token and backup remain local-only.
- **Declared normalization.** OpenNova renders the identical aspect-1.667
  frustum as a raw 2000x1200 frame. The registered builder performs exactly
  one full-source HighQualityBicubic horizontal 2000→1920 normalization to the
  raw 1920x1200 retail backbuffer, with no crop, translation, vertical scale,
  or second normalization. The transform and every input/output hash and
  dimension are recorded.
- **Matched presentation.** Every registered catalog fixture is `hud_hidden`
  and requires the M16 Burst, bare `IndoArms.3di` arms (camo `[1,0,0]`),
  viewmodel, terrain, hipfire/no sights, no big map, and the same requested
  teleport source in both engines, with gameplay HUD content absent.
  `player_pose_source` names that request;
  retail's observed body may settle afterward, while its correlated inverse
  view matrix remains the authoritative visual camera. OpenNova records runtime
  evidence after pose settle and
  before fixture freeze, including `WPN_M16BURST`, 30 rounds loaded, 270 in
  reserve, character `0x0402`, active-but-content-hidden HUD infrastructure,
  visible viewmodel infrastructure, active player-view effects, and available
  and visible terrain.
  The manifest and every emitted state sidecar carry the identical witness.
- **Retail presentation truth.** CAP is pre-onHook-overlay, not world-only. It
  retains the viewmodel, terrain, post-processing, and live effects but is
  registered as `game_composite_hud_detail_3` with
  `retail_gameplay_hud_visible=false`. The sanitized stage manifest proves HUD
  detail 3 and approved `weapon.sav` SHA-256
  `f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623`;
  its decoded slot-0 blue profile is class/avatar `9/2/0/0x0402` and includes
  `WPN_M16BURST`. An operator separately confirms the visible M16, bare arms,
  viewmodel, terrain, hipfire/no sights, no big map, and absent gameplay HUD in
  the selected hashed PNG. That observation is bound to the image hash and is
  not retail telemetry; retail ammo values are not inferred.
- **Metric policy.** Side-by-side, 50/50 overlay, and absolute-difference
  images support visual review. Full-frame cross-engine metrics are always
  `qualitative_only_matched_hud_hidden_cross_engine`. Each comparison also
  carries the required `world_center=[240,180,1320,420]` and
  `viewmodel_arms=[850,700,900,500]` regions under
  `descriptive_explicit_presentation_regions`. Those numbers are not
  pixel-parity gates.

The exact capture, registration, and comparison commands are maintained in
the [render evidence README](README.md#registered-retailopennova-comparison-capture).
Raw bundles remain machine-local under `.scratch/` per the asset-gated policy;
selected registered derivatives may be published only with their comparison
manifest and captions intact.

The superseded HUD-hidden publication formerly occupied the current
publication path and remains available only through repository history. It
contained all 15 registered pairs, 15 overlays, 15 absolute-difference
images, 15 normalized OpenNova images, 75 raw OpenNova diagnostic images, and
their portable evidence records: 150 PNG and 135 JSON files plus the root
index. All manifests agree on frozen source
`b476b1ef8acd65d18f7149f744e72d48f086e506`, GDExtension
`56b4c14c0d334ab34d67f41a50b2553fb216c9642877e24b31c84cf8a0129f32`, catalog
`6948682efc5e58d0ea887728e1318dd3c618b286a322b1bce430e9fbfff6ebf2`,
retail registrar 1.2.0/schema `opennova.registered-retail-capture.v2`, stage
tool 1.0.0/schema `opennova.retail-presentation-stage.v1`, and comparison
builder 1.3.2/schema `opennova.retail-comparison.v3`.

This refresh is the terrain-correction witness, not merely a re-publication.
OpenNova now composes retail-shaped 256×256 pages in a strict 128-entry cache,
uses the authored detail density at retail's `/512` scale with 16× anisotropy,
applies ordered `.til` source-over to RGB and render-target alpha before the
additive DOT3 term, and rasterizes supported static projected silhouettes into
page alpha rather than receiving them through a second whole-scene directional
light. Terrain and detail foliage bind the same current-frame page. The close
fire-barrel and courtyard rows expose the corrected terrain frequency; the
CP12 truck row shows the restored road-marking tiles and no longer contains the
prior broad OpenNova-only right-edge static shadow. Tile-marking contrast and
other scene lighting are still visibly non-identical.

Two CP01 catalog cameras were recalibrated from their registered HUD-hidden
inverse view matrices without changing yaw, pitch, or FOV:
`cp01-water-shallow-retail` moved from
`[-738.756812949,-754.324273981,22.051407255]` to
`[-738.8314622233299,-754.2562303493884,22.02728343982874]`, and
`cp01-water-oblique-retail` moved from
`[-738.565886752,-754.373620694,22.147367885]` to
`[-738.5563172384901,-754.3033306500754,22.034346508456707]`. This removes a
catalog-camera inconsistency; it is not an image-space alignment transform.

The superseded comparison-v3 descriptive metrics were:

| Fixture | Full MAE | Full RMS | `world_center` MAE | `world_center` RMS | `viewmodel_arms` MAE | `viewmodel_arms` RMS |
|---|---:|---:|---:|---:|---:|---:|
| `00tra-armory-glass-retail` | 11.573599 | 20.038291 | 12.745770 | 24.038533 | 8.275459 | 12.108769 |
| `00tra-courtyard-retail` | 6.774782 | 12.853416 | 4.432509 | 11.090609 | 8.423486 | 12.520295 |
| `00tra-fire-barrel-close-retail` | 21.726386 | 31.732540 | 22.455532 | 30.571325 | 22.154302 | 35.377431 |
| `00tra-fire-barrel-east-retail` | 27.712417 | 39.942489 | 32.137639 | 42.842025 | 23.340581 | 35.117458 |
| `00tra-fire-barrel-full-composite-retail` | 22.690368 | 31.690166 | 27.904099 | 34.505853 | 20.087127 | 30.991688 |
| `cp01-water-oblique-retail` | 11.795113 | 20.148972 | 13.212732 | 19.256464 | 13.351918 | 22.447448 |
| `cp01-water-shallow-retail` | 14.360274 | 25.079958 | 17.108431 | 27.731357 | 15.144431 | 28.345038 |
| `cp01-water-steep-retail` | 18.316210 | 28.364724 | 8.262014 | 13.476636 | 22.952868 | 34.561724 |
| `cp01-water-wide-retail` | 15.623756 | 26.179093 | 16.146837 | 26.351890 | 15.461413 | 27.299431 |
| `cp01-waterline-above-retail` | 13.644798 | 24.245648 | 15.886334 | 26.819165 | 14.091064 | 26.985823 |
| `cp01-waterline-below-retail` | 5.459962 | 9.306184 | 7.159897 | 11.211689 | 4.462837 | 6.976587 |
| `cp04-checkpoint-fires-retail` | 9.549140 | 14.497185 | 14.218179 | 21.249043 | 9.251667 | 14.091841 |
| `cp12-truck-material-retail` | 7.946208 | 10.844670 | 7.009431 | 9.015118 | 9.738097 | 14.005616 |
| `cp12-yard-road-retail` | 15.552747 | 20.343150 | 21.639992 | 25.455187 | 8.570433 | 12.096005 |
| `cp12-yard-tanks-retail` | 7.089630 | 9.951393 | 4.783038 | 6.270751 | 6.981205 | 9.752189 |

The below-water row is closest overall, but its waterline/glare ordering and
murk remain visibly non-identical. Surface-water reflection/noise differs by
angle; the steep row's world region is relatively close while its foreground
viewmodel region is not. Fire-barrel rows retain the largest deltas because
particle phase, blur, and local-light spill are unsynchronized. CP04 retains
fire, foliage, and live-actor differences. CP12 retains night exposure,
vegetation/ground sampling, residual road-marking contrast, vehicle/material,
and live NPC or flag-phase differences. The weapon and bare-arm silhouette now
match closely, but placement, material response, lighting, and animation phase
are not pixel-identical. These values describe this witness set; they do not
close the renderer or define a gate.

## Retail comparison bounds — live IDA Engine evidence

These witnesses define what the retail side of each comparison is expected to
contain. They do not transfer retail implementation details into the Godot
lifecycle fix.

| Retail subsystem | Comparison expectation and live witness |
|---|---|
| Dynamic projected-shadow admission | Use retail's model/entity admission as the reference for which dynamic entities enter the projected-shadow path. `[orig: Entity_InitFromModel @ 0x40E1BC..0x40E1F7]` |
| Static shadow filtering/submission | Compare static model eligibility and submission separately from dynamic actors. `[orig: Terrain_CollectAndRenderTileModels @ 0x60D421..0x60D463; Render_SubmitEntity @ 0x60D971]` |
| Terrain-tile shadow invocation/composite | Treat the retail tile render and composite as the reference destination for static sun shadows; do not infer whole-scene receiver behavior from a Godot directional light. `[orig: PolyTrn_RenderTile @ 0x60DC83..0x60DC99; @ 0x60E0C6..0x60E19D]` |
| Per-entity sun visibility | Separate entity illumination/visibility from cast-shadow admission when interpreting lighting-only and shadow-off variants. `[orig: Entity_ComputeSunVisibility @ 0x5C6800..0x5C68FF]` |
| Point-light attenuation | The fire-barrel comparison is bounded by the retail EffectWorld attenuation path. OpenNova now evaluates that color/range/attenuation math and feeds object shaders through an explicitly approximate camera-global four-light selection; retail instead selects per draw context. `[orig: Light_GetPointLightParams @ 0x5A9180..0x5A927A; update_light_slots @ 0x5ABC50]` |
| Authored model lights | Retail instantiates authored `LGHT` records into EffectWorld at mission start. OpenNova does the same for mission-start sources and late ObjectModel nodes; static batched-object destruction/husk rebinding and bone-follow remain residual. `[orig: parse_lights_chunk @ 0x5B47B0; Entity_SpawnGlowEffects @ 0x56C7C0]` |
| Sky | Compare the retail sky and its pass structure at the catalog poses/TODs; this session did not modify OpenNova sky behavior. `[orig: render_skybox @ 0x579080; passes @ 0x57988E / @ 0x579AC7]` |
| Water reflection | Every reflected-world leg — the sector-building pass included — draws only the collection filtered by flag `0x400` above water; the flag's writers are the vehicle item type and the mission-authored BMS `Reflective` attribute. OpenNova mirrors that admission. `[orig: Water_ReflectionPrerender @ 0x5C2780..0x5C27CE; Water_RenderReflectedWorldScene @ 0x5C8510; Terrain_RenderSectorModels @ 0x5C5D30; calls @ 0x5C8576/0x5C857B/0x5C8590/0x5C8599; collector filter @ 0x5C6C32..0x5C6C39; writers @ 0x40E208..0x40E20A / @ 0x40ED1D..0x40ED2B]` |
| Dynamic environment cube | Normal environment-reflection comparisons are bounded by retail's live environment-cube update path. `[orig: update_environment_cubemap @ 0x6106A0..0x6107C6]` |
| Analytic glass/specular cube | Glass/specular highlights are also bounded by retail's static cubemap fill, distinct from the dynamic environment cube. `[orig: Render_FillStaticCubemaps @ 0x58F290..0x58F34C]` |

The broader interpretation and current divergence statuses remain in
[`render-lighting-re.md`](render-lighting-re.md),
[`render-order-re.md`](render-order-re.md), and
[`env-tod-re.md`](../env/env-tod-re.md). This dated record only fixes the live
witness set used for this comparison run.

## Evidence boundaries and limitations

- **EffectWorld object lighting is only partially implemented.** The portable pool,
  lifecycle, target-disable gates, safe handles, and transient routes are live.
  The Godot renderer currently publishes one camera-global object-light set;
  it has no per-draw owner/interior isolation, terrain projection, foliage
  sampling, or corona pass. Registered retail telemetry and dedicated light
  fixtures are still required before treating this as visual closure.
- **Pixel gates remain within one OpenNova launch.** The fixture resets
  weather to the requested TOD, advances water once for the realized pose,
  then freezes presentation before its five variants. Weather is canonical,
  but water and particle phases are frozen noncanonical. Same-run
  `beauty`/`shadows_off` comparisons can be metric gates. Every cross-engine
  full-frame metric is qualitative because phases and renderer presentation
  still differ. The required world and viewmodel/arms ROIs are descriptive,
  never a pixel-parity verdict.
- **The matched contract is narrower than pixel-identical presentation.** Both
  sides now show the M16 Burst, bare arms, viewmodel, and terrain from the same
  catalog pose while gameplay HUD text is hidden. OpenNova's runtime witness
  also records 30/270 ammunition and active HUD infrastructure. Retail's
  visible presentation statement is an operator observation bound to the PNG
  hash, not telemetry; staged config/profile facts are separate machine
  evidence. Viewmodel placement/lighting, post-processing, water, vegetation,
  and live effects remain visible residuals. The separate OpenNova
  `world_only` workflow remains useful for subsystem isolation, not registered
  comparison.
- **Resolution and normalization are fixed.** Retail registration requires a
  raw 1920×1200 backbuffer and viewport. OpenNova supplies a raw 2000×1200
  image with the same aspect-1.667 projection. The registered builder performs
  one HighQualityBicubic horizontal 2000→1920 normalization over the complete
  source rectangle, with no crop, translation, vertical scaling, or second
  normalization, and records the transform and artifact hashes.
- **Retail telemetry is bounded.** The retail sidecar can record same-callback
  frame serial/QPC, player/camera state, D3D view/projection matrices, and
  viewport where available. Mission identity is verified externally through
  the engine VFS and same-PID launch log; caller fixture context is accepted
  only with an exact same-instance application result plus observed pose and
  projection checks. TOD/weather, water state, active lights, material ledger,
  and shadow/pass telemetry remain unsupported and must not be inferred.
- **No broad rendering closure is claimed.** This slice repairs directional
  shadow orientation/admission and water reflected-world admission, replaces
  the generated object-shader boundary without changing intended material
  math, and adds observation/capture tooling. Glass/environment cubes, sky,
animated effects, EffectWorld point lights, presentation blend space, and water
  spatial batching remain governed by their divergence records.

## PR evidence contract

Unselected raw bundles, retail tool transcripts, and full baseline sets remain
machine-local under `.scratch` according to the
[asset-gated evidence policy](../asset-gated-tests.md); the selected published
captures and derivatives do not. The current
[2026-08-18 max-quality registered set](../../screenshots/parity/render-lighting-2026-08/registered-2026-08-16/README.md)
supplies the following catalog-v2/stage-v3/raw-v4/registered-v5/comparison-v6
inventory:

1. selected within-run OpenNova `subsystem-ab` sheets and heatmaps committed
   under `screenshots/parity/` (for example, the same frozen pose's `beauty`,
   `shadows_off`, and `lighting_only` variants);
2. selected registered retail/OpenNova side-by-side, 50/50 overlay, and
   absolute-difference images with their create-new comparison manifest;
3. the source/generated SHA-256 values, frozen source commit, Godot,
   GDExtension, retail executable, MCP/proxy/forwarder, packed mission/archive,
   same-PID frame, pose/projection, and catalog facts retained by registration;
4. the mission, final fixture ID, current catalog hash
   `607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8`,
   exact fixture-result FOV/TOD/mission bindings, authored minute, raw
   resolutions, declared 2000→1920 transform, capture modes, captions, and the
   matched HUD-hidden M16/bare-arms/viewmodel/terrain contract;
   and
5. identical OpenNova manifest/state runtime witnesses, the retail raw-v4
   frame-correlated maximum-video/D3D/pre-HUD-snapshot witnesses, the sanitized
   highest-quality stage record with unchanged ordinary HUD state, and
   full-frame metrics labeled
   `qualitative_only_matched_hud_hidden_cross_engine`, with both named ROI
   families explicitly labeled descriptive presentation-region measurements.

Use
[`build_render_comparison.ps1`](../../scripts/render/build_render_comparison.ps1)
only for `subsystem-ab` evidence whose OpenNova variants share one frozen
animated phase. Registered cross-engine evidence goes through
[`register_retail_capture.py`](../../scripts/render/register_retail_capture.py)
and
[`build_retail_side_by_side.py`](../../scripts/render/build_retail_side_by_side.py).
The latter rejects pre-normalized or undeclared inputs, identity/build
mismatches, and presentation-contract drift; it preserves both witness types
and emits the qualitative metric policy with every comparison. Neither metric
set substitutes for subsystem-specific visual review.
