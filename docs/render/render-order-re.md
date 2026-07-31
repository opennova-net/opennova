# Draw order / batching — reverse-engineering record

The runtime path from `Render_SubmitEntity` to sorted, ordered draws: the four
batch queues, the sort keys, the render-state stack, the technique-class
selection, and the frame's pass sequence, witnessed in retail `Jointops.exe`
(imagebase `0x400000`, IDB `Jointops.exe.kong.i64`). Implementing code:
`libs/renderer` (`render_order`, this slice's port; `material_classify` /
`object_shader_template` from REN-2), `godot/engine/object/nova_object_model.gd`
+ `nova_object_shader_cache.cpp` (ladder application),
`godot/engine/environment/{nova_celestial,nova_water}.gd` (the generalized
priority ladder). Landed by maturity REN-3
([maturity-program.md](../maturity-program.md); standing rules
[ADR 0023](../adr/0023-render-visual-parity.md) — the queue machinery is
witness-source, never a port target; the ORDERING SEMANTICS are the port). The
T1 instrument (`tests/renderer/state_vectors_test.cpp`) pins the sort-key and
pass-class functions in this record.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Transparent ordering ladder (sky → far-water-side → water → camera-side → glow overlays) | MATCHING (ported this slice) | frame bracket `[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`, queue split `[orig: collect_render_objects_for_batch @ 0x5d8f20]`; `renderer_render_order` ctest + `renderer_state_vectors` section 3; per-strip vs per-object residual = D-RORD-3 |
| Sort keys (opaque composite key; transparent `~float_bits` back-to-front) | MATCHING (key semantics ported as pure functions) | `[orig: @ 0x5d928e..0x5d92c8; @ 0x5d931c..0x5d9326]`; `renderer_state_vectors` sort-key vectors; the two original key quirks are D-RORD-6 (permanent candidates) |
| Technique-class selection (submit flags + state-stack defaults → 6 classes) | MATCHING (selection fn ported; class BEHAVIORS ride REN-4) | `[orig: @ 0x5d90d7..0x5d9145; @ 0x5d95c0..0x5d961f]`; `renderer_state_vectors` pass-class vectors; burns down part of D-RMAT-6 |
| Batch queue machinery (17-DWORD entries, CDynList68, per-frame quicksort) | witnessed / not a port target | ADR 0023: device-era artifact; the reimpl renderer owns its queues — semantics captured in the rows above |
| Opaque state-sort (coarse depth slabs → effect index → fine depth, alpha-tested last) | witnessed / reimpl-internal equivalent | `[orig: RenderBatch_QuickSort @ 0x5d8b40]` unsigned-ascending + key layout below; Godot's opaque pass sorts front-to-back with its own state batching — same intent, D-RORD-2 permanent candidate |
| Frame pass sequence (shadow slots → viewmodel → sky → world → overlays → bloom) | witnessed (confirm-only) | `[orig: Render_ProcessMainSceneFrame @ 0x5ca0f0; Terrain_RenderSceneWithReflection @ 0x5c93a0]`; correspondence row |
| Viewmodel pass (near-Z 0.05 + viewport depth [0, 0.1], drawn FIRST, own flush) | witnessed / reimpl visible equivalent ported | dedicated shared-world SubViewport at weapon `renderfov`, composited after the finished world so it occludes later global particles like retail's compressed depth band; `[orig: Player_RenderFirstPersonViewModel @ 0x4ded60; Render_SwapProjectionNearZ @ 0x58a8f0; Render_SetViewportDepth01 @ 0x58a7b0]`; D-RORD-4 |
| EffectWorld particle ordering | BOUNDED deterministic port | `ParticleFrameCompiler` orders emitter AABB centers back-to-front with source-index ties, then particles within each emitter by depth/source index, before adjacent state runs. `NovaParticleCompositorEffect` draws every command sequentially with depth test/no write, so reimpl surface/material sorting cannot reorder packet commands; exact equivalence to retail's recursive alternating-axis/depth-bin order remains open `[orig: CParticleManager_RecursiveSortAndRender @0x5ec980; CParticleManager_RenderBatch @0x5e9890]` |
| EffectWorld pass placement around water | bounded reimpl mapping | retail invokes particle pass A before water and pass B after camera-side transparents; the reimpl currently issues one main-camera POST_TRANSPARENT compositor pass after the world transparents (D-RORD-7) |
| Glow/envmap duplicate queue (Q3) + bloom flush | witnessed / deferred | `[orig: @ 0x5d93b5..0x5d9447; FrameFX_RenderBloomPass @ 0x582940]`; D-RORD-5 |

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
| `0x10000000` | repeat-draw marker: dual-LOD entities draw far LOD then near LOD with this set (`@ 0x5c8020`), and `BoneCallback_org0_World @ 0x4e3940` skips the held-weapon + mounted-child overlay draws under it (world §13) — one entity, N draws, one-shot children drawn once |

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
   `{x,y,w,h,MinZ 0, MaxZ 0.1}`), per-weapon FOV, a state-stack push carrying
   the character's effect scale, submits with flags 0x80 when armed, pops,
   and flushes mode 0 — the compressed depth band keeps the world from ever
   overlapping the gun. `[orig: Player_RenderFirstPersonViewModel @ 0x4ded60]`
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
   - underwater murk scissor overlay (camera below water only); skybox sun
     glow last (modulator swapped around it) — the lens-glare overlay.
   With reflection enabled, each entity wave adds a mirrored sub-pass +
   flush(1) under mirrored lighting (`CTerrainRenderer_BuildLightingShaderConstants
   @ 0x5c8090` arg 1), using the mirror matrix/CLIP machinery above and the
   mirror-winding byte — the full reflection spec is env #30 (REN-6).
6. Player shadow/scar dispatch (`Render_DispatchShadowByType @ 0x584440` —
   the Shadow_/Scar_ family; out of REN port scope), radar/scope overlays,
   HUD (`HUD_RenderAllOverlays @ 0x5a8070`, mode-0 flushes for 3D HUD
   elements), fades, tips.
7. `FrameFX_RenderBloomPass` (flush 4 = Q3), present,
   `RenderBatchCtx_EndFrameStats`.

**Dead variants** (zero live callers; never order-witness from them):
`render_skybox_layers @ 0x5ac230`, `render_sun_lens_flare @ 0x5ad490`,
`render_foliage_at_camera @ 0x5ac100` (the env record's catalog).

## The ported ordering semantics (this slice)

The reimpl keeps its own queues (ADR 0023); what ports is the ORDER as data +
pure functions in `libs/renderer/render_order.{h,cpp}`:

- the transparent priority ladder (sky dome < celestial bodies < glare <
  far-water-side world alpha < water surface < camera-side world alpha <
  weather/particle overlays < glow), applied as Godot `render_priority`
  rungs — `nova_celestial.gd`'s local ladder re-derives from it,
  `nova_water.gd` takes the water rung, and `nova_object_model.gd` assigns
  blended object materials their water-side rung;
- `opaque_sort_key()` / `transparent_sort_key()` / `transparent_queue_for()`
  / `technique_class_for_submit()` — the witnessed key and class semantics,
  T1-pinned (`renderer_state_vectors` section 3) so future port slices
  (REN-4's class behaviors, REN-6's leftovers) converge against them.

## Divergence catalog

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RORD-1 | No global transparent ordering: water, world alpha, and weather all at priority 0 (one depth-sorted queue); the celestial ladder local to `nova_celestial.gd` | fixed pass bracket: sky → far-water-side alpha → water → camera-side alpha → overlays → glow (`[orig: @ 0x5c93a0]`) | FIXED (this slice: the ladder in `libs/renderer/render_order`, applied at celestial/water/object-model sites) |
| D-RORD-2 | Reimpl-internal opaque ordering (Godot front-to-back + its own state batching) | per-frame CPU quicksort by the composite key (alpha-test bit → 256-unit depth slabs → effect index → fine depth) (`[orig: @ 0x5d8b40; @ 0x5d928e]`) | PERMANENT-candidate (class C): same intent, device-era mechanism; key semantics preserved as T1-pinned functions |
| D-RORD-3 | Water-side rung assigned per OBJECT (model origin vs water height, at rebuild / `refresh_render_order()`) | per STRIP, per frame (strip center height `[orig: @ 0x5d932e..0x5d9354]`) | OPEN (partial) — straddling or water-crossing models can mis-bin strips; revisit if a T2/T3 scene shows it |
| D-RORD-4 | Viewmodel is a camera-tracked node with no depth treatment (clips into near walls) | drawn FIRST with near-Z 0.05 + viewport depth range [0, 0.1], own mode-0 flush (`[orig: @ 0x4ded60; @ 0x58a7b0]`) | RESOLVED — ported 2026-07-09: dedicated shared-world SubViewport composite at the weapon `renderfov` (h→v via aspect, near 0.05) over the finished frame, the depth window's visible equivalent; per-weapon def plumbing landed same day (net-re §5.40 fifth pass) |
| D-RORD-5 | No glow/envmap duplicate pass | strips with effect capability 0x10000000 get a back-to-front Q3 copy (GLOW class when a GLOW block exists), flushed in the bloom pass (`[orig: @ 0x5d93b5; @ 0x582a54]`) | WITNESSED-READY-DEFERRED — REN-4 landed the capability semantics (the 0x10000000 dialects resolved = glow-capable; `is_glow_capable` classification + the corrected FFP_GLASS row; the GLOW technique CONTENT witnessed — LUM copies the NORMAL pass, glass swaps to the TexCubeRotSpecular sun-glint cube). Residual = the reimpl bloom wiring (selective glow over the glow-capable set) + the specular cube source (`generate_cubemap_lighting @ 0x685bb0`, REN-5); FrameFX bloom itself stays out of REN scope |
| D-RORD-6 | Not reproduced | two original key quirks: opaque key bits 15+ carry residual stack garbage (`@ 0x5d92b9`), and the transparent key lags one strip within a render object (`@ 0x5d9326` vs the `fst @ 0x5d9347` overwrite) | PERMANENT-candidates (original-bug/garbage class): reproducing either manufactures garbage (ADR 0022) |
| D-RORD-7 | One main-camera POST_TRANSPARENT EffectWorld particle draw, after water and both transparent sides | two calls to the global particle manager: pass A between far-side transparents and water, pass B after camera-side transparents (`[orig: @0x5c93a0; EffectWorld_RenderParticlePass @0x5f7240]`) | OPEN (bounded ordering/pass-placement residual) — packet command preservation, blend state, scene-color capture, and viewmodel occlusion match; exact recursive sort equivalence remains unproven, and the pass should split only if a water-intersection parity scene demonstrates the visible need |

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
| 0x843580 / 0x843780 | dword_843580 / dword_843780 | g_RenderStateStack / g_RenderStateStackTop | the 16-byte-entry state stack + top index |
| 0x8437C4 / 0x843784 | flt_8437C4 / unk_843784 | g_WaterSplitHeightFloat / g_ActiveMirrorClipMatrix | queue-split threshold; active mirror matrix |
| 0x2980514 / 0x2980518 | dword_2980514 / flt_2980518 | g_WaterMirrorActive / g_WaterMirrorMatrix | reflection-pass machinery (env #30) |
| 0x8409DC / 0x8409D8 | flt_8409DC / flt_8409D8 | g_ProjectionNearZ / g_ProjectionFarZ | D3DX projection args |
| 0x2C25CD8 | dword_2C25CD8 | g_EffectWorld | the particle/effect world singleton |
| 0x31BC918 | dword_31BC918 | g_WaterActive | gates water render + reflection prerender |
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
