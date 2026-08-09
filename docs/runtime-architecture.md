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
inside each system: the WAC VM (`WacScript_AdvanceTick @0x4f81a0`) executes once per **62 ticks** (the
`0x3E` divider), normal BMS events run a 16-tick gate over a quarter cursor, and the
AI/entity motor runs every tick — witnessed in
[bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6. OpenNova mirrors this shape.

## How OpenNova maps onto it

Since [ADR 0033](adr/0033-engine-owned-loops-device-shells.md) (R1, 2026-08-09) the loop
itself is engine code: `engine/runtime/frame` `FrameDriver` owns the bank/catch-up/
present-once shape AND the fixed per-frame leg order; the shells install device legs
(node writes, GPU dispatch, audio players) as hooks and read perf spans back.

```
main_game.gd
  -> GameWorld.tick(camera, delta)                  the device shell: stashes camera state,
       MissionRuntime.tick_realtime(delta)          gates on the transport, then drives
         Simulation.frame_realtime(delta)       ONE engine frame:
           frame::FrameDriver.run_frame             [orig: Game_MainLoop @0x52b630]
             terrain leg                              (engine TerrainFrameCompiler draw list -> Terrain applies; ADR 0033 R2)
             foliage leg                              (engine FoliageFrameCompiler draw list -> dispatcher applies; ADR 0033 R2)
             listener stamp                           (fire-sound gate, before the batch)
             bank delta; for each banked 16 ms quantum:
               Simulation.step()                  one engine tick  [Game_ProcessMainFrame @0x5263f0]
                 host_pump/joiner pump                 net drain -> Server_TickUpdate owns the tick
                   World.run_logic_tick()              WAC -> BMS -> AI over one world
               begin effect tick / throwable sync / drain effects / fixed_tick_completed
             present ONCE after the batch             (rows-only on a zero-tick frame)
               MissionPresentPass / WirePresentPass / Fire / Destruction / Throwable
             net-drive leg                            (session edges, join-wire gate)
             weather leg                              (the distinct 62 Hz TOD clock)
             blink-gates leg                          (only after a batch that ran)
             occlusion-frame leg                      (camera-driven, every render frame)
             iris leg                                 (marched exposure samples)
             audio leg                                (mission audio + music var pump)
```

`GameWorld`, entered through `MainGame`, is the sole live owner of
`MissionRuntime`. ONED has no embedded mission simulation: F5 launches the
normal standalone game, F6 launches the current saved top-level loose `.bms`,
and F8 stops the one managed child. Launch reads saved loose assets from the
mounted resource directory and never saves, exports, copies, or stages editor
state. See [ADR 0025](adr/0025-standalone-game-is-the-only-live-mission-runtime.md).

The per-tick and per-frame ORDER is pinned by `tests/frame/frame_driver_test.cpp`
(engine, hook-trace) — changing the sequence is now an engine change with a failing
test, not a GDScript edit. A paused runtime and the duck-typed test runtimes
(implementing `tick()` alone) take a legacy explicit sequence in `game_world.gd`
with the same leg order.

Inside that one live runtime there is one entity index and one present *step* —
the surviving core of
[ADR 0006](adr/0006-unified-mission-runtime-present-pass.md). That step has
grown into a fixed sequence of per-system passes, each owning one drain of the
sim, and each installed only where its role applies: a host or single-player
session runs the full ladder. A production joiner instead starts from the exact
616-byte S2C `0x0B` header, with no authored mission-body nodes. Its decoded
`0x10`/`0x0D`/`0x20` load stream materializes native pools 2/1/3 at the host's
exact packed handles for deploy, mount, collision, and other world-side consumers;
remote pool-0 organics remain decoded client state. `WirePresentPass` renders every
remote row from that state because no local placed-node identity exists, while
`MissionPresentPass` remains responsible for authored nodes on hosts, single-player,
and explicit complete-BMS/debug joins. The decoded fire/throwable passes follow the
same sequence. Adding a system means adding a pass to that sequence, not a second
present loop.
The single-tick `MissionRuntime.tick()` survives as the deterministic primitive
for runtime debug/MCP controls and tests; it delegates to the driver's
`run_single` (listener, one step, the per-tick legs, one present — no frame legs,
no accumulator).

## The render half (transition state)

The render FRAME has no engine counterpart yet — that is ADR 0033's spike-gated
stage R3 (the witnessed seven-pass order lives in
[render/render-order-re.md](render/render-order-re.md); the portable ordering
math in `engine/runtime/renderer` currently has no caller for its sort keys
because Godot's scene renderer owns the sort). Two R2 domains are cut over:

- Terrain: `opennova::TerrainFrameCompiler` (`engine/runtime/terrain/terrain_frame.h`)
  owns the per-frame walk — the 512-unit sector window, quadtree traversal,
  foliage detail-cell handoff, front-to-back order, patch budget, and
  LOD-family resolve — and `Terrain.render_frame()` applies the typed
  patch draw list onto its RenderingServer instance pool from the frame's terrain
  leg (ordered before foliage, whose applier consumes the draw list's detail
  cells the same frame).
- Foliage: `renderer::FoliageFrameCompiler` (`engine/runtime/renderer/foliage_frame.h`)
  owns the silhouette anchor gate, both retail placement algorithms,
  per-identity vertex expansion, per-submission uniform state, and both retail
  wind clocks; `FoliageDispatcher` applies the typed `FoliageDrawList`
  (ArrayMesh uploads for the draw list's mesh builds, draw-node pooling, material
  binding, the eviction lifecycle).

One per-frame render loop remains deliberately SELF-DRIVEN outside the engine
frame until its output becomes a draw list (stage R2):
`ParticleRenderer`'s `_process` (frame compile + compositor dispatch);
per-model material eval self-parks on the native `ObjectModel`
(`godot/src/object`), whose event-driven runtime frame wakes only when it
holds live work. The occlusion leg runs AFTER the present in the frame order
above — the scene-graph-ownership inverse of retail's collect-then-submit —
and stays that way until R2/R3 make the retail order expressible.

## Single-player is a listen server

There is no no-net path. Single-player constructs the same in-process host the LAN and NovaWorld
paths use, and the local player is a host-side server entity driven by a wire-shaped intent
([ADR 0011](adr/0011-single-player-in-process-listen-server.md),
[ADR 0012](adr/0012-player-is-host-side-server-entity.md)). The consequence for this map: the net
pump is inside the 62.5 Hz tick for *every* session — `Simulation.step` routes the authority
roles through `np::host_session_pump`, whose `Server_TickUpdate` owns the logic tick (the
former NetSystem-as-ISystem seam retired at P8; D-NET-123/125) — `local_player_presenter.gd` feeds
intent rather than writing entity state, and the wire encoders run in single-player exactly as they
do for a joined client. The in-match runtime behind that seam is `engine/net/npruntime`
([ADR 0013](adr/0013-consolidated-net-core.md)); the wire record is
[net/novaworld-net-re.md](net/novaworld-net-re.md).

Decoded entities now have one client-state path
([ADR 0026](adr/0026-one-client-replica-pipeline.md)): the live `ClientRuntime`
reduces S2C messages through `ClientReplicaPipeline`, which exposes the `PF_*`
projection to `wire_present_pass.gd`. (The replay/spectate consumer —
`NovaNetClient` + `ReplicaHistory` — was removed 2026-08-05 as unused dev
scaffolding; see the ADR's amendment.) `NovaWorldClient` remains the
matchmaking/handoff client and does not own a second gameplay entity model.

## Layers

- **Logic (portable C++)** — `engine/runtime/world` `World` + `ISystem`s (WAC VM, BMS events, AI) over one
  shared registry + var store + `EffectLog`. `World::logic_tick` is the 62 Hz engine tick
  (`Game_ProcessMainFrame @0x5263f0`, `current_tick @0x24c1968`); dividers live inside each system —
  the WAC VM runs once per 62 ticks (`WacSystem::kTicksPerExecution`, the `0x3E` divider of
  `WacScript_AdvanceTick`), BMS events run a 16-tick gate over a quarter cursor, and the AI motor runs every
  tick (witnessed in [bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6/§2).
- **Binding (C++ GDExtension)** — `Simulation` wraps the World, exposes transport
  (`step` = exactly one 62 Hz logic tick, `restart`), `drain_effects`, and **one batched present snapshot**
  (`get_present_snapshot()` → a flat `PackedFloat32Array`, `PF_*` field layout) so the per-tick
  present loop makes one call, not ~10 Variant-boxed scalar getters per entity.
- **Frame (portable C++)** — `engine/runtime/frame` `FrameDriver` owns the loop: the
  62.5 Hz bank/catch-up/present-once batch, the per-tick leg order (effect rebind,
  throwable sync, drain, fixed-tick broadcast), and the post-batch frame-leg order
  (net-drive, weather, blink, occlusion, iris, audio) — pinned by
  `tests/frame/frame_driver_test.cpp` (ADR 0033 R1).
- **Shell composition (GDScript)** — `mission_runtime.gd` composes `{sim, present
  passes, index}`, installs the shell device legs on the sim
  (`set_frame_shell_hooks`), and exposes the transport (`play`/`pause`/`step_once`,
  the lockout predicate); its `tick_realtime`/`tick` are thin delegates over
  `Simulation.frame_realtime`/`frame_single`. `game_world.gd` is its only live
  owner and installs the world legs (`set_frame_world_hooks`); every hook binds a
  NODE (never a RefCounted presenter) so a leaked instance cannot crash teardown.
  ONED authoring previews do not drive a mission runtime.
- **Present passes** — `mission_present_pass.gd` applies each entity's transform + PANM part
  channels + visibility onto its placed node. Hybrid: the engine decides the state (snapshot), the
  shell writes the `Node3D`. Its per-row hot loop (row plan, snapshot reads, change-gated dispatch)
  is native — `PresentApplier` (`godot/src/simulation/nova_present_applier.cpp`), with the
  GDScript file as the shell-facing facade and the node-side visual contract (ADR 0007) still
  GDScript; the aim-overlay/emplaced-weapon adapters delegate to the same native statics so the
  mission and wire passes share one implementation. The basis convention is single-sourced in
  `MissionObjectPlacer.bms_to_godot_basis` (`Entity_SpawnFromBMSRecord @0x40eb66` +
  `Math_BuildFixedPointMatrixFromEulerAngles @0x613f40`); the native twin is pinned to it by
  `mission_present_pass_test.gd`'s basis parity case. Measured on the ASH_I5A spawn (143-marker / ~200-model vantage, matched ~10 ms frames): the runtime present leg 1.73 -> 1.21 ms avg; under CPU contention the native walk degrades far less (3.3 -> 1.5 ms at ~22 ms frames).
  The WIRE pass (`wire_present_pass.gd`, the MP twin) follows the same split: its
  per-row hot walk — plan validity, snapshot reads, body-anim arbitration, the
  rigid held-weapon attach (parity-pinned against `present_held_weapon.gd`'s
  reference math) — is native (`nova_present_applier_wire.cpp`, same class),
  while the facade keeps the cold path: spawn/defer/unresolved bookkeeping, the
  liveness prune, held-weapon model builds, spawn callbacks, and stats.
  The sibling passes listed in the map above
  follow the same rule for their own systems: each reads a drain or snapshot the sim produced and
  writes shell nodes/effects, so the simulation itself stays render-free and headless-testable.
  Local-player presentation (viewmodel, aim overlay, HUD feed, view effects) hangs off
  `local_player_presenter.gd` and the `world/player_*` / `present_*` scripts on the same principle.
- **Audio** — name-keyed sound sets (`SoundProfile_FindLoadedByName @0x5274f0`); the member-selection
  state machine lives in portable `engine/runtime/audio` ([ADR 0004](adr/0004-audio-selection-pushdown.md)).

## F3 frame-stat diagnostics

The shipped F3 **Stats** tab observes the same runtime rather than installing a
second update loop. A shell-owned `FrameStatsBoard` is the fixed-slot boundary:
opening the tab emits one capture edge, producers add integer microseconds (or
plain counts), and the pane drains a roughly half-second window. Multiple
62.5 Hz catch-up ticks written during one render frame are folded before the
peak comparison, so the displayed peak is a worst **render frame**, not a
worst individual fixed tick.

Capture is default-off. The close edge disables native runtime profiling and
both measured viewports immediately; leaving their parent trees performs the
same teardown. While closed, the trace/net/occlusion paths take no diagnostic
clock reads. While open, `MissionRuntime` samples projectile attribution
without a per-tick `Dictionary`: `Simulation` returns value types through
`get_last_projectile_trace_times_us()` (`terrain/static/dynamic/person`),
`get_last_projectile_trace_counts()` (`calls/static/dynamic/person
survivors`), and `get_last_projectile_trace_faces()` (`static/dynamic`).
The UI labels their sum **Projectile trace (attributed)** because water and
owner/exclusion setup are intentionally outside those native timing buckets.

## Two animation systems (do not conflate)

- **PANM (procedural part anim)** — the vehicle/emplacement part system (turret/dish), driven by the
  `PLAYPARTANIM` mission action, case `0x22` of `Entity_ApplyCommand @0x43ab60`. The phase is
  integrated **in-engine** (`AiSystem::advance_part_anim`) and the present pass poses it via
  `set_part_phase`. **Implemented.**
- **`.bad` / `.adm` (main skeletal/skinning)** — the primary infantry/view-model animation (walk/idle/
  fire), selected by AI state. **Implemented** (see
  [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md)): portable `engine/runtime/anim` samples `.bad` clips
  (per-bone keyframe-duration walk, engine-native Y-up), `SkeletalAnim` builds the bind from the
  BadBone matrix, and `ObjectModel` drives a `Skeleton3D` + rest-derived `Skin` (skinned organics +
  fake-skinned rigid weapons). Mission NPCs pick a clip from `Entity.anim_slot` (`engine/runtime/world`
  `body_anim.h`), driven by AI state (`Entity_UpdateInfantryAI @0x4b9910`); the present pass routes it via
  `play_body_anim`. Pose chain: `BoneAnim_FindKeyframeAtTime @0x410220` → `build_world_bone_matrices
  @0x40c770` → `Entity_BuildBoneTransformMatrices @0x4b1290`. The player-avatar `off_8135F0` slot table and
  the two-channel aim blend remain deferred (ADR 0007). (`AnimMap_PlayAnimBySlot @0x40bda0` is the
  weapon/recoil channel layer, not the skeletal evaluator.)

## Known gaps / deferred seams

- Main-body skeletal `.bad`/`.adm` runtime is now implemented
  ([ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md)); deferred within it: player-avatar
  `off_8135F0` slot table, two-channel upper/lower-body blend, aim/lean overlays, fixed-tick playhead.
- Present transform carries full pitch/yaw/roll: `PF_PITCH_DEG`/`PF_ROLL_DEG` are live in the
  snapshot (from `Entity.pitch`/`Entity.roll`, or the client attachment pose for a mounted entity)
  and consumed by all three entity present passes. The former yaw-only restriction is closed.
- The fixed-62.5 Hz accumulator is **implemented** (`frame::FrameDriver`, driven through
  `MissionRuntime.tick_realtime`): the sim runs
  at a constant rate decoupled from the render frame rate, faithful to `Game_MainLoop @0x52b630`
  ([bms-event-runtime-re.md](mission/bms-event-runtime-re.md) §1.6 / §2a). There is no inter-tick
  render interpolation — entities step at 62.5 Hz and the present pass writes current state once per
  render frame; this matches the original (which also does not interpolate), so it is a faithful
  property, not a gap.
- Audio: reverb preset table (`Audio_LoadReverbDefs @0x766d80`) and MUS music
  (`AudioVM_OpenMusicContext @0x6722a0`) are seams; dialog-id resolution stays shell-side (it is bound
  to `DbfData`).
- The exact main-loop / entity-render order is cited from existing RE notes; a focused `grill-ida`
  pass to pin `WacScript_AdvanceTick`'s surroundings + the entity-render function is a tracked
  follow-up (TODO.md § Cleanup & verification backlog).
