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
priority ladder). Landed by maturity REN-3
([maturity-program.md](../maturity-program.md); standing rules
[ADR 0023](../adr/0023-render-visual-parity.md) — the queue machinery is
witness-source, never a port target; the ORDERING SEMANTICS are the port). The
T1 instrument (`tests/renderer/state_vectors_test.cpp`) pins the sort-key and
pass-class functions in this record.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Transparent ordering ladder (sky → far-water-side → water → camera-side → glow overlays) | MATCHING | frame bracket `[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`, rigid per-strip queue split `[orig: collect_render_objects_for_batch @ 0x5d8f20]`, bone-path submit flag `0x20` `[orig: collect_render_batches_for_entity @ 0x5d95c0..0x5d961f]`; `renderer_render_order` ctest + `renderer_state_vectors` section 3 + `object_model_runtime_gate_test.gd` |
| Sort keys (opaque composite key; transparent `~float_bits` back-to-front) | MATCHING (key semantics ported as pure functions) | `[orig: @ 0x5d928e..0x5d92c8; @ 0x5d931c..0x5d9326]`; `renderer_state_vectors` sort-key vectors; the two original key quirks are D-RORD-6 (permanent candidates) |
| Technique-class selection (submit flags + state-stack defaults → 6 classes) | MATCHING for the locked highest-quality path | `[orig: @ 0x5d90d7..0x5d9145; @ 0x5d95c0..0x5d961f]`; `renderer_state_vectors` pins selection and `auxiliary_technique_validation.json` pins each class behavior; D-RMAT-6 fixed |
| Batch queue machinery (17-DWORD entries, CDynList68, per-frame quicksort) | witnessed / not a port target | ADR 0023: device-era artifact; the reimpl renderer owns its queues — semantics captured in the rows above |
| Opaque state-sort (coarse depth slabs → effect index → fine depth, alpha-tested last) | witnessed / reimpl-internal equivalent | `[orig: RenderBatch_QuickSort @ 0x5d8b40]` unsigned-ascending + key layout below; Godot's opaque pass sorts front-to-back with its own state batching — same intent, D-RORD-2 permanent candidate |
| Frame pass sequence (shadow slots → viewmodel → sky → world → overlays → bloom) | witnessed (confirm-only) | `[orig: Render_ProcessMainSceneFrame @ 0x5ca0f0; Terrain_RenderSceneWithReflection @ 0x5c93a0]`; correspondence row |
| Viewmodel pass (near-Z 0.05 + viewport depth [0, 0.1], drawn FIRST, own flush) | witnessed / reimpl ported | the gun draws INSIDE the beauty pass: every viewmodel instance rewrites its clip position through the weapon `renderfov` focal ratio and the near-Z 0.05 swap, with the clip depth remapped into the nearest tenth of the reversed-Z range (the retail depth band — `shaders/viewmodel_pass.gdshaderinc`, `ObjectModel.set_viewmodel_pass`), so the world drawn after it never overlaps the gun and the murk/bloom composite covers scene and weapon alike; its alpha strips take `kRungViewmodel` (before the sky pass); `[orig: Player_RenderFirstPersonViewModel @ 0x4ded60; Render_SwapProjectionNearZ @ 0x58a8f0; Render_SetViewportDepth01 @ 0x58a7b0]`; the pass's `viewportScaleY` is `flt_8409E8` — the same `Render_SetAspectRatioMode @ 0x58d870` scale the world pass gets through `Render_SetViewProjectionWithDefaults @ 0x58f6b0` — so the FP frustum equals the world frustum (`[orig: Player_RenderViewModelIfAlive @ 0x4e0154 pushes it; @ 0x4dee5a..0x4dee7f]`, net-re §5.40 eighth pass); D-RORD-4 |
| EffectWorld particle ordering | BOUNDED deterministic port | `ParticleFrameCompiler` orders emitter AABB centers back-to-front with source-index ties, then particles within each emitter by depth/source index, before adjacent state runs. `ParticleCompositorEffect` draws every command sequentially with depth test/no write, so reimpl surface/material sorting cannot reorder packet commands; exact equivalence to retail's recursive alternating-axis/depth-bin order remains open `[orig: CParticleManager_RecursiveSortAndRender @0x5ec980; CParticleManager_RenderBatch @0x5e9890]` |
| EffectWorld pass placement around water | BOUNDED (D-RORD-7) | the portable compiler applies retail's emitter-scope strict-below/above-inclusive split and reverses far/camera subsets with the main eye. Pass A is the PRE_TRANSPARENT compositor callback (before water and every transparent), pass B the POST_TRANSPARENT one; the mirror publishes the same camera-selected subsets as two consecutive POST effects using its own view basis. Residual: far-side object ALPHA strips draw after pass A instead of before it. The 2026-08-22 auxiliary view that captured sky + far-side alpha and restored it before pass A was withdrawn 2026-08-23 — it cost a second full-resolution scene render per frame for that residual |
| Glow/envmap duplicate queue (Q3) + bloom flush | MATCHING for the locked highest-quality path | Producers publish typed object/water/celestial snapshots to `Q3FrameCompiler`; `FrameFx` executes its retained draw list into a compositor-owned full-resolution target attached to resolved beauty depth. LUM re-shades the SELFLUM NORMAL block (the GLOW slot is a copy of it), Glass emits depth-tested/no-depth-write rotated specular, and water draws its NV bright pass. The terminal reproduces the witnessed capture, 256² weighted blur, and half-strength additive FrameFX composite; D-RORD-5 and D-RORD-10 fixed. Two admission facts pinned 2026-08-29 on synthetic fixtures (`framefx_test`): an AlphaBlend LUM strip's copy carries SELFLUM's zero material alpha and paints nothing (`mount_mtrl2_ab_lum`), and a per-vertex skinned model never publishes a Q3 source at all (`person_mtrl0_ad_lum`): `Render_SubmitEntity @ 0x5dade0` sends a skinned model (modelData+16 & 1) to `collect_render_batches_for_entity @ 0x5d94b0`, which appends only the opaque list and the Q1/Q2 alpha queues (`@ 0x5d97ca`, `@ 0x5d983b`); the Q3 copy `@ 0x5d93b5..0x5d9447` is `collect_render_objects_for_batch`'s alone (`renderer::q3_object_source_admitted`; no shipped skinned shader tag carries the GLOW capability either, `kMaterialDescriptorTable`) |
| Authored object RLOD selection | MATCHING (D-RORD-11 closed 2026-08-29: the hard switch IS retail's behaviour) | `renderer::select_object_lod` walks the fine-to-coarse authored thresholds against the projected screen radius (`renderer::project_bound_sphere_radius_q16`, the witnessed integer projection) scaled by `renderer::object_lod_frame_scale` (detail 3 = 2.0 quality over the viewport width times 640; lower profiles detail * 0.33 + 0.34), with equality-to-coarser behavior, the coarsest-slot back-off (a final or zero-next row whose UNSCALED radius still exceeds it draws the level one finer), and the missing-level fallback; `kObjectLodSubPixelCullQ16` (0.75 px) drops an instance from every level before the selector. `ObjectModel.update_authored_lods` (individual models, each swapping the selected level onto its own retained surface slots) and `MissionObjectPlacer.update_static_lods` (every retained static instance, per entity inside the 512-unit bins) consume it each frame. Every RLOD remains retained (an individual model caches each level as mesh/material rows, a static graphic as one population per level) and a transition swaps the rows onto the model's slots or moves which level population carries the static slot's row, without rebuilding meshes. Device mechanism (2026-08-30, no semantic change: which level is drawn, when, and its blend/order are untouched): each level's population holds only the slots currently at its level, packed dense from row 0 with `visible_instance_count` = live rows, a crossing swap-removes the row from the old level's population and appends it to the new one (shadow twins and the blended global rows the same), and a population with no live row is hidden so the GPU and every viewport's cull see one row per live instance instead of a zero-scaled row per level. An individual model (animated, portal-carrying, live-PANM) retains ONE `MeshInstance3D` per surface slot, sized to its widest level, and a crossing swaps the level's cached mesh, material, ROBJ/skeleton parent, skin binding and Q3 source registration onto those slots in place (the build serial and the instance ids never move; a slot the coarser level has no submesh for is parked hidden; the placer's shadow-only siblings are bound to their harvested level through `ObjectModel.add_level_bound_visual` and follow the same switch), so every presented transform propagates through one instance per drawn strip rather than through every retained level's hidden instances (`object_model_lod_occluder_test`). A threshold crossing hard-switches the drawn level exactly as retail does: `Model_SelectRlodLevel @ 0x5c3b20` returns one level in EAX, the overlap fraction it also writes is dead retail data (global `0x29ACD9C`, read only by a callerless stub `@ 0x5c38c0`), and no submit, collector or flush cross-fades or dual-submits across a threshold. Attachments draw at the parent's level clamped to their own LOD count (`renderer::attachment_lod_index`; `ObjectModel.set_authored_lod_owner` stamps the third-person held weapon), and the composed avatar's head and body each walk their own table with the entity's one projected radius |

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
frame begin and every flush; dev collector toggles at `+843/+844` (written
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
(`entry[9]/[10]` = the floats, `entry[15]` = aux2). `setup_terrain_effect_for_entity
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
depth is smaller than the radius. The sector-entity draw returns before the
selector when that radius is at most 0.75 px (49152). The table is ordered
fine/near to coarse/far; threshold slot 0 is unused, comparison starts at
slot 1, and equality advances to the coarser level. The selected level is
clamped to the final row; on the final row (or a row whose next threshold
is zero) the UNSCALED radius must also exceed the row's threshold, or the
level one finer is drawn. The submit then walks a missing model back toward
lower indices (finer geometry). The frame multiplier is the detail profile's quality term
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
`collect_render_objects_for_batch @ 0x5d8f20`,
`collect_render_batches_for_entity @ 0x5d94b0` and the flush never reference
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
(`render_sector_entity(entity, radius, flags, head) @ 0x5c7ffc`), then the
body after `Avatar_SetBodyCamoCtrl @ 0x57a390` with flag `0x10000000`
(`@ 0x5c8020`); `render_sector_entity`'s fourth argument (`forced_model`)
replaces entity+48 as the RLOD table `@ 0x5c41b4..0x5c41b6`, so head and body
each walk their own table with one radius. Attachments never run the walk:
`BoneCallback_org0_World @ 0x4e3940` receives the parent's level as its frame
index and indexes every overlay model with `min(level, graphicModel[4] - 1)`
(the item overlay `@ 0x4e39c4..0x4e39ce`, NVG `@ 0x4e3b82..0x4e3b8c`,
binoculars `@ 0x4e3c2d..0x4e3c37`, the held weapon `@ 0x4e3ce8..0x4e3cf2`,
the mounted child `@ 0x4e3e34..0x4e3e51`), which is
`renderer::attachment_lod_index`, consumed by `ObjectModel.set_authored_lod_owner`
(the third-person held weapon follows the head part of a composed avatar, the
body otherwise; the NVG/binocular overlays take the same owner when they are
ported). Bounded residual (2026-08-30): the mounted child is NOT yet stamped
— riders are presented as independent wire entities (`wire_present_pass.cpp`
carries no seat leg; the row's `PF_CARRIER_HANDLE` names the carrier) and
walk their own thresholds from their own projected radius where retail
indexes them with the vehicle's level `@ 0x4e3e34..0x4e3e51`; the stamp
belongs beside the wire walk that positions them. The entity+36&4 / entity+52/+56 machinery with
`compute_lod_fade_timers @ 0x5c3f40` (CTRL registers 65..70, span def+424
default 50, step def+428 default 25) is a controlled-material MODEL SWAP, not
an RLOD fade. `[orig: Model_SelectRlodLevel @ 0x5c3b20, back-off
@ 0x5c3b88..0x5c3b9b, dead fraction store @ 0x5c3bb3/0x5c3bc2; sub-pixel
gate and level consumption render_sector_entity @ 0x5c42de..0x5c42f0, forced
table @ 0x5c41b4..0x5c41b6; projected-radius producer
Viewport_TransformAndClipPoint @ 0x41177a..0x4117ec, stored per entry by
collect_visible_entities_for_terrain @ 0x5c8eca; frame scale
Terrain_RenderSceneWithReflection @ 0x5c940c..0x5c9499 and
Terrain_CollectVisibleEntitiesForReflection @ 0x5c90c3..0x5c9121; override
writers @ 0x5c08d1, @ 0x6106cb]`

**Submission.** `Render_SubmitEntity @ 0x5dad80`: stamps the shader clock
globals (`GetTickCount`, seconds = `(tick % 0xFA000) · 0.001` → `flt_272140C`),
allocates LOD GPU buffers, then dispatches on `modelData[16] & 1` to the
bone-path collector (`collect_render_batches_for_entity @ 0x5d94b0`, gated on
ctx+843) or the object-path collector (`collect_render_objects_for_batch
@ 0x5d8f20`, gated on ctx+844). **Its 4th argument is the render-flags word**
(the IDB's `numBones` and world §13's `numEntries` were misnomers):

| Flag | Meaning (witnessed at both collectors unless noted) |
|---|---|
| `0x1` | CLIP-class request; the bone path SKIPS entirely (`@ 0x5d94bc` — skinned entities are absent from clip passes) |
| `0x2` | PROJSHAD class |
| `0x4` | DEPTHMASK class; object path renders opaque strips only (`@ 0x5d948b` breaks before the alpha pass) |
| `0x8` | entry flag bit 2 = run only the technique's FIRST pass (`@ 0x5da23d`) |
| `0x10` | entry flag bit 3 = force `ZFUNC = ALWAYS` for the entry — a per-entry z-read-off override (witnessed at REN-4 in the FlushBatches pass loop; else the pass zmode bit 0x80 picks ALWAYS/LESSEQUAL) |
| `0x20` | bone path: transparents to Q2 instead of Q1 (the caller picks the water side) |
| `0x40` | object path: alt-vertex-stream request for sub-objects (`robjIndex > 0`) |
| `0x80` | alt-vertex-stream request (entry bit 1); used by parented sector entities (`entity+464`) and the armed viewmodel |
| `0x100` | suppress the Q3 glow/envmap copy (used by celestial submits `@ 0x5acc0a`) |
| `0x200` | MATCHTERRAIN class (the water-side waves' decal sub-pass) |
| `0x10000000` | second-part marker of the two-model player avatar (NOT a dual-LOD "far then near" repeat draw; D-RORD-11): `Terrain_RenderSectorEntitiesBySide` draws the HEAD model first (`@ 0x5c7ffc`, after `Avatar_SetHeadCamoCtrl @ 0x57a370`) and then the BODY model with this flag (`@ 0x5c8020`, after `Avatar_SetBodyCamoCtrl @ 0x57a390`); its ONLY consumer is `BoneCallback_org0_World @ 0x4e3c87`, which skips the held-weapon (`@ 0x4e3d99`) and mounted-child (`@ 0x4e3e95`) overlay draws on the second part so they render once per entity (the NVG/binocular overlays are not gated; world §13). It never reaches the batch queues: `collect_render_objects_for_batch` tests only 0x1/2/4/8/0x40/0x80/0x100/0x200 |

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
registry slots cached by `resolve_effect_subobjects_and_shader @ 0x5b1870`),
and runs the block's passes 0..n-1 in order (block +4 = pass count,
+16+8i = rules/z/a flags, +20+8i = the pass FOGMODE), gating each on its
light rules (flags & 0x3C vs the entry's ≤3 light handles, spot/point split
by `Light_IsSpotlight @ 0x5a9040`: 0x10 run-if-no-spots, 0x20 run-if-spots,
0x04 run-if-pointlights, 0x08 run-if-spots) — witnessed at REN-4: rules 4/8
multiply into ONE DRAW PER LIGHT (point params / spot projection set per
iteration + CommitChanges), rule 2 fills the PointLight*Array set for the
per-count VS variants, and MATCHTERRAIN-class entries bind the terrain tile
texture under the object (`terrain_tile_cache_lookup @ 0x604140`). Full pass
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
  index (matdef+596 ptr − `HLSLEffect_Registry`, ÷1004 via the 0x828CBFBF
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
  0x20. The split threshold is written per frame from `Env_WaterHeightFixed`
  (`@ 0x5c93e2..0x5c93f0`).

**Sort-and-flush.** `CRenderBatchQueue_SortAndFlush @ 0x5dae40` (mode):
mode 1/2/3/4 = sort+flush Q0/Q1/Q2/Q3 alone; **mode 0 = Q0, then Q2, then
Q1** — the mini-scene flush (viewmodel, loading screen, UI/avatar previews,
HUD 3D elements, the sky/celestial family, tile models, shadow slots — every
caller outside the world pass uses mode 0). Q3 is flushed ONLY by
`FrameFX_RenderBloomPass @ 0x582940` (mode 4 `@ 0x582a54`): the glow/envmap
duplicates draw during the bloom overlay after the scene. After flushing,
counts reset; blend-op and fog state are restored.

**Highest-quality reimpl mapping (D-RORD-5 fixed 2026-08-22; D-RORD-10 fixed
2026-08-29).** Retail draws its Q3 flush into a
backbuffer-sized altbuffer only because D3D9 keeps the beauty depth-stencil
bound there (`FrameFX_CreateAltBufferTexture` creates it from the D3DPRESENT_PARAMETERS block
with the backbuffer's size and multisample mode; nothing opaque is ever
re-rasterized), and the only consumer of that surface is the StretchRect
into the power-of-two capture feeding the 256² kernel
(`create_frame_effect_render_targets @ 0x583c40`,
`FrameFX_CaptureRenderTarget @ 0x584020`). `FrameFxCompositorEffect` owns a
full-resolution Q3 color target with resolved beauty depth attached.
Main-thread producers publish generation-bound mesh, transform/bone, texture,
and material values; `Q3FrameCompiler` rejects unsupported rows (a geometry
lease whose generation no longer matches the published one included), sorts
object duplicates back-to-front, and retains the fixed water/celestial/sun
bracket. Geometry is packed once per (source, surface) into the adapter's
retained cache (`q3_geometry_cache`): one server read at first sight, one
device buffer per entry uploaded per generation, MultiMesh rows read once per
instance generation (no Q3 producer carries a bone palette: the bone path is
never a Q3 source and the fake-skinned rigid models that could are never on
the world layer); the water strip publishes the CPU arrays it uploads each frame, and a
static RLOD switch or destruction carve invalidates only its populations'
instance rows (the packed surfaces and their device buffers stay; the rows
are packed dense, so the re-read covers live rows only), so a stable
frame reads nothing back and re-packs nothing and a row rewrite re-reads rows
alone (the backend report's `q3_readbacks_this_frame`,
`q3_instance_row_reads_this_frame` and `q3_packed_vertices` pin it).
The render callback re-shades each LUM copy as the SELFLUM NORMAL block into
the black-cleared Q3 target (`_FFP.fx` copies the NORMAL pass block into the
GLOW slot `@ 0x5afc7f`: Diffuse1 x Detail MODULATE2X x RGB modulator x
min(gain, 1) x 2, the wrapper's fog policy, alpha 0, so blended LUM
contributes nothing and additive LUM adds its colour; nothing samples the
beauty colour), then emits Glass's CubeRotSpecular and water's NV bright pass
(`Water_PSBumpReflectNV`: `color *= saturate(luma(0.25,0.60,0.15)² − 0.15)`
with the device fog color forced black — `render_water_surface(view, 1)
@ 0x5c3442..0x5c3492`, `CD3DDevice_SetFogAndBlendMode(2) @ 0x6778ed`),
then the celestial discs and the glare through the far-band viewport:
`Render_SetViewportFarDepth @ 0x582a70` (MinZ 0.98 / MaxZ 0.99996948
`@ 0x58a840`) remaps their depth into that band before the ordinary z-tested
flush, so they survive only over cleared sky (`kQ3FarBandMinZ..MaxZ` in
`renderer/q3_frame.h`; depth-tested, no depth writes). No Q3
viewport, camera mask, depth-occluder rerasterization, proxy geometry, or
compatibility renderer remains. The beauty camera is standardized to mask
`0x18C01` (the viewmodel layer folded in, 2026-08-26).

The POST_TRANSPARENT `FrameFxCompositorEffect` performs the witnessed
RenderingDevice sequence: RGBA8 capture of the full-resolution focused source
into the 256² working target; a 256² four-tap downsample at 30° with base `1/2048`, radius
`1/1024`, and weights `0.50@0.5`, `0.46@2.5`, `0.35@4.5`, `0.19@6.5`; additive
weighted pairs at 90°/270° then 0°/180°; and a four-tap 45° final average at
radius `0.0027621093`, alpha 0.5, blended SRCALPHA/ONE over the beauty target.
The terminal gamma decode follows that composite. Particle compositor assembly
always places this terminal effect last, and built-in Godot glow is disabled.
The `glow` probe pins all 24 technique contracts plus the native backend
report (typed submitted/rejected/drawn counters, resolved-depth facts, and
exact constants). `Q3FrameCompiler` owns back-to-front Q3 order. D-RORD-3 closed with one
priority-bearing material instance per retained rigid strip, transformed
strip-center classification whenever the model transform or the water plane
changes (the same result as retail's per-frame recompute without a server
round trip per strip per frame), the bone-path entity-side selector, and the
camera-underwater ladder reversal.

**The frame** (`Render_ProcessMainSceneFrame @ 0x5ca0f0`, the live per-frame
driver; camera above water shown — the sides mirror when underwater):

1. `RenderBatchCtx_BeginFrame`; `Render_TerrainScene @ 0x610c80` runs the
   offscreen prep: the per-entity shadow-slot pass (`render_shadow_pass
   @ 0x5d7b70` → `RenderSlot_RenderEntityAndChildren @ 0x5d7690` per active
   slot, each flushing mode 0), the reflection prerender when water is active
   (`g_WaterActive @ 0x31BC918` → `Water_ReflectionPrerender @ 0x5c2780` →
   `render_main_scene @ 0x5c1240`, the reusable offscreen scene renderer also
   used by `render_cinematic_multiview @ 0x570940`), the environment cubemap
   update, and the terrain lighting ramps.
2. Clear: color = `Env_SkyfogBlock` above water / `Env_WaterColorLit` below
   (`@ 0x5ca78b..0x5ca792`; gray 0x808080 in scope views).
3. **Viewmodel first** (`Player_RenderViewModelIfAlive @ 0x4e0140`, renamed
   from `sub_4E0140` — skips when dead or spectating): the gun+arms mini-scene
   with projection near-Z swapped to 0.05 (`Render_SwapProjectionNearZ
   @ 0x58a8f0`, renamed from `Scar_SetShadowBias`; the global is
   `g_ProjectionNearZ @ 0x8409DC`, consumed by
   `D3DXMatrixPerspectiveFovLH @ 0x58d9cc`) and the viewport depth range
   clamped to `[0, 0.1]` (`Render_SetViewportDepth01 @ 0x58a7b0`, renamed
   from `Scar_SubmitDecalToRenderObject` — it builds a D3D viewport
   `{x,y,w,h,MinZ 0, MaxZ 0.1}`), per-weapon FOV (`renderfov` @ `Def+0x148`,
   horizontal; the vertical is derived through the SAME `flt_8409E8` viewport
   Y-scale the world pass uses — `Player_RenderViewModelIfAlive @ 0x4e0154`
   pushes it as the pass argument, `Render_SetAspectRatioMode @ 0x58d870` sets
   it to `target_ratio / (h/w)`, 0.96 in 16:10 mode on 1920×1200 — so the FP
   and world frusta are identical, `proj[0][0] = cot(40°)` in both), a
   state-stack push carrying the equipped weapon item's `light_transfer`
   (`ItemDef+0x218`, entry dword 1 — a lighting term; the effect-scale dword 0
   keeps 1.0, the sun factor is discarded), submits with flags 0x80 when armed,
   pops, and flushes mode 0 — the compressed depth band keeps the world from
   ever overlapping the gun. `[orig: Player_RenderFirstPersonViewModel
   @ 0x4ded60; @ 0x4dee5a..0x4dee7f; @ 0x4def3c]`
4. Sky pass (`Terrain_RenderSkyboxPass @ 0x610ac0`): dome → celestial bodies
   → terrain surface, per the env record's witnessed order (env-tod-re.md
   §Sky dome); the celestial submits write the 16.16 alpha global
   `Render_SubmitAlpha16 @ 0x83fde8` (clamped [0, 0x10000] `@ 0x5acbdd..0x5acbfa`
   — a real submit-alpha global, not a queue; the name stands) and pass flag
   0x100 (no Q3 copy), flushing mode 0.
5. The world (`Terrain_RenderSceneWithReflection @ 0x5c93a0`):
   - sector models + the first entity wave → flush(1); the water-side wave
     (`Terrain_RenderSectorEntitiesBySide @ 0x5c7d50`, renamed from
     `Terrain_RenderSectorEntities_0` — arg selects BELOW(1)/ABOVE(0) water
     entities by height vs `Env_WaterHeightFixed`) runs the far side:
     MATCHTERRAIN sub-pass (submit 0x200, entities gated on `+300 & 0x300`)
     → flush(1), then normal → flush(1); terrain LOD update → flush(1)
   - **flush(3)** — the far-side (below-water) transparents
   - trails (`CEffectEmitterPool_RenderMainPass @ 0x5dcaf0`; this record's earlier
     `render_all_trail_strips` label was superseded in the IDB by the
     `CEffectEmitterPool_*` family — see [correspondence.md](../correspondence.md))
     + particle pass A (`EffectWorld_RenderParticlePass @ 0x5f7240` →
     `EffectWorld_DrawParticles @ 0x5f6680`, renamed from the
     `CNapiSession_*` misnomers — `g_EffectWorld @ 0x2C25CD8`)
   - decal/scar pass 0; **the water surface** (`Terrain_RenderWaterPass
     @ 0x610640`, renamed from `sub_610640` → `render_water_surface
     @ 0x5c32c0`, both view variants, self-gated by camera side)
   - the camera-side (above-water) entity wave: MATCHTERRAIN → flush(1),
     normal → flush(1); foliage batches; decal pass 1
   - **flush(2)** — the camera-side (above-water) transparents
   - trails + particle pass B; per-entity projectile trails; weather
     particles; foliage billboards; sun-glare occlusion update
   - underwater murk scissor overlay (`Terrain_RenderSceneWithReflection
     @ 0x5c96c5..0x5c96fa`): the independent gate is
     `camera_z <= Env_WaterHeightFixed` (`jg` skips), so equality is covered
     even though the environment fog classifier uses strict `<`. It calls
     `Terrain_DrawScissorRect @ 0x5c38e0` for the full current viewport with
     RGB = `Env_WaterColorLit` and
     `alpha = 0x80 - trunc(Env_WaterMurk * -96.0)` =
     `128 + trunc(96 * murk)` (CP01 `0.8` -> `204/255`). The shader state is
     standard `SRCALPHA` / `INVSRCALPHA`, so
     `out = lit_water * alpha + prior * (1 - alpha)`; pass flags disable
     Z-write and force ZFUNC ALWAYS. The early first-person viewmodel, world,
     particles, weather, and foliage are therefore all attenuated, while the
     HUD is not.
   - skybox sun glow is drawn after the murk overlay (`@ 0x5c9714`) and before
     the later HUD (`@ 0x5cad04`), so retail glare is deliberately not
     attenuated by the underwater quad.
   With reflection enabled, each entity wave adds a mirrored sub-pass +
   flush(1) under mirrored lighting (`CTerrainRenderer_BuildLightingShaderConstants
   @ 0x5c8090` arg 1), using the mirror matrix/CLIP machinery above and the
   mirror-winding byte — the full reflection spec is env #30 (REN-6).
6. Player shadow/scar dispatch (`Render_DispatchShadowByType @ 0x584440` —
   the Scar_ decal/overlay family; out of REN port scope. The render-slot
   ENTITY ground shadows are a different family and are ported — they render
   at frame open (step 1's slot pass) and drape during the terrain scene
   walk via `RenderSlot_DrawAllDrapes @ 0x5d6e20`; see
   render-lighting-re.md's render-slot section), radar/scope overlays,
   HUD (`HUD_RenderAllOverlays @ 0x5a8070`, mode-0 flushes for 3D HUD
   elements), fades, tips.
7. `FrameFX_RenderBloomPass` (flush 4 = Q3), present,
   `RenderBatchCtx_EndFrameStats`.

**Dead variants** (zero live callers; never order-witness from them):
`render_skybox_layers @ 0x5ac230`, `render_sun_lens_flare @ 0x5ad490`,
`render_foliage_at_camera @ 0x5ac100` (the env record's catalog).

## The ported ordering semantics (this slice)

The reimpl keeps its own queues (ADR 0023); what ports is the ORDER as data +
pure functions in `engine/runtime/renderer/render_order.{h,cpp}`:

- the transparent priority ladder (sky dome < celestial bodies < glare <
  far-water-side world alpha < water surface < camera-side world alpha <
  weather/particle overlays < glow), applied as Godot `render_priority`
  rungs — `godot/src/env/celestial.cpp`'s local ladder re-derives from it,
  `godot/src/env/water.cpp` takes the water rung, and `godot/src/object/object_model.cpp` assigns
  blended object materials their water-side rung;
- the BmTxMirrT P3 post-multiply rung (`kRungObjectPostMultiply = -3`,
  between the celestial bodies and far-side alpha): retail has no separate
  submit for P3 — it is a pass of the opaque strip's own technique, and
  `CRenderBatchQueue_FlushBatches` runs every pass of one entry back to back
  (`[orig: the pass loop @ 0x5da20b..0x5da23d; per-pass fog/blend
  @ 0x5da2f7]`) inside the Q0 flush(1) brackets (`[orig: @ 0x5c9506..0x5c9581;
  @ 0x5c9630..0x5c9647]`), never inside the Q1/Q2 flushes (`[orig: @ 0x5c9596;
  @ 0x5c967a]`). Godot cannot blend inside its opaque stage, so the port's
  rung is the lowest world slot above the sky: after every opaque, before
  far-side alpha and the water. Bounded residual: retail's far-side opaque
  flush follows the water pass (`@ 0x5c9630` after `Terrain_RenderWaterPass
  @ 0x5c95dc`), so a far-side P3 multiplies over the drawn water there while
  the single rung draws it beneath the water — same class as D-RORD-7;
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
| D-RORD-4 | Viewmodel is a camera-tracked node with no depth treatment (clips into near walls) | drawn FIRST with near-Z 0.05 + viewport depth range [0, 0.1], own mode-0 flush (`[orig: @ 0x4ded60; @ 0x58a7b0]`) | RESOLVED — ported 2026-07-09 as a dedicated shared-world SubViewport composite; re-ported 2026-08-26 INSIDE the beauty pass: a shader-side projection override per viewmodel instance (renderfov focal ratio, near 0.05, clip depth remapped into the nearest tenth of the reversed-Z range = the retail depth band) so the gun is under the murk/bloom composite like retail and no second full-window scene render exists. Bounded residual: Godot alpha strips write no depth, so a world transparent nearer than the band could blend over gun glass (retail's Z-write state for the viewmodel flush is unwitnessed) |
| D-RORD-5 | GLOW was hosted through Forward+ HDR extraction instead of retail's isolated Q3 target and FrameFX kernel | strips with effect capability 0x10000000 get a Q3 copy (GLOW class when present), flushed by `FrameFX_RenderBloomPass` together with the NV water redraw, the celestial bodies, and the sun glow (`[orig: @ 0x5d93b5; @ 0x582a54..0x582a80]`) | **FIXED (2026-08-22; source replaced 2026-08-29)** — typed LUM NORMAL-copy, Glass CubeRotSpecular, water NV, celestial/sun draws, capture, weighted blur/final kernel, SRCALPHA/ONE composite, and terminal ordering are native and RenderingDevice-pinned; no glow proxies or compatibility path remain |
| D-RORD-6 | Not reproduced | two original key quirks: opaque key bits 15+ carry residual stack garbage (`@ 0x5d92b9`), and the transparent key lags one strip within a render object (`@ 0x5d9326` vs the `fst @ 0x5d9347` overwrite) | PERMANENT-candidates (original-bug/garbage class): reproducing either manufactures garbage (ADR 0022) |
| D-RORD-7 | Two immutable main-view particle submissions use the exact emitter-scope water predicate: strict `< water` below, equality above, with far/camera order reversing at the main eye. Pass A is the PRE_TRANSPARENT compositor callback (before every transparent, water included) and pass B the POST_TRANSPARENT one; the mirror pair is consecutive after reflected geometry, selected by the main-camera side. Far-side object ALPHA strips therefore draw AFTER pass A | two calls to the global particle manager: pass A between far-side transparents and water, pass B after camera-side transparents (`[orig: Terrain_RenderSceneWithReflection @0x5c93a0; EffectWorld_RenderParticlePass @0x5f7240; CParticleGroup_RenderChildren @0x5e5890]`). Reflection receives the main-camera `< water` boolean and calls its two particle passes consecutively after reflected geometry (`[orig: render_main_scene @0x5c16ed..0x5c171f; Water_RenderReflectedWorldScene @0x5c8510]`) | OPEN (bounded) — the residual is a submerged/far-side transparent strip overlapping a far-side particle in screen space (strip-over-particle instead of particle-over-strip). The 2026-08-22 auxiliary far-alpha view closed it at the price of a second full-resolution scene render every frame and was withdrawn 2026-08-23; `framefx_test.gd` keeps the water-attenuation differential (far packet attenuated by water, camera packet not). The portable adversarial sorter contract separately pins retail's projected Z→X→Y recursive leaves and non-stable equal-key order (D-PTL-21 fixed). Reopen only with a scene that shows the strip/particle overlap |
| D-RORD-8 | FIXED 2026-08-12. `GameFramePipeline` now runs session tick → local-view placement → terrain → foliage → the remaining device legs. Terrain samples the live viewport camera internally and foliage receives `GameWorld._render_camera_xform()`, so both compile from the view this frame's player state produced; foliage retains the frame-entry transform when no live camera exists, while terrain has no headless draw. Occlusion's post-present slot stands — present re-asserts base visibility, occlusion layers hides, Godot renders after both | collect-then-submit runs inside the render frame, before submission, against the view built from current player state (`Render_ProcessMainSceneFrame @0x5ca0f0`) | MATCHING for the camera-phase contract; `game_frame_pipeline_test` pins the order and a post-present camera-generation marker for both terrain and foliage |
| D-RORD-10 | The Q3 bloom source was a second shared-world scene submission that rerasterized terrain/opaque depth occluders at kernel size | retail draws Q3 objects, NV water, celestial bodies, and sun glow into a backbuffer-sized altbuffer with beauty depth-stencil still bound; the only consumer is the StretchRect into the power-of-two capture (`[orig: FrameFX_RenderBloomPass @ 0x582940; FrameFX_CreateAltBufferTexture altbuffer CreateRenderTarget @ 0x58217e; create_frame_effect_render_targets @ 0x583c40; FrameFX_CaptureRenderTarget @ 0x584020]`) | **FIXED (2026-08-29)** — `Q3FrameCompiler` retains the retail bracket and typed generation-bound inputs; the terminal compositor draws them into one full-resolution Q3 attachment sharing resolved beauty depth from a retained per-source geometry cache (no per-frame server readback or re-upload). No auxiliary camera/view, camera-mask shader selection, or depth rerasterization remains |
| D-RORD-11 | The authored RLOD selector, its coarsest-slot back-off, the frame scale, the integer projected radius and the sub-pixel cull are ported, every level remains retained (individual models and the per-instance static populations alike), a threshold crossing hard-switches the drawn level, attachments take the parent's level clamped to their own count (`renderer::attachment_lod_index`, the held weapon through `ObjectModel.set_authored_lod_owner`), and the composed avatar's head and body select independently from one projected radius | the same: `Model_SelectRlodLevel @ 0x5c3b20` returns one level (EAX); its overlap fraction is written only to `0x29ACD9C`, read solely by the callerless stub `@ 0x5c38c0`, and no submit/collector/flush consumes it. The `0x10000000` pair at `@ 0x5c7ffc`/`@ 0x5c8020` is the head+body avatar, not a far/near dual submission; overlays index their LOD with the parent's level `@ 0x4e39c4..0x4e3e51` | **FIXED (MATCHING, 2026-08-29)** — the "dual-submit through the overlap fraction" reading was refuted at the binary: the hard switch IS retail's behaviour, the dead `blend_fraction` output was deleted from `renderer::select_object_lod`, and the attachment rule was ported (`object_lod` ctest, `object_model_lod_occluder_test`, `wire_present_pass_test`). Bounded residual: only the third-person held weapon is stamped with an owner; mounted riders (`@ 0x4e3e34..0x4e3e51`) and the unported NVG/binocular overlays still select their own level (the witness map names the leg) |
| D-RORD-9 | The underwater murk quad is a `PlayerViewEffects` overlay after the shared-world `ViewmodelPass` (CanvasLayer 0) and behind the HUD (CanvasLayer 1), so it correctly covers scene + weapon and excludes HUD; it currently also covers the reimpl's 3D celestial/glow. `PlayerViewEffects` is created with the local-player HUD, so no-local-player/spectator views currently receive no murk quad | retail draws the source-over murk quad after the viewmodel/world/weather/foliage and then draws sun glow bright on top (`[orig: @ 0x5c96c5..0x5c9714]`) | MATCHING for CP01 and the registered full-frame fixtures, whose capture contract requires a spawned local player and HUD; OPEN bounded residuals are glare ordering and generic spectator/no-local-player parity |

## IDB changes made during the session

| Address | Old | New | Basis |
|---|---|---|---|
| 0x5f7240 | CNapiSession_PumpWithMode | EffectWorld_RenderParticlePass | body is the particle render pass; no net I/O |
| 0x5f6680 | sub_5F6680 | EffectWorld_DrawParticles | CEffectWorld_SetupRenderState + CParticleManager_BeginFrame + VB flush |
| 0x4dbc70 | CNetPlayer_PushPositionToHistory | RenderStateStack_Push | copies stack entry top→top+1 at ctx+240, ++top — identical to the waves' inline push |
| 0x5d8990 | sub_5D8990 | RenderBatchCtx_BeginFrame | resets queue counts, stack top, stack defaults, stats |
| 0x5d87d0 | sub_5D87D0 | RenderBatchCtx_EndFrameStats | copies live stats to the last-frame block |
| 0x5dcaf0 | CEffectEmitterPool_RenderMainPass | render_all_trail_strips | iterates 256 trail channels → render_trail_strip |
| 0x610640 | sub_610640 | Terrain_RenderWaterPass | calls render_water_surface under g_WaterActive; stores the terrain render mode |
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
| 0x31BC918 | dword_31BC918 | g_WaterActive | gates water render + reflection prerender; recomputed per frame by terrain_setup_view_and_lighting @ 0x60fe40 from the tracked visible-terrain height range vs Env_WaterHeightFixed, OR last frame's g_BlinkWaterVisible (env-tod-re.md, the water-active predicate) |
| 0x2721A08..38 | flt_2721A08.. | g_BatchSortDepthPlane{X,Y,Z,W} | the camera-forward plane the sort distance dots against |

REN-4 additions (the pass-execution grill): `Light_IsSpotlight @ 0x5a9040`,
`Light_GetPointLightParams @ 0x5a9180`, `Light_ApplyAsD3DLight @ 0x5abd50`
(ex-`sub_*`) — the pass-rules light machinery; the full REN-4 rename set is
logged in [render-material-re.md](render-material-re.md).

## Open questions

- **Closed at REN-4**: entry flag bit 3 (= force ZFUNC ALWAYS, the submit-0x10
  override — witnessed in the pass loop); the `+841` byte (its reader is
  `setup_entity_lighting_and_shader_constants @ 0x5d98a0` — it gates the
  mirror-clip constants: MatTexClipPlane ← base × the active mirror matrix
  ctx+756, VecDepthMaskPlane ← ctx+824, with ctx+842 tracking applied
  clip-plane state); the MATCHTERRAIN class MECHANISM (binds the terrain tile
  texture under the object — [render-material-re.md](render-material-re.md)
  §Pass execution).
- **Closed at REN-6 (2026-07-06)** — `Water_RenderReflectedWorldScene` was a 5-byte header
  (`call sub_58AA80`) falling through into an unclaimed body (the same split
  shape as `render_water_surface`; a stale NORET flag on `sub_58AA80`
  truncated the analysis). Renamed `Water_RenderReflectedWorldScene`: the
  offscreen reflection's world subscene — fog/ambient push, NORMAL lighting
  constants (arg 0), builds `g_WaterMirrorMatrix` as the water CLIP-plane
  TEXTURE matrix (`u = y − waterHeight + 0.5`; TexClip1D ref-128 cuts at the
  plane) and arms `g_WaterMirrorActive`, then sector models → entity wave →
  flush(1) → below-side/above-side waves → flush(0) → foliage-tile/LOD
  updates → flush(0) → particles → trails. Full pipeline:
  [env-tod-re.md](../env/env-tod-re.md) §Reflection pipeline.
- `Terrain_RenderSectorEntities` (list `0x2999518`) vs the BySide wave's list
  (`0x2984890`) — which world-object populations feed which list (sector
  models vs placed entities) rides the world-record's population map.
- The MATCHTERRAIN sub-pass ENTITY gate (`entity+300 & 0x300`) — which item
  flags those bits are (the consumer side is closed); world-record scope.
