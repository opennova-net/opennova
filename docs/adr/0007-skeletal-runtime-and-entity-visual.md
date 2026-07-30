# ADR 0007 — Runtime skeletal animation (`.bad`/`.adm`) + the `NovaEntityVisual` contract

Status: accepted (branch `unified-edit-history`). Supersedes the "main-body skeletal" deferred seam in
[ADR 0006](0006-unified-mission-runtime-present-pass.md).

## Context

ADR 0006 unified the mission runtime + present pass but left the largest seam open: the main-body
skeletal runtime did not exist. `libs/bad` + `libs/adm` (since folded into `libs/anim`) were parse-only, `NovaObjectData` dropped the
`.3di` skin weights, no `Skeleton3D`/`Skin` was ever built, and `mission_present_pass.gd::apply_body_anim`
was a stub, so skinned organics (infantry) rendered frozen at their `.3di` rest pose. This ADR records
building that runtime and the IDA fidelity grill against `Jointops.exe`.

The IDA anchor in the old CONTEXT (`AnimMap_PlayAnimBySlot @0x40bda0`) is the weapon/recoil channel layer
plus the **player-avatar** 252-entry slot table `off_8135F0`, NOT the skeletal evaluator. The real pose
chain is `BoneFile_Load @0x40fff0` → `BoneAnim_FindKeyframeAtTime @0x410220` → `Math_QuaternionSlerp
@0x615e20` → `BoneAnim_TransformBones @0x410360` → `AnimChannel_ComputeBoneMatrices @0x410da0` →
`build_world_bone_matrices @0x40c770` → `Entity_BuildBoneTransformMatrices @0x4b1290`.

## Decision

**Libs-first split.** Portable `libs/anim` (`opennova_anim`, depends on `opennova_bad` + `opennova_io`; the
separate `opennova_adm` target was folded into `libs/anim` in the 2026-07 restructure)
samples `.bad` clips into per-bone local transforms in engine-native (Y-up) space. `NovaSkeletalAnim`
(godot-cpp `Resource`) loads `.adm` + `.bad` over the VFS, builds the bind-pose bones, resolves AI body-anim
slots to clip keys, and exposes `eval_pose(key, seconds)`. `NovaObjectModel` builds the `Skeleton3D` + a
rest-derived `Skin` and writes the per-frame bone poses. Skin plumbing lives in `NovaObjectData`: skinned
surfaces emit `ARRAY_BONES`/`ARRAY_WEIGHTS` remapped through the strip `bone_table`; rigid first-person
weapon parts "fake-skin" every vertex to their subobject `part_index`, so one `.adm` drives a skinned-arms +
rigid-gun view model.

**Three conventions** (validated visually against the proven oscarmike port, then IDA-confirmed):
1. Engine-native **Y-up**, not Blender Z-up: bone position as-is `(x,y,z)`, channel quaternion read directly
   `Quaternion(qx,qy,qz,qw)`; the mesh carries the `(-x,y,z)` handedness (`build_world_bone_matrices`
   negates X — a left/right flip, not a Y flip).
2. Skeleton bind/REST from the `.bad` **BadBone bind matrix** (`rotation[9]` 3×3 + `position`,
   `mat3_to_basis(bone_mat · parent_mat⁻¹)`), NOT a sampled clip frame — the `.3di` mesh is skinned to it
   (`AnimChannel_ComputeBoneMatrices @0x410da0` uses the bone's +64 3×3).
3. **Per-bone keyframe sampling**: each bone walks its **own** `frame_lengths` duration table
   (`BoneAnim_FindKeyframeAtTime @0x410220`), `targetTick = seconds·fps`, slerp `rot[i]→rot[i+1]` by the
   in-window fraction. A flat `rotations[frame]` index snaps a sparsely-keyed bone to identity once the
   frame index passes its keyframe count — which collapses *compressed* clips (≈11% of the corpus: idle/
   run/attack variants). The FK accumulates over **one shared skeleton's** rest origins across all of a
   model's clips (a clip's own `.bad` bone positions can be zero and would otherwise collapse the pose).
   **The header `frame_count` counts INTERVALS** — a dense channel carries `frame_count + 1` keys
   (fence-post; the original's walk visits every key and holds the last). The baked pose table must
   include the final key: truncating at `frame_count` is invisible on loops (the seam key ≈ key 0) but
   collapses a ONE-frame clip — 2 keys, one motion window — to a static pose (the M4 viewmodel fire
   kick `m4_1f`, frozen through every volley until fixed 2026-07-12, D-ANIM-1). The translation block
   carries exactly `frame_count` rows; the final key holds the last row.

**`NovaEntityVisual` contract.** The mission present pass duck-types each placed entity node:
`transform`, `set_part_phase` (PANM), `set_entity_visible`/`visible`, and `play_body_anim(slot)`. Mission
NPCs select a body anim from AI state: `libs/world` `body_anim.h` maps `Entity.anim_slot` → an `anim_*`
`.adm` key; `MissionObjectPlacer` auto-loads each animated entity's `.adm`. No churn to the registry /
selection / drag / undo (ADR 0006 invariant).

**Authoritative skeletal collision consumer.** Organic bullet collision samples this runtime directly from
simulation state, not from a presentation-node snapshot. `NovaSimulation` keeps one ADM/rest source per
entity so actors sharing a graphic may occupy different clips/playheads, evaluates the primary pose plus
aim/body overlay (and the authoritative secondary weapon channel when live), resolves FK/rest deformation,
and emits FINAL world-space fixed matrices through `ICollisionSectionMatrixProvider`. The entity placement
is composed exactly once. Mounted infantry capture seat yaw/pitch/roll before the local look mirror and
synchronize body heading, both leg yaw/target chains, body pitch, and roll into that seat frame. Remote
entity heading follows the captured seat heading; the local entity keeps full-precision look heading/pitch
as a separate overlay input while its body remains seat-bound.
`Physics_RaycastAgainstBoneSections @0x4e4670` then pairs `COBJ[i]` strictly with
matrix `i`; COBJ parent/offset and CXLT metadata do not select or further transform organic sections. This
matches `BoneCallback_org0_Bone @0x4e34b0` → `Entity_BuildBoneTransformMatrices @0x4b1290` →
`Math_FloatMatrixToFixedPoint22 @0x611140` and keeps headless authority, live bullets, and F3 on one pose.
Late-spawn attachment reuses the same mission-lifetime graphic/ADM caches. Per-entity ADM resolution is a
player-spawn invariant: the host retains the resource root and item database and binds every later
local/remote player to its own `items.def` `anim_def` before its first authoritative body update, rather
than leaving it on default `adm_id 0`. Rebuilding the animation registry invalidates every stored id, so
the resolver rewinds its append-only high-water mark and repopulates all live entries. Play→Stop restore
likewise rewinds `AiSystem` and the resolver mark before re-resolving the restored baseline; an equal entry
count cannot hide restored default ids. Because packed pool handles are recycled, collision instances and
per-entity skeletal sources are also keyed by a monotonic registry spawn identity (whose high-water mark
survives editor snapshot restore), preventing a new occupant from inheriting an old model, husk,
failed-resolution result, or animation source.

**Mounted overlay selection is animation-owned and result-shared.** `AimOverlayInputs` carries a small
`MountMode` (`OnFoot`, `Seated`, or `Gunner`) plus an explicit `mount_config_valid` / `mount_config` pair.
The value is the mounted target item definition's authored `phrase_set` dword, parsed at
`ItemDef+0x86c` (`ItemDef_ParseProperty @0x49eb00`, store `@0x49fa0a`) and consumed by
`Entity_BuildBoneTransformMatrices @0x4b1884`. Presence is independent of value: absent metadata is
unknown, while `{valid=true, value=0}` is the real witnessed counter-lean branch. Mission promotion copies
the pair to the mount target, attachment copies it to the occupant, snapshot/restore preserves it, and
dismount clears the occupant's mode-driving copy and validity.

`NovaSimulation` translates seat state to `MountMode` once (retail slots 2/5 = seated, slot 3 = gunner),
builds the authoritative `AimOverlayInputs`, and calls
`opennova::anim::compute_aim_overlay_angles`. Local pose export/rendering and organic collision consume
that same selector result. Entity presentation snapshots carry the final body frame plus all nine selected
overlay angles; `MissionPresentPass` and `WirePresentPass` only adapt that result to
`NovaObjectModel.set_aim_overlay`, so placed and remote actors do not carry a second config switch or a
collision-only heuristic. CXLT remains independent metadata: organic COBJ section `i` consumes final bone
matrix `i`, with no CXLT selection or post-transform.

**Grill verdict (`Jointops.exe.kong`): DIVERGENT (core matches).** Confirmed faithful: quat layout, slerp
(with the small-angle nlerp fast path), the keyframe-duration walk, the bind matrix, the shared-rest FK, the
`(-x,y,z)` handedness, no animation root motion, no standalone 180° flip, and the fixed-point separation.
The single correctness fix the grill produced is convention #3 (per-bone keyframe-duration sampling).

- **Facing** stays in the present-pass placement basis (`MissionObjectPlacer.bms_to_godot_basis`); there is
  **no** flip in the skeleton or model. The "apparent 180°" is the composition of the X-negation handedness
  + the Y-negated Z-X-Y euler + the render-space transpose, already reproduced for the validated yaw-only
  case. Bones stay posed in raw model space; the entity world matrix is applied at placement.
- **Root motion** is **not** applied — entity world position is sim-driven (`Entity_UpdateInfantryAI` AI
  target-seeking + the movement collision resolver). The `.bad` translation channel (flag bit
  `0x2`) is model-space bone deformation only; `BadEvent.velocity` has no position consumer.
- **Bone record sizes**: the on-disk `.bad` bone (100 B, raw IEEE floats) and the runtime skeleton bone
  (108 B, fp16.16 with `parentIndex@+40`, pos@`+56/60/64`) are distinct records; `*1/65536` is the
  runtime/entity path only, never the `.bad` float path. Our parser reads raw floats; we never decode one as
  the other.

## Consequences

Skinned organics and rigid view-model weapons animate from one path; mission NPCs walk/idle from AI state;
the object workspace has an `.adm` preview (the smoke-test gate). Dense clips (e.g. US01) are byte-unchanged
by convention #3; compressed clips (C4* etc.) that previously collapsed now pose correctly. Organic
collision spheres follow that same current pose per section, including in headless simulation, so the bone
reported to damage/death selection cannot drift from the authoritative animation. The F3 hit-mesh view
samples that same authoritative pose at six Hz and retains the sampled snapshot between diagnostic reads;
live projectile queries remain exact per-tick queries.

## Deferred seams (feature gaps, IDA-cited)

- The **secondary weapon channel's own transition cross-fade** remains deferred under D-INF-1;
  its mask composition is live, but target changes still switch immediately. The PRIMARY
  locomotion/body cross-fade is **FIXED 2026-07-29**: source and target retain independent
  fixed-tick playheads, use the exact float32 10/15-tick weight, and blend before the
  weapon-mask/aim-overlay compose `[orig: AnimMap_UpdateEntity @ 0x40b5f0;
  AnimChannel_BlendTwoChannels @ 0x410740]`. Simulation root/capsule output, rendered pose,
  and authoritative organic collision consume that same transition state.
- Body-segment **aim/lean overlays** (`Entity_BuildBoneTransformMatrices @0x4b1290` bone-index switch +
  `Math_BuildFixedPointToFloatMatrix4x4 @0x612200`) — **WITNESSED IN FULL 2026-07-08** and ported for
  local/placed/remote rendering plus the synchronous organic-collision pose
  (docs/world/world-wac-ai-re.md §14/§15.8b: the bone→overlay map, mounted config table, seven blend
  matrices, and pivot recomposition). Remaining D-INF-11 scope is NPC/remote secondary-weapon threading,
  attachments, and secondary-channel blend windows; mounted selection now shares the landed seat-frame
  body/leg/pitch/roll inputs without replacing that synchronization.
  Hex-Rays renders the switch labels shifted −1 (bone 0 = default).
- Full **anim-slot table** (`Entity_ComputeAnimSlotIndex @0x43a690`, base 180 + 4·variant) and the
  player-avatar `off_8135F0` table — current selector is walk/run/idle by speed+alert.
- Free-running editor preview still advances on Godot wall-clock × clip `fps`; simulation-owned
  body playheads and their primary cross-fades are now driven by fixed half-frame ticks.
- Turn-rate clamp (sim-side infantry heading update); pitch/roll present path is ported
  but unexercised (flag for a visual check when a non-zero source exists).

## Verification

`tests/anim/anim_sample_test` (native conventions, world↔local self-consistency, shared-rest regression,
and a synthetic compressed-clip regression: a sparsely-keyed bone holds its keyframe, never identity),
`tests/world/ai_test` (state → `anim_slot`, plus a real lethal RoundSim edge that retains
the outgoing root sample and enters death at weight zero), `tests/world/infantry_test`
(exact float32 10/15-tick weights, independent playheads, A→B→C retargeting, target-only
events, and death during an active blend), GUT `skeletal_anim_test`/`object_editor_test`/
`mission_present_pass_test`. Native `mount_test` pins the exact non-cardinal seat-heading conversion,
remote seat-frame state, and the local look/body split. Focused GUT `nova_simulation_test` test
`test_local_round_damages_enemy_mounted_on_rotated_emplaced_gun` fires a local-owned round through a
rotated mounted enemy's posed section and confirms authoritative damage. Native `collision_test`
additionally pins a moved posed head (section 14),
strict reverse-scan/mask and radius rules, propagation of the primary/reaction bone into the directional
death animation, and the independent secondary normal-infantry damage-zone multiplier. It also pins the
native F3 object prefilter's organic and out-of-range rejection before matrix-provider work. GUT
`hitbox_debug_view_test` pins the person-section roles, retained shared-sphere packed
batch at the full 96×19 budget, six-Hz cadence, unchanged-snapshot cache, and real-input/label-only
invalidation. F3 omits the local avatar and bounds posed/fallback remote targets to 80 units under its
96-actor diagnostic cap. Headless dump confirms US01 and C4Ground (40+ clips, compressed) pose as humanoids
with no collapse. User-validated US01 walk/idle in the object preview.

GUT `nova_simulation_test::test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose`
reproduces the multiplayer spawn order and pins B50 `phrase_set=4` selecting US01's
`anim_emplaced_5`; it may not silently fall back to the generic `anim_emplaced` pose.

The mounted-selector slice is intended to be accepted only after focused native/GUT coverage pins:
`phrase_set` presence (including unknown versus explicit zero), every witnessed seated/gunner row,
snapshot/restore and dismount clearing, local-render/collision selector parity, placed/wire presentation
parity, and an end-to-end rendered-bone versus shot result. The focused run must also retain the rotated
mounted-enemy and CXLT regressions plus F3's 80-unit cutoff, six-Hz sampling, dense 96-actor packed batch,
local-player omission, and existing MultiMesh behavior; the enforced maturity ratchet and CI matrix remain
required release gates. This paragraph records the verification target, not a result.
