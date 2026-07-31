# ADR 0006 — Unified mission runtime present pass

Status: accepted for the game runtime; the embedded editor-host portion was
superseded by [ADR 0025](0025-standalone-game-is-the-only-live-mission-runtime.md)
on 2026-07-29.

> The game-side host class `NovaWorld` discussed here was renamed `GameWorld`
> in June 2026 (the `NovaWorld` name now belongs to the online service); this
> record uses the new name.
>
> This ADR records the 2026-06 consolidation as it was made. Its one runtime
> present pass and entity index remain current inside `GameWorld`. References
> below to editor Play, self-ticking, rewind-on-Stop, and an editor-owned
> `NovaSimulation` are historical: ADR 0025 later removed embedded mission
> execution from ONED.

## Context

A prior change consolidated the mission **logic** onto one faithful tick (`libs/world` `World` +
`TickService` + `ISystem`s WAC→BMS→AI, modelled on `WacScript_AdvanceTick`). But the layer **above** the logic —
"drive the sim and draw its entities onto the scene" — was still split into two divergent paths:

- **Game**: `game_world.gd` → `MissionCommandHost` applied **only PANM part-anim phase**, keyed by
  `bms_id` via `MissionEntityRegistry`. In-game NPCs did not move (static-placed).
- **Editor "Play"**: `mission_sim_driver.gd` `_apply()` applied **only position + yaw**, keyed by
  `(kind, index)` via a `_pickable` array. No part-anim.

Each rendered a different half of the same entity state, through a separate `NovaSimulation`, with its
own loop and its own entity→node index. There were ~7 parallel entity→node indexing schemes. The two
single-action/object-preview part-anim integrators added more duplication. `game_world.gd` even
claimed in a comment to be "the one path both go through" while the editor bypassed it entirely.

## Decision

One runtime, one present pass, one present index — both products go through them.

- **`mission_runtime.gd`** (Node) owns `{ NovaSimulation, MissionPresentPass, MissionEntityRegistry }`
  and single-sources the per-tick order: **advance logic → present → drain effects**. The game drives
  it explicitly from `GameWorld.tick()` (DIVIDED, 62-frame divider); the editor self-ticks it via
  `_process` while playing (EVERY_PROCESS). Stop rewinds the world (`World::restore`) **and** restores
  the authored node transforms captured at setup.
- **`mission_present_pass.gd`** applies each entity's **transform + PANM channels + visibility** from
  one batched `NovaSimulation.get_present_snapshot()` (a flat `PackedFloat32Array`, `PF_*` layout —
  avoids ~10 Variant-boxed scalar getter calls per entity). Hybrid split: **C++ decides the present
  state, one GDScript presenter writes the `Node3D`.** The basis is built through the single placement
  convention `MissionObjectPlacer.bms_to_godot_basis`.
- **`MissionEntityRegistry.resolve(bms_id, kind, index)`** is the one present index: `bms_id` primary
  (stable when a mission loads from disk), `(kind, index)` fallback (the editor sims the in-memory
  `bms::File`, where `bms_id` may be 0).

`MissionCommandHost` and `MissionSimDriver` are deleted.

## Consequences

- In-game NPCs now move from the sim (new behaviour); the editor preview now also shows PANM +
  visibility. Both come from one code path.
- Editing is untouched: `_pickable` `(kind,index)`, MultiMesh-slot rewrite, pick colliders, selection,
  drag, and undo keep working. The runtime writes only animated nodes' transform/phase/visible during
  play; on play it `cancel_drag()` + `disarm_placement()`; on stop it restores authored transforms.
- `MissionEntityRegistry` is kept (now also the present index) — the editor's single-action PLAYPARTANIM
  preview still uses its `resolve_single`/`resolve_group`/`resolve_zone`.

### Amendment (2026-07-29)

ADR 0025 supersedes the editor-host consequences above. ONED no longer creates
or transports a live mission runtime, so editor Play/Stop transform ownership
and rewind behavior no longer exist. The game-side result remains: `GameWorld`
owns the consolidated runtime, present sequence, and entity index. Authoring
previews may still reuse public render/data seams, but mission validation runs
in the managed standalone game from saved loose assets.

## Deferred seams

- **Main-body skeletal `.bad`/`.adm`** — **superseded by [ADR 0007](0007-skeletal-runtime-and-entity-visual.md)**:
  the skeletal runtime is now built (`libs/anim` + `NovaSkeletalAnim` + `Skeleton3D`/`Skin`), the present
  pass drives `play_body_anim(slot)`, and the re-anchored pose chain is `BoneAnim_FindKeyframeAtTime
  @0x410220` → `build_world_bone_matrices @0x40c770` (not `AnimMap_PlayAnimBySlot @0x40bda0`, which is the
  weapon/recoil + player-avatar `off_8135F0` layer).
- **Pitch/roll** are reserved snapshot fields (yaw-only today), gated behind a basis-parity check.
- No **inter-tick interpolation** for the 62-frame game cadence yet.

## Verification

`tests/mission_present_pass_test.gd` (transform/PANM/visibility/fallback/options), additions to
`nova_simulation_test.gd` (snapshot shape + scalar-parity), `mission_entity_registry_test.gd`. Full GUT
suite green (730 passing). End-to-end: game NPCs walk + PANM poses + dialog audio fires; editor Play
moves + poses, Stop restores, selection/drag/undo intact, single-action preview works.
