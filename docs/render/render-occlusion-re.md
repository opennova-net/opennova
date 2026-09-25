# Rendering occlusion / blink-box visibility — reverse-engineering record

Engine-research record (2026-07-16, port pass 2026-07-17) for the
blink-box-driven visibility system: the per-frame building section masks, the
portal traversal, the occluder culling pass, the frame-level indoor gates, and
the occlusion model data they all consume. Binary: retail **Jointops.exe**
(IDB `Jointops.exe.kong.i64`, imagebase 0x400000). All addresses below are
that binary's. The consumer engine is **PORTED** (2026-07-17):
`engine/runtime/world/occlusion.{h,cpp}` (`OcclusionWorld` + the render-float math),
the `CollisionWorld` camera blink query, and the reimpl wiring
(`Simulation::run_occlusion_frame`, `OcclusionFrame::apply_frame (godot/src/world/occlusion_frame.cpp)`,
the placer de-batch + `ObjectModel.set_section_visibility_mask`); ctest
`occlusion` + GUT `game_world_test` / `object_model_section_mask_test`
cover it. The *producer* side (blink volume queries, the indoors bit,
per-entity blink hits) is §15 of
[world-wac-ai-re.md](../world/world-wac-ai-re.md); this record is the
*consumer* side that §15 deferred as "REN-scope follow-ups". Sound occlusion is
witnessed here too (its port target is `godot/src/world` audio; the catalog
entry stays [lwf-dbf-sound-re.md](../audio/lwf-dbf-sound-re.md) D-SND-7).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Occlusion model data (`OVRT`/`OPLN`/`OFAC`/`OOBJ` chunks → 60 B runtime records) | **PORTED** (2026-07-17: the 3DI3-side promotion `ThreediIROcclusion` → `world::OcclusionModel`; the GPM-path runtime is deliberately unsupported — project decision, no GP runtime/ONED support) | `[orig: load_occlusion_model_data @ 0x5b4a00]`, caller `[orig: ThreediGp_LoadFromFile @ 0x5b5c37]`; tag immediates witnessed in disasm; copy loops re-derived from disasm (OVRT/OPLN identity, OFAC keeps disk field order, OOBJ 36→60 with sequential slice pointers) |
| Mission-start portal init (register + weld + per-building flags) | **PORTED** (2026-07-17, `OcclusionWorld::init_mission` + `Simulation::occlusion_init_mission`; the weld's type-5 rewrite mutates the SHARED per-graphic record array like retail's model cache). 2026-09-24: the register+weld arg is `Game_StartMission`'s first-start flag (D-OCC-4 closed, §2), and the mission-start blink stamp runs ahead of the init (`CollisionWorld::refresh_mission_start_blink`, D-OCC-15 closed) | `[orig: Terrain_InitBuildingPortals @ 0x5c7480 (tail @ 0x5c5860)]` from `[orig: Game_StartMission @ 0x525e11]`; thresholds 0.80000001f / −0.89999998f / 0.2 from decompile; `[orig: Entity_BuildProximityListsForPools12 @ 0x5240a0]` from `[orig: Game_StartMission @ 0x525898]`; ctest `occlusion` (mission-start stamp) |
| Camera blink query | **PORTED** (2026-07-17, `CollisionWorld::query_blink_boxes_at_point`) | `[orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]` |
| Per-frame section-mask build | **PORTED** (2026-07-17, `OcclusionWorld::build_section_masks`; masks keyed by pool-2 handle slot like `Pool_GetIndexFromPtr`). 2026-09-24: the RAW word (`section_mask`) and the def's forced bits (`forced_section_mask`) are kept apart; only the part draw reads `section_draw_mask` = raw OR forced (§5, D-OCC-9 closed) | `[orig: build_sector_visibility_masks @ 0x5c8610]` |
| Portal slot sort + clamp | **PORTED** (2026-09-24, `OcclusionWorld::sort_portal_slots`, "Port the retail occlusion collector, mask and sub-pixel rules"; closes D-OCC-10) | `[orig: Terrain_SortPortalSlotsByPriority @ 0x5c4410]`: stable descending sort on the slot priority (`cmp ecx,[eax+1Ch]; jge` `@ 0x5c4443`), then `g_PortalSlotCount = min(count, 14)` `@ 0x5c449f..0x5c44a4`; called `@ 0x5c9188`; ctest `occlusion` (slot clamp) |
| Portal traversal (section expansion, window/viewthru wedges) | **PORTED** (2026-07-17, `OcclusionWorld::traverse` + seeders, in the render float world — see §1a). 2026-09-24: a welded type-5 link also recurses under the def's recurse-windows bit (§3.2) | `[orig: render_visibility_portal_traversal @ 0x5c4ae0]` + the two seeders `@ 0x5c73d0 / @ 0x5c7330`; recursion args witnessed at `@ 0x5c5619/0x5c57fc` (backface-latch recursion re-uses the CALLER's planes with section 0); ctest `occlusion` (the type-5 recursion) |
| Occluder culling (render_TOC) | **PORTED** (2026-07-17, `OcclusionWorld::toc_occluded` + `build_occluder_planes`). 2026-09-24: the candidate radius is entity+0, and the render waves run render_TOC on every collected entity in no blink box (`render_wave_toc_occluded`, "Run render_TOC on collected entities outside blink boxes") | `[orig: test_sector_entity_occlusion @ 0x5c4610]`, planes `[orig: Terrain_BuildPortalOccluderPlanes @ 0x5c44c0 → build_clip_planes_from_collision @ 0x5b34e0]`; the 8-corner refinement's collision-AABB swizzle witnessed @ 0x5c4920; the wave calls `[orig: Terrain_RenderSectorEntities @ 0x5c7b92..0x5c7ba6]`, `[orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d8b..0x5c7da0]`; ctest `occlusion` (`test_render_wave_toc_skips_contained_entities`, TOC radius) |
| Entity collectors (blink-hits gate, person leg, model leg, sub-pixel floor) | **PORTED** (2026-09-24, `OcclusionWorld::entity_render_visible` / `person_render_visible` / `sphere_render_visible`; §3 step 5) | `[orig: collect_visible_entities_for_terrain @ 0x5c8c60]` person leg `@ 0x5c8df3..0x5c8e10`, floor `@ 0x5c8e5e`; the full-Euler sphere pose `[orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40]`; ctests `occlusion` (person leg, full Euler), `occlusion_camera` (focal) |
| Native visibility bound-sphere arithmetic | **PORTED** (2026-09-13, closes D-OCC-16) | `OcclusionWorld::bound_sphere_fixed` takes the positive-side half on odd Q16 widths (`max - midpoint`, with the +-0x40000000 unset-bound clamps) and multiplies center and radius by the entity's nonzero Q16 scale with the +0x8000 rule `[orig: Entity_ComputeBoundingSphere @ 0x5C69A0, @ 0x5C6A02..0x5C6B52]`; the static-batch, TOC and per-entity visibility callers pass `Entity::uniform_scale_q16`. `occlusion_test` pins radius 7 for `(-3,-2,10)..(4,7,15)`, the 1.5x scaled form and the sentinel bounds; the render RLOD projection shares the arithmetic (D-RORD-12). |
| Entity-vs-terrain three-ray occlusion latch | **PORTED** (2026-07-17, `OcclusionWorld::three_rays_clear` + the `Entity.occlusion_latch` byte). 2026-09-24: the rays start 1 u above the eye, and the re-arm jitter draws the world's shared `PRNG_Next16_C` stream (`World::prng16_c_state`, bound by `OcclusionWorld::bind_focal_wind_random`), closing the D-OCC-15 stream facet | `[orig: terrain_occlusion_check_three_rays @ 0x610ed0]` + the three collectors below; ctest `occlusion` (three-ray start) |
| Frame-level blink gates (terrain / sky bracket / water / foliage) | **PORTED** (2026-07-16, `OcclusionFrame::apply_blink_gates`; 2026-07-17: the `g_BlinkWaterVisible` straddle/window-latch override + the `Bms_AttribFlags & 0x10` force-indoors landed in `OcclusionFrame::apply_frame`). 2026-09-24 ("Split the dome's cloud pass after the bodies and port the sky pass gates"): per-pass sky gates (`renderer::scene_pass_gates` -> `OcclusionFrame::apply_scene_pass_gates`); the beauty frame has no indoor black clear, the black clear is the mirror's (§4; D-OCC-1 closed) | gates at `@ 0x5c1342..0x5c1353 / @ 0x5d0570 / @ 0x5ca197..0x5ca19f / @ 0x5ca1a3..0x5ca1bd / @ 0x5c93cb / @ 0x5c95bf / @ 0x5c9665`; ctest `renderer_render_order` (the gate law), `environment_state`; GUT `game_world_test.gd` blink-gate + occlusion-frame cases |
| Draw-side mask consumption (hidden-section mask, two-pass open buildings, per-light section masks) | **PORTED** (2026-07-17, visibility data only: the mask drives `ObjectModel` per-part visibility on de-batched buildings; the two-pass draw order / per-light scoping are renderer-specific legs, D-OCC-13). 2026-09-24: the part draw ORs the def's forced bits over the raw mask (D-OCC-9 closed), a part hides on the destroyed mask OR the occlusion verdict, and the water mirror's per-draw CLIP arming is ported (§5) | `[orig: Terrain_RenderSectorModels @ 0x5c5d30]`; `[orig: BoneCallback_bldg_World @ 0x4e22cf..0x4e22e2]`; ctests `occlusion` (forced bits split), `doors` (retail Iblock01 forced mask); GUT `object_model_section_mask_test` |
| Conservative device occluders | **PORTED (2026-08-29)** — opted-in mission buildings convert their eligible OOBJ face sets (record type and slice validity are the only gates; the type-0/1 authoring meaning stays unwitnessed, D-OCC-3, so the sets are not proven closed) into section-owned `ArrayOccluder3D`/`OccluderInstance3D` children. Portal, window, open, welded, and malformed record-local slices remain exclusively under the retail section/portal pass; its section mask also gates each generated occluder. Godot occlusion culling is a conservative second layer, not a replacement for the retail visibility owner; the world owns the switch (no model touches the viewport) and keeps it OFF by default: measured 2026-08-30 through the typed `occlusion_culling` debug row (device row, `GameWorld.set_occlusion_culling_enabled`; 1600x900, Ryzen 7735HS iGPU, `perf_mission_rows` medians of p50 over two runs, the missions placing 19 / 26 buildings with authored occluders), the occluder pass cost 0.64 / 0.59 ms of `render_root_cpu` (frame 14.47 -> 13.55 ms on 00TRa, 14.50 -> 13.68 ms on CP01; `draw` 3.92 -> 3.19 / 3.27 -> 2.60 ms) and culled nothing at the spawn poses (root draw calls 487 -> 489 / 416 -> 416, primitives 870797 -> 871141 / 1109933 -> 1109933), because the retail section verdict already hides what the OOBJ faces would. The row switches the pass on live for an RD-backed viewport (`game_render_diagnostics` reports the placed occluder count as `mission_placement.authored_occluder_models`, so a toggle over a mission without occluders is visibly a no-op); a load and an unload re-apply the default. Master had the consumer off, so this closes the 0.6 ms of the PR's `render_root_cpu` gap that was attributed to it. The `render_root_cpu` that remains above master after it (+0.61 / +0.51 / +0.35 ms on 00TRa / CP01 / CP19, 2026-08-30 A/B, n=3) is not the population count: one population per (graphic, policy, level, submesh) instead of the bins (448 vs 835 populations on 00TRa) left it unchanged (1.83 vs 1.79 ms). It is the focused Q3 capture/blur/composite pass that master ran as a separate viewport with its own `render_q3_cpu` slot (1.17 / 0.95 / 0.49 ms) and that now executes as a compositor callback inside the root viewport's measured span; the render server's CPU total over root + Q3 + water + slot is 0.59 / 0.52 / 0.17 ms BELOW master. A lane on that row toggles the FrameFx terminal effect first, not the water viewport | `renderer::build_authored_occluder_sections`; `ObjectModel.set_authored_occluders_enabled` / `get_authored_occluder_count`; `GameWorld._apply_occlusion_culling_policy` (after `place()`, and in `unload()`), `GameWorld.set_occlusion_culling_enabled` / `is_occlusion_culling_enabled` / `get_authored_occluder_model_count`, `DebugControls` row `occlusion_culling` |
| Sound occlusion (distance inflation + LOS legs) | **PORTED** (2026-07-16, closes D-SND-7; residue D-SND-9) | `[orig: Sound_ApplyOcclusionDistance @ 0x529970]` + callees; ported as `CollisionWorld::sound_occlusion_inflate` + `terrain_raycast_los_clear`; `collision` + `terrain_raycast` ctests; see [lwf-dbf-sound-re.md](../audio/lwf-dbf-sound-re.md) |
| BMS `blink_parent`/`blink_group` (record bytes 84–87) | runtime-unconsumed (probable) | not read by `[orig: Entity_SpawnFromBMSRecord @ 0x40e9f0]`; no other consumer found; matches the net-RE parsed-but-unconsumed note |

## 1. The occlusion model data

GP-era (GPM) models carry a dedicated occlusion/portal model in four chunks,
loaded by `[orig: load_occlusion_model_data @ 0x5b4a00]` (sole caller
`[orig: ThreediGp_LoadFromFile @ 0x5b5c37]`) into one `"GPM Occ"` arena:

| Chunk | Disk record | Runtime | Content |
|---|---|---|---|
| `OVRT` | 12 B | 12 B | float3 vertex positions |
| `OPLN` | 16 B | 16 B | float4 planes (normal.xyz + d) |
| `OFAC` | 12 B | 12 B (reordered) | faces: vertex index bytes + plane index byte + 3 edge words (bit 0x8000 = winding) |
| `OOBJ` | 36 B | 60 B | per-portal-face objects (layout below) |

The tags were pinned from the push immediates `@ 0x5b4a0c/0x5b4a27/0x5b4a46/0x5b4a60`
(`0x5452564F`/`0x4E4C504F`/`0x4341464F`/`0x4A424F4F`); the decompiler's
auto-comment names (OVRP/ONRM/OCIT) are wrong. This is the same chunk family
`engine/formats/threedi`'s 3DI3 reader already parses as
`occlusion_vertices/planes/faces/objects` — the 3DI3-side loader in Jointops
was not located this session (open item D-OCC-7). Copy-loop details re-derived
from the disasm (port pass): OVRT/OPLN copy verbatim; OFAC keeps the disk field
order (`u8 v0,v1,v2, u8 planeIdx, u16 edge[3]`, bytes 10-11 uninitialized pad);
OOBJ 36 B disk (`type/secA/secB/pad, pos f3 @+4, radius @+16, SLOT PRIORITY
WEIGHT f32 @+20, vert/plane/face counts @+24/+28/+32`) → the 60 B runtime shape below with
per-object slice pointers advancing sequentially. The disk +20 float is the
portal-slot priority weight (runtime +0x2C): the slot collector folds it into the
slot's +8 sort key, and the slot sort is that key's only reader (§3 step 2).
`engine/formats/threedi`'s former `unk1` field, briefly named `glow_scale`, is
`slot_priority_scale` since 2026-09-24 (zero in all 806 JO OOBJ records; the
earlier "window-glow scale" reading is refuted, D-OCC-13). The loader stores
count/array through a `this` alias 4 bytes below the model consumers read
(loader writes +0xE0/+0xE4; every consumer reads model +0xDC/+0xE0).

**The 60 B runtime record** (the "portal face"; runtime model `+0xDC` = count,
`+0xE0` = array):

```
+0   u8   type: 0/1 = occluder slot (1 = "open"), 2 = exterior window,
          3 = interior portal, 5 = welded cross-building link (runtime-made)
+1   u8   section A   (COBJ section ordinal; 0 = exterior)
+2   u8   section B
+4   f32[3] position (model space)   +0x10 f32 radius
+0x14 i32 vertex count    +0x18 ptr → OVRT slice
+0x20 ptr → OPLN slice    +0x24 i32 face count   +0x28 ptr → OFAC slice
+0x2C f32 slot priority weight  (+0x30.. spare)
```

Section ordinals are the §15 COBJ section indices — the same values the blink
query packs into hit slots (`(section & 0x1F) << 12 | pool2 << 20` — the
section LOOP INDEX; the pre-incremented type-8 ordinal counter only feeds the
`< 16` pack cap `[orig: @ 0x4af294-0x4af2bb]`), and the same bit indices in
the render mask below. Ordinal 0 is the exterior: interior blink volumes live
in nonzero COBJ sections, so a packed hit's low word is nonzero for real
interiors. The mask builder resolves a slot's ENTITY only when that slot's
low word is nonzero `[orig: @ 0x5c8840-0x5c88bf]`, but the camera-inside
branch itself tests hit slot 0's WHOLE dword `[orig: the hit_results branch
@ 0x5c86c9-0x5c86d5]` — a section-0 hit (packed = pool2 << 20) enters the
inside branch with a null slot entity.

### 1a. The render float world (port-critical)

Every occlusion float test runs in the engine's render float space:
`(X, Y, Z) = (−mission_y, mission_z, mission_x) / 65536`
`[orig: Math_FixedPointToFloat3_YNegated @ 0x611210]` — a det = −1 mapping of
mission axes, so cross-product handedness differs from mission space; the
authored OFAC winding bits and every wedge cross order are calibrated to it,
and the port works natively in it (hosts convert at the boundary; Godot space
is this space with X/Z swapped). The entity pose matrix
`[orig: Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200]` is row-vector
convention (`p' = p·M`, translation in the last row), composed
roll·pitch·yaw about the float Z/X/Y axes, each factor SKIPPED when its BAM
is exactly 0, with per-axis quantized trig:
`c = float(ftol(cos(θ)·2²²))/2²²`, `s = float(ftol(sin(θ)·−2²²))/2²²` — the
sin sign is baked into the constant (`dbl_7C57B0 = −2²²`), and the angle
scale `dbl_7C3608 = 0x3E19222D9890E4A8` (1.4629627251502471e-9) is NOT exactly 2π/2³²
(about 30.5 ppm above; the port carries the exact bits). The TOC corner refinement's
collision-AABB swizzle (`X = −y, Y = z, Z = x` of the mission-axis bounds
`[orig: @ 0x5c4920-0x5c49c1]`) and `Entity_ComputeBoundingSphere @ 0x5c69a0`
(center = AABB midpoints, radius = min(√Σhalf², 0x7FFF0000f)) are consistent
witnesses of the same frame. The fixed 3x4 transform family the collectors
use (`Math_FixedPointTransformPoint22 @ 0x615810`) rotates >>22 with a
+0x200000 rounding bias AND translates (rows `[r r r t]`) — an earlier
collision-record note calling it rotate-only was imprecise.

## 2. Mission-start portal init

`[orig: Terrain_InitBuildingPortals @ 0x5c7480, tail @ 0x5c5860]`, called
from `[orig: Game_StartMission @ 0x525e11]`. Its arg (pushed as `ebp`) is
`[esp+eventId]` loaded `[orig: @ 0x52540a]`, which holds
(`Game_StartMission`'s own argument == 0) (`setz al` `[orig: @ 0x524385]`,
stored `[orig: @ 0x5243a6]`). The first start calls `Game_StartMission(0)`
(the push `[orig: @ 0x526375]` before the call `@ 0x526377`), so the portal
arg is 1 and register + weld run; `Game_RestartRoundSP` calls it with 1
(`[orig: @ 0x5263d9]`), so the portal arg is 0 and a round restart skips
register + weld, keeping the shared retyped type-5 records and the weld list. The flag tail always runs (D-OCC-4 closed 2026-09-24). Ahead of it the
mission-start blink stamp `[orig: Entity_BuildProximityListsForPools12
@ 0x5240a0]` (from `[orig: Game_StartMission @ 0x525898]`, after
`Entity_InitAllFromModels` built the static table) refreshes the blink hits of
every pool-1 entity, then every non-building pool-2 entity (the def-type-5
skip `[orig: @ 0x5240f3]`); ported 2026-09-24 as
`CollisionWorld::refresh_mission_start_blink`, run by
`MissionKernel::occlusion_init_mission` before `OcclusionWorld::init_mission`.

1. arg ≠ 0 (first start): **register** every building's type-2 faces —
   `[orig: Terrain_RegisterExteriorPortalFaces @ 0x5c5b80]` walks the static
   prox building prefix and appends 44 B entries (entity, model, record array,
   face index, world position, world plane normal via `OPLN[face.planeIdx]`,
   flag bit 0 = **itemDef attrib bit 6**, the "weldable" def flag).
2. **Weld** — `[orig: Terrain_WeldOppositePortalFaces @ 0x5c5910]`: pairs
   entries of *different* entities with distance ≤ 0.8 u, world-normal dot
   ≤ −0.9, coplanarity ≤ 0.2 u, and ≥ 1 side weldable. Each pair writes two
   24 B records into `g_PortalWeldRecords @ 0x2967250`
   (`+0` own entity, `+4` own section A, `+8` own face idx, `+12` other
   entity, `+16` other face idx, `+20` other section; count
   `g_PortalWeldCount @ 0x2967248`) and rewrites **both** faces' type byte to
   5. This is how adjacent modular building pieces see into each other.
3. Always: stamp per-building entity flag bytes from the record types
   present: `+0x2CC` = has type-1, `+0x2CD` = has type-2 (windows),
   `+0x2CE` = has type-5, `+0x2CF` = 0 `[orig: @ 0x5c5899-0x5c58e8]`. Only
   `+0x2CD` has a witnessed reader (the outdoor mask rule); D-OCC-2 tracks
   the other two.

## 3. The per-frame pipeline

`[orig: Terrain_CollectVisibleEntities @ 0x5c9160]` (from
`[orig: Terrain_RenderWorldScene @ 0x5c93a0, call @ 0x5c94f0]`), in
order:

1. **Building batch + portal slots** —
   `[orig: collect_visible_sector_userpoints @ 0x5c6b60]`: walks the static
   building prefix; distance/def-flag/frustum culls (and the §3.4 latch —
   buildings run it too `@ 0x5c6cd9-0x5c6d0d`); appends visible buildings
   to `g_VisibleBuildingBatch @ 0x2985C90` (20 B stride: entity, depth,
   screen x/y, depth2, `+16` flag = "has ANY record with type ≥ 1" — set for
   window/portal/link records too, not only type 1; witnessed
   `@ 0x5c6dcd-0x5c6de1`, correcting this record's earlier "has a type-1
   record" reading — and only stamped within the 250 u slot range); collects
   records with type ∈ {0,1} within 250 u into the 128-cap slot arrays
   `g_PortalSlots @ 0x2983E88` (5-dword stride: entity, record ptr, priority
   key, viewthru plane ref, viewthru count; count
   `g_PortalSlotCount @ 0x298056C`); these are the frame's occluders. Slot
   eligibility per record: the viewport sphere clip plus an angular-size gate
   `radius²/dist² > 0.01` (collect within 10 radii); the slot's +8 priority
   key = `ftol(ratio · slotPriorityWeight · 2²⁴)`
   `[orig: @ 0x5c6e48-0x5c6eb1]`. Its only reader is the step-2 sort compare
   (xrefs to `0x2983E90`: the writer `@ 0x5c6eb1` and the compare); there is no
   window-glow renderer.
2. **Slot sort + clamp**: `[orig: Terrain_SortPortalSlotsByPriority @ 0x5c4410]`
   (the name is a misnomer; it does not sort by distance): a stable descending
   sort on the slot priority key (adjacent 20-byte swaps while
   `slot[j].key < slot[j+1].key`, `cmp ecx,[eax+1Ch]; jge` `[orig: @ 0x5c4443]`),
   then `g_PortalSlotCount = min(count, 14)` `[orig: @ 0x5c449f..0x5c44a4]`;
   called `@ 0x5c9188` between the collect and the occluder planes. JO's
   authored weights are all zero, so retail keeps the first 14 slots in
   collection order and drops the rest from every later slot consumer. Ported
   2026-09-24 (`OcclusionWorld::sort_portal_slots`).
3. **Occluder planes** — `[orig: Terrain_BuildPortalOccluderPlanes @ 0x5c44c0]`
   per slot calls `[orig: build_clip_planes_from_collision @ 0x5b34e0]`
   (thunk `@ 0x5b3ac0`): from the camera position, front/back-classifies the
   record's faces, cancels shared edges, and outputs the silhouette
   camera-edge planes + the facing face planes into
   `g_PortalOccluder{EdgePlanes,EdgePlaneCounts,FacePlanes,FacePlaneCounts,Entities}
   @ 0x2983D74/78/7C/80/70`.
4. **Section masks** — `[orig: build_sector_visibility_masks @ 0x5c8610]`
   (below).
5. **Entity collectors** — non-building statics
   `[orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20]`, then pools 0/1
   `[orig: collect_visible_entities_for_terrain @ 0x5c8c60]` ×2, then minimap
   slots and a 512-slot effects pool walk. The pool collector skips an entity
   with Flags & 1 (hidden / carried, `[orig: @ 0x5c8cef..0x5c8cf4]`), one
   without a render model pointer (entity+0x30, `[orig: @ 0x5c8cf6..0x5c8cff]`)
   and def type 5 (buildings draw only through the building batch,
   `[orig: @ 0x5c8dd7..0x5c8de0]`). Per entity:
   - **blink-hits gate**: an entity with a nonzero `blink_hits[4]` quad
     (entity `+464`, §15) is collected only if one of its packed
     (building, section) hits has its section bit set in the RAW
     `g_BuildingSectionVisMask` `[orig: @ 0x5c7028-0x5c708a / @ 0x5c8d70-0x5c8dd1]` —
     contents of an interior render only while that interior is render-active;
   - **view sphere, model leg**: the collision-bound sphere centre
     (entity+0x1FC) placed through the FULL Euler pose
     (`[orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40]` from
     `@ 0x5c6c6a`, `@ 0x5c70b1`, `@ 0x5c8f25`), view-culled;
   - **view sphere, person leg** (def type 3): the entity position with the
     entity+0 bound radius (`mov esi,[edi]` `[orig: @ 0x5c8df3]`), or under
     Flags 0x20 the special item-185 (parachute) model's radius
     (`gItemDefs[g_ParachuteItemIndex]+0xF0` -> model +0x14,
     `[orig: @ 0x5c8df7..0x5c8e10]`); a sphere that projects to at most
     0.75 px is dropped (`cmp dword_A784F0,0C000h; jle` `[orig: @ 0x5c8e5e]`)
     BEFORE the latch is touched. The projected radius scales by the viewport
     focal `viewport+0x40 = ftol(width · 0.5 / tan(fov_h / 2) + 0.5)`, the fov
     at `viewport+0x3C` being Q16 degrees (`dbl_7C3620` = π/180/65536)
     `[orig: Viewport_BuildProjectionMatrix @ 0x410fb0, @ 0x410fe1..0x410ff7,
     stored @ 0x4110e1; read by Viewport_TransformAndClipPoint @ 0x4117b0]`;
   - **three-ray terrain occlusion latch** (outdoors only, below);
   - **render-wave render_TOC**: both render waves test every collected entity
     whose first blink hit (entity+0x1D0 = +464, written by
     `Entity_BuildProximityList @ 0x4b3dc0`, the store `@ 0x4b406b`) is zero against the frame's
     occluder slots and window groups, and skip the test for a contained
     entity, which draws on the blink-hits gate alone
     (`cmp dword ptr [eax+1D0h],0; jnz` around the call
     `[orig: Terrain_RenderSectorEntities @ 0x5c7b92..0x5c7ba6]`,
     `[orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d8b..0x5c7da0]`, the
     BySide call `@ 0x5c7d96`).

### 3.1 The section-mask build

`g_BuildingSectionVisMask @ 0x297F250` — 1200 dwords, one per pool-2 static
(the §15 prox-table cap): bit N = COBJ section N renders this frame; bit 0 =
exterior; `0xFFFFFFF` = everything; bits 30/31 = slot markers (below).
`[orig: build_sector_visibility_masks @ 0x5c8610]` zeroes it, runs the camera
blink query, then:

- **Camera blink query** — `[orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]`:
  clears the §15 blink globals, walks the building prefix (axis + euclid
  broad phase at `buildingRadius + 0x8000`), runs the §15 point query
  (`Entity_TestCollisionSections @ 0x4aef90`, one point, radius `0x8000`) and
  returns the packed 4-slot hit set + count — the camera-position analog of
  the entity-side `refresh_blink`.
- **Camera inside ≥ 1 blink box**: per containing building,
  `mask |= [orig: Terrain_TraversePortalsFromSection @ 0x5c73d0]`(entity,
  hitSection) — the portal traversal seeded at the camera's section. Matched
  cross-links found during those traversals (tracked in
  `g_PortalLinkTrackList @ 0x29BEE54`, cap 32) then OR the *linked* buildings'
  masks via the same traversal from the linked section
  `[orig: @ 0x5c893f-0x5c89c1]`. Every other visible building: if the TOC
  test (below) passes, `mask = 0xFFFFFFF`, or — when its batch +16 flag says
  it has an open (type-1) portal — an outside-in traversal
  `[orig: Terrain_TraversePortalsFromExterior @ 0x5c7330]` computes which
  interior sections show through its windows and banks per-window
  **viewthru** wedges, patching the slot's viewthru ref/count
  `[orig: @ 0x5c8a43-0x5c8aab]`.
- **Camera outdoors** (`g_PortalExteriorVisible @ 0x29ACE20` = 1): buildings
  with an active slot get marker 0x80000000 + the outside-in traversal; all
  other non-occluded buildings get
  `mask = (+0x2CD windows flag) ? 0xFFFFFFF : 1` — **exterior-only unless the
  building has window portals** `[orig: @ 0x5c87d9-0x5c8825]`.
- `g_BlinkWaterVisible @ 0x29ACE40` = 1 when outdoors, when a
  camera-containing or weld-linked building straddles the water height, or
  when the traversal latched the exterior — the water-pass override consumed
  at `@ 0x5c93cb/0x5c95d2`. The two straddle tests are NOT the same form: a
  camera-containing building uses its collision header z extents
  (`[model+0xB0]+0x2C / +0x28`, `[orig: @ 0x5c8900..0x5c8922]`); a linked
  building uses its bound sphere (entity z + centre z (+0x204) ± radius
  (+0x208), `[orig: @ 0x5c8985..0x5c89a9]`). Ported 2026-09-24 with the
  sphere form on the link leg (ctest `occlusion`,
  `test_link_water_straddle_uses_bound_sphere`).

### 3.2 The portal traversal

`[orig: render_visibility_portal_traversal @ 0x5c4ae0]` — "render_VPT()" by
its own debug strings. Takes stack args `(currentSection, frustumPlanes,
planeCount)` (frameless function; Hex-Rays drops them — witnessed at the
seeders' call sites `@ 0x5c7448-0x5c745b` pushing
`(cameraSection, g_CameraFrustumPlanes5 @ 0xA7849C, 5)`). Per 60 B record:

- Section match: record must touch `currentSection` via bytes +1/+2
  (`currentSection == 0`: type-2 faces only — the outside-in mode).
- Backface: `dot(vertex0 − camera, plane normal) > 0` rejects, with one
  special case — a type-2 face of the *camera's own* section latches
  "exterior visible" (mask bit 0, `g_PortalExteriorPlaneSet @ 0x29ACE24`,
  and the face plane into `flt_29ACE28..3C` — consumed by the TOC early
  test) `[orig: @ 0x5c5748-0x5c57f2]`.
- Frustum-clip the face polygon against the caller's planes; fully-out skips.
- `mask |= 1 << farSection` `[orig: @ 0x5c4e31]` — the actual expansion.
- Build the **portal wedge**: shared-edge-cancelled boundary edges →
  camera-through-edge planes (winding via the 0x8000 index bit), plus the
  parent planes that clipped the polygon, plus the face plane (≤ 64 planes,
  "render_VPT() : too many planes generated!").
- Dispatch: camera-inside mode (`g_PortalCtxCameraInsideMode @ 0x29ACE10`):
  type 2 banks the wedge as a **window frustum**
  (`g_WindowFrustumPlanePtrs/Counts @ 0x29B4E44/48`, groups ≤ 512, plane
  arena ≤ 2048) and sets `g_PortalExteriorVisible`; type 5 looks up its weld
  record and pushes it onto the track list. Outside-in mode
  (`g_PortalCtxBankViewthru @ 0x29ACE18`): non-type-5 wedges bank as
  **viewthru** groups (`g_Viewthru* @ 0x29BDE4C/50, 0x29BEE4C/50`). Type 3
  (and type 2 in outside-in mode, or when the def's attrib bit 27
  `g_PortalCtxRecurseWindows @ 0x29ACE1C` is set) **recurses** with
  `(farSection, wedge planes)`; depth cap 10 `[orig: @ 0x5c4af4]`. In
  camera-inside mode BOTH the type-5 track leg (`[orig: @ 0x5c54b7..0x5c5519]`)
  and the window bank leg join the recurse-windows gate (`jmp loc_5C560C`
  `[orig: @ 0x5c5519 / @ 0x5c54ed]`, the gate `@ 0x5c560c`), so a def with
  attrib bit 27 also recurses through a welded link into its far section
  (`[orig: @ 0x5c5619]`). Ported 2026-09-24; the weld test that had pinned
  "no type-5 recursion" now pins the retail result.

### 3.3 The occluder pass (render_TOC)

`[orig: test_sector_entity_occlusion @ 0x5c4610]` — "render_TOC()". Candidate
building vs every slot occluder (skipping itself; a type-1 "open" slot only
occludes when its building carries marker bit 31): candidate center behind
all face planes → not occluded by this slot; otherwise fully inside the
silhouette wedge (center + radius, with an 8-corner collision-AABB refinement
when partially behind) → **occluded** — unless the candidate intersects the
slot's banked viewthru wedges (`[orig: Terrain_TestSphereInPlaneGroups
@ 0x5c4580]`). Camera-inside early rule: a candidate must intersect at least
one banked window frustum (or the latched exterior half-space) to be visible
at all `[orig: @ 0x5c4662-0x5c4721]`. An occluded candidate's batch entry is
zeroed. The candidate is read through its list row's entity pointer: position
entity+4 (`@ 0x5c4639`) and radius entity+0 (`fild` × 1/65536,
`[orig: @ 0x5c463e..0x5c4640]`, the `fmul` `@ 0x5c464f`), not a
collision-derived bound (ported 2026-09-24). The same test runs from the
building batch (the mask build) and from both entity render waves for every
collected entity in no blink box (§3 step 5).

### 3.4 The three-ray terrain occlusion latch

All three entity collectors run, **only while the local player is outdoors**
(`(~g_LocalPlayerBlinkFlags & 2) >> 1` `[orig: @ 0x5c6f56 / @ 0x5c6b92 /
@ 0x5c8c96]`), a per-entity visibility latch: byte `entity+342` counts down
per frame while latched visible; at 0 the entity is re-probed by
`[orig: terrain_occlusion_check_three_rays @ 0x610ed0]` — three heightfield
rays from the camera raised 1.0 u (`add edx, 10000h` `[orig: @ 0x610eef]`;
the IDB function comment's "z+0x4000" is wrong) to the bound-sphere top (z + radius) and the
two lateral silhouette points (± radius along one camera basis row, + radius/2
along another), early-out on the first clear ray
(`Terrain_RaycastHeightmapHiRes @ 0x60c760`, nonzero = clear). Pass →
visible + relatch for `(rand & 7) + 16` frames; fail → culled this frame and
re-probed next. `rand` is `PRNG_Next16_C @ 0x6131b0` on the process-wide stream
its other consumers share; the port draws the world's shared
`prng16_c_state` (`MissionKernel` binds it through
`OcclusionWorld::bind_focal_wind_random`). Indoors the probe is skipped and everything passes (an indoor
camera's terrain rays are meaningless). The reflection pass
(filter flag 0x2000000) bypasses the latch machinery entirely.

## 4. Frame-level blink gates

`g_LocalPlayerBlinkFlags @ 0x24C1934` accumulates `bvol.flags ^ 6` from the
§15 queries; mission start clears the letter bits:
`and g_LocalPlayerBlinkFlags, 0xFFFFFFC1` `[orig: Game_StartMission
@ 0x525c45]`. A mission attribute forces indoors every frame:
`Bms_AttribFlags & 0x10 → |= 2` `[orig: Render_ProcessMainSceneFrame
@ 0x5ca1c8-0x5ca1cd]`.

| Accum bit | Effect when set | Witness |
|---|---|---|
| 0x2 (indoors) | the water mirror's PolyTrn terrain pass and its sky pass skipped, and the mirror RTT cleared black (`@ 0x5c1597` is the reflection target's clear, not the beauty frame's); `NVG_RenderSceneToTarget` skips its own PolyTrn pass on the same letter | `[orig: render_main_scene @ 0x5c1342..0x5c1353 → @ 0x5c166b / @ 0x5c1597]`, `[orig: NVG_RenderSceneToTarget @ 0x5d0570]` |
| 0x2 | the main terrain sector pass skipped (`Terrain_RenderMainSectorPass @ 0x610ac0`: the plain lit sector batch, `Terrain_RenderSectorBatchLit @ 0x60c670`, then the drapes; it draws no sky) | `[orig: Render_ProcessMainSceneFrame @ 0x5ca197..0x5ca19f → the skip @ 0x5ca84f over the call @ 0x5ca867]` |
| 0x2 | far-foliage passes skipped | `[orig: Terrain_RenderWorldScene @ 0x5c95bf / @ 0x5c9665 → Foliage_RenderDetailPatchesPass @ 0x60c6d0]` |
| 0x2 | three-ray entity occlusion disabled (everything passes) | §3.4 |
| 0x2 | outdoors flag forwarded into `Render_TerrainScene @ 0x610c80` (arg use unwitnessed, D-OCC-6) and `Render_RadarCompassOverlay @ 0x5ca949` | `[orig: @ 0x5ca504 / @ 0x5ca949]` |
| 0x4 | the whole main sky bracket skipped: the sky dome (`SkyDome_RenderWithSkyfog` → `render_skybox`, `@ 0x5ca81a`), the sun/moon discs it draws and the `Render_ResetFixedFunctionState` fixed-function reset (D-OCC-1). The bracket also needs the eye strictly above the water: sky drawn = !(accum & 4) && camZ > waterHeight | `[orig: Render_ProcessMainSceneFrame @ 0x5ca1a3..0x5ca1bd → @ 0x5ca7c4..0x5ca81a]` |
| 0x8 | both water passes skipped unless `g_BlinkWaterVisible` | `[orig: @ 0x5c93cb / @ 0x5c95d2-0x5c95ea]` |

The beauty frame is never cleared black: its clear is thermal ? 0x808080 :
camZ > waterHeight ? skyfog : the lit water colour
`[orig: Render_ProcessMainSceneFrame @ 0x5ca771..0x5ca792]` (the `jle`
`@ 0x5ca790` keeps the water colour at eye == waterHeight). The sun glow, the
water glint and the veil are drawn outside the sky bracket and are never
gated here. Reimpl (2026-09-24, "Split the dome's cloud pass after the bodies
and port the sky pass gates"): `renderer::scene_pass_gates`
(`engine/runtime/renderer/scene_pass_gates.h`, sky letter
`world::kBlinkSkyOffBit` = 0x4) → `OcclusionFrame::apply_scene_pass_gates`
applies the terrain gate, the beauty sky gate (dome + discs,
`u_beauty_pass_drawn`) and the mirror sky gate (`u_mirror_pass_drawn`) per
pass, re-deriving the waterline side every display frame;
`EnvironmentState::frame_clear_color_for(eye_above_water)` is the beauty
clear (ctest `renderer_render_order`, `environment_state`; GUT
`game_world_test.gd`).

Debug: `Debug_DrawEnvironmentValues @ 0x4ef1cc` prints the accum (overlay
only). Bits 0x10/0x20 have no witnessed frame consumer.

## 5. Draw-side mask consumption

`[orig: Terrain_RenderSectorModels @ 0x5c5d30]` per visible building:
`mask = g_BuildingSectionVisMask[pool2]`, then for each nonzero def byte the
part draw ORs `-1 << byte` over it (x86 `shl` masks the count by 31):
`itemDef+0x891` (`mov cl,[eax+891h]; test cl,cl; jz; or edx,-1; shl edx,cl`
`[orig: @ 0x5c5d7c..0x5c5d8d]`) and `itemDef+0x892`
(`[orig: @ 0x5c5d95..0x5c5da4]`). The two bytes are the def's forced-visible
section bases written by the items.def parser: +0x891 = `first_door` - 1
(or `rotor_parts`' second argument), +0x892 = `first_subobject` - 1 (or
`aux_parts`' third argument) (`[orig: ItemDef_ParseProperty
@ 0x49f7ba..0x49f7de, @ 0x49f9b0..0x49f9cc]` signed low-byte clamps for the
`first_*` keys; `@ 0x49ef5d..0x49efb6, @ 0x49eff3..0x49f04c` raw bytes for
the `*_parts` keys). The earlier "+2193/+2194 destruction bone map" naming
is a misnomer. The per-draw hidden mask is
`g_HiddenSectionMask @ 0xB7965C = ~(mask | forced)`. The forced bits exist
only in this draw: every other reader of the word reads it RAW (the
render_TOC bit-31 test `@ 0x5c4764`, the collector gates `@ 0x5c706d /
@ 0x5c8db4`, `Scar_RenderCache @ 0x5cd93d / @ 0x5cd993`,
`Terrain_IsBuildingSectionBitSet @ 0x5c6988`, the effect-group gate
`CEffectGroup_IsSectionVisible @ 0x5f6d5e`). A part then hides when bit (i & 31) of
entity+0x138 (the destroyed sections) OR `g_HiddenSectionMask` is set
`[orig: BoneCallback_bldg_World @ 0x4e22cf..0x4e22e2]` (the gnrc/Sway
callbacks read the same word, `@ 0x4e2965 / @ 0x4e2ba9`).
Buildings whose batch entry carries the open-portal flag draw **two passes** —
exterior only (all interior bits hidden), then interiors (bit 0 hidden) —
`[orig: @ 0x5c5e17-0x5c5e43]`. Per-light interior passes AND the mask with the
light entity's own `blink_hits` quad sections, so an interior light lights
only the sections it sits in `[orig: @ 0x5c5f5a-0x5c5fa7]`;
`Lighting_SetInteriorLightGroup @ 0x5a90e0` is set per building and cleared
after — corroborating §15's sampler note (`terrain_sector_compute_lighting
@ 0x5c7550`, confirm-only). In the reflection scene a building whose
Position.Z + the CMDL header bbox z-lo (`graphicModel(+0xB0)->(+0x28)`) lies
below the water height - 0.25 u (`wh - 0x4000`) is armed with the mirror
CLIP technique `[orig: @ 0x5c5e57..0x5c5e75]` (the clip matrix
`@ 0x5c5e75-0x5c5e9b`); an unarmed draw keeps its NORMAL technique in the
mirror. Ported 2026-09-24 ("Arm the water mirror's CLIP technique per draw,
as retail does": `env::water_mirror_clip_armed`, the entity waves' own arming
rule and the CLIP mechanics are in env-tod-re.md #30).
`g_BuildingSectionVisMask` is also read by the scar-cache renderer
(`Scar_RenderCache @ 0x5cd830` — ex `terrain_render_sector_userpoints`,
renamed 2026-08-21: it draws the per-entity bullet-scar ring, not window
glows; world-wac-ai-re.md §24.9), the bit-query helper
`[orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960]` (permissive
out-of-range; the owned-corona gate), and the effect-group section gate
`[orig: CEffectGroup_IsSectionVisible @ 0x5f6d10]` (witnessed 2026-09-24, closing D-OCC-5): a
group spawned by a tagged descriptor is stamped with the blink volumes that
contain its spawn position (`CEffectWorld_SpawnEmitterAtPosition
@ 0x5f6df0`: the owner tag at group+0 `@ 0x5f6efb`, the
`Entity_QueryBlinkBoxesAtPoint` call `@ 0x5f6f5a` into group+0x1C..0x28), and
every `CEffectGroup_AdvanceChildrenAndReap` (`@ 0x5e59cf..0x5e59d8`) rewrites
group+0x6C with it: visible when the tag is 0 (`@ 0x5f6d14`) or hit slot 0 is
empty (`@ 0x5f6d1f`), else when any hit's section bit is set in the RAW
`g_BuildingSectionVisMask[hit >> 20]` (`@ 0x5f6d5e`; no forced bits). A
hidden group draws on no particle pass (`CParticleGroup_RenderChildren
@ 0x5e5893..0x5e5897`). Ported as `particle::effect_section_gate_visible`
("Gate effect groups by the building sections their spawn point lies in";
the tag survey and the particle-side readers are in
[ptl-format-re.md](../particles/ptl-format-re.md)).

## 6. Sound occlusion (witness home: this section; catalog: D-SND-7)

`[orig: Sound_ApplyOcclusionDistance @ 0x529970]`, applied at
`[orig: Sound_Play3DPositional @ 0x527d95]` and
`[orig: SoundEmitter_UpdateAndMixTop8 @ 0x528659]` (both confirmed):

- `base = min(d/8, 10u)` (d = current effective distance, 16.16; clamp
  `@ 0x529982`).
- Target z += 0x2000 (0.125 u) for both rays; restored after.
- Ray 1 (offset 0): blocked → `base = 2·base + 5u` `[orig: @ 0x5299b6]`.
- Ray 2 (offset −0x8000: `Physics_CheckTerrainLineOfSight` subtracts the
  offset from both endpoint z's, so the segment runs 0.5 u higher): clear →
  `d += base`; blocked → `d += 2·base + 5u` `[orig: @ 0x5299e6]`.
- Net: both clear `+base₀`; exactly one blocked `+2·base₀+5u`; both blocked
  `+4·base₀+15u`. **One add total** — the inflation compounds; it is not
  per-ray additive.
- LOS = `[orig: Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130]`
  (six args; sound passes `allowAllTypes = 0` — witnessed pushes
  `@ 0x52999e/0x5299c3`): terrain leg
  `[orig: Physics_CheckTerrainLineOfSight @ 0x53b080]` — **skipped as clear
  when both entities are indoors** (Flags 0x800000) `[orig: @ 0x53b0a0]`;
  the no-entity path first requires both endpoints above the heightfield
  `[orig: @ 0x53b100]`; `Terrain_RaycastHeightmapHiRes @ 0x60c760` returns
  nonzero = clear. Entity leg
  `[orig: raycast_find_collision_entity @ 0x539a70]`: walks the listener's
  §15 proximity candidate slice and, with `allowAllTypes = 0`, **only
  def-type-5 (building-kind) candidates can block**, via
  `raycast_against_entity_pool @ 0x538720` per candidate.

## 7. BMS `blink_parent` / `blink_group`

The .mis/BMS record bytes 84–87 (`blink_parent_a/b`, `blink_group_a/b`) are
**not consumed by the JO runtime**: `[orig: Entity_SpawnFromBMSRecord
@ 0x40e9f0]` reads the surrounding bytes (72–81, words 26–34, 120, 153–155,
164–165) but never 84–87, and no other reader was found (bounded search;
matches the parsed-but-unconsumed note in
[novaworld-net-re.md](../net/novaworld-net-re.md) D-NET-151's investigation).
Editor-authored data with no runtime effect in JO — our `engine/runtime/mission` parse
keeps the fields for .mis round-trip fidelity only.

## 8. Divergence / open-item catalog (D-OCC)

D-OCC-1..8 opened as research-session witnesses and stay record-only (open
witness details, not port divergences). The 2026-07-17 port pass added the
reimpl-mapping divergences D-OCC-9..15; the 2026-08-30 post-merge tidy tabled
those in the ledger's "Render — occlusion" section and registered the four
deliberate ones (standing rule 2 — a divergence gets its ledger row at birth).
The 2026-09-24 rendering parity pass (§8a) closed D-OCC-1, -4 and -5
(witnessed) and D-OCC-9, -10, -14 and -15 (ported), narrowed D-OCC-12 and
D-OCC-13, and refuted D-OCC-13's window-glow facet.

| ID | Status | Summary |
|---|---|---|
| D-OCC-1 | CLOSED 2026-09-24 (witnessed; "Split the dome's cloud pass after the bodies and port the sky pass gates") | Blink accum bit 0x4 gates the whole main sky bracket: the dome, the sun/moon discs it draws and the `Render_ResetFixedFunctionState @ 0x58aa80` write (§4). The former "float-1.0 device state" is `Render_ResetFixedFunctionState` → `sub_677700(0, 1, &unk_7D8A70)` `[orig: @ 0x58aa9e]`, whose third argument the IDB types as a D3DMATRIX: the 64 bytes at `0x7D8A70` are the identity matrix. The 2026-09-24 reading saw only that stage-0 write: a stale NORET flag on `set_texture_stage_state` truncated the body at the call. The whole function (re-bounded 2026-09-25, `@ 0x58aa80..0x58abc2`) resets the fixed-function state before the dome: identity transforms on stages 0..1 (2..3 with caps 0x100), DIFFUSE/AMBIENT/EMISSIVEMATERIALSOURCE = MATERIAL, `D3DRS_AMBIENT` 0xFFFFFF, LIGHTING on, `SetMaterial` and the default pass `[orig: @ 0x58aab1..0x58abb9]`. The bracket also needs the eye strictly above the water `[orig: Render_ProcessMainSceneFrame @ 0x5ca1a3..0x5ca1bd → @ 0x5ca7c4..0x5ca81a]`. Ported as the beauty sky gate of `renderer::scene_pass_gates`. |
| D-OCC-2 | OPEN (witness detail) | Per-building flag bytes `+0x2CC` (has type-1) and `+0x2CE` (has type-5) are stamped at mission start but no reader was found (single-register modrm scan only; SIB-form reads unscanned). `+0x2CD` (has windows) is the witnessed outdoor-mask input. The port stamps all three (`OcclusionWorld::BuildingFlags`). |
| D-OCC-3 | OPEN (witness detail) | Record type 0 vs 1 distinction beyond witnessed uses: both collect as occluder slots; type 1 flags the building "open" (outside-in traversal + two-pass draw + occludes only when bit-31-marked, with viewthru exceptions); the authoring-side meaning (door vs window vs destroyed state, and what mutates the byte at runtime besides welding) is unwitnessed. The port's test fixtures use the derived side conventions (record normal points a→b; windows author A = interior, B = 0). |
| D-OCC-4 | CLOSED 2026-09-24 (witnessed) | `Terrain_InitBuildingPortals`'s register+weld half is gated on its arg; the `ebp` pushed at `Game_StartMission @ 0x525e11` is `[esp+eventId]` loaded `@ 0x52540a` = (`Game_StartMission`'s argument == 0) (`setz al` `@ 0x524385`, stored `@ 0x5243a6`). The first start calls `Game_StartMission(0)` (`@ 0x526375`, call `@ 0x526377`), so register + weld run; `Game_RestartRoundSP` passes 1 (`@ 0x5263d9`), so a round restart skips them and keeps the shared retyped type-5 records and the weld list; the flag tail `@ 0x5c5860` always runs (§2). The port's default `do_register_weld = true` is the first-start value; the port has no round-restart re-init of the portal state (see "Open after the 2026-09-24 pass"). |
| D-OCC-5 | CLOSED 2026-09-24 (witnessed) | `g_BuildingSectionVisMask` readers: ~~`terrain_render_sector_userpoints @ 0x5cd830` (window glows)~~ decompiled 2026-08-21 as `Scar_RenderCache @ 0x5cd830`, the bullet-scar ring renderer (the mask gates attached-entity scars by building section; world-wac-ai-re.md §24.9). 2026-09-24: `CEffectGroup_IsSectionVisible @ 0x5f6d10` is the effect-group section gate over the RAW word (§5; ported, "Gate effect groups by the building sections their spawn point lies in"); the render waves' interplay with the collect-time blink-hits gate is witnessed: a contained entity (first blink hit nonzero) skips the waves' render_TOC and draws on the collector gate alone, every other collected entity takes render_TOC (§3 step 5; ported, "Run render_TOC on collected entities outside blink boxes"). |
| D-OCC-6 | OPEN (witness detail) | `Render_TerrainScene @ 0x610c80` receives the outdoors flag as arg 0; its consumption inside (frameless-callee arg pattern) is unwitnessed. |
| D-OCC-7 | CLOSED-BY-DECISION (2026-07-17) | Only the GPM-path occlusion loader is witnessed; the 3DI3-path loader in Jointops was never located. The port promotes the 3DI3-parsed OCCL tables (identical disk family) into the witnessed 60 B runtime shape; the GPM/GP runtime path is deliberately unsupported (project decision: no `threedi_gp` runtime or ONED support). |
| D-OCC-8 | OPEN (suffix-consumer reconciliation) | Super OED Manual v1.1 officially defines BB plus `W`/`S`/`V` as preserving water/sky/voxels; exporter reconstruction also accepts `L`/`O` and clears bits from initial 0x3E. Runtime consumption is separately witnessed: bit 0x2 indoors, 0x4 the main sky (dome + bodies; D-OCC-1 closed 2026-09-24), 0x8 water suppress, 0x10/0x20 no frame consumer. Reconcile the author-facing sky/voxel split with those consumer bits; `L`/`O` expansions remain unverified. |
| D-OCC-9 | FIXED 2026-09-24 ("Port the retail occlusion collector, mask and sub-pixel rules") | The forced-visible def bytes (`itemDef+2193/+2194` = +0x891/+0x892, read `@0x5C5D7C` / `@0x5C5D95` in `Terrain_RenderSectorModels @0x5C5D30`) had been wired through `OcclusionWorld::EntityDefBits` but defaulted 0 (re-scoped 2026-09-23 from the closed D-COL-2). Both writers are now witnessed: +0x891 = `first_door` - 1 (or `rotor_parts`' second argument), +0x892 = `first_subobject` - 1 (or `aux_parts`' third argument) `[orig: ItemDef_ParseProperty @ 0x49F7BA..0x49F7DE, @ 0x49F9B0..0x49F9CC, @ 0x49EF5D..0x49EFB6, @ 0x49EFF3..0x49F04C]`; the "destruction bone map" naming was a misnomer. Ported: the items.def parser writes the retail bytes, every building feeds `OcclusionWorld::assign_forced_sections`, and only the part draw ORs `-1 << byte` over the raw mask (`section_draw_mask`); every other reader keeps the raw word (§5). The building feed carries raw + forced, and a collision-backed building without OOBJ now gets the true retail mask (outdoors: exterior only, `@ 0x5c87d7..0x5c8830`) instead of the all-parts stand-in, so the windowless Iblock01/Iblock02 ("Indonesian town bld. #1/#2", First_Door 2) draw their door part from outside. ctests `def_parse_item_part_bytes`, `occlusion` (forced bits split), `doors`. |
| D-OCC-10 | CLOSED 2026-09-24 (PORTED; the 2026-08-30 PERMANENT registration was wrong) | `Terrain_SortPortalSlotsByPriority @ 0x5c4410` is a stable descending sort on the slot priority key (`@ 0x5c4443`) followed by `g_PortalSlotCount = min(count, 14)` (`@ 0x5c449f..0x5c44a4`), called `@ 0x5c9188` between the collect and the occluder planes. The registration's claim that it "orders retail draw calls only; results order-independent" is false because of the clamp: slots past 14 drop out of the occluder set. Ported as `OcclusionWorld::sort_portal_slots` (ctest `occlusion`, slot clamp). |
| D-OCC-11 | PERMANENT (register 2026-08-30, class D — reimpl safety) | Cap semantics: retail's wedge builder writes past 64 planes into adjacent stack after printing "too many planes" (and the viewthru bank writes unguarded, erroring only after 512/2048) — the port clamps the wedge at 64 and guards the viewthru bank like the window bank. Divergence only in the overflow regime where retail corrupts its own memory. Scratch caps (128 edge words / 64 polygon verts / 128 front-face planes) sized above any witnessed record. |
| D-OCC-12 | OPEN (bounded stand-in; PERMANENT candidate — tabled 2026-08-30; narrowed 2026-09-24) | The host frustum planes stand in for the viewport projector's side planes: the reimpl builds 5 frustum planes (near + 4 sides, inward normals, render float space) from the camera drawing the frame's image and tests bound spheres against them + a Q22 forward-row depth cull vs the fog distance, where retail projects through `Viewport_TransformAndClipPoint @ 0x4115e0`. The projector's cull is the same shape: plane distance < -radius per side plane plus the depth/fog test `[orig: @ 0x411622..0x411763]`; its 1.125 × radius term only sets clip flags. There is no screen-space epsilon (the earlier wording was wrong). The traversal consumes the same reimpl planes where retail passes `g_CameraFrustumPlanes5`. |
| D-OCC-13 | PERMANENT (renderer-replaced legs — register 2026-08-30; narrowed 2026-09-24) / window-glow facet REFUTED 2026-09-24 | Not ported by design: the two-pass open-building draw ORDER (exterior then interiors — Godot's depth buffer replaces the ordering; the visibility UNION is applied), the per-light interior section scoping (`Lighting_SetInteriorLightGroup @ 0x5a90e0` — Godot's light model differs) and the reflection-pass collector variant (def-flag 0x2000000 — Godot water reflections). The water-mirror clip leg (`@ 0x5c5e57..0x5c5e75`) is PORTED 2026-09-24 as per-draw CLIP arming (§5, "Arm the water mirror's CLIP technique per draw, as retail does"). The window-glow facet is refuted: the slot +8 value is a priority key whose only reader is the slot sort compare (xrefs to `0x2983E90`: the writer `@ 0x5c6eb1` and the compare `@ 0x5c4443`), the OOBJ +20 float is the slot priority weight (zero in all 806 JO records), and no window-glow renderer exists (§1, §3 step 2). |
| D-OCC-14 | CLOSED 2026-09-24 ("Port the retail occlusion collector, mask and sub-pixel rules") | Entity-gate coverage. Batched statics: node-less verdicts (the collector gates `@ 0x5c7022..0x5c708a`, the latch `@ 0x5c7118..0x5c7162`, the building batch/TOC verdicts for non-OOBJ buildings) land on the placer's retained instance (`MissionObjectPlacer::set_static_instance_occlusion_hidden`); a culled instance carries no row at any level. Organics: the 1 u bound-sphere stand-in is replaced by retail's person leg (entity position + entity+0 radius, the item-185 radius under the parachute flag, the 0.75 px floor before the latch, §3 step 5) for placed organics and bare wire rows (the type's entity+0 radius via the shared collision-shape resolve; Player/Infantry rows take the person leg, other bare rows the model leg's bound sphere placed by the row's Euler pose). The "wire avatars ride their own present path ungated" facet was stale since 2026-09-05. 2026-09-05 (kept): the listen host's OWN runtime spawns with no placed identity (an addeweap gun child) take the live registry verdict (`entity_render_visible`) like its placed rows; the sphere-at-the-decoded-row path is for bare wire rows only, because the host's loopback 0x0A is header-only and that row keeps the spawn image — a driven DBuggy's gun was culled the moment the buggy left it (GUT `vehicle_emplacement_alignment_test` driven-away case, red before the fix). |
| D-OCC-15 | CLOSED 2026-09-24 (both facets) | Static refresh: `tick_item_event_pool` refreshes pool-2 `blink_hits` on the tick & 7 cohort with the 62-tick reload (`engine/runtime/world/item_events.cpp`, `[orig: Entity_UpdateAllEntities @ 0x4C2244..0x4C22C9]`, the `Entity_BuildProximityList` call `@ 0x4C229C`), and the one residual, the mission-start stamp, is ported (`CollisionWorld::refresh_mission_start_blink` run by `MissionKernel::occlusion_init_mission`, `[orig: Entity_BuildProximityListsForPools12 @ 0x5240a0]` from `[orig: Game_StartMission @ 0x525898]`: every pool-1 row, then every non-building pool-2 row, the def-type-5 skip `@ 0x5240F3`, before the portal init; ctest `occlusion`, `test_mission_start_blink_stamp`). PRNG stream: the re-arm jitter draws the world's shared `prng16_c_state` (`occlusion.bind_focal_wind_random(&world.prng16_c_state)` in `engine/runtime/mission/mission_kernel.cpp`), the same stream the other `PRNG_Next16_C` consumers share, so the class-C "owned stream" registration no longer describes a divergence. The TOC candidate radius is entity+0 read directly (`[orig: test_sector_entity_occlusion @ 0x5c4610, @ 0x5c463e..0x5c4640]`, `fmul` `@ 0x5c464f`; the entity+0 bound is the D-COL-3 stamp). |
| D-OCC-16 | FIXED (2026-09-13; minted and closed in the same PR) | `OcclusionWorld::bound_sphere_fixed` measured `((max-min)>>1)` rather than `max-midpoint` and omitted the scale leg. `[orig: Entity_ComputeBoundingSphere @ 0x5C69A0, @ 0x5C6A3B..0x5C6B52]` uses the larger positive-side half on an odd Q16 width (raw bounds `(-3,-2,10)..(4,7,15)` yield midpoint `(0,2,12)` and radius **7** in the retail PE execution, versus the helper's former **5**) and then multiplies the three center words and the radius by the nonzero anim/definition scale with the +0x8000 rule (`@ 0x5C6AC8..0x5C6B52`). Ported as the witnessed form, including the +-0x40000000 unset-bound clamps; the static-batch memo, `toc_occluded` and `entity_render_visible` pass `Entity::uniform_scale_q16` (the shell resolves the entity+0x158 override before itemDef+0x1B8, so one operand stands for retail's pair). Pinned by `occlusion_test` (odd width, 1.5x scale, sentinel bounds); the render RLOD producer (D-RORD-12) and this helper now agree. |

## 8a. 2026-09-24 rendering parity pass

The visibility stream of the rendering parity pass (PR #678) ported the
collector, mask and sub-pixel rules and closed D-OCC-1, -4, -5, -9, -10, -14
and -15, narrowed D-OCC-12 and D-OCC-13, and refuted the window-glow reading.
Commits: "Port the retail occlusion collector, mask and sub-pixel rules",
"Run render_TOC on collected entities outside blink boxes", "Split the dome's
cloud pass after the bodies and port the sky pass gates", "Gate effect groups
by the building sections their spawn point lies in", "Arm the water mirror's
CLIP technique per draw, as retail does". The witnesses sit in §1-§5 and §8;
the port facts not stated there:

- **Sub-pixel floor on every world model.** Every world `ObjectModel`
  (placed, avatar, wire, husk; any level count) takes retail's 0.75 px return
  `[orig: render_sector_entity @ 0x5c42d8..0x5c42de]`
  (`renderer::kObjectLodSubPixelCullQ16`) as a third visibility bit beside the
  section mask and the occlusion verdict; attachments take their owner's
  verdict (retail draws them inside the owner's bone callback). The RLOD walk
  it precedes is in [render-order-re.md](render-order-re.md).
- **The frame's camera.** The occlusion and LOD frames derive the frustum
  from the camera drawing the frame's image (the stretched target's camera
  while live; the keep-aspect mode decides which axis the Godot fov names)
  and the focal from the surface width (`OcclusionFrameCamera::focal_pixels`,
  §3 step 5; ctest `occlusion_camera`).
- **Destroyed sections and the occlusion verdict** are separate
  `ObjectModel` channels ORed per part like entity+0x138 |
  `g_HiddenSectionMask` (§5); the corona gate reads the raw verdict.
- **Wire rows.** A decoded wire row without a registry twin takes its blink
  quad from the client's own blink walk (the one the lighting feed runs), so
  it passes the same collector gate before the legs and the same contained
  test after them; bare wire rows take their type's entity+0 radius and leg.
- **Foliage anchors.** The foliage MODEL anchors are pushed from the same
  post-TOC verdicts, so BySide's render_TOC (`@ 0x5c7d96`) reaches them
  ([foliage-re.md](../foliage/foliage-re.md) D-FOLIAGE-9).

### Open after the 2026-09-24 pass

- **Round restart.** `Game_RestartRoundSP` re-runs `Game_StartMission` with
  register + weld skipped (D-OCC-4). The port has no round-restart re-init of
  the portal state; a future restart path must keep the flag-stamp tail only
  and never re-run register + weld over the already-welded (shared, retyped)
  models.
- **Multi-view LOD and sub-pixel.** Retail runs the collector, the sub-pixel
  floor and the RLOD walk once per scene pass: the main view and the weapon
  Inset pass (`sub_5705E0` → `Terrain_RenderWorldScene`) each compute
  their own `g_RlodFrameScale` and projections. Godot draws both passes from one
  node per entity, so each model takes the finest level any view selects and
  drops only when every view that sees it projects it at or below 0.75 px.
  With the Inset pass inactive the main view is exact.
- **Model leg without a collision instance.** A non-organic entity with no
  collision instance still takes a position-centred unit sphere in
  `OcclusionWorld::entity_render_visible`; retail's collector skips an entity
  with no render model (`@ 0x5c8cf6..0x5c8cff`) and reads the model bounds of
  every entity it keeps.

## 9. IDB changes made during the session (2026-07-16, saved)

Port-pass additions (2026-07-17, saved): corrected the batch `+16` flag
comment on `collect_visible_sector_userpoints @ 0x5c6b60` (set for ANY record
type ≥ 1, not only type 1 — witnessed `@ 0x5c6dcd-0x5c6de1`), and reimpl
pointers on the ported function comments (`opennova: world/occlusion.cpp`).

Renames (ex auto/kong-misnomer, all behavior-witnessed): functions
`Terrain_InitBuildingPortals @ 0x5c7480` (ex sub_),
`Terrain_WeldOppositePortalFaces @ 0x5c5910` (ex
"find_and_merge_opposite_terrain_decals"),
`Terrain_RegisterExteriorPortalFaces @ 0x5c5b80` (ex
"collect_userpoint_effect_slots"),
`Terrain_TraversePortalsFromExterior @ 0x5c7330` (ex sub_),
`Terrain_TraversePortalsFromSection @ 0x5c73d0` (ex
"Terrain_RenderEntityAtLodLevel" — renders nothing),
`Terrain_BuildPortalOccluderPlanes @ 0x5c44c0` (ex sub_),
`Terrain_TestSphereInPlaneGroups @ 0x5c4580` (ex sub_; probable),
`Terrain_IsBuildingSectionBitSet @ 0x5c6960` (ex sub_),
`Entity_QueryBlinkBoxesAtPoint @ 0x4af350` (ex "Entity_FindEntitiesInRadius");
45 globals (`g_BuildingSectionVisMask @ 0x297F250` ex "dest",
`g_BlinkWaterVisible @ 0x29ACE40` ex "render_mode",
`g_HiddenSectionMask @ 0xB7965C` ex "g_MaybeHiddenBoneMask",
`g_SectorEntityList @ 0x2999518` ex "iniEntry",
`g_WindowFrustumGroupCount @ 0x29B5E44` ex "num_groups", the
`g_PortalCtx*` traversal context block @ 0x29ACDEC–0x29ACE1C, the
window-frustum/viewthru group arrays, `g_PortalWeldRecords @ 0x2967250` +
count, `g_PortalFaceRegistry @ 0x29BEED8` + count,
`g_PortalSlots @ 0x2983E88` + count, `g_PortalOccluder* @ 0x2983D70..80`,
`g_VisibleBuildingBatch @ 0x2985C90` + count,
`g_CameraFrustumPlanes5 @ 0xA7849C`, `g_SectorEntityCount @ 0x2999510`).
Entry comments on all of the above plus corrections of two wrong
auto-comments (`Physics_CheckTerrainLineOfSight @ 0x53b080` return
convention; the `Sound_ApplyOcclusionDistance @ 0x529970` per-ray-add
simplification), the mission-start blink clear `@ 0x525c45`, the
`Bms_AttribFlags` 0x10 override `@ 0x5ca1c8`, and the frame gates
`@ 0x5c1353 / @ 0x5ca84f / @ 0x5c93cb / @ 0x5ca1ab`.
