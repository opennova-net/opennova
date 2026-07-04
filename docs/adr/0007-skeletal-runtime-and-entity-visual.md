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

**`NovaEntityVisual` contract.** The mission present pass duck-types each placed entity node:
`transform`, `set_part_phase` (PANM), `set_entity_visible`/`visible`, and `play_body_anim(slot)`. Mission
NPCs select a body anim from AI state: `libs/world` `body_anim.h` maps `Entity.anim_slot` → an `anim_*`
`.adm` key; `MissionObjectPlacer` auto-loads each animated entity's `.adm`. No churn to the registry /
selection / drag / undo (ADR 0006 invariant).

**Grill verdict (`Jointops.exe.kong`): DIVERGENT (core matches).** Confirmed faithful: quat layout, slerp
(with the small-angle nlerp fast path), the keyframe-duration walk, the bind matrix, the shared-rest FK, the
`(-x,y,z)` handedness, no animation root motion, no standalone 180° flip, and the fixed-point separation.
The single correctness fix the grill produced is convention #3 (per-bone keyframe-duration sampling).

- **Facing** stays in the present-pass placement basis (`MissionObjectPlacer.bms_to_godot_basis`); there is
  **no** flip in the skeleton or model. The "apparent 180°" is the composition of the X-negation handedness
  + the Y-negated Z-X-Y euler + the render-space transpose, already reproduced for the validated yaw-only
  case. Bones stay posed in raw model space; the entity world matrix is applied at placement.
- **Root motion** is **not** applied — entity world position is sim-driven (`Entity_UpdateInfantryAI` AI
  target-seeking + `Entity_ProcessCollisionAndPlatformPhysics`). The `.bad` translation channel (flag bit
  `0x2`) is model-space bone deformation only; `BadEvent.velocity` has no position consumer.
- **Bone record sizes**: the on-disk `.bad` bone (100 B, raw IEEE floats) and the runtime skeleton bone
  (108 B, fp16.16 with `parentIndex@+40`, pos@`+56/60/64`) are distinct records; `*1/65536` is the
  runtime/entity path only, never the `.bad` float path. Our parser reads raw floats; we never decode one as
  the other.

## Consequences

Skinned organics and rigid view-model weapons animate from one path; mission NPCs walk/idle from AI state;
the object workspace has an `.adm` preview (the smoke-test gate). Dense clips (e.g. US01) are byte-unchanged
by convention #3; compressed clips (C4* etc.) that previously collapsed now pose correctly.

## Deferred seams (feature gaps, IDA-cited)

- Two-channel **upper/lower-body blend** (`AnimChannel_BlendTwoChannels @0x410740`) — for aim/walk
  separation; single clip is fine for AI walk cycles.
- Body-segment **aim/lean overlays** (`Entity_BuildBoneTransformMatrices @0x4b1290` bone-index switch +
  `Math_BuildFixedPointToFloatMatrix4x4 @0x612200`) — needs the 3DI bone-def pivot table.
- Full **anim-slot table** (`Entity_ComputeAnimSlotIndex @0x43a690`, base 180 + 4·variant) and the
  player-avatar `off_8135F0` table — current selector is walk/run/idle by speed+alert.
- **Cross-fade** on slot change (currently a hard cut); needs the two-channel blend first.
- Playhead advances on Godot wall-clock × clip `fps`, not the fixed sim tick (`flt_A78354`) — a timing-parity
  follow-up; visually fine at `fps`.
- Turn-rate clamp (sim-side, `Entity_ProcessCollisionAndPlatformPhysics`); pitch/roll present path is ported
  but unexercised (flag for a visual check when a non-zero source exists).

## Verification

`tests/anim/anim_sample_test` (native conventions, world↔local self-consistency, shared-rest regression,
and a synthetic compressed-clip regression: a sparsely-keyed bone holds its keyframe, never identity),
`tests/world/ai_test` (state → `anim_slot`), GUT `skeletal_anim_test`/`object_editor_test`/
`mission_present_pass_test`. Headless dump confirms US01 and C4Ground (40+ clips, compressed) pose as
humanoids with no collapse. User-validated US01 walk/idle in the object preview.
