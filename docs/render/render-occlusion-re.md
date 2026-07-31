# Rendering occlusion / blink-box visibility — reverse-engineering record

Engine-research record (2026-07-16, port pass 2026-07-17) for the
blink-box-driven visibility system: the per-frame building section masks, the
portal traversal, the occluder culling pass, the frame-level indoor gates, and
the occlusion model data they all consume. Binary: retail **Jointops.exe**
(IDB `Jointops.exe.kong.i64`, imagebase 0x400000). All addresses below are
that binary's. The consumer engine is **PORTED** (2026-07-17):
`libs/world/occlusion.{h,cpp}` (`OcclusionWorld` + the render-float math),
the `CollisionWorld` camera blink query, and the reimpl wiring
(`NovaSimulation::run_occlusion_frame`, `OcclusionFramePass.apply_frame (godot/engine/world/occlusion_frame_pass.gd)`,
the placer de-batch + `NovaObjectModel.set_section_visibility_mask`); ctest
`occlusion` + GUT `game_world_test` / `nova_object_model_section_mask_test`
cover it. The *producer* side (blink volume queries, the indoors bit,
per-entity blink hits) is §15 of
[world-wac-ai-re.md](../world/world-wac-ai-re.md); this record is the
*consumer* side that §15 deferred as "REN-scope follow-ups". Sound occlusion is
witnessed here too (its port target is `godot/engine/world` audio; the catalog
entry stays [lwf-dbf-sound-re.md](../audio/lwf-dbf-sound-re.md) D-SND-7).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Occlusion model data (`OVRT`/`OPLN`/`OFAC`/`OOBJ` chunks → 60 B runtime records) | **PORTED** (2026-07-17: the 3DI3-side promotion `ThreediIROcclusion` → `world::OcclusionModel`; the GPM-path runtime is deliberately unsupported — project decision, no GP runtime/ONED support) | `[orig: load_occlusion_model_data @ 0x5b4a00]`, caller `[orig: ThreediGp_LoadFromFile @ 0x5b5c37]`; tag immediates witnessed in disasm; copy loops re-derived from disasm (OVRT/OPLN identity, OFAC keeps disk field order, OOBJ 36→60 with sequential slice pointers) |
| Mission-start portal init (register + weld + per-building flags) | **PORTED** (2026-07-17, `OcclusionWorld::init_mission` + `NovaSimulation::occlusion_init_mission`; the weld's type-5 rewrite mutates the SHARED per-graphic record array like retail's model cache) | `[orig: Terrain_InitBuildingPortals @ 0x5c7480 (tail @ 0x5c5860)]` from `[orig: Game_StartMission @ 0x525e11]`; thresholds 0.80000001f / −0.89999998f / 0.2 from decompile |
| Camera blink query | **PORTED** (2026-07-17, `CollisionWorld::query_blink_boxes_at_point`) | `[orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]` |
| Per-frame section-mask build | **PORTED** (2026-07-17, `OcclusionWorld::build_section_masks`; masks keyed by pool-2 handle slot like `Pool_GetIndexFromPtr`) | `[orig: build_sector_visibility_masks @ 0x5c8610]` |
| Portal traversal (section expansion, window/viewthru wedges) | **PORTED** (2026-07-17, `OcclusionWorld::traverse` + seeders, in the render float world — see §1a) | `[orig: render_visibility_portal_traversal @ 0x5c4ae0]` + the two seeders `@ 0x5c73d0 / @ 0x5c7330`; recursion args witnessed at `@ 0x5c5619/0x5c57fc` (backface-latch recursion re-uses the CALLER's planes with section 0) |
| Occluder culling (render_TOC) | **PORTED** (2026-07-17, `OcclusionWorld::toc_occluded` + `build_occluder_planes`) | `[orig: test_sector_entity_occlusion @ 0x5c4610]`, planes `[orig: Terrain_BuildPortalOccluderPlanes @ 0x5c44c0 → build_clip_planes_from_collision @ 0x5b34e0]`; the 8-corner refinement's collision-AABB swizzle witnessed @ 0x5c4920 |
| Entity-vs-terrain three-ray occlusion latch | **PORTED** (2026-07-17, `OcclusionWorld::three_rays_clear` + the `Entity.occlusion_latch` byte; own `PRNG_Next16_C`-form stream, D-OCC-15) | `[orig: terrain_occlusion_check_three_rays @ 0x610ed0]` + the three collectors below |
| Frame-level indoor gates (terrain/sky/water/foliage) | **PORTED** (2026-07-16, `OcclusionFramePass.apply_blink_gates` + the indoor black clear; 2026-07-17: the `g_BlinkWaterVisible` straddle/window-latch override + the `Bms_AttribFlags & 0x10` force-indoors landed in `OcclusionFramePass.apply_frame`) | gates at `@ 0x5c1353 / @ 0x5d0570 / @ 0x5ca84f / @ 0x5c93cb / @ 0x5c95bf / @ 0x5c9665 / @ 0x5ca1ab`; GUT `game_world_test.gd` blink-gate + occlusion-frame cases |
| Draw-side mask consumption (hidden-section mask, two-pass open buildings, per-light section masks) | **PORTED** (2026-07-17, visibility data only: the union mask + forced def bits drive `NovaObjectModel.set_section_visibility_mask` per-part visibility on de-batched buildings; the two-pass draw order / per-light scoping / mirror clip are renderer-specific legs, D-OCC-13) | `[orig: Terrain_RenderSectorModels @ 0x5c5d30]` |
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
`libs/threedi`'s 3DI3 reader already parses as
`occlusion_vertices/planes/faces/objects` — the 3DI3-side loader in Jointops
was not located this session (open item D-OCC-7). Copy-loop details re-derived
from the disasm (port pass): OVRT/OPLN copy verbatim; OFAC keeps the disk field
order (`u8 v0,v1,v2, u8 planeIdx, u16 edge[3]`, bytes 10-11 uninitialized pad);
OOBJ 36 B disk (`type/secA/secB/pad, pos f3 @+4, radius @+16, GLOW SCALE f32
@+20, vert/plane/face counts @+24/+28/+32`) → the 60 B runtime shape below with
per-object slice pointers advancing sequentially. The disk +20 float is the
window-glow scale (runtime +0x2C) — `libs/threedi`'s former `unk1` field,
renamed `glow_scale` (zero across the JO 3DI3 corpus). The loader stores
count/array through a `this` alias 4 bytes below the model consumers read
(loader writes +0xE0/+0xE4; every consumer reads model +0xDC/+0xE0).

**The 60 B runtime record** (the "portal face"; runtime model `+0xDC` = count,
`+0xE0` = array):

```
+0   u8   type: 0/1 = occluder+glow slot (1 = "open"), 2 = exterior window,
          3 = interior portal, 5 = welded cross-building link (runtime-made)
+1   u8   section A   (COBJ section ordinal; 0 = exterior)
+2   u8   section B
+4   f32[3] position (model space)   +0x10 f32 radius
+0x14 i32 vertex count    +0x18 ptr → OVRT slice
+0x20 ptr → OPLN slice    +0x24 i32 face count   +0x28 ptr → OFAC slice
+0x2C f32 glow scale      (+0x30.. spare)
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
scale `dbl_7C3608 = 0x3E19222D9890E4A8` is NOT exactly 2π/2³² (≈ 2.1 ppm
above; the port carries the exact bits). The TOC corner refinement's
collision-AABB swizzle (`X = −y, Y = z, Z = x` of the mission-axis bounds
`[orig: @ 0x5c4920-0x5c49c1]`) and `Entity_ComputeBoundingSphere @ 0x5c69a0`
(center = AABB midpoints, radius = min(√Σhalf², 0x7FFF0000f)) are consistent
witnesses of the same frame. The fixed 3x4 transform family the collectors
use (`Math_FixedPointTransformPoint22 @ 0x615810`) rotates >>22 with a
+0x200000 rounding bias AND translates (rows `[r r r t]`) — an earlier
collision-record note calling it rotate-only was imprecise.

## 2. Mission-start portal init

`[orig: Terrain_InitBuildingPortals @ 0x5c7480, tail @ 0x5c5860]`, called once
from `[orig: Game_StartMission @ 0x525e11]` (arg pushed as `ebp`; structurally
nonzero — D-OCC-4):

1. arg ≠ 0: **register** every building's type-2 faces —
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
`[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0, call @ 0x5c94f0]`), in
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
   `g_PortalSlots @ 0x2983E88` (5-dword stride: entity, record ptr, glow
   value, viewthru plane ref, viewthru count; count
   `g_PortalSlotCount @ 0x298056C`) — these double as the frame's occluders
   and the window-glow sources. Slot eligibility per record: the viewport
   sphere clip plus an angular-size gate `radius²/dist² > 0.01` (collect
   within 10 radii); the banked glow = `ftol(ratio · glowScale · 2²⁴)`
   `[orig: @ 0x5c6e48-0x5c6eb1]`.
2. `[orig: Terrain_SortSectorCacheByDistance @ 0x5c4410]`.
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
   slots and a 512-slot effects pool walk. Per entity, two occlusion rules:
   - **blink-hits gate**: an entity with a nonzero `blink_hits[4]` quad
     (entity `+464`, §15) is collected only if one of its packed
     (building, section) hits has its section bit set in
     `g_BuildingSectionVisMask` `[orig: @ 0x5c7028-0x5c708a / @ 0x5c8d70-0x5c8dd1]` —
     contents of an interior render only while that interior is render-active;
   - **three-ray terrain occlusion latch** (outdoors only, below).

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
  camera-containing building straddles the water height, or when the
  traversal latched the exterior — the water-pass override consumed at
  `@ 0x5c93cb/0x5c95d2`.

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
  `(farSection, wedge planes)`; depth cap 10 `[orig: @ 0x5c4af4]`.

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
zeroed.

### 3.4 The three-ray terrain occlusion latch

All three entity collectors run, **only while the local player is outdoors**
(`(~g_LocalPlayerBlinkFlags & 2) >> 1` `[orig: @ 0x5c6f56 / @ 0x5c6b92 /
@ 0x5c8c96]`), a per-entity visibility latch: byte `entity+342` counts down
per frame while latched visible; at 0 the entity is re-probed by
`[orig: terrain_occlusion_check_three_rays @ 0x610ed0]` — three heightfield
rays from the camera (z +0x4000) to the bound-sphere top (z + radius) and the
two lateral silhouette points (± radius along one camera basis row, + radius/2
along another), early-out on the first clear ray
(`Terrain_RaycastHeightmapHiRes @ 0x60c760`, nonzero = clear). Pass →
visible + relatch for `(rand & 7) + 16` frames; fail → culled this frame and
re-probed next. Indoors the probe is skipped and everything passes (an indoor
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
| 0x2 (indoors) | PolyTrn terrain render skipped + frame clear color 0 (not skyfog) | `[orig: render_main_scene @ 0x5c1353 / @ 0x5c1597]`, `[orig: terrain_scene_render @ 0x5d0570]` |
| 0x2 | skybox pass skipped | `[orig: @ 0x5ca84f → Terrain_RenderSkyboxPass @ 0x610ac0]` |
| 0x2 | far-foliage passes skipped | `[orig: Terrain_RenderSceneWithReflection @ 0x5c95bf / @ 0x5c9665 → Foliage_RenderFarPatchesPass @ 0x60c6d0]` |
| 0x2 | three-ray entity occlusion disabled (everything passes) | §3.4 |
| 0x2 | outdoors flag forwarded into `Render_TerrainScene @ 0x610c80` (arg use unwitnessed, D-OCC-6) and `Render_RadarCompassOverlay @ 0x5ca949` | `[orig: @ 0x5ca504 / @ 0x5ca949]` |
| 0x4 | the `sub_58AA80` float-1.0 device state skipped (also skipped underwater); state semantics open (D-OCC-1) | `[orig: @ 0x5ca1ab / @ 0x5ca7cc]` |
| 0x8 | both water passes skipped unless `g_BlinkWaterVisible` | `[orig: @ 0x5c93cb / @ 0x5c95d2-0x5c95ea]` |

Debug: `Debug_DrawEnvironmentValues @ 0x4ef1cc` prints the accum (overlay
only). Bits 0x10/0x20 have no witnessed frame consumer.

## 5. Draw-side mask consumption

`[orig: Terrain_RenderSectorModels @ 0x5c5d30]` per visible building:
`mask = g_BuildingSectionVisMask[pool2]`; bits ≥ `itemDef+2193` and
≥ `itemDef+2194` are forced visible (blink sections occupy the low bone
indices; the §15 destruction bone map sits above) `[orig: @ 0x5c5d7c-0x5c5da8]`;
the per-draw hidden mask is `g_HiddenSectionMask @ 0xB7965C = ~mask`.
Buildings whose batch entry carries the open-portal flag draw **two passes** —
exterior only (all interior bits hidden), then interiors (bit 0 hidden) —
`[orig: @ 0x5c5e17-0x5c5e43]`. Per-light interior passes AND the mask with the
light entity's own `blink_hits` quad sections, so an interior light lights
only the sections it sits in `[orig: @ 0x5c5f5a-0x5c5fa7]`;
`Lighting_SetInteriorLightGroup @ 0x5a90e0` is set per building and cleared
after — corroborating §15's sampler note (`terrain_sector_compute_lighting
@ 0x5c7550`, confirm-only). Buildings fully below the water plane get the
mirror clip matrix in the reflection scene `[orig: @ 0x5c5e75-0x5c5e9b]`.
`g_BuildingSectionVisMask` is also read by the window-glow renderer
(`terrain_render_sector_userpoints @ 0x5cd830`), the bit-query helper
`[orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960]` (permissive out-of-range),
and `sub_5F6D10 @ 0x5f6d10` — none decompiled this session (D-OCC-5).

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
Editor-authored data with no runtime effect in JO — our `libs/mission` parse
keeps the fields for .mis round-trip fidelity only.

## 8. Divergence / open-item catalog (D-OCC)

D-OCC-1..8 opened as research-session witnesses; the 2026-07-17 port pass
added the reimpl-mapping divergences D-OCC-9..15.

| ID | Status | Summary |
|---|---|---|
| D-OCC-1 | OPEN (witness detail) | Blink accum bit 0x4 gates the `sub_58AA80 @ 0x58aa80` float-1.0 device state (`sub_677700(0, 1, &1.0f)`), also skipped underwater — which D3D state that is (and its visual consequence) is unwitnessed. |
| D-OCC-2 | OPEN (witness detail) | Per-building flag bytes `+0x2CC` (has type-1) and `+0x2CE` (has type-5) are stamped at mission start but no reader was found (single-register modrm scan only; SIB-form reads unscanned). `+0x2CD` (has windows) is the witnessed outdoor-mask input. The port stamps all three (`OcclusionWorld::BuildingFlags`). |
| D-OCC-3 | OPEN (witness detail) | Record type 0 vs 1 distinction beyond witnessed uses: both collect as occluder/glow slots; type 1 flags the building "open" (outside-in traversal + two-pass draw + occludes only when bit-31-marked, with viewthru exceptions); the authoring-side meaning (door vs window vs destroyed state, and what mutates the byte at runtime besides welding) is unwitnessed. The port's test fixtures use the derived side conventions (record normal points a→b; windows author A = interior, B = 0). |
| D-OCC-4 | OPEN (witness detail) | `Terrain_InitBuildingPortals`'s register+weld half is gated on its arg; the sole call site pushes `ebp` (`Game_StartMission @ 0x525e11`) whose value was not traced — structurally assumed nonzero at mission start (the weld machinery is live in retail; the port defaults `do_register_weld = true`). |
| D-OCC-5 | OPEN (witness detail) | `g_BuildingSectionVisMask` readers not decompiled: `terrain_render_sector_userpoints @ 0x5cd830` (window glows), `sub_5F6D10 @ 0x5f6d10`, and the `Terrain_RenderSectorEntities`/`BySide` interplay with the collect-time blink-hits gate. |
| D-OCC-6 | OPEN (witness detail) | `Render_TerrainScene @ 0x610c80` receives the outdoors flag as arg 0; its consumption inside (frameless-callee arg pattern) is unwitnessed. |
| D-OCC-7 | CLOSED-BY-DECISION (2026-07-17) | Only the GPM-path occlusion loader is witnessed; the 3DI3-path loader in Jointops was never located. The port promotes the 3DI3-parsed chunks (`ThreediIROcclusion`, identical disk family) into the witnessed 60 B runtime shape; the GPM/GP runtime path is deliberately unsupported (project decision: no `threedi_gp` runtime or ONED support). |
| D-OCC-8 | OPEN (suffix-consumer reconciliation) | Super OED Manual v1.1 officially defines BB plus `W`/`S`/`V` as preserving water/sky/voxels; exporter reconstruction also accepts `L`/`O` and clears bits from initial 0x3E. Runtime consumption is separately witnessed: bit 0x2 indoors, 0x4 the D-OCC-1 state, 0x8 water suppress, 0x10/0x20 no frame consumer. Reconcile the author-facing sky/voxel split with those consumer bits; `L`/`O` expansions remain unverified. |
| D-OCC-9 | PORT DIVERGENCE | The forced-visible def bytes (`itemDef+2193/+2194`, the destruction bone-map bases) are wired through `OcclusionWorld::EntityDefBits` but default 0 — the destruction system is unmodeled (D-COL-2), and their load-time source is unwitnessed. Identical behavior for buildings without destruction bones (retail skips zero bytes too). |
| D-OCC-10 | PORT DIVERGENCE | `Terrain_SortSectorCacheByDistance @ 0x5c4410` is not ported: it orders retail draw calls/slot iteration only; the mask/TOC results are order-independent per candidate, and Godot owns draw order. |
| D-OCC-11 | PORT DIVERGENCE (reimpl safety) | Cap semantics: retail's wedge builder writes past 64 planes into adjacent stack after printing "too many planes" (and the viewthru bank writes unguarded, erroring only after 512/2048) — the port clamps the wedge at 64 and guards the viewthru bank like the window bank. Divergence only in the overflow regime where retail corrupts its own memory. Scratch caps (128 edge words / 64 polygon verts / 128 front-face planes) sized above any witnessed record. |
| D-OCC-12 | PORT DIVERGENCE (reimpl mapping) | View culling: the reimpl builds 5 frustum planes (near + 4 sides, inward normals, render float space) from its camera and tests bound spheres against them + a Q22 forward-row depth cull vs the fog distance — standing in for `Viewport_TransformAndClipPoint @ 0x4115e0`'s project-and-clip (same culling intent; the retail projector's screen-space epsilon behavior is not replicated). The traversal consumes the same reimpl planes where retail passes `g_CameraFrustumPlanes5`. |
| D-OCC-13 | PORT DIVERGENCE (renderer-specific legs) | Not ported by design: the two-pass open-building draw ORDER (exterior then interiors — Godot's depth buffer replaces the ordering; the visibility UNION is applied), the per-light interior section scoping (`Lighting_SetInteriorLightGroup @ 0x5a90e0` — Godot's light model differs), the water-mirror clip matrix leg (`@ 0x5c5e75`), the reflection-pass collector variant (def-flag 0x2000000 — Godot water reflections), and the window-glow renderer (the slot glow value is computed and banked but unconsumed). |
| D-OCC-14 | PORT DIVERGENCE (reimpl coverage) | The entity render gates apply to registry-resolvable placed entities (bms_id ≠ 0); pooled MultiMesh statics keep the placer's documented always-drawn batching tradeoff (no per-instance visibility), and wire avatars ride their own present path ungated. Organics without collision instances use a position-centered 1 u bound-sphere stand-in for the graphic bounds `Entity_ComputeBoundingSphere` reads. |
| D-OCC-15 | PORT DIVERGENCE (state/reimpl coverage) | The three-ray latch re-arm jitter uses its own `PRNG_Next16_C`-form stream seeded from the BSS-zero boot state; retail shares the stream with unrelated consumers, so per-frame re-arm values differ from any given retail run (distribution identical). The TOC candidate radius uses the D-COL-3 collision-AABB derivation for the def bound radius. Retail also refreshes pool-2 static `blink_hits` in staggered batches; the port currently has no placement or staggered static refresh, so static objects inside rooms can remain unstamped. |

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
