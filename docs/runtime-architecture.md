# Runtime architecture — the consolidated mission loop

This is the map of how OpenNova runs a mission, and how it lines up with the original
NovaLogic engine so IDA ports drop in cleanly. It complements `GOALS.md` (the why) and
the ADRs under `docs/adr/` (specific decisions).

## The original main loop (target shape)

The master loop is a **fixed-timestep accumulator** (`Game_MainLoop @0x52b630`): it banks
real elapsed time and dispatches the simulation update at a constant **62.5 Hz (16 ms)**,
**decoupled** from rendering, which runs once per outer iteration at the variable frame rate:

```
Game_MainLoop @0x52b630  (per outer iteration; render rate)
  bank real elapsed time (16 ms quantum)
  while banked >= one quantum:                          fixed-timestep catch-up (0, 1, or N)
      Game_ProcessMainFrame (one 62.5 Hz engine tick)   input -> net -> per-system dividers (WAC/BMS/AI)
  Render_ProcessMainSceneFrame (once)                   draw each entity, then audio
```

A long frame runs **multiple** sim ticks; a short frame runs **zero**; the accumulator is
clamped at ~500 ms / ~31 ticks against the spiral of death, and there is **no inter-tick
render interpolation** (render reads current entity state). The engine tick itself is
`Game_ProcessMainFrame @0x5263f0` (`current_tick @0x24c1968`). Dividers are **per system**,
inside each system: the WAC VM (`sub_4F81A0 @0x4f81a0`) executes once per **62 ticks** (the
`0x3E` divider), normal BMS events run a 16-tick gate over a quarter cursor, and the
AI/entity motor runs every tick — witnessed in
[bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6. OpenNova mirrors this shape.

## How OpenNova maps onto it

```
main_game.gd / editor _process(delta)
  -> GameWorld.tick(camera, delta)                  host frame (game)
       foliage dispatch                             client render pass
       MissionRuntime.tick_realtime(delta)          == the fixed-timestep server tick + entity render:
         bank delta; for each banked 16 ms quantum:   [Game_MainLoop @0x52b630 accumulator, 62.5 Hz]
           NovaSimulation.step()                        one engine tick; per-system dividers  [Game_ProcessMainFrame @0x5263f0]
             World.run_logic_tick()                       WAC -> BMS -> AI over one world
           drain effects -> effects_drained             host-presentation side effects (per tick)
         MissionPresentPass.present()                  draw each entity once after the batch (transform/PANM/visibility)
       NovaMissionAudio.tick(camera)                 audio render pass
```

The editor "Play the mission" goes through the **same** `MissionRuntime` + `MissionPresentPass`
and the same `tick_realtime` accumulator (the editor preview self-ticks via `_process`). There is
one runtime, one present pass, one entity index — see [ADR 0006](adr/0006-unified-mission-runtime-present-pass.md).
The single-tick `MissionRuntime.tick()` survives as the deterministic primitive for editor Step,
the MCP, and tests.
`GameWorld` (game) and `MissionController` (editor) are **sibling hosts** of that one runtime: exactly one
`NovaSimulation` per host context is intentional, not duplication.

## Layers

- **Logic (portable C++)** — `libs/world` `World` + `ISystem`s (WAC VM, BMS events, AI) over one
  shared registry + var store + `EffectLog`. `World::logic_tick` is the 62 Hz engine tick
  (`Game_ProcessMainFrame @0x5263f0`, `current_tick @0x24c1968`); dividers live inside each system —
  the WAC VM runs once per 62 ticks (`WacSystem::kTicksPerExecution`, the `0x3E` divider of
  `sub_4F81A0`), BMS events run a 16-tick gate over a quarter cursor, and the AI motor runs every
  tick (witnessed in [bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6/§2).
- **Binding (C++ GDExtension)** — `NovaSimulation` wraps the World, exposes transport
  (`step` = exactly one 62 Hz logic tick, `restart`), `drain_effects`, and **one batched present snapshot**
  (`get_present_snapshot()` → a flat `PackedFloat32Array`, `PF_*` field layout) so the per-tick
  present loop makes one call, not ~10 Variant-boxed scalar getters per entity.
- **Runtime driver (GDScript)** — `mission_runtime.gd` owns `{sim, present pass, index}` and
  single-sources the per-tick order (advance → drain → present). `tick_realtime(delta)` is the
  fixed-timestep accumulator (banks `delta`, runs 0..N 62.5 Hz ticks, presents once); `tick()` is
  the deterministic single-tick primitive (Step / MCP / tests). Both `game_world.gd` (game) and
  `mission_controller.gd` (editor) drive it.
- **Present pass (GDScript)** — `mission_present_pass.gd` applies each entity's transform + PANM part
  channels + visibility onto its placed node. Hybrid: the engine decides the state (snapshot), the
  host writes the `Node3D`. The basis convention is single-sourced in
  `MissionObjectPlacer.bms_to_godot_basis` (`Entity_SpawnFromBMSRecord @0x40eb66` +
  `Math_BuildFixedPointMatrixFromEulerAngles @0x613f40`).
- **Audio** — name-keyed sound sets (`SoundProfile_FindLoadedByName @0x5274f0`); the member-selection
  state machine lives in portable `libs/audio` ([ADR 0004](adr/0004-audio-selection-pushdown.md)).

## Two animation systems (do not conflate)

- **PANM (procedural part anim)** — the vehicle/emplacement part system (turret/dish), driven by the
  `PLAYPARTANIM` mission action, case `0x22` of `Entity_ApplyCommand @0x43ab60`. The phase is
  integrated **in-engine** (`AiSystem::advance_part_anim`) and the present pass poses it via
  `set_part_phase`. **Implemented.**
- **`.bad` / `.adm` (main skeletal/skinning)** — the primary infantry/view-model animation (walk/idle/
  fire), selected by AI state. **Implemented** (see
  [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md)): portable `libs/anim` samples `.bad` clips
  (per-bone keyframe-duration walk, engine-native Y-up), `NovaSkeletalAnim` builds the bind from the
  BadBone matrix, and `NovaObjectModel` drives a `Skeleton3D` + rest-derived `Skin` (skinned organics +
  fake-skinned rigid weapons). Mission NPCs pick a clip from `Entity.anim_slot` (`libs/world`
  `body_anim.h`), driven by AI state (`Entity_UpdateInfantryAI @0x4b9910`); the present pass routes it via
  `play_body_anim`. Pose chain: `BoneAnim_FindKeyframeAtTime @0x410220` → `build_world_bone_matrices
  @0x40c770` → `Entity_BuildBoneTransformMatrices @0x4b1290`. The player-avatar `off_8135F0` slot table and
  the two-channel aim blend remain deferred (ADR 0007). (`AnimMap_PlayAnimBySlot @0x40bda0` is the
  weapon/recoil channel layer, not the skeletal evaluator.)

## Known gaps / deferred seams

- Main-body skeletal `.bad`/`.adm` runtime is now implemented
  ([ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md)); deferred within it: player-avatar
  `off_8135F0` slot table, two-channel upper/lower-body blend, aim/lean overlays, fixed-tick playhead.
- Present transform is **yaw-only**; pitch/roll are reserved fields in the snapshot (`PF_PITCH_DEG`/
  `PF_ROLL_DEG`, emitted as 0) gated behind a basis-parity check.
- The fixed-62.5 Hz accumulator is **implemented** (`MissionRuntime.tick_realtime`): the sim runs
  at a constant rate decoupled from the render frame rate, faithful to `Game_MainLoop @0x52b630`
  ([bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6 / §2a). There is no inter-tick
  render interpolation — entities step at 62.5 Hz and the present pass writes current state once per
  host frame; this matches the original (which also does not interpolate), so it is a faithful
  property, not a gap.
- Audio: reverb preset table (`Audio_LoadReverbDefs @0x766d80`) and MUS music
  (`AudioVM_OpenMusicContext @0x6722a0`) are seams; dialog-id resolution stays host-side (it is bound
  to `NovaDbfData`).
- The exact main-loop / entity-render order is cited from existing RE notes; a focused `grill-ida`
  pass to pin `sub_4F81A0`'s surroundings + the entity-render function is a tracked follow-up.
