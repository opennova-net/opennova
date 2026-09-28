# Draw order / batching — reverse-engineering record

The runtime path from `Render_SubmitEntity` to sorted, ordered draws: the four
batch queues, the sort keys, the render-state stack, the technique-class
selection, and the frame's pass sequence, witnessed in retail `Jointops.exe`
(imagebase `0x400000`, IDB `Jointops.exe.kong.i64`). Implementing code:
`engine/runtime/renderer` (`render_order`, this slice's port; `material_classify` /
`object_shader_template` typed descriptor from REN-2),
`godot/src/object/object_model.cpp`
+ `object_shader_cache.cpp` (ladder application),
`godot/src/env/{celestial,water}.cpp` (the generalized
priority ladder); since the 2026-09-24 rendering parity pass also
`engine/runtime/renderer/scene_overlay` (the post-particle overlay tail),
`engine/runtime/renderer/frame_fx_effects` (the FrameFX screen effects) and
`godot/src/render/{scene_overlay_compositor,frame_fx}.cpp`. Landed by maturity REN-3
([maturity-program.md](../maturity-program.md); standing rules
[ADR 0023](../adr/0023-render-visual-parity.md) — the queue machinery is
witness-source, never a port target; the ORDERING SEMANTICS are the port). The
T1 instrument (`tests/renderer/state_vectors_test.cpp`) pins the sort-key and
pass-class functions in this record.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Transparent ordering ladder (sky → far-water-side → water → camera-side → glow overlays) | MATCHING | frame bracket `[orig: Terrain_RenderWorldScene @ 0x5c93a0]`, rigid per-strip queue split `[orig: Render_CollectRenderObjectsForBatch @ 0x5d8f20]`, bone-path submit flag `0x20` `[orig: Render_CollectRenderBatchesForEntity @ 0x5d95c0..0x5d961f]`; `renderer_render_order` ctest + `renderer_state_vectors` section 3 + `object_model_runtime_gate_test.gd` |
| Sort keys (opaque composite key; transparent `~float_bits` back-to-front) | MATCHING (key semantics ported as pure functions) | `[orig: @ 0x5d928e..0x5d92c8; @ 0x5d931c..0x5d9326]`; `renderer_state_vectors` sort-key vectors; the two original key quirks are D-RORD-6 (permanent candidates) |
| Technique-class selection (submit flags + state-stack defaults → 6 classes) | MATCHING for the locked highest-quality path | `[orig: @ 0x5d90d7..0x5d9145; @ 0x5d95c0..0x5d961f]`; `renderer_state_vectors` pins selection and `auxiliary_technique_validation.json` pins each class behavior; D-RMAT-6 fixed |
| Batch queue machinery (17-DWORD entries, CDynList68, per-frame quicksort) | witnessed / not a port target | ADR 0023: device-era artifact; the reimpl renderer owns its queues — semantics captured in the rows above |
| Opaque state-sort (coarse depth slabs → effect index → fine depth, alpha-tested last) | witnessed / reimpl-internal equivalent | `[orig: RenderBatch_QuickSort @ 0x5d8b40]` unsigned-ascending + key layout below; Godot's opaque pass sorts front-to-back with its own state batching — same intent, D-RORD-2 permanent candidate |
| Frame pass sequence (shadow slots → sky → viewmodel → terrain surface → world → overlay tail → FrameFX → HUD) | witnessed (confirm-only; corrected 2026-09-24: the sky pass precedes the viewmodel) | `[orig: Render_ProcessMainSceneFrame @ 0x5ca0f0: SkyDome_RenderWithSkyfog (the sky pass) @ 0x5ca81a, Player_RenderViewModelIfAlive @ 0x5ca829, Terrain_RenderMainSectorPass (the terrain surface) @ 0x5ca867; Terrain_RenderWorldScene @ 0x5c93a0]`; the frame list below; correspondence row |
| Viewmodel pass (near-Z 0.05 + viewport depth [0, 0.1], after the sky pass, own flush) | witnessed / reimpl ported (gates, blend order and fog ported 2026-09-24) | the gun draws INSIDE the beauty pass: every viewmodel instance rewrites its clip position through the weapon `renderfov` focal ratio and the near-Z 0.05 swap, with the clip depth remapped into the nearest tenth of the reversed-Z range (the retail depth band — `shaders/viewmodel_pass.gdshaderinc`, `ObjectModel.set_viewmodel_pass`), so the world drawn after it never overlaps the gun and the murk/bloom composite covers scene and weapon alike; its alpha strips take `kRungViewmodel`, AFTER the sky group, and the gun's blended strips are discarded where the opaque pass left a world depth (`viewmodel_world_covers`, `OBJ_BLENDED_PASS`), so later world draws paint over them exactly where retail's LESSEQUAL world draws wipe the no-z-write strips ("Let the world paint over the viewmodel's blended strips"); the gun takes the dry-pass fog (step 4 below, "Fog the viewmodel under the dry pass") and the retail draw gates (`fp_viewmodel_retail_submit`, "Port the first-person viewmodel draw gates"); `[orig: Player_RenderFirstPersonViewModel @ 0x4ded60; Render_SwapProjectionNearZ @ 0x58a8f0; Render_SetViewportDepth01 @ 0x58a7b0]`; the pass's `viewportScaleY` is `flt_8409E8` — the same `Render_SetAspectRatioMode @ 0x58d870` scale the world pass gets through `Render_SetViewProjectionWithDefaults @ 0x58f6b0` — so the FP frustum equals the world frustum (`[orig: Player_RenderViewModelIfAlive @ 0x4e0154 pushes it; @ 0x4dee5a..0x4dee7f]`, net-re §5.40 eighth pass); D-RORD-4; GUT `viewmodel_blend_order_test`, ctest `player_view` |
| EffectWorld particle ordering | MATCHING (deterministic port) | `ParticleFrameCompiler` orders emitter AABB centers back-to-front with source-index ties, then particles within each emitter by depth/source index, before adjacent state runs. `ParticleCompositorEffect` draws every command sequentially with depth test/no write, so reimpl surface/material sorting cannot reorder packet commands; retail's recursive alternating-axis/depth-bin order is ported (D-PTL-21 fixed 2026-08-22, [ptl-format-re.md](../particles/ptl-format-re.md)) `[orig: CParticleManager_RecursiveSortAndRender @0x5ec980; CParticleManager_RenderBatch @0x5e9890]` |
| EffectWorld pass placement around water | MATCHING (D-RORD-7 closed 2026-09-24, "Draw particle pass A inside the transparent list") | the portable compiler applies retail's emitter-scope strict-below/above-inclusive split and reverses far/camera subsets with the main eye. Pass A (`EffectWorld_RenderParticlePass(0)` `@ 0x5c95b5`) draws inside the transparent list at `kRungParticleFarSide`, after the far-side alpha strips and tracers and before the far-side detail foliage and the water, as `ParticleFarPass` render-list runs (one shared sort origin, sorting offsets keep the compiler's order); only its distortion-pipeline commands stay on the PRE_TRANSPARENT compositor effect, and the class-7 distortion defs draw in FrameFX's type-0 row. Pass B is the POST_TRANSPARENT compositor effect; the mirror publishes the same camera-selected subsets as two consecutive POST effects using its own view basis. The thermal byte selects the secondary particle materials in both passes ("Bind the thermal secondary materials in particle pass A too") |
| Glow/envmap duplicate queue (Q3) + bloom flush | MATCHING for the locked highest-quality path | Producers publish typed object/water/celestial snapshots to `Q3FrameCompiler`; `FrameFx` executes its retained draw list into a compositor-owned full-resolution target attached to resolved beauty depth. LUM re-shades the SELFLUM NORMAL block (the GLOW slot is a copy of it), Glass emits depth-tested/no-depth-write rotated specular, and water draws its NV bright pass. The terminal runs the bloom as the planner's DrawPass rows (`renderer::frame_fx_bloom_passes`): the altbuffer StretchRect into the power-of-two floor of (frame - 1) per axis (`[orig: FrameFX_CreateRenderTargets @ 0x583c7f..0x583c97]`), its 256² downsample, the two weighted pairs (the first 90+270 degrees with its U taps x0.75, `[orig: FrameFX_BloomKernel @ 0x58429e; FrameFX_BuildWeightedTapQuad @ 0x5815da]`) and the half-strength SRCALPHA/ONE composite; D-RORD-5 and D-RORD-10 fixed. Two admission facts pinned 2026-08-29 on synthetic fixtures (`framefx_test`): an AlphaBlend LUM strip's copy carries SELFLUM's zero material alpha and paints nothing (`mount_mtrl2_ab_lum`), and a per-vertex skinned model never publishes a Q3 source at all (`person_mtrl0_ad_lum`): `Render_SubmitEntity @ 0x5dade0` sends a skinned model (modelData+16 & 1) to `Render_CollectRenderBatchesForEntity @ 0x5d94b0`, which appends only the opaque list and the Q1/Q2 alpha queues (`@ 0x5d97ca`, `@ 0x5d983b`); the Q3 copy `@ 0x5d93b5..0x5d9447` is `Render_CollectRenderObjectsForBatch`'s alone (`renderer::q3_object_source_admitted`; no shipped skinned shader tag carries the GLOW capability either, `kMaterialDescriptorTable`) |
| Post-particle overlay tail (NVG lasers, precipitation, coronas, water glint, murk, sun glare) | MATCHING (2026-09-24, "Draw the scene's post-particle tail in its own overlay stage", "Draw the water glint and the sun glare at the end of the overlay tail"; the NVG IR laser beams since 2026-09-25, D-AI-12 (f); the weapon Inset pass's own tail since 2026-09-26) | retail's scene core ends with a fixed tail after particle pass B `[orig: Terrain_RenderWorldScene @ 0x5c9695..0x5c9722]`; `renderer::scene_overlay` carries it as data (`kSceneOverlayOrder`, `kMirrorOverlayOrder`, `kInsetOverlayOrder`) with typed batch builders; GameWorld's `scene_overlay` leg gathers one immutable frame and each view's `SceneOverlayCompositorEffect` runs it after that view's particle pair and before FrameFx (§The post-particle overlay stage); ctest `renderer_scene_overlay`, GUT `scene_overlay_stage_test` |
| FrameFX screen effects (distortion, damage/death blur, thermal, monitor, NVG) | MATCHING (2026-09-24, "Port FrameFX's screen effects and the NVG render-to-texture view", "Port the NVG view's scene raster, Scoped lens and Sighted card", "Draw the distortion particles and tracer ribbons in FrameFX's type-0 row") | `FrameFX_ApplyScreenEffect @ 0x584440` and `FrameFX_ApplyWeaponViewEffect @ 0x5845b0` are FrameFX's dispatchers (types 0/1/2/4 and 8/9), not shadow code; `renderer::plan_frame_fx` turns the frame's facts into the `FrameFX_DrawPass @ 0x582ab0` DrawPass rows in dispatch order (`[orig: Render_ProcessMainSceneFrame @ 0x5ca8f6..0x5caad5]`) and `FrameFxCompositorEffect` executes them; the first-person NVG view replaces the chain with the 512² scene / persistent 256² glow / tint composite (`[orig: @ 0x5ca516..0x5ca5cd; @ 0x5ca6ab..0x5ca73a]`). The NVG view's arms (`Render_ProcessMainSceneFrame @ 0x5ca516..0x5ca5b0` scene, `@ 0x5ca6f5..0x5ca73a` composite): the death screen and the binoculars take the full-screen composite; the Scoped byte takes `NVG_RenderScopedScene` (a square frustum at `ftol(flt_8409EC x 10485760 / zoom x 0.5)` Q16, no viewmodel) and the lens `NVG_DrawScopedLensThunk -> NVG_DrawScopedLens(1) @ 0x5d1d10` in place of the composite and the NVG mask; the Sighted byte takes `NVG_RenderSightedScene @ 0x5d2a50` (the frame's frustum at `ftol(5242880 / zoom)`, no viewmodel), whose `NVG_RenderSceneToTarget` draws the SIGHTS card into the 512 scene (`@ 0x5d08cb..0x5d0952`, overlayCtx 512 x 512), and the full-screen composite; otherwise `NVG_RenderScene` with the viewmodel. The frame-shaped arms rasterise 512 x 512 at the frame's frustum through a one-view XR projection (`TargetProjectionXrInterface`), as retail does (`NVG_RenderScene @ 0x5d2954..0x5d296d`, `NVG_RenderSightedScene @ 0x5d2aa9..0x5d2ada`; 2026-09-26), so FrameFX's copy of the 512 scene is 1:1. Every `NVG_RenderSceneToTarget` ends with the 64 x 16 polar unwrap (`@ 0x5d0a0e..0x5d0eb4`; eight blend-off passes, so pass 7 survives). The type-0 distortion sets draw through `FrameFxDistortionDrawer` (the effects device) with slot 2 bound to 256A / 256B. Ported: `renderer::frame_fx_nvg_view`, `renderer/nvg_scope_lens.h`, `NvgViewDevice`, `world::nvg_view_projection`; ctests `renderer_frame_fx_effects`, `renderer_nvg_scope_lens`, `player_view`, `local_player_view`; GUT `framefx_screen_effects_test`, `local_player_presenter_test` |
| Tracer ribbon slots | MATCHING (2026-09-24, "Port the tracer ribbon build and its three materials") | `CEffectEmitterPool_RenderMainPass @ 0x5dcaf0` draws only when exactly one of its two arguments is nonzero (`@ 0x5dcaf0..0x5dcb05`); the scene calls it `(0, eyeBelow)` `@ 0x5c95ac` and `(1, eyeBelow)` `@ 0x5c9687`, eyeBelow = `setz @ 0x5c959d` over the eye-above latch `setnl @ 0x5c93b0`: below the water the ribbons draw in the far-side slot, at or above it in the camera-side slot. Ported as `renderer::tracer_rung` on `kRungTracerFarSide` / `kRungTracerCameraSide`; each channel is its own draw call in pool order (the slot walk `@ 0x5dcb17..0x5dcb2e`), not a joined per-family strip. The ribbon build and materials are world-wac-ai-re.md §25; ctest `renderer_tracer_frame` |
| Scene projection near plane | MATCHING (2026-09-24, "Pin the world pass's 0.2 near plane in every camera mode") | `Render_ProcessMainSceneFrame` re-pins the near plane every frame, whatever the camera mode: `Render_SwapProjectionNearZ(flt_7C3340 = 0.2)` `[orig: @ 0x5ca4d7..0x5ca4e0]` (the device default `g_ProjectionNearZ` is the same 0.2 `@ 0x58ac0a`); only the viewmodel pass swaps in 0.05 (`@ 0x4dee1f..0x4dee29`) and restores 0.2 (`@ 0x4df0a0..0x4df0aa`). `renderer::kScenePassNearZ`, pinned by GameWorld's scene-environment leg on the render camera beside the far plane (live and frozen replay); the render-fixture capture probe no longer forces 0.05 |
| Authored object RLOD selection | MATCHING (D-RORD-11 selector closed 2026-08-29; D-RORD-12 entity projection producer corrected 2026-09-13; threshold units and slot order, the per-model sub-pixel floor, the frame focal and every org0 overlay's owner corrected 2026-09-24) | The native `ObjectProjectionSphere` supplies each ordinary entity's rotated CMDL midpoint and full half-diagonal, scaled once; persons use their initialized entity radius (type-185 model radius when flag 0x20 is set), composed head/body share the projection, and husks retain the primary graphic's sphere. `renderer::select_object_lod` walks the fine-to-coarse authored thresholds against the projected screen radius (`renderer::project_bound_sphere_radius_q16`, the witnessed integer projection, with the focal from the image camera's surface width) scaled by `renderer::object_lod_frame_scale` (detail 3 = 2.0 quality over the viewport width times 640; lower profiles detail * 0.33 + 0.34): slot i is level i's own threshold, the RMDL integer pixel count shifted into Q16.16 by the loader (`renderer::rlod_threshold_q16_from_rmdl`), and the walk starts at slot 0 and advances while the scaled radius is at or below the slot (equality goes coarser), so a table whose first slot is 0 pins the model to LOD0 (79 JO models); the coarsest-slot back-off compares the UNSCALED radius with the previous level's threshold when the selected level is the last or its own slot is zero, and draws that finer level when it is exceeded; then the missing-level fallback ("Read RLOD thresholds as retail pixel counts in their own level slot"). `kObjectLodSubPixelCullQ16` (0.75 px) drops an instance from every level before the selector, on every world `ObjectModel` (placed, avatar, wire, husk; any level count) as well as the static populations; attachments take their owner's verdict ("Port the retail occlusion collector, mask and sub-pixel rules"). `ObjectModel.update_authored_lods` (individual models, each swapping the selected level onto its own retained surface slots) and `MissionObjectPlacer.update_static_lods` (every retained static instance, per entity inside the 512-unit bins) consume it each frame. Every RLOD remains retained (an individual model caches each level as mesh/material rows, a static graphic as one population per level) and a transition swaps the rows onto the model's slots or moves which level population carries the static slot's row, without rebuilding meshes. Device mechanism (2026-08-30, no semantic change: which level is drawn, when, and its blend/order are untouched): each level's population holds only the slots currently at its level, packed dense from row 0 with `visible_instance_count` = live rows, a crossing swap-removes the row from the old level's population and appends it to the new one (shadow twins and the blended global rows the same), and a population with no live row is hidden so the GPU and every viewport's cull see one row per live instance instead of a zero-scaled row per level. An individual model (animated, portal-carrying, live-PANM) retains ONE `MeshInstance3D` per surface slot, sized to its widest level, and a crossing swaps the level's cached mesh, material, ROBJ/skeleton parent, skin binding and Q3 source registration onto those slots in place (the build serial and the instance ids never move; a slot the coarser level has no submesh for is parked hidden; the placer's shadow-only siblings are bound to their harvested level through `ObjectModel.add_level_bound_visual` and follow the same switch), so every presented transform propagates through one instance per drawn strip rather than through every retained level's hidden instances (`object_model_lod_occluder_test`). Measured 2026-08-30 against the dense-population head on the same machine and protocol (1600x900, `perf_mission_rows` medians of p50, n=2 on 00TRa/CP01 and n=1 on CP19): the retained ObjectModel surface instances fell 1423 -> 560 / 4243 -> 1376 / 5828 -> 1640, the Q3 source registry 76 -> 30 / 63 -> 21 / 138 -> 45, the frame 14.65 -> 14.47 / 14.80 -> 14.50 / 17.45 -> 15.98 ms, `world` 7.97 -> 7.81 / 8.35 -> 8.11 / 11.16 -> 9.91 ms, `present_mission` 0.32 -> 0.30 / 0.42 -> 0.34 / 1.28 -> 0.70 ms and `world_runtime` 1.69 -> 1.69 / 2.15 -> 2.03 / 4.26 -> 3.46 ms, with root draw calls and primitives identical (hidden instances were never drawn). The per-frame selector walks are one flat pass each: `update_static_lods` projects every retained instance (`ObjectLodFrame::project_q16` rejects a sphere outside the frustum before projection arithmetic) and `update_authored_lods` keeps its switch/attachment scratch across frames. A per-512-unit-cell bound-sphere rejection ahead of the projection was tried and dropped 2026-08-30: at the mission spawn poses (most cells in view) every run's `world_material` p50 moved UP with it by 0.015-0.03 ms (CP01 0.326/0.327 -> 0.342/0.342 ms, 00TRa 0.318/0.342 -> 0.353/0.357 ms, n=2 each), a small uniform cost and no gain. Against master the two walks are `world_material`'s measured +0.20 / +0.17 / +0.19 ms (00TRa / CP01 / CP19, the `model_*` sub-rows flat at +0.00..+0.03 ms): the price of selecting a level per retained instance where master retained no level at all. A threshold crossing hard-switches the drawn level exactly as retail does: `Model_SelectRlodLevel @ 0x5c3b20` returns one level in EAX, the overlap fraction it also writes is dead retail data (global `0x29ACD9C`, read only by a callerless stub `@ 0x5c38c0`), and no submit, collector or flush cross-fades or dual-submits across a threshold. Attachments draw at the parent's level clamped to their own LOD count (`renderer::attachment_lod_index`; `ObjectModel.set_authored_lod_owner` stamps every org0 overlay: the held weapon, the parachute canopy, the goggles, the binoculars and the carried object, 2026-09-24), and the composed avatar's head and body each walk their own table with the entity's one projected radius. Multi-view: retail runs the collector, the sub-pixel floor and the RLOD walk once per scene pass (the main view, then the weapon Inset pass `Render_WeaponInsetScene @ 0x5c9740`, called `@ 0x5ca949`, whose `Terrain_RenderWorldScene` call `@ 0x5c9de9` re-runs the collect `@ 0x5c94f0`; each pass computes its own scale and projections); the port collects per view (`OcclusionWorld::select_view`) and each view selects its own level, sub-pixel and section verdicts; where they differ the Inset draws a twin on its own layer (INSET_VIEW), and the terrain is per view too (2026-09-26; render-occlusion-re.md §8a) |

## Witness map

**The batch context.** One global object, `g_RenderBatchQueues @ 0x843490`
(renamed from `dword_843490`; the `ecx` of every family call). Layout (byte
offsets): four queues of 68-byte entries at `+0/+12/+24/+36` as
`{ptr, count, capacity}` triples (`CDynList68_Resize @ 0x5daf80`) — **Q0
opaque, Q1 above-water transparents, Q2 below-water transparents, Q3
glow/envmap duplicates**; the per-frame bone-matrix list (64-byte matrices)
at `+48` with count `+52`; frame stats at `+60..+80` (draw calls, batches,
tris, verts, effect switches — `RenderBatchCtx_EndFrameStats @ 0x5d87d0`
copies them to `+84..` for the HUD readouts); the render-state stack at
`+240` (`g_RenderStateStack @ 0x843580`, 32 × 16-byte entries) with its top
index at `+752` (`g_RenderStateStackTop @ 0x843780`); the active mirror/clip
matrix at `+756` (`g_ActiveMirrorClipMatrix @ 0x843784`, 64 B); the
water-split float at `+820` (`g_WaterSplitHeightFloat @ 0x8437C4`); the
mirror-winding byte at `+840` (REN-2: CULLMODE CW when set); `+841` set at
frame begin and every flush (the per-flush first-entry latch of
`Render_SetupEntityLightingAndShaderConstants`, which pushes the shared
constants on each flush's first entry and clears it; render-material-re.md
§Open questions); dev collector toggles at `+843/+844` (written
only by `Input_HandleActionBinding @ 0x49ad40` — debug keybinds).
`RenderBatchCtx_BeginFrame @ 0x5d8990` (sole per-frame reset, renamed from
`RenderBatchCtx_BeginFrame`) zeroes the queue counts, bone count, stats, and stack top, and
sets stack entry 0 to the defaults `{scale 1.0, 0.0, flags &= ~7, 0}`.

**The render-state stack.** Entries are `{float effectScale, float aux,
uint32 flags, uint32 aux2}`. `RenderStateStack_Push @ 0x4dbc70` (renamed from
`CNetPlayer_PushPositionToHistory` — it copies entry[top] to entry[top+1] and
increments the top; the wave renderers inline the same push) gives
copy-down inheritance; callers pop by decrementing the top. Flags bits 0/1/2
are the **class defaults** (CLIP/PROJSHAD/DEPTHMASK) for everything submitted
under that stack frame; batch entries capture the top's fields
(`entry[9]/[10]` = the floats, `entry[15]` = aux2). `Terrain_SetupEffectForEntity
@ 0x5c74a0` writes the per-entity effect scale into the pushed top. The
first sector wave (`Terrain_RenderSectorEntities @ 0x5c7b60`) additionally
sets flags bit 0 (CLIP) on the pushed entry and copies `g_WaterMirrorMatrix
@ 0x2980518` into `g_ActiveMirrorClipMatrix` when `g_WaterMirrorActive
@ 0x2980514` is set and the entity dips below the water plane — the
mirrored-pass clip machinery (env #30).

**Object RLOD selection.** The input to the authored RLOD table is the model
bound sphere projected into screen pixels, represented as Q16.16: the point
projector computes `2^32 / depth`, `(focal << 16) * that + 0x8000 >> 16`,
then `radius * that + 0x8000 >> 16`, and reports a fixed 4096 px when the
depth is smaller than the radius. The focal is half the viewport WIDTH over
tan(fov_h / 2), rounded: `viewport+0x40 = ftol(width x 0.5 / tan(fov_h / 2) +
0.5)`, the fov at `viewport+0x3C` being Q16 degrees (`dbl_7C3620` =
pi/180/65536) `[orig: Viewport_BuildProjectionMatrix @ 0x410fb0; (fov >> 1)
x dbl_7C3620 @ 0x410fc0..0x410fdb, width @ 0x410fc3..0x410fd3, the quotient
+ 0.5 -> _ftol2_sse @ 0x410fe1..0x410ff7, stored @ 0x4110e1]`; the port
derives the frustum from the camera drawing the frame's image (the stretched
target's camera while live; the keep-aspect mode decides which axis the Godot
fov names) and the focal from the surface width. The sector-entity draw
returns before the selector when that radius is at most 0.75 px (49152). The
table is ordered fine/near to coarse/far and slot i is level i's OWN
threshold: the RMDL chunk authors an integer pixel count (Armry01 200, 60,
20, 0; Ashed1 96, 38, 12, 0; 47gl_3RD 160, 64, 19, 6) that the loader shifts
into Q16.16 in the level's own slot `[orig: ThreediGp_LoadFromFile
@ 0x5b5bdf..0x5b5be5; the model is loader+4, ThreediGp_LoadModel @ 0x5b6273]`. The
walk starts at slot 0 (model+0x40) and advances while the scaled radius is
at or below the slot, so equality advances to the coarser level and a table
whose first slot is 0 pins the model to LOD0 (79 JO models)
`[orig: Model_SelectRlodLevel @ 0x5c3b3b..0x5c3b5a]`. The selected level is
clamped to the final row; when the level is the last or its own slot is
zero, the UNSCALED radius is compared with T[level-1] and, if it exceeds it,
the level one finer is drawn `[orig: @ 0x5c3b5d..0x5c3b8c]` (corrected
2026-09-24: the earlier reading had slot 0 unused and the walk starting at
slot 1). The submit then walks a missing model back toward lower indices
(finer geometry). The frame multiplier is the detail profile's quality term
(`detail * 0.33 + 0.34`, replaced by the fixed 2.0 on the highest shipped
profile 3) over the viewport width, times 640; a nonzero capture-quality
override (`dword_B4C3C0`, an input-binding toggle that also forces the 512
reflection target and a full cubemap refresh) substitutes 4.0 and is not
modelled. **The selection is a hard switch (D-RORD-11 closed 2026-08-29,
all addresses re-verified against Jointops.exe).** `Model_SelectRlodLevel
@ 0x5c3b20` (ex `sub_5C3B20`) returns the level index in EAX; Hex-Rays'
`double __cdecl` return is a misread of the x87 scaled radius left on the FPU
stack. The fraction it optionally computes is written only to global
`0x29ACD9C` (`fstp @ 0x5c426c` / `@ 0x5c3e88`, its address passed
`@ 0x5c42e4` / `@ 0x5c3ea1`), whose only read is a callerless `fld; retn`
stub `@ 0x5c38c0`; `Render_SubmitEntity @ 0x5dad80`,
`Render_CollectRenderObjectsForBatch @ 0x5d8f20`,
`Render_CollectRenderBatchesForEntity @ 0x5d94b0` and the flush never reference
it. Retail therefore neither cross-fades nor dual-submits across a threshold:
the fraction is dead retail data and the port does not reproduce it. A debug
level override (`g_DebugRlodLevelOverride @ 0x29ACD98`, read `@ 0x5c4265`;
its setter `Debug_SetRlodLevelOverride @ 0x5c38d0` has no callers) draws
level `override - 1` and skips the selector, and the sub-pixel gate runs only
while it is 0. The "dual-LOD submission" at `Terrain_RenderSectorEntitiesBySide
@ 0x5c7fea..0x5c8020` is the two-part player avatar, not an RLOD pair: when
the CharacterEntity blip (entity+0x3C) carries both a body (+0) and a head
(+4) model the entity is drawn twice with the SAME projected radius (the
collector's entry `*entityListIter`, stored `@ 0x5c8eca` from the entity
bound), head first after `Avatar_SetHeadCamoCtrl @ 0x57a370`
(`Render_SectorEntity(entity, radius, flags, head) @ 0x5c7ffc`), then the
body after `Avatar_SetBodyCamoCtrl @ 0x57a390` with flag `0x10000000`
(`@ 0x5c8020`); `Render_SectorEntity`'s fourth argument (`forced_model`)
replaces entity+48 as the RLOD table `@ 0x5c41b4..0x5c41b6`, so head and body
each walk their own table with one radius. Attachments never run the walk:
`BoneCallback_org0_World @ 0x4e3940` receives the parent's level as its frame
index and indexes every overlay model with `min(level, graphicModel[4] - 1)`
(the parachute canopy `@ 0x4e39c4..0x4e39ce`, NVG `@ 0x4e3b82..0x4e3b8c`,
binoculars `@ 0x4e3c2d..0x4e3c37`, the held weapon `@ 0x4e3ce8..0x4e3cf2`,
the carried object `@ 0x4e3e34..0x4e3e51`), which is
`renderer::attachment_lod_index`, consumed by `ObjectModel.set_authored_lod_owner`
(the third-person held weapon follows the head part of a composed avatar, the
body otherwise). The `@ 0x4e3e34..0x4e3e51` clamp is draw 6 of the PERSON
callback, the CARRIED object (carrier+0x268, set by the carry attach
`@ 0x43c144`, the CTF flag), not a vehicle's seat riders (corrected
2026-09-24). Seated riders are collected independently in retail too: the
entity collector (`Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60`) has no
parent test, it skips only Flags & 1 (`@ 0x5c8cef..0x5c8cf4`; riders carry
0x40) and item type 5 (`@ 0x5c8dd7..0x5c8de0`), and stores each rider's own
projected radius (`@ 0x5c8eca`), so riders walking their own thresholds IS
retail. The carried object (draw 6) is PORTED (2026-09-24,
`PersonOverlayModels`, "Draw the person overlays beside their bodies"): it
draws at the carrier's first-submit level clamped to its own count, and the
NVG/binocular/canopy overlays at each submit's level (draws 1, 3 and 4 are not
gated on 0x10000000 and render in both submits of a composed avatar; the
draw list is world-wac-ai-re.md §13.1). The entity+36&4 / entity+52/+56 machinery with
`Entity_PublishSwapFadePhases @ 0x5c3f40` (CTRL registers 65..70, span def+424
default 50, step def+428 default 25) is a controlled-material MODEL SWAP, not
an RLOD fade. `[orig: Model_SelectRlodLevel @ 0x5c3b20, back-off
@ 0x5c3b88..0x5c3b9b, dead fraction store @ 0x5c3bb3/0x5c3bc2; sub-pixel
gate and level consumption Render_SectorEntity @ 0x5c42de..0x5c42f0, forced
table @ 0x5c41b4..0x5c41b6; projected-radius producer
Viewport_TransformAndClipPoint @ 0x41177a..0x4117ec, stored per entry by
Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8eca; frame scale
Terrain_RenderWorldScene @ 0x5c940c..0x5c9499 and
Terrain_CollectVisibleEntitiesForReflection @ 0x5c90c3..0x5c9121; override
writers @ 0x5c08d1, @ 0x6106cb]`

**Submission.** `Render_SubmitEntity @ 0x5dad80`: stamps the shader clock
globals (`GetTickCount`, seconds = `(tick % 0xFA000) · 0.001` → `flt_272140C`),
allocates LOD GPU buffers, then dispatches on `modelData[16] & 1` to the
bone-path collector (`Render_CollectRenderBatchesForEntity @ 0x5d94b0`, gated on
ctx+843) or the object-path collector (`Render_CollectRenderObjectsForBatch
@ 0x5d8f20`, gated on ctx+844). **Its 4th argument is the render-flags word**
(the IDB's `numBones` and world §13's `numEntries` were misnomers):

| Flag | Meaning (witnessed at both collectors unless noted) |
|---|---|
| `0x1` | CLIP-class request; the bone path SKIPS entirely (`@ 0x5d94bc` — skinned entities are absent from clip passes) |
| `0x2` | PROJSHAD class |
| `0x4` | DEPTHMASK class; object path renders opaque strips only (`@ 0x5d948b` breaks before the alpha pass) |
| `0x8` | entry flag bit 2 = run only the technique's FIRST pass (`@ 0x5da23d`) |
| `0x10` | entry flag bit 3 = force `ZFUNC = ALWAYS` for the entry — a per-entry z-read-off override (witnessed at REN-4 in the FlushBatches pass loop `@ 0x5da32a..0x5da341`; else the pass zmode bit 0x80 picks ALWAYS/LESSEQUAL); the sun glow and the water glint submit with it (0x110: the glow `@ 0x5ad0f7`, the glint `@ 0x5ad470`). `renderer::kSubmitZAlways` (was `kSubmitEntryBit3`, "consumer unwitnessed") |
| `0x20` | bone path: transparents to Q2 instead of Q1 (the caller picks the water side) |
| `0x40` | object path: alt-vertex-stream request for sub-objects (`robjIndex > 0`) |
| `0x80` | alt-vertex-stream request (entry bit 1); used by parented sector entities (`entity+464`) and the armed viewmodel |
| `0x100` | suppress the Q3 glow/envmap copy (used by celestial submits `@ 0x5acc0a`) |
| `0x200` | MATCHTERRAIN class (the water-side waves' decal sub-pass) |
| `0x10000000` | second-part marker of the two-model player avatar (NOT a dual-LOD "far then near" repeat draw; D-RORD-11): `Terrain_RenderSectorEntitiesBySide` draws the HEAD model first (`@ 0x5c7ffc`, after `Avatar_SetHeadCamoCtrl @ 0x57a370`) and then the BODY model with this flag (`@ 0x5c8020`, after `Avatar_SetBodyCamoCtrl @ 0x57a390`); its ONLY consumer is `BoneCallback_org0_World @ 0x4e3c87`, which skips the held-weapon (`@ 0x4e3d99`) and carried-object (`@ 0x4e3e95`) overlay draws on the second part so they render once per entity (the canopy, NVG and binocular overlays are not gated; world §13). It never reaches the batch queues: `Render_CollectRenderObjectsForBatch` tests only 0x1/2/4/8/0x40/0x80/0x100/0x200. `renderer::kSubmitAvatarSecondPart` (was `kSubmitRepeatDraw`) |

**Batch entries** are 17 DWORDs: `[0]` strip ptr, `[1]` sort key, `[2]` bone
list offset, `[3]` robj field / bone count, `[4]` flags (bit 0 = bone-path
entry, bit 1 = alt stream, bit 2 = first-pass-only, bit 3 = flag 0x10, bits
4-6 = technique class), `[5..7]` up to three in-frustum light handles +
`[8]` count, `[9]/[10]/[15]` state-stack captures, `[11..14]` controlled-anim
slots (`dword_83FCE8[2·c]` for the material's anim-name chars at matdef+588),
`[16]` low word = LOD override. Technique class selection (`@ 0x5d90d7..0x5d9145`
object / `@ 0x5d95c0..0x5d961f` bone): stack-top flags bit 0 → CLIP (0x30),
bit 1 → PROJSHAD (0x10), bit 2 → DEPTHMASK (0x20); else submit flags
1/4/2/0x200 → CLIP/DEPTHMASK/PROJSHAD/MATCHTERRAIN; else NORMAL. The Q3 copy
switches to GLOW (0x40) when the material carries a GLOW pass block
(matdef+920). `CRenderBatchQueue_FlushBatches @ 0x5d9f50` maps class → the
cached pass block in the material def: NORMAL +600, PROJSHAD +680, DEPTHMASK
+760, CLIP +840, GLOW +920, MATCHTERRAIN +1000 (80-byte blocks; REN-2's
registry slots cached by `Material_ResolveEffectSubobjectsAndShader @ 0x5b1870`),
and runs the block's passes 0..n-1 in order (block +4 = pass count,
+16+8i = rules/z/a flags, +20+8i = the pass FOGMODE), gating each on its
light rules (flags & 0x3C vs the entry's ≤3 light handles, spot/point split
by `Light_IsSpotlight @ 0x5a9040`: 0x10 run-if-no-spots, 0x20 run-if-spots,
0x04 run-if-pointlights, 0x08 run-if-spots) — witnessed at REN-4: rules 4/8
multiply into ONE DRAW PER LIGHT (point params / spot projection set per
iteration + CommitChanges), rule 2 fills the PointLight*Array set for the
per-count VS variants, and MATCHTERRAIN-class entries bind the terrain tile
texture under the object (`TerrainTile_CacheLookup @ 0x604140`). Full pass
detail: [render-material-re.md](render-material-re.md) §Pass execution.

**Sort keys.** `RenderBatch_QuickSort @ 0x5d8b40`: in-place Hoare quicksort
of the 68-byte entries comparing the DWORD at entry+4 **unsigned, ascending**
(`cmp/jnb @ 0x5d8b93`); FlushBatches iterates ascending, so the smallest key
draws first.

- *Opaque key* (`@ 0x5d928e..0x5d92c8`, identical at `@ 0x5d9735..0x5d9781`):
  `dist = clamp(ftol(robj_view_depth), 0, 1023)` where the depth is the robj
  origin dotted with the camera-forward plane
  (`g_BatchSortDepthPlane{X,Y,Z,W} @ 0x2721A08/18/28/38`). Bit layout:
  `[14]` = matdef+514 bit 0 (**the alpha-test material flag** — REN-2's flag
  byte, offset pinned here: +513 = skip/disabled byte, +514 = flag byte,
  +515 = alpha ref), `[13..12]` = dist bits 9..8, `[11..6]` = effect registry
  index (matdef+596 ptr − `g_HLSLEffectRegistry`, ÷1004 via the 0x828CBFBF
  magic divide) & 0x3F, `[5..0]` = dist bits 9..4. Draw order: plain opaques
  before alpha-tested; within: 256-unit depth slabs front-to-back, effects
  grouped within a slab, 16-unit fine depth within an effect. Bits 15..31 OR
  in an uninitialized stack slot (`@ 0x5d92b9..0x5d92c4`) — constant within
  one collect call, so harmless; see D-RORD-6.
- *Transparent / Q3 key* (`@ 0x5d931c..0x5d9326; @ 0x5d9405`): `key = -1 −
  bit_cast<int32>(depth_float)` = `~bits` — under the unsigned ascending sort
  this is exact **back-to-front by IEEE float order**. Quirk: the slot it
  reads holds the robj camera depth for the robj's first alpha strip but is
  then overwritten (`fst @ 0x5d9347`) by each strip's water-split height, so
  later strips key on the PREVIOUS strip's value (D-RORD-6).
- *Queue pick* (object path, per strip): the strip center's world height
  (matrix column-1 dot `@ 0x5d932e..0x5d9344`) ≥ `g_WaterSplitHeightFloat`
  → Q1 (above water), else Q2 (below). The bone path picks by submit flag
  0x20. The split threshold is written per frame from `g_EnvWaterHeightFixed`
  (`@ 0x5c93e2..0x5c93f0`).

**Sort-and-flush.** `CRenderBatchQueue_SortAndFlush @ 0x5dae40` (mode):
mode 1/2/3/4 = sort+flush Q0/Q1/Q2/Q3 alone; **mode 0 = Q0, then Q2, then
Q1** — the mini-scene flush (viewmodel, loading screen, UI/avatar previews,
HUD 3D elements, the sky/celestial family, tile models, shadow slots — every
caller outside the world pass uses mode 0). Q3 is flushed ONLY by
`FrameFX_RenderGlowSource @ 0x582940` (mode 4 `@ 0x582a54`): the glow/envmap
duplicates draw during the bloom overlay after the scene. After flushing,
counts reset; blend-op and fog state are restored.

**Highest-quality reimpl mapping (D-RORD-5 fixed 2026-08-22; D-RORD-10 fixed
2026-08-29).** Retail draws its Q3 flush into a
backbuffer-sized altbuffer only because D3D9 keeps the beauty depth-stencil
bound there (`FrameFX_CreateAltBufferTexture` creates it from the D3DPRESENT_PARAMETERS block
with the backbuffer's size and multisample mode; nothing opaque is ever
re-rasterized), and the only consumer of that surface is the StretchRect
into the power-of-two capture (the highest power of two not above
backbuffer - 1 per axis: 1920 x 1080 captures at 1024 x 1024;
`FrameFX_CreateRenderTargets @ 0x583c7f..0x583c97`) whose 256²
downsample feeds the kernel (`FrameFX_CaptureAltBuffer
@ 0x5840a6..0x5841c0`; the kernel itself is `FrameFX_BloomKernel @ 0x5841d0`, which
calls `FrameFX_RenderGlowSource @ 0x584234` for the source only). The
altbuffer is created by `IDirect3DDevice9::CreateRenderTarget` (vtable +0x70)
`@ 0x58217e`, not CreateTexture. `FrameFxCompositorEffect` owns a
full-resolution Q3 color target with resolved beauty depth attached.
Main-thread producers publish generation-bound mesh, transform/bone, texture,
and material values; `Q3FrameCompiler` rejects unsupported rows (a geometry
lease whose generation no longer matches the published one included), sorts
object duplicates back-to-front, and retains the fixed water/celestial/sun
bracket. Geometry is packed once per (source, surface) into the adapter's
retained cache (`q3_geometry_cache`): one server read at first sight, one
device buffer per entry uploaded per generation, MultiMesh rows read once per
instance generation (no Q3 producer carries a bone palette: the bone path is
never a Q3 source, and a fake-skinned rigid strip (the FP gun's) draws at its
one bone's part matrix); the water strip publishes the CPU arrays it uploads each frame, and a
static RLOD switch or destruction carve invalidates only its populations'
instance rows (the packed surfaces and their device buffers stay; the rows
are packed dense, so the re-read covers live rows only), so a stable
frame reads nothing back and re-packs nothing and a row rewrite re-reads rows
alone (the backend report's `q3_readbacks_this_frame`,
`q3_instance_row_reads_this_frame` and `q3_packed_vertices` pin it).
The producer registry is a set of persistent per-source records
(`q3_source_registry`, 2026-08-30): a registration stores the source's kind
and, at its first in-frustum sight, its surface list with each material's
classification and Q3 block (`u_diffuse`/`u_detail`/`u_rgb_mod`/
`u_alpha_mod`/`u_reflect_color`/the UV rows); the node's `tree_entered`/
`tree_exiting`/`visibility_changed` signals move the record between the live
and dormant sets and mark its visibility (`tree_exiting`, not `tree_exited`:
a source freed inside an ancestor's exit-tree handler, the shell's world
releasing its water strip as the shell leaves the tree, never emits
`tree_exited`; `framefx_test` pins it), ObjectModel's runtime parameter
writes name the material (`FrameFx::invalidate_q3_object_material`, a
parameter version per registered material) so a stable object material is
never re-read, and a record outside the tree is the only one checked
against ObjectDB. An ObjectModel level swap re-registers the same node with
the level's material: the record keeps its identity and bumps its geometry
generation (the swapped mesh is re-packed once), and a level whose material
carries no glow (or `FrameFx::unregister_q3_source`) parks the record
inactive without touching its generations, so the geometry cache never sees
one node's generation twice. The per-frame compile walks the live, active
records alone: it probes each visible record's transform and layer mask
(Godot exposes no transform-changed signal for an engine-class node to an
extension),
frustum-tests the cached world bounds and, for a MultiMesh population, its
cached per-row world bounds (the live rows only, read once per instance
generation; a carved row never enters the list), and re-reads only the
water and celestial blocks, whose producers publish per frame. The report's
`q3_records`, `q3_records_touched_this_frame` and
`q3_material_reads_this_frame` pin it (`framefx_test`: a stable frame of
an object-only scene touches 0 records and reads 0 object materials; a
move, a visibility change or a named material is picked up through its
record alone). In a mission a stable frame still touches the records whose
transforms move every frame, the water strip plus the celestial bodies
(their bounds refresh on every transform compare mismatch): sampled at one
frame the counter read 5 / 12 / 5, 5 / 8 / 1 and 1 / 12 / 1 across three
runs each of 00TRa / CP01 / CP19 (2026-08-30 A/B), so a one-frame sample
is not a per-frame rate. Measured with the perf probe (1600x900 windowed,
Ryzen 7735HS iGPU, median of per-run p50 over 2 runs): WORLD_FRAMEFX
1.16 -> 0.57 ms on 00TRa and 1.04 -> 0.40 ms on CP19 with the records
alone; the stream packer reading the surface arrays through their raw
pointers into one sized buffer (the water strip re-packs every frame) takes
the leg to 0.22 ms and 0.08 ms. The packer's gain is that leg's alone: the
wall frame between its before and after sessions moved 14.11 -> 14.59 ms
(00TRa) and 16.24 -> 16.75 ms (CP19) with `render_root_gpu` +0.65 ms while
within-session runs agreed to 0.03 ms, so those sessions drifted and carry
no frame-level claim. On the merged head (the records over the dense
populations and the surface slots, Godot occlusion culling off, same
protocol, n=2 on 00TRa/CP01 and n=1 on CP19, measured 2026-08-30 against
the surface-slot head in one session): WORLD_FRAMEFX 1.04 -> 0.21 ms
(00TRa), 1.09 -> 0.14 ms (CP01) and 1.26 -> 0.12 ms (CP19), the frame
13.48 -> 13.05, 13.73 -> 13.00 and 15.98 -> 14.47 ms, with root draw
calls, primitives and the Q3 command count unchanged for a given spawn
state (the Q3 source count on CP01 varies run to run with the bots' held
weapons in view, 17 to 21).

**Frame cost against master, 2026-08-30 (the closing A/B).** Master
e17349529 against the head carrying every lane above plus the closing
fixes (the slot-capture cache keyed on the mesh, the static populations
under one holder, the cell walk dropped), 1600x900 windowed D3D12
Forward+, Ryzen 7735HS iGPU, vsync off, Godot occlusion culling off on
both, `--exp jox01` on both, `perf_mission_rows` (6 s warmup, three 8 s
windows), three interleaved launches per tree and mission, medians of
per-run p50 in ms (00TRa / CP01 / CP19, delta = head - master). Wall frame
13.89 -> 12.90 / 14.12 -> 12.93 / 17.01 -> 14.48 (-0.99 / -1.19 / -2.53),
frame p95 -1.06 / -3.89 / -2.26, `draw` (the RS::draw span) -1.20 / -1.43
/ -3.83, GPU total over root + Q3 + water + slot -2.26 / -2.48 / -5.82,
render-server CPU total over the same four -0.62 / -1.98 / -0.17; the
128-frame environment-cube cycle costs 1.1 / 1.2 / 1.6 ms above the
baseline frame instead of master's 35.0 / 57.8 / 53.9 (its worst frame
16.8 / 16.6 / 19.2 ms instead of 39.9 / 53.0 / 57.1). Root draw calls
487 -> 489 / 376 -> 416 / 284 -> 296 (the bins holding entities at two
levels), root primitives -17 % / -5 % / -22 %. The main-thread `world`
tick is the row that stays above master, +1.52 / +0.67 / +1.21, and it
decomposes into rows that are each attributed: `world_slot_shadow` +0.33 /
+0.16 / +0.24 is the render-slot compile that master's separate viewports
did inside `render_slot_cpu` (1.89 / 1.00 / 0.17 ms mean there, 0 here), a
row move with a net win; `world_material` +0.18 / +0.17 / +0.19 is the
two RLOD selector walks (the RLOD row above; master selected no level);
`world_framefx` +0.19 / +0.12 / +0.09 is the incremental Q3 compile over
its records (master's 0.03 ms compiled nothing on the main thread and paid
1.19 / 0.97 / 0.48 ms of `render_q3_cpu` instead); `world_runtime` +0.19 /
+0.06 / +0.23 is `present_snapshot` and `present_mission_core` over the
retained surface instances (560 / 1376 / 1640 geometry instances against
master's per-level nodes); `world_light` +0.17 / +0.02 / -0.02, down from
+0.51 / +0.15 / +0.10 before the populations moved under their holder:
`EffectLightDirector.render_frame` casts every child of the container to
`ObjectModel` each frame, and the placer had emitted 835 / 490 / 494
populations beside the 77 / 244 / 252 models, so the row tracked the
population count and not the instance stamping (the remaining +0.17 on
00TRa is within that mission's run-to-run band for the row, 0.76-0.93 ms
on either tree). `render_root_cpu` +0.62 / +0.51 / +0.35 is the focused Q3
capture/blur/composite pass executing as a compositor callback inside the
root viewport's span (render-occlusion-re.md, the device-occluder row,
carries the elimination of the population count and the occluders); the
render server's CPU total is below master. `render_water_gpu` 0.72 ->
1.55 / 0.94 -> 1.79 / 0.61 -> 1.48 is the one row without an attribution:
the mirror viewport draws the same objects, draw calls and primitives on
both trees (290 / 275 / 664K-668K on 00TRa), at the same 256-square
target, through the same decode-only terminal effect and the same
particle POST pair, and the only mirror-side code difference is the
camera cull mask gaining the thirteen visual layers the retired slot
captures reserved, which nothing in the scene occupies (no light, decal or
instance sits on them). The mirror's terminal effect is eliminated: with
it not installed at all (00TRa, two runs, same protocol) the row read
1.50 ms against 1.55 with it, and the mirror still drew 290 objects / 275
draw calls / 663734 primitives. What remains is the particle POST pair on
the mirror camera and the render-server timestamp span absorbing device
work the beauty chain now records around it; a lane on the row masks the
particle pair next. The GPU total is what the frame pays, and it is
2.3-5.8 ms below master.
The render callback re-shades each LUM copy as the SELFLUM NORMAL block into
the black-cleared Q3 target (`_FFP.fx` copies the NORMAL pass block into the
GLOW slot `@ 0x5afc7f`: Diffuse1 x Detail MODULATE2X x RGB modulator x
sat(SelfLumColor x gain) x 2 (`renderer::q3_emissive_modulate2x`; the
earlier min(gain, 1) form corrected 2026-09-24), the wrapper's fog policy,
alpha 0, so blended LUM contributes nothing and additive LUM adds its colour;
nothing samples the beauty colour), then emits Glass's CubeRotSpecular and
water's NV bright pass (`g_WaterPSBumpReflectNV`: `color *=
saturate(luma(0.25,0.60,0.15)² − 0.15)` with the device fog color forced
black; the NV path `@ 0x5c3442..0x5c3458`, `CD3DDevice_SetFogAndBlendMode(2)
@ 0x6778ed`). The bloom pass calls `Render_WaterSurface(0, 1)`
(`@ 0x582a59..0x582a5d`) unconditionally: no `g_WaterActive` or blink gate,
only `Render_WaterSurface`'s own gates (`g_EnvWaterHeightFixed` != 0, the
camera STRICTLY above the water `@ 0x5c3304`, detail > 1). It regenerates the
noise pair and marches the above-water rows with nightvision = 1 (the flat
0.1 base `@ 0x5c2d5a`, specular RGB dropped `@ 0x5c2ef8`, mode-2 black fog,
`g_WaterShaderBlendNV`), and never draws underwater, so the Q3 copy has no
underwater branch; the port's water owns a second strip (layer mask 0, the
typed Q3 `WaterNightVision` source) marched with the NV row colours every
frame the camera is above the water ("Port the water strip's texcoords,
depth and nightvision redraw"; env-tod-re.md #29). Then the celestial discs
(`Render_CelestialBodies(1) @ 0x582a77`) and the glow
(`Render_SkyboxSunGlow(0, 0) @ 0x582a80`) through the far-band viewport:
`Render_SetViewportFarDepth @ 0x582a70` (MinZ 0.98 / MaxZ 0.99996948
`@ 0x58a840`) remaps their depth into that band before the ordinary z-tested
flush. The beauty depth they test was written through the scene viewport
(`Render_SetViewport @ 0x58a720`, MinZ 0 / MaxZ 0.99996948), so a fragment at
view depth w survives over beauty depth D iff MinZ + (MaxZ - MinZ) z(w) <=
MaxZ z(D); the port scales its own reverse-Z depth by (MaxZ - MinZ) /
SceneMaxZ (`q3_far_band_reverse_z` in `renderer/q3_frame.h`, "Test the bloom
far band against the scene viewport's MaxZ"; depth-tested, no depth writes;
env-tod-re.md §Celestial bodies). Each disc's colour is its authored SELFLUM
material's RgbGen at the pass's UPL_INTENSITY, emissive sat(SelfLumColor x
gain) x 2 ("Saturate the celestial bloom emissive like the SELFLUM copy"). No Q3
viewport, camera mask, depth-occluder rerasterization, proxy geometry, or
compatibility renderer remains. The beauty camera is standardized to mask
`0x78C01` (the viewmodel layer folded in 2026-08-26, the terrain foliage
bit 17 admitted 2026-09-01 — the mirror camera drops it, env #30; the
empty-sector flat terrain fallback bit 18 admitted 2026-09-13, also dropped by
the mirror, terrain-re.md "Empty-sector flat fallback").

The POST_TRANSPARENT `FrameFxCompositorEffect` performs the witnessed
RenderingDevice sequence as the planner's DrawPass rows
(`renderer::frame_fx_bloom_passes`): RGBA8 capture of the full-resolution
focused source into the power-of-two floor of (frame - 1) per axis, then its
256² downsample; a four-tap downsample at 30° with base `1/2048`, radius
`1/1024`, and weights `0.50@0.5`, `0.46@2.5`, `0.35@4.5`, `0.19@6.5`; additive
weighted pairs at 90°/270° (its U taps x0.75, the builders' `reduced` flag)
then 0°/180°; and a four-tap 45° final average at
radius `0.0027621093`, alpha 0.5, blended SRCALPHA/ONE over the beauty target.
The terminal gamma decode follows that composite. Particle compositor assembly
always places this terminal effect last, and built-in Godot glow is disabled.
The `glow` probe pins all 26 technique contracts plus the native backend
report (typed submitted/rejected/drawn counters, resolved-depth facts, and
exact constants). `Q3FrameCompiler` owns back-to-front Q3 order. D-RORD-3 closed with one
priority-bearing material instance per retained rigid strip, transformed
strip-center classification whenever the model transform or the water plane
changes (the same result as retail's per-frame recompute without a server
round trip per strip per frame), the bone-path entity-side selector, and the
camera-underwater ladder reversal.

**Entity projection sphere (D-RORD-12, re-grilled 2026-09-13).** The RLOD
selector's input is not the GHDR origin-centered radius or the longest render
AABB axis. `[orig: Entity_ComputeBoundingSphere @ 0x5C69A0]` reads the primary
entity graphic at `entity+48`, then its collision header bounds. Each minimum
is capped at `0x40000000`, each maximum floored at `-0x40000000`; 32-bit
subtract, arithmetic shift-right one and add produce the midpoint. The radius
uses **maximum minus that midpoint** on each axis, so an odd Q16 width uses
the larger positive-side half, then square-root, clamp at `0x7FFF0000` and
truncate. A nonzero runtime scale (`entity+0x158`) wins over definition scale
(`itemDef+0x1B8`); the chosen nonzero Q16 scale RHU-multiplies both center and
radius once `[orig: @ 0x5C69C4..0x5C69DF; @ 0x5C6A3B..0x5C6B52]`.

The ordinary pool collector takes the stored radius and transforms the stored
center through the **unscaled** entity pose and the view before projection
`[orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5C8C60,
@ 0x5C8F0E..0x5C8F4F]`; the static collector does the same
`[orig: Terrain_CollectVisibleEntities_0 @ 0x5C6F20,
@ 0x5C709A..0x5C7107]`. `ThreediCollisionModelData` retains the exact CMDL
Q16 words; `world::collision_projection_sphere_from_3di` and
`renderer::object_projection_sphere_from_bounds_q16` own the producer.
`ObjectLodFrame` converts the source axes and transforms that already-scaled
center for both individual and static submissions. Geometry-only resolved
static fixtures derive a midpoint and full half-diagonal from their LOD0
bounds; the loaded-model path uses CMDL.

Persons (item type 3) instead project their entity position with the initialized
`entity+0` radius. Flag `entity+36 & 0x20` substitutes the special parachute
model's header radius directly, without the person's scale
`[orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5C8DF3..0x5C8E21]`.
`[orig: Entity_PreloadSpecialItems @ 0x43C220]` resolves that special graphic
from item type 185. The composed head and body receive this **same projected
value** but each walks its own RLOD table
`[orig: Terrain_RenderSectorEntitiesBySide @ 0x5C7FEA..0x5C8020]`.
`ObjectModel` retains this entity mode across data/scale rebuilds and caches
one projection per frame for its linked parts. The native present rows carry
current flag 0x20 for player and infantry replicas and authoritative entities;
both Godot presenters publish it before their memoized draw-state legs.
Deployment/physics, initial marker authoring and own-server flag emission are
still D-INF-20; a received full-spawn flag is already a reachable input.

Destruction changes the draw graphic and its threshold table, not the primary
entity sphere: the producer reads `entity+48`, the collector retains
`entity+508/+520`, and `[orig: Render_SectorEntity @ 0x5C4190]` selects
`entity+52` only for the husk draw (`huskfinal` changes that husk pointer).
An individual husk shares its intact model's projection owner; a carved static
husk receives the retained primary local sphere and scale. Neither recomputes
its projection from the replacement CMDL. Held weapons and mine overlays
retain their existing separate parent-LOD-index ownership.

Validation uses the retail PE (SHA256
`b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`)
executed at `0x5C69A0` under Unicorn, with the producer unmodified: bounds
`(-2,1,3)..(6,5,9)` in whole units yield center `(2,3,6)` and radius
`352922` Q16; raw Q16 words `(-3,-2,10)..(4,7,15)` yield center `(0,2,12)`
and radius `7`, runtime scale `98304` yields `(0,3,18)` and `11` despite a
`131072` definition scale, and runtime zero uses that definition scale to
yield `(0,4,24)` and `14`. Native `object_lod`, `world_model_geometry` and
`netsim_present_rows` pin the arithmetic, exact CMDL read, and actual received
player/infantry flag publication. GUT `object_projection_lod_test` selects
actual LODs through live/static placement, both fallback-person construction
orders and rebuild, composed parts with distinct tables, wire flag set/clear,
and individual/static husks with different primary/replacement collision data
and nonunit scale. `OcclusionWorld::bound_sphere_fixed` is the separate
native visibility producer; it carries the same midpoint, positive-side half
and Q16 scale form since D-OCC-16 closed (2026-09-13, `occlusion_test`).

**The frame** (`Render_ProcessMainSceneFrame @ 0x5ca0f0`, the live per-frame
driver; camera above water shown — the sides mirror when underwater;
re-walked 2026-09-24, which corrected the sky/viewmodel order and the scene
core's labels):

1. `RenderBatchCtx_BeginFrame`; `Render_TerrainScene @ 0x610c80` runs the
   offscreen prep: the per-entity shadow-slot pass (`Render_ShadowPass
   @ 0x5d7b70` → `RenderSlot_RenderEntityAndChildren @ 0x5d7690` per active
   slot, each flushing mode 0), the reflection prerender when water is active
   (`g_WaterActive @ 0x31BC918` → `Water_ReflectionPrerender @ 0x5c2780` →
   `Render_MainScene @ 0x5c1240`, the reusable offscreen scene renderer also
   used by `Render_CinematicMultiview @ 0x570940`), the environment cubemap
   update, and the terrain lighting ramps. The world pass's near plane is
   re-pinned to 0.2 every frame, whatever the camera mode
   (`Render_SwapProjectionNearZ(flt_7C3340)` `@ 0x5ca4d7..0x5ca4e0`), beside
   the far plane (§Projection and thermal-wave follow-up).
2. Clear: color = 0x808080 in the THERMAL seat view (the thermal byte is
   `Player_IsHeldWeaponThermal` `@ 0x5ca2da..0x5ca2e3`), else
   `g_EnvSkyfogBlock` while the eye is strictly above the water, else
   `g_EnvWaterColorLit` (`@ 0x5ca771..0x5ca792`; the `jle @ 0x5ca790` keeps the
   lit water at eye == water height). The beauty frame is never cleared black
   (render-occlusion-re.md §4).
3. **Sky pass** (`SkyDome_RenderWithSkyfog @ 0x5ca81a` → `Render_Skybox @ 0x579080`): the
   dome gradient pass (its draw `@ 0x5798dc`), then `Render_CelestialBodies(0)`
   (`@ 0x5798e0`), then the cloud pass (`@ 0x5798f1..0x579b15`), so the clouds
   cover the sun and moon. Both dome passes run under pass flags 0x300000
   (z-write off, ZFUNC ALWAYS): the dome never writes depth. The bracket is
   skipped under blink bit 0x4 and whenever the eye is not strictly above the
   water (`@ 0x5ca1a3..0x5ca1bd` → `@ 0x5ca7c4..0x5ca81a`; render-occlusion-re.md
   §4). The celestial submits write the 16.16 alpha global
   `g_CtrlGlobalUplIntensity @ 0x83fde8` (the global CTRL register UPL_INTENSITY,
   clamped [0, 0x10000] `@ 0x5acbdd..0x5acbfa`), which the authored SELFLUM
   material's RgbGen reads at flush time, and pass flag 0x100 (no Q3 copy);
   the sun and moon submit back to back and flush once, mode 0, each batch
   restoring its own snapshotted register. No star field is drawn (its
   renderer has no caller). Full witness: env-tod-re.md §Sky dome and
   §Celestial bodies. The reimpl rungs are `kRungSkyDome` → `kRungSkyBody` →
   `kRungSkyClouds` (§The ported ordering semantics).
4. **Viewmodel** (`Player_RenderViewModelIfAlive @ 0x4e0140`, called
   `@ 0x5ca829`, AFTER the sky pass; renamed from `sub_4E0140`): the gun is
   skipped when the local entity is dead (Flags & 2 `@ 0x4e0145`) or a round
   winner is set (`g_EndRoundWinnerTeam` `@ 0x4e014b`). Inside
   `Player_RenderFirstPersonViewModel`, an Emplaced weapon skips the showhud
   bit-0 test (`@ 0x4dedd9..0x4dedf1`), and CanFire && IsScoped && def+0xC &
   0x200 (INSET) skips the draw (`@ 0x4dedf7..0x4dee19`); ported as
   `fp_viewmodel_retail_submit` / `FpViewmodelSubmitGates`
   (`engine/runtime/world/player_present.h`, "Port the first-person viewmodel
   draw gates"). The gun+arms mini-scene draws with projection near-Z swapped
   to 0.05 (`Render_SwapProjectionNearZ @ 0x58a8f0`, renamed from
   `Scar_SetShadowBias`; the global is `g_ProjectionNearZ @ 0x8409DC`,
   consumed by `D3DXMatrixPerspectiveFovLH @ 0x58d9cc`; swapped in
   `@ 0x4dee1f..0x4dee29`, 0.2 restored `@ 0x4df0a0..0x4df0aa`) and the
   viewport depth range clamped to `[0, 0.1]` (`Render_SetViewportDepth01
   @ 0x58a7b0`, renamed from `Scar_SubmitDecalToRenderObject` — it builds a
   D3D viewport `{x,y,w,h,MinZ 0, MaxZ 0.1}`), per-weapon FOV (`renderfov` @
   `Def+0x148`, horizontal; the vertical is derived through the SAME
   `flt_8409E8` viewport Y-scale the world pass uses:
   `Player_RenderViewModelIfAlive @ 0x4e0154` pushes it as the pass argument,
   `Render_SetAspectRatioMode @ 0x58d870` sets it to `target_ratio / (h/w)`,
   0.96 in 16:10 mode on 1920×1200, so the FP and world frusta are
   identical, `proj[0][0] = cot(40°)` in both), a state-stack push whose aux
   is the INTERIOR BUILDING's `light_transfer` (entity+0x1D0's ItemDef+0x218,
   `@ 0x4def0a..0x4def3c`; the effect-scale dword keeps 1.0, the sun factor is
   discarded), submits with flags 0x80 iff that interior building is nonzero
   (`@ 0x4def52..0x4def6a`), pops, and flushes mode 0; the compressed depth
   band keeps the world from ever overlapping the gun. The viewmodel draws
   under the DRY pass fog even when the eye is underwater:
   `Environment_ApplyFogAndAmbient(0, thermal)` `@ 0x5ca3bf..0x5ca3ce` runs
   before it and the underwater re-apply `@ 0x5ca82e..0x5ca841` after it, with
   the fog colour `SkyDome_RenderWithSkyfog` restores (`g_EnvFogBlock`
   `@ 0x579ce6..0x579cf6`), or 0x808080 under thermal when the dome was drawn;
   ported as `EnvironmentState::build_viewmodel_fog` into the globals
   `opennova_viewmodel_fog_color` / `opennova_viewmodel_fog_range` ("Fog the
   viewmodel under the dry pass"). `[orig: Player_RenderFirstPersonViewModel
   @ 0x4ded60; @ 0x4dee5a..0x4dee7f; @ 0x4def3c]`
5. **Terrain surface** (`Terrain_RenderMainSectorPass @ 0x610ac0`, called
   `@ 0x5ca867`; the name is a misnomer, it draws no sky):
   `Terrain_RenderSectorBatchLit @ 0x610c34`, then the render-slot drapes
   `RenderSlot_DrawAllDrapes @ 0x610c47`; skipped indoors (`@ 0x5ca84f`).
   The reimpl draws the drapes at `kRungSlotDrape`, reading a stencil mark
   the terrain and the dome write, and closes them with the terrain gate
   indoors (render-lighting-re.md, the slot drape; 2026-09-26).
6. The world (`Terrain_RenderWorldScene @ 0x5c93a0`):
   - sector models + the first (non-person) entity wave → flush(1)
     (`@ 0x5c9506`, which carries the buildings' and vehicles' post-multiply
     passes and draws no foliage masks);
   - the far-side person waves (`Terrain_RenderSectorEntitiesBySide
     @ 0x5c7d50`, renamed from `Terrain_RenderSectorEntities_0`; the arg
     selects BELOW(1)/ABOVE(0) water entities by height vs
     `g_EnvWaterHeightFixed`, the BySide list `0x2984890` holding ItemDef
     type-3 persons only): the MATCHTERRAIN sub-pass (submit 0x200, entities
     gated on `entity+0x12C & 0x300`, MoveOrder prone/crouch `@ 0x5c7dc2`)
     `@ 0x5c9548` → flush(1) `@ 0x5c9557`, then the normal wave `@ 0x5c955f`
     → flush(1) `@ 0x5c956e`; inside the wave, after its MATCHTERRAIN
     sub-pass and before its queued entities, the foliage MODEL depth masks
     draw immediately (`Foliage_UpdateModelTiles`); terrain LOD update →
     flush(1) `@ 0x5c9581`;
   - **flush(3)** `@ 0x5c9596`: the far-side (below-water) transparents;
   - tracer pass 0 (`CEffectEmitterPool_RenderMainPass(0, eyeBelow)`
     `@ 0x5c95ac`; this record's earlier `render_all_trail_strips` label was
     superseded in the IDB by the `CEffectEmitterPool_*` family — see
     [correspondence.md](../correspondence.md)), then particle pass A
     (`EffectWorld_RenderParticlePass(0) @ 0x5c95b5` →
     `EffectWorld_DrawParticles @ 0x5f6680`, renamed from the
     `CNapiSession_*` misnomers — `g_EffectWorld @ 0x2C25CD8`);
   - the far detail-foliage patches (`Foliage_RenderDetailPatchesPass(0)`
     `@ 0x5c95c5`), then **the water surface** (`Terrain_RenderWaterPass
     @ 0x610640`, renamed from `sub_610640`, called `@ 0x5c95dc` →
     `Render_WaterSurface @ 0x5c32c0`, both view variants, self-gated by
     camera side; the vehicle wake rings draw inside it, `WaterRing_DrawAll`
     `@ 0x5c3432`);
   - the camera-side (above-water) person waves: MATCHTERRAIN `@ 0x5c9621`
     → flush(1) `@ 0x5c9630`, normal `@ 0x5c9638` (with its MODEL masks) →
     flush(1) `@ 0x5c9647`; then the impact scars (`Scar_DrawBatches
     @ 0x5c9658`) and the camera-side detail foliage
     (`Foliage_RenderDetailPatchesPass(1) @ 0x5c9665`);
   - **flush(2)** `@ 0x5c967a`: the camera-side (above-water) transparents;
   - tracer pass 1 (`@ 0x5c9687`), particle pass B (`@ 0x5c9690`);
   - the post-particle overlay tail (§The post-particle overlay stage): the
     NVG laser beams (`Render_NVGLaserBeamsForVisiblePersons @ 0x5c9695` → `Entity_RenderNVGLaserBeam`),
     the precipitation (`Render_WeatherTrailParticles @ 0x5c96a6`), the
     coronas (`EffectWorld_RenderLightCoronas(1) @ 0x5c96ad`), the water
     glint (`Environment_UpdateSunGlare @ 0x5c96c0`, only while the water height is
     nonzero `@ 0x5c96b5`), the underwater murk (`Render_DrawViewportColorQuad
     @ 0x5c96f5`), the modulator forced to 0xFF404040 (`@ 0x5c96fd` /
     `@ 0x5c9702`), the sun glare `Render_SkyboxSunGlow(1, 1)`
     (`@ 0x5c9714`, function `@ 0x5acd00`, skipped when the scene's stack arg
     is zero, `test edi @ 0x5c970a`), and the modulator restored
     (`@ 0x5c9722`). The murk's gate is `camera_z <= g_EnvWaterHeightFixed`
     (`cmp/jg @ 0x5c96ca..0x5c96d1`), so equality is covered even though the
     environment fog classifier uses strict `<`; it covers the full current
     viewport with RGB = `g_EnvWaterColorLit` and `alpha = 0x80 -
     trunc(g_EnvWaterMurk * -96.0)` = `128 + trunc(96 * murk)` (CP01 `0.8` ->
     `204/255`, `@ 0x5c96d3..0x5c96f0`), standard `SRCALPHA` / `INVSRCALPHA`,
     so `out = lit_water * alpha + prior * (1 - alpha)`; pass flags 0x300000
     (`@ 0x5c39bd`) disable Z-write and force ZFUNC ALWAYS. The viewmodel,
     world, particles, weather and foliage are therefore all attenuated, while
     the glare after it and the HUD are not.
   The 4th argument of `Terrain_RenderWorldScene` is the THERMAL byte
   (`@ 0x5ca8e3`; the IDB parameter, once `reflectionEnabled`, is `thermalView` since 2026-09-25): under
   it each far-water-side BySide wave runs ONCE under the flat 0.25 block
   (`CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090` arg 1, `@ 0x5c9511` /
   `@ 0x5c95f8`) with its MATCHTERRAIN sub-pass (BySide 4th arg 1) skipped, then Build(0)
   restores the grey world block (`@ 0x5c9534` / `@ 0x5c9616`); the same byte
   (stored by `EffectWorld_RenderParticlePass @ 0x5f7274`) selects the
   secondary particle materials in both particle passes
   (`CParticleBatch_FlushAndBindMaterial @ 0x5e42bf..0x5e42db`). The mirror
   matrix/CLIP machinery above is the reflection spec proper — env #30
   (REN-6). After the main scene core, the weapon Inset pass
   (`Render_WeaponInsetScene @ 0x5c9740`, called `@ 0x5ca949`) runs a whole
   second scene for its own eye: its own terrain frame, sector pass, collect
   and scene core `Terrain_RenderWorldScene(view, 0, 0, 0)` (the call `@ 0x5c9de9`) with
   the tail above (§The weapon Inset pass's tail; render-occlusion-re.md §8a).
7. **FrameFX screen effects** (`FrameFX_ApplyScreenEffect @ 0x584440` and
   `FrameFX_ApplyWeaponViewEffect @ 0x5845b0`, the FrameFX dispatchers;
   types 0 = distortion, 1 = damage blur, 2 = bloom, 4 = death blur, 8 =
   thermal view, 9 = monitor scanlines; 3 and 5 have no live caller). In
   order: type 0 unless dead in a session or under the red flash outside
   camera mode 3, at FBEFFECTS >= 2 with a distortion channel or particle
   (`@ 0x5ca8f6..0x5ca92e`; the backbuffer capture `@ 0x584463..0x58448b`);
   then type 4 while dead (`d = clamp(tick - g_CameraLerpStartTick - 30,
   0, 200) / 15`, `@ 0x5ca9f5..0x5caa3f`) or type 1 under the red flash
   (`p = word / 120`, `@ 0x5caa41..0x5caa71`); then the bloom (step 8); then
   types 8/9 on the frame's CanFire latches for weapon.def flags2 & 4 / & 8
   (`@ 0x5ca2da..0x5ca2f1`, `@ 0x5caa9c..0x5caad5`). The first-person NVG
   view (`g_NVGActive && g_CameraMode == 0`) skips this whole step and
   step 8 (`@ 0x5ca6ab..0x5ca73a` jumps to `@ 0x5cab1d`). The red damage
   VIGNETTE is a separate quad in step 9 (`@ 0x5cabd5`). The render-slot
   ENTITY ground shadows are not this family: they render at frame open
   (step 1's slot pass) and drape in step 5 (render-lighting-re.md's
   render-slot section). Ported as `renderer::plan_frame_fx` /
   `FrameFxCompositorEffect` (the verdict row).
8. **`FrameFX_RenderGlowSource` (flush 4 = Q3)** — corrected 2026-09-16: this
   step is NOT after the HUD. It is reached only through
   `if (FrameFX_QualityAtLeast3()) FrameFX_ApplyScreenEffect(..., 2, ...)`
   `@ 0x5caa7b..0x5caa97` → `FrameFX_BloomKernel @ 0x5844eb` →
   `FrameFX_RenderGlowSource @ 0x584234` (its ONLY caller chain in the image),
   and that call sits **before** the scoped-view overlay fork
   `@ 0x5caae1..0x5cab26` (binocular mask / SIGHTS card / the scope circle mask
   `Hud_DrawScopeCircleMask @ 0x5cab15` / the entity markers) and before the
   whole HUD block `@ 0x5cab2b` onward. The reimpl already matches the binary:
   the FrameFX compositor effect runs as a viewport compositor pass and the HUD
   is a canvas layer drawn over its output.
9. Radar/scope overlays and the HUD (`HUD_RenderAllOverlays @ 0x5a8070`,
   mode-0 flushes for 3D HUD elements), the fullscreen feedback quads (the
   red damage vignette `@ 0x5cabd5`), fades, tips.
10. Present, `RenderBatchCtx_EndFrameStats`.

**Dead variants** (zero live callers; never order-witness from them):
`Render_SkyboxLayers @ 0x5ac230`, `Render_SunLensFlare @ 0x5ad490`,
`Render_FoliageAtCamera @ 0x5ac100` (the env record's catalog).

## The ported ordering semantics (this slice)

The reimpl keeps its own queues (ADR 0023); what ports is the ORDER as data +
pure functions in `engine/runtime/renderer/render_order.{h,cpp}`:

- the transparent priority ladder, applied as Godot `render_priority` rungs
  (`renderer::kRung*` in `render_order.h`, mirrored by
  `ObjectShaderCache.RENDER_RUNG_*`; renumbered for the retail frame slots
  2026-09-24, "Renumber the render ladder for the retail frame slots", with
  the later dome, mask and particle rungs, and 2026-09-26 for the slot
  drape): `kRungSkyDome` -16 (the dome
  gradient, which writes no depth and so sits in Godot's transparent list;
  without its rung it would paint over the bodies and clouds, "Open the sky
  pass with the dome gradient's own rung"), `kRungSkyBody` -15, `kRungSkyClouds`
  -14, `kRungViewmodel` -13, `kRungSlotDrape` -12 (the render-slot ground-shadow
  drapes, which retail draws right after the terrain batch inside
  `Terrain_RenderMainSectorPass` (the `Terrain_RenderSectorBatchLit` call `@ 0x610c34`,
  then the `RenderSlot_DrawAllDrapes` call `@ 0x610c47`), before any model;
  render-lighting-re.md, the slot drape), `kRungObjectPostMultiply` -11,
  `kRungFoliageMaskFarSide` -10 (the far person wave's foliage MODEL masks,
  `Foliage_UpdateModelTiles` inside the wave `@ 0x5c955f`), `kRungAlphaFarSide`
  -9, `kRungTracerFarSide` -8, `kRungParticleFarSide` -7 (particle pass A),
  `kRungFoliageFarSide` -6, `kRungWater` -5, `kRungWaterDecals` -4 (the vehicle
  wakes, `WaterRing_DrawAll @ 0x5c3432` inside the water pass),
  `kRungFoliageMaskCameraSide` -3 (the camera wave's masks `@ 0x5c9638`),
  `kRungScars` -2 (`Scar_DrawBatches @ 0x5c9658`), `kRungFoliageCameraSide` -1,
  `kRungAlphaCameraSide` 0 (Godot's default, so unclassified transparents land
  there), `kRungTracerCameraSide` 1. Particle pass B and the post-particle
  overlay tail are compositor passes, not rungs; the star rung is gone with
  the dead star draw, and the former `kRungOverlayFx` / `kRungSunGlow` are gone
  (the glare and the glint draw in the overlay stage).
  `godot/src/env/celestial.cpp` takes the sky rungs, `godot/src/env/water.cpp`
  the water rungs, and `godot/src/object/object_model.cpp` assigns blended
  object materials their water-side rung. The foliage masks also draw into a
  PRE_OPAQUE mask texture that the person consumers' opaque shaders test
  ([foliage-re.md](../foliage/foliage-re.md) D-FOLIAGE-10);
- the BmTxMirrT P3 post-multiply rung (`kRungObjectPostMultiply`, after the
  sky group, the viewmodel and the slot drape, before both foliage-mask rungs and every world
  transparent): retail has no separate submit for P3: it is a pass of the
  opaque strip's own technique, and `CRenderBatchQueue_FlushBatches` runs
  every pass of one entry back to back (`[orig: the pass loop
  @ 0x5da20b..0x5da23d; per-pass fog/blend @ 0x5da2f7]`) inside the Q0
  flush(1) brackets (`[orig: @ 0x5c9506..0x5c9581; @ 0x5c9630..0x5c9647]`),
  never inside the Q1/Q2 flushes (`[orig: @ 0x5c9596; @ 0x5c967a]`). Godot
  cannot blend inside its opaque stage, so the port's rung is the lowest
  world slot above the sky: after every opaque, before far-side alpha and the
  water. The buildings' and vehicles' P3 passes flush with the non-person
  sector wave (flush(1) `@ 0x5c9506`, after `Terrain_RenderSectorEntities`,
  which draws no masks), before either person wave, so the rung also precedes
  both mask rungs; person P3 layers test the mask texture themselves. The
  former "far-side P3 after the water" residual was mis-sided and is deleted
  (2026-09-24): the far-side BySide wave and its Q0 flushes (`@ 0x5c9548` /
  `@ 0x5c955f` → `@ 0x5c9557` / `@ 0x5c956e`) run BEFORE
  `Terrain_RenderWaterPass` (`@ 0x5c95dc`), the same as the single rung; the
  flush `@ 0x5c9630` after the water belongs to the camera-side wave;
- the submit-flag names: `kSubmitZAlways` = 0x10 (entry bit 3 forces ZFUNC
  ALWAYS `@ 0x5da32a..0x5da341`) and `kSubmitAvatarSecondPart` = 0x10000000
  (the composed avatar's body part), formerly `kSubmitEntryBit3` and
  `kSubmitRepeatDraw`;
- the scene pass near plane `renderer::kScenePassNearZ` = 0.2 beside
  `scene_far_plane` (the verdict row);
- `opaque_sort_key()` / `transparent_sort_key()` / `transparent_queue_for()`
  / `technique_class_for_submit()` — the witnessed key and class semantics,
  T1-pinned (`renderer_state_vectors` section 3) so future port slices
  (REN-4's class behaviors, REN-6's leftovers) converge against them.

## Divergence catalog

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RORD-1 | No global transparent ordering: water, world alpha, and weather all at priority 0 (one depth-sorted queue); the celestial ladder local to the celestial presenter (now `godot/src/env/celestial.cpp`) | fixed pass bracket: sky → far-water-side alpha → water → camera-side alpha → overlays → glow (`[orig: @ 0x5c93a0]`) | FIXED (this slice: the ladder in `engine/runtime/renderer/render_order`, applied at celestial/water/object-model sites) |
| D-RORD-2 | Reimpl-internal opaque ordering (Godot front-to-back + its own state batching) | per-frame CPU quicksort by the composite key (alpha-test bit → 256-unit depth slabs → effect index → fine depth) (`[orig: @ 0x5d8b40; @ 0x5d928e]`) | PERMANENT-candidate (class C): same intent, device-era mechanism; key semantics preserved as T1-pinned functions |
| D-RORD-3 | One retained material instance per alpha strip; rigid strips classify their transformed authored min/max center whenever their model transform or the water plane changes (a transform notification re-runs the classifier in place; still models park), bone-path strips use the submitting entity side, and the ladder mirrors with the adjusted render-eye side | rigid path: per strip and per frame (`[orig: @ 0x5d932e..0x5d9354]`); bone path: caller-selected Q1/Q2 via submit flag `0x20` (`[orig: @ 0x5d95c0..0x5d961f]`) | **FIXED (2026-08-22; change-driven 2026-08-23)** — straddling, transform changes, and both camera sides are runtime-pinned |
| D-RORD-4 | Viewmodel is a camera-tracked node with no depth treatment (clips into near walls) | drawn right after the sky pass (before every world draw; the earlier "drawn FIRST" reading corrected 2026-09-24) with near-Z 0.05 + viewport depth range [0, 0.1], own mode-0 flush (`[orig: @ 0x4ded60; @ 0x58a7b0]`) | RESOLVED — ported 2026-07-09 as a dedicated shared-world SubViewport composite; re-ported 2026-08-26 INSIDE the beauty pass: a shader-side projection override per viewmodel instance (renderfov focal ratio, near 0.05, clip depth remapped into the nearest tenth of the reversed-Z range = the retail depth band) so the gun is under the murk/bloom composite like retail and no second full-window scene render exists. The Z-write residual is witnessed and closed 2026-09-24: blended FF strips have NOWRITE forced (`@ 0x5afc8d..0x5afcaa`) and ZWRITEENABLE = ~(passflags >> 6) & 1 (`@ 0x5da318..0x5da324`); the "nearer world transparent over gun glass" worry does not arise, because world depth enters the [0, 0.1] band only within about 0.222 u in retail (viewport MaxZ 0.1 vs 0.99997, near 0.05 vs 0.2) and in the port alike. The real residual was the sky/world-over-strip order, fixed by "Let the world paint over the viewmodel's blended strips" (the viewmodel rung after the sky group; blended strips discarded where the opaque pass left a world depth) |
| D-RORD-5 | GLOW was hosted through Forward+ HDR extraction instead of retail's isolated Q3 target and FrameFX kernel | strips with effect capability 0x10000000 get a Q3 copy (GLOW class when present), flushed by `FrameFX_RenderGlowSource` together with the NV water redraw, the celestial bodies, and the sun glow (`[orig: @ 0x5d93b5; @ 0x582a54..0x582a80]`) | **FIXED (2026-08-22; source replaced 2026-08-29)** — typed LUM NORMAL-copy, Glass CubeRotSpecular, water NV, celestial/sun draws, capture, weighted blur/final kernel (the capture at the power-of-two floor of the frame, the first weighted pair's U taps x0.75), SRCALPHA/ONE composite, and terminal ordering are native and RenderingDevice-pinned; no glow proxies or compatibility path remain |
| D-RORD-6 | Not reproduced | two original key quirks: opaque key bits 15+ carry residual stack garbage (`@ 0x5d92b9`), and the transparent key lags one strip within a render object (`@ 0x5d9326` vs the `fst @ 0x5d9347` overwrite) | PERMANENT-candidates (original-bug/garbage class): reproducing either manufactures garbage (ADR 0022). Consequence (2026-09-26): the fixed-function hemisphere latch (`Render_SetupEntityLightingAndShaderConstants`, `@ 0x5d9d78..0x5d9da2`, `@ 0x5d9eee..0x5d9f14`) re-sets D3D lights 2/3 only on a flush's first entry and on an interior/outdoor transition, so the later interior FF entries of a run inherit its first entry's `light_transfer` lerp in flush order; that order's key carries the residual stack bits 15..31 (`@ 0x5d92b9..0x5d92c2`), so the sharing is part of this register, not a separate divergence; the port lights each draw by its own lerp (render-lighting-re.md) |
| D-RORD-7 | Particle pass A was the PRE_TRANSPARENT compositor callback (before every transparent, water included) and pass B the POST_TRANSPARENT one, so far-side object ALPHA strips drew AFTER pass A (a submerged strip overlapping a far-side particle composited strip-over-particle) | two calls to the global particle manager: pass A between far-side transparents and water, pass B after camera-side transparents (`[orig: Terrain_RenderWorldScene @0x5c93a0; EffectWorld_RenderParticlePass @0x5f7240; CParticleGroup_RenderChildren @0x5e5890]`). Reflection receives the main-camera `< water` boolean and calls its two particle passes consecutively after reflected geometry (`[orig: Render_MainScene @0x5c16ed..0x5c171f; Water_RenderReflectedWorldScene @0x5c8510]`) | **FIXED (2026-09-24, "Draw particle pass A inside the transparent list")**: pass A (`EffectWorld_RenderParticlePass(0) @0x5c95b5`) draws inside the transparent list at `kRungParticleFarSide`, after the far-side ALPHA strips (`kRungAlphaFarSide`) and tracers (`kRungTracerFarSide`) and before the far detail foliage and the water, as `ParticleFarPass` render-list runs (one shared sort origin; sorting offsets keep the compiler's order). Only its distortion-pipeline commands stay on the PRE_TRANSPARENT compositor effect; the class-7 distortion defs draw in FrameFX's type-0 row. The emitter-scope water predicate, subset reversal, recursive packet order (D-PTL-21) and the mirror's consecutive POST pair are unchanged; `framefx_test.gd` keeps the water-attenuation differential |
| D-RORD-8 | FIXED 2026-08-12. The `GameWorld` frame-leg table (`kFrameLegs`, `godot/src/world/game_world_frame.cpp`; ex `GameFramePipeline`) runs session tick → local-view placement → terrain → the remaining device legs, with foliage after occlusion (2026-09-24: the MODEL anchors are that frame's collected entities, so the `foliage` leg follows the `occlusion` leg in the live table and in the frozen-pose replay). Terrain samples the live viewport camera internally and foliage receives `GameWorld::render_camera_xform()`, so both compile from the view this frame's player state produced; foliage retains the frame-entry transform when no live camera exists, while terrain has no headless draw. Occlusion's post-present slot stands — present re-asserts base visibility, occlusion layers hides, Godot renders after both | collect-then-submit runs inside the render frame, before submission, against the view built from current player state (`Render_ProcessMainSceneFrame @0x5ca0f0`) | MATCHING for the camera-phase contract; `godot/tests/world_frame_order_test.gd` pins the leg order (the local view ahead of terrain, foliage and every other camera consumer, foliage after occlusion) |
| D-RORD-10 | The Q3 bloom source was a second shared-world scene submission that rerasterized terrain/opaque depth occluders at kernel size | retail draws Q3 objects, NV water, celestial bodies, and sun glow into a backbuffer-sized altbuffer with beauty depth-stencil still bound; the only consumer is the StretchRect into the power-of-two capture (`[orig: FrameFX_RenderGlowSource @ 0x582940; FrameFX_CreateAltBufferTexture altbuffer CreateRenderTarget @ 0x58217e; FrameFX_CreateRenderTargets @ 0x583c40; FrameFX_CaptureAltBuffer @ 0x584020]`) | **FIXED (2026-08-29)** — `Q3FrameCompiler` retains the retail bracket and typed generation-bound inputs; the terminal compositor draws them into one full-resolution Q3 attachment sharing resolved beauty depth from a retained per-source geometry cache (no per-frame server readback or re-upload). No auxiliary camera/view, camera-mask shader selection, or depth rerasterization remains. The viewmodel is a Q3 producer (2026-09-26): the FP pass submits the gun with flags 0/0x80, never 0x100 (`Player_RenderFirstPersonViewModel @ 0x4def5c..0x4def6a`, submits `@ 0x4defc1`/`@ 0x4df032`), so `Render_CollectRenderObjectsForBatch @ 0x5d93b5..0x5d9449` queues its rigid glow strips at their part matrices; mode 0 leaves Q3 (`@ 0x5dae5b..0x5daeb4`) and FrameFX's mode 4 draws them under the world projection against the beauty depth (`@ 0x582a45..0x582a54`). The port draws the viewmodel's rigid strips (a fake-skinned strip at its bone's part matrix) with the world camera's view-projection against the resolved depth, where the gun's band depth rejects a copy over its own opaque strips; the bone-path arms stay excluded (`framefx_test`) |
| D-RORD-11 | The authored RLOD selector, its coarsest-slot back-off, the frame scale, the integer projected radius and the sub-pixel cull are ported, every level remains retained (individual models and the per-instance static populations alike), a threshold crossing hard-switches the drawn level, attachments take the parent's level clamped to their own count (`renderer::attachment_lod_index`, the held weapon through `ObjectModel.set_authored_lod_owner`), and the composed avatar's head and body select independently from one projected radius | the same: `Model_SelectRlodLevel @ 0x5c3b20` returns one level (EAX); its overlap fraction is written only to `0x29ACD9C`, read solely by the callerless stub `@ 0x5c38c0`, and no submit/collector/flush consumes it. The `0x10000000` pair at `@ 0x5c7ffc`/`@ 0x5c8020` is the head+body avatar, not a far/near dual submission; overlays index their LOD with the parent's level `@ 0x4e39c4..0x4e3e51` | **FIXED (MATCHING, 2026-08-29)** — the "dual-submit through the overlap fraction" reading was refuted at the binary: the hard switch IS retail's behaviour, the dead `blend_fraction` output was deleted from `renderer::select_object_lod`, and the attachment rule was ported (`object_lod` ctest, `object_model_lod_occluder_test`, `wire_present_pass_test`). 2026-09-24 ("Draw the person overlays beside their bodies"): every org0 overlay (held weapon, canopy, goggles, binoculars, carried object) is stamped with its submit's owner; `@ 0x4e3e34..0x4e3e51` is the carried object's clamp, and seated riders are independent entities in retail as here (the witness map names the legs) |
| D-RORD-12 | The authored threshold selector, frame scale, integer projection and hard switch were ported, but individual models supplied GHDR at their origins, static populations supplied render bounds, and composed parts/husks projected independently. | `Model_SelectRlodLevel @ 0x5C3B20` selects one level; `Entity_ComputeBoundingSphere @ 0x5C69A0` supplies the scaled collision midpoint/diagonal for ordinary entities. Person radius/position and flag-0x20 substitution are `@ 0x5C8DF3..0x5C8E21`; head/body share the result at `@ 0x5C7FEA..0x5C8020`; husk draw `@ 0x5C4190` keeps the primary sphere while choosing the replacement table. | **FIXED (2026-09-13, projection producer and consumers)**. The native typed sphere and exact CMDL words feed live/static placement; person mode survives construction/rebuild; current player/infantry parachute flags select type185 radius; head/body and husks share the primary projection while retaining their own tables. Native `object_lod`, `world_model_geometry`, `netsim_present_rows`, GUT `object_projection_lod_test`, plus the existing LOD/attachment suites. The earlier selector/crossfade finding and attachment residual remain D-RORD-11. The native visibility producer shares the arithmetic since D-OCC-16 closed (2026-09-13). |
| D-RORD-9 | The underwater murk quad was a `PlayerViewEffects` overlay after the shared-world viewmodel and behind the HUD, created with the local-player HUD, so no-local-player/spectator views received no murk quad and the glare was not drawn after it | retail draws the source-over murk quad after the viewmodel/world/weather/foliage and then draws the sun glare bright on top under the forced 0xFF404040 modulator (`[orig: @ 0x5c96c5..0x5c9722]`) | **FIXED (2026-09-24, "Draw the scene's post-particle tail in its own overlay stage", "Draw the water glint and the sun glare at the end of the overlay tail")**: the murk is no longer a CanvasItem (the `UnderwaterMurk` rect and `MissionEnvironment`'s `underwater_overlay_changed` signal are deleted): it draws in each view's post-particle overlay pass from that view's render eye (spectator, third person and death cam included), before the bloom, and the glare draws in the same pass right after it under the forced 1.0 light scale (§The post-particle overlay stage) |

## IDB changes made during the session

| Address | Old | New | Basis |
|---|---|---|---|
| 0x5f7240 | CNapiSession_PumpWithMode | EffectWorld_RenderParticlePass | body is the particle render pass; no net I/O |
| 0x5f6680 | sub_5F6680 | EffectWorld_DrawParticles | CEffectWorld_SetupRenderState + CParticleManager_BeginFrame + VB flush |
| 0x4dbc70 | CNetPlayer_PushPositionToHistory | RenderStateStack_Push | copies stack entry top→top+1 at ctx+240, ++top — identical to the waves' inline push |
| 0x5d8990 | sub_5D8990 | RenderBatchCtx_BeginFrame | resets queue counts, stack top, stack defaults, stats |
| 0x5d87d0 | sub_5D87D0 | RenderBatchCtx_EndFrameStats | copies live stats to the last-frame block |
| 0x5dcaf0 | CEffectEmitterPool_RenderMainPass | render_all_trail_strips | iterates 256 trail channels → render_trail_strip |
| 0x610640 | sub_610640 | Terrain_RenderWaterPass | calls Render_WaterSurface under g_WaterActive; stores the terrain render mode |
| 0x58a8f0 | Scar_SetShadowBias | Render_SwapProjectionNearZ | swaps g_ProjectionNearZ (0x8409DC), the D3DXMatrixPerspectiveFovLH zNear |
| 0x58a7b0 | Scar_SubmitDecalToRenderObject | Render_SetViewportDepth01 | builds a D3D viewport {x,y,w,h,0,0.1} → device SetViewport |
| 0x4e0140 | sub_4E0140 | Player_RenderViewModelIfAlive | dead/spectate gate around the viewmodel render |
| 0x5c7d50 | Terrain_RenderSectorEntities_0 | Terrain_RenderSectorEntitiesBySide | renders the sector-entity list filtered to one water side, with the MATCHTERRAIN sub-pass |
| 0x843490 | dword_843490 | g_RenderBatchQueues | the batch context (layout above) |
| 0x5c3b20 | sub_5C3B20 | Model_SelectRlodLevel | 2026-08-29 (D-RORD-11): returns the RLOD level in EAX (the Hex-Rays `double` return is the x87 leftover); locals `config`/`viewDist` -> `model`/`projectedRadiusQ16`; the function comment records the dead fraction |
| 0x5c38d0 | sub_5C38D0 | Debug_SetRlodLevelOverride | callerless debug setter of the RLOD level override |
| 0x29acd9c | blendFraction | g_RlodBlendFractionDead | the selector's out-param; its only reader is the orphan `fld; retn` stub @0x5c38c0 (both commented) |
| 0x29acd98 | dword_29ACD98 | g_DebugRlodLevelOverride | the level override read @0x5c4265 / @0x5c3e71 |
| 0x5c7d50 (locals) | dualLodData / lodLevel1 / lodLevel2 | characterBlip / bodyModel / headModel | the "dual LOD" was the two-part avatar; block comments @0x5c7fea / @0x5c7ffc / @0x5c8020 |
| 0x5c4190 (locals) | view_distance / fade_progress | projected_radius_q16 / rlod_model | the projected radius and the RLOD table model; comment @0x5c41b6 (the forced table) |
| 0x4e3940 (comments) | — | — | @0x4e3c87: 0x10000000 = the avatar's second part, not a repeat-draw marker; @0x4e39c4: the attachment RLOD rule |
| 0x582120 (reimpl link) | godot/src/render/frame_fx.cpp | engine/runtime/renderer/q3_frame.h | the altbuffer knowledge moved with the typed Q3 draw list (cite_sweep reimpl-orphan closed) |
| 0x843580 / 0x843780 | dword_843580 / dword_843780 | g_RenderStateStack / g_RenderStateStackTop | the 16-byte-entry state stack + top index |
| 0x8437C4 / 0x843784 | flt_8437C4 / unk_843784 | g_WaterSplitHeightFloat / g_ActiveMirrorClipMatrix | queue-split threshold; active mirror matrix |
| 0x2980514 / 0x2980518 | dword_2980514 / flt_2980518 | g_WaterMirrorActive / g_WaterMirrorMatrix | reflection-pass machinery (env #30) |
| 0x8409DC / 0x8409D8 | flt_8409DC / flt_8409D8 | g_ProjectionNearZ / g_ProjectionFarZ | D3DX projection args |
| 0x2C25CD8 | dword_2C25CD8 | g_EffectWorld | the particle/effect world singleton |
| 0x31BC918 | dword_31BC918 | g_WaterActive | gates water render + reflection prerender; recomputed per frame by Terrain_SetupViewAndLighting @ 0x60fe40 from the tracked visible-terrain height range vs g_EnvWaterHeightFixed, OR last frame's g_BlinkWaterVisible (env-tod-re.md, the water-active predicate) |
| 0x2721A08..38 | flt_2721A08.. | g_BatchSortDepthPlane{X,Y,Z,W} | the camera-forward plane the sort distance dots against |

REN-4 additions (the pass-execution grill): `Light_IsSpotlight @ 0x5a9040`,
`Light_GetPointLightParams @ 0x5a9180`, `Light_ApplyAsD3DLight @ 0x5abd50`
(ex-`sub_*`) — the pass-rules light machinery; the full REN-4 rename set is
logged in [render-material-re.md](render-material-re.md).

## Open questions

- **Closed at REN-4**: entry flag bit 3 (= force ZFUNC ALWAYS, the submit-0x10
  override — witnessed in the pass loop); the `+841` byte (its reader is
  `Render_SetupEntityLightingAndShaderConstants @ 0x5d98a0`; corrected
  2026-09-24: it is the per-flush FIRST-ENTRY latch, not a mirror gate: on the
  first entry of every flush the reader pushes the shared constants
  MatTexClipPlane ← base × the active mirror matrix ctx+756, FloatTicks and
  DirLightVector, and clears the byte; VecDepthMaskPlane ← ctx+824, with
  ctx+842 tracking applied clip-plane state; render-material-re.md §Open
  questions); the MATCHTERRAIN class MECHANISM (binds the terrain tile
  texture under the object — [render-material-re.md](render-material-re.md)
  §Pass execution).
- **Closed at REN-6 (2026-07-06)** — `Water_RenderReflectedWorldScene` was a 5-byte header
  (`call Render_ResetFixedFunctionState`) falling through into an unclaimed body (the same split
  shape as `Render_WaterSurface`; a stale NORET flag on `Render_ResetFixedFunctionState`
  truncated the analysis). Renamed `Water_RenderReflectedWorldScene`: the
  offscreen reflection's world subscene — fog/ambient push, NORMAL lighting
  constants (arg 0), builds `g_WaterMirrorMatrix` as the water CLIP-plane
  TEXTURE matrix (`u = y − waterHeight + 0.5`; TexClip1D ref-128 cuts at the
  plane) and arms `g_WaterMirrorActive`, then sector models → entity wave →
  flush(1) → below-side/above-side waves → flush(0) → foliage-tile/LOD
  updates → flush(0) → particles → trails. Full pipeline:
  [env-tod-re.md](../env/env-tod-re.md) §Reflection pipeline.
- **Closed 2026-09-24**: the sector-list populations. The BySide list
  `0x2984890` holds ItemDef type-3 persons only (`@ 0x5c8de6..0x5c8ef9`);
  `g_SectorEntityList` `0x2999518` holds the other pool-0/1 types
  (`@ 0x5c9035..0x5c907d`) plus the 512-entry pool from `sub_4E4030`
  (`@ 0x5c91c6..0x5c936f`) `[orig: Terrain_CollectVisibleEntitiesForTerrain
  @ 0x5c8c60; Terrain_RenderWorldScene @ 0x5c93a0]`.
- **Closed 2026-09-24**: the MATCHTERRAIN sub-pass ENTITY gate
  (`entity+0x12C & 0x300` = `entity+300`) is the MoveOrder prone/crouch
  stance bits (`@ 0x5c7dc2`). Retail AI never writes them, so the sub-pass and
  the foliage MODEL masks are player-only ([foliage-re.md](../foliage/foliage-re.md)).

## Projection and thermal-wave follow-up (2026-09-11)

The main projection far plane uses floor(raw fog distance)+1. Retail reads
the signed high word at 0x26C681E, adds one, and passes it to Render_SwapProjectionFarZ,
the far-Z setter. The source is the raw
Q16 g_EnvFogDistCurrent. [orig: Render_ProcessMainSceneFrame @ 0x5CA0F0,
load/add/call @ 0x5CA4BA/0x5CA4C1/0x5CA4D0; Render_SwapProjectionFarZ @ 0x58A8D0]

In a vehicle-seat thermal frame (Terrain_RenderWorldScene's fourth
argument, `Player_IsHeldWeaponThermal` pushed by Render_ProcessMainSceneFrame
@ 0x5CA8E3; every other caller pushes 0) both water-side BySide entity waves,
the far side @ 0x5C951F (before the water surface) and the camera side
@ 0x5C9600 (after it), run under
CTerrainRenderer_BuildLightingShaderConstants(1) @ 0x5C9511 / @ 0x5C95F8 and
receive the flat 0.25 lighting lane for ItemDef type-3 (person) entities;
Build(0) @ 0x5C9534 / @ 0x5C9616 restores the normal lane. The wave-side gate
is BySide's own (`test ebp,ebp` @ 0x5C7E01: isNearPass 1 draws only the
entities below the water, 0 only those above), so the first wave with
edx = camera-above is the far side and the second with edi = camera-below is
the camera side, matching the record's ladder (sky, far-water-side, water,
camera-side). The object shader applies
the per-instance value. No exclusion remains (corrected 2026-09-24): type 5
is never collected into the waves (`cmp eax,5; jz` `@ 0x5c8ddd`) and the
first-person pass always builds the normal lane, `Build(0)`
(`@ 0x4dee9d..0x4deea4`). The same thermal byte selects the secondary
particle materials (step 6 of the frame). Native render_order pins the pass
assignment.
[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5C8090;
Terrain_RenderWorldScene @ 0x5C93A0; Render_ProcessMainSceneFrame
@ 0x5CA0F0]

## 2026-09-24 rendering parity pass

The pass re-walked `Render_ProcessMainSceneFrame` and the scene core against
the binary. It corrected the frame order (the sky pass precedes the
viewmodel; `Terrain_RenderMainSectorPass` is the terrain surface), the scene
core's labels (foliage far patches, scars, NVG lasers, precipitation,
coronas, the glint), the ladder, and the FrameFX dispatch, and ported what
the walk found missing. Commits: "Renumber the render ladder for the retail
frame slots", "Port the first-person viewmodel draw gates", "Fog the viewmodel
under the dry pass", "Let the world paint over the viewmodel's blended
strips", "Pin the world pass's 0.2 near plane in every camera mode", "Draw the
scene's post-particle tail in its own overlay stage", "Draw the water glint
and the sun glare at the end of the overlay tail", "Draw particle pass A
inside the transparent list", "Give the foliage MODEL masks their own ladder
rungs", "Open the sky pass with the dome gradient's own rung", "Port FrameFX's
screen effects and the NVG render-to-texture view", "Port the NVG view's scene
raster, Scoped lens and Sighted card", "Port the tracer ribbon build and its
three materials", "Draw the distortion particles and tracer ribbons in
FrameFX's type-0 row", "Read RLOD thresholds as retail pixel counts in their
own level slot", "Draw the person overlays beside their bodies", "Settle the
viewmodel's Q3 exclusion note in FrameFx". D-RORD-7 and D-RORD-9 closed;
D-RORD-4's residual is witnessed and closed; no divergence row was opened.

### The post-particle overlay stage

Retail's scene core ends with a fixed tail after particle pass B
`[orig: Terrain_RenderWorldScene @ 0x5c93a0]`: the NVG laser beams
(`Render_NVGLaserBeamsForVisiblePersons @ 0x5c9695`) → the precipitation
(`Render_WeatherTrailParticles @ 0x5c96a6`) → the coronas
(`EffectWorld_RenderLightCoronas(1) @ 0x5c96ad`) → the water glint
(`Environment_UpdateSunGlare @ 0x5c96c0`, only while the water height is nonzero,
`@ 0x5c96b5`) → the underwater murk (`Render_DrawViewportColorQuad @ 0x5c96f5`) →
the modulator forced to 0xFF404040 (`@ 0x5c96fd` / `@ 0x5c9702`) → the sun
glare `Render_SkyboxSunGlow(1, 1)` (`@ 0x5c9714`; skipped when the scene's
stack arg is zero, `test edi @ 0x5c970a`) → the modulator restored
(`@ 0x5c9722`). The frame effects (`FrameFX_QualityAtLeast3 @ 0x5caa7b` →
`FrameFX_ApplyScreenEffect(2) @ 0x5caa97`, the bloom) come after the whole
scene. The water mirror (`Water_RenderReflectedWorldScene @ 0x5c8510`) draws
only the coronas (`@ 0x5c85fd`) after its particle and tracer passes; then
`Render_MainScene` closes the mirror target with the fullscreen dim (at
water detail >= 2, gate `@ 0x5c1727`; DESTCOLOR / ZERO under 0xFF404040,
`@ 0x5c1856..0x5c189e`) and, at FrameFX quality >= 3 (`@ 0x5c18c6`), the
sun/moon redraw and the glow inside the far depth band
(`Render_SetViewportFarDepth` called `@ 0x5c18f4`, `Render_CelestialBodies(0)
@ 0x5c18fb`, `Render_SkyboxSunGlow(0, 0) @ 0x5c1904`).

Reimpl: `engine/runtime/renderer/scene_overlay.h` carries the tail as data
(`kSceneOverlayOrder`, `kMirrorOverlayOrder`, `kInsetOverlayOrder`) plus the typed batch builders;
GameWorld's `scene_overlay` frame leg gathers one immutable frame, and each
view's `SceneOverlayCompositorEffect` (`godot/src/render/scene_overlay_compositor.*`)
runs after that view's particle pair and before FrameFx (`ParticleRenderer`
composes the chain).

- **Murk**: the full viewport, `g_EnvWaterColorLit` under the alpha byte
  0x80 - ftol(murk x -96) (`@ 0x5c96d3..0x5c96f0`), SRCALPHA / INVSRCALPHA,
  ZFUNC ALWAYS (pass flags 0x300000 `@ 0x5c39bd`), drawn per VIEW while that
  view's render eye is at or below the water height (`cmp/jg
  @ 0x5c96ca..0x5c96d1` includes equality), so every camera mode gets it.
- **Precipitation**: scene views only, never the mirror. The Precipitation
  node copies `renderer::compile_precipitation_frame`'s triangles into the
  stage (`renderer::append_precipitation_overlay`: MODULATE2X,
  SRCALPHA/INVSRCALPHA, z-test without z-write, the one diffuse
  `g_EnvTerrainLightCombined | 0xFF000000`; mode word 0x651, pass flags
  0x10500000 `[orig: Render_WeatherTrailParticles @ 0x5dee10]`); it owns no
  mesh or shader of its own any more.
- **Coronas**: scene and mirror; `renderer::append_corona_overlay` builds the
  billboards from LightScene's corona walk (`collect_corona_rows`)
  `[orig: EffectWorld_RenderLightCoronas @ 0x5aaf40]`.
- **Glint and glare**: the glare model's SELFLUM surfaces, read from
  Celestial's `get_overlay_bodies` seam (the model, its UPL_INTENSITY value,
  whether retail submits it), drawn tex.rgb x 2 x sat(SelfLumColor x light
  scale) ONE / ONE, fogged to black, ZFUNC ALWAYS (submit 0x110). The glint
  runs under the frame's light scale (`g_RenderLightScaleR/G/B` = the effect's
  ColorSrcGlobalGain, `Material_ApplyShaderParameters @ 0x58e05d`;
  `Render_UnpackModulatorToLightScale @ 0x58db30` = byte / 64 per channel);
  the glare, last, under the forced 0xFF404040 = 1.0. The stage zeroes both
  meshes' layer masks (the Q3 redraw reads the node).
- **The mirror's close**: the dim and the far-band disc and glow redraw close
  the mirror's overlay pass ("Close the water mirror with its dim and the
  far-band sky redraw"); the mirror glow's alpha is the no-occlusion (0, 0)
  form at the MIRROR camera's view dot (`env::mirror_glare_upl`), and the
  discs keep the beauty submit value (env-tod-re.md #30 / #37).
- **NVG laser beams**: `world/nvg_laser` (the gate, the clip and the point
  run) and `FirePresenter::append_nvg_laser_beams` over
  `Simulation::nvg_laser_sources` fill the `NvgLaserBeams` slot
  (`Render_NVGLaserBeamsForVisiblePersons @ 0x5c63b0` ->
  `Entity_RenderNVGLaserBeam @ 0x5c6090`; D-AI-12 (f) closed 2026-09-25).

**The weapon Inset pass's tail** (`kInsetOverlayOrder`, 2026-09-26). The
Inset pass (`Render_WeaponInsetScene @ 0x5c9740`, called `@ 0x5ca949` after
the main scene core `@ 0x5ca8ec`) runs the same scene core,
`Terrain_RenderWorldScene(view, 0, 0, 0)` (the call `@ 0x5c9de9`), so it draws the main
tail in the main order, every draw its own over the Inset's camera:
`kInsetOverlayOrder` = {`InsetNvgLaserBeams`, `InsetPrecipitation`,
`InsetLightCoronas`, `InsetWaterGlint`, `UnderwaterMurk`}.

- **Beams**: over the visible persons of the Inset's own collect (the list is
  zeroed `@ 0x5c916b` and appended `@ 0x5c8eb1..0x5c8ef9`).
- **Precipitation**: the drawer runs per call at the Inset camera on the one
  camera memory and fall accumulator both passes share
  (`@ 0x5dee74..0x5deed8`, `@ 0x5deede..0x5deef4`; env-tod-re.md
  §Precipitation).
- **Coronas**: the Inset's own corona walk
  (`EffectWorld_RenderLightCoronas(1) @ 0x5c96ad`) at the corona phase the
  per-scene prologue advances once per pass (`EffectWorld_BeginScenePass @ 0x5a9f70`, called
  `@ 0x5ca69c` by the main frame and `@ 0x5c9a5d` by the Inset); owned
  coronas are gated on the section masks the Inset's own collect wrote
  (`Terrain_IsBuildingSectionBitSet @ 0x5c6960`).
- **Glint**: `Environment_UpdateSunGlare` again at the Inset camera (the call
  `@ 0x5c96c0`), on the one accumulator both passes share.
- **No glare**: the scene core's second argument is zero, and the glare is
  gated on it (`@ 0x5c970a..0x5c970e` skips the `Render_SkyboxSunGlow` call
  `@ 0x5c9714`); the murk quad is the main tail's batch, which each view gates
  on its own eye.
- **Fog**: the Inset's own `ApplyFogAndAmbient(0, eye below water)`
  (`@ 0x5c9d41..0x5c9d4f`), never the thermal grey.

Retail composes the Inset camera inside the pass that draws it (the compose
`@ 0x5c9841`, the view `@ 0x5c997f`, the scene `@ 0x5c9de9`); the port's
`GameWorld::present_local_view_frame` hands the camera over before any Inset
leg reads it, so every Inset leg reads this frame's pose
(render-occlusion-re.md §8a).

Ctest `renderer_scene_overlay`; GUT `scene_overlay_stage_test`.

### Open after the 2026-09-24 pass

- None.
