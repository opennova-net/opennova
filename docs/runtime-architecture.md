# Runtime architecture — in-match session and Godot frame pipeline

This is the current map of one OpenNova mission. It complements `GOALS.md`
and the decisions under `docs/adr/`; unlike the historical ADRs, this file
describes only the live path.

## Target loop

NovaLogic's master loop (`Game_MainLoop @0x52b630`) banks real time in 16 ms
quanta, runs zero or more fixed logic ticks, then renders once:

```text
outer frame
  bank elapsed time
  while one 16 ms quantum is due:
    Game_ProcessMainFrame                 one 62 Hz logic tick
  Render_ProcessMainSceneFrame           one variable-rate render
```

OpenNova keeps that shape. Catch-up is capped at 31 ticks, render reads the
latest state without interpolation, and WAC/BMS dividers remain inside their
own systems.

## Live OpenNova path

[ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md) (d3/d9, superseding ADR 0036) splits the frame
between one portable session module and one first-class Godot pipeline:

```text
MainGame._process
  sample player input once
  GameWorld.tick(camera, delta, MissionFrameInput)
    GameWorld.advance_frame          the static leg table (godot/src/world/game_world_frame.cpp)
      begin device frame
      MissionRoot.advance_session_frame
        Simulation.advance_session_frame
          inmatch::Session.advance           state + bank + input retention
            0..N Simulation mission ticks   net pump + World.run_logic_tick
            typed TickOutcome values
          per-tick Godot presentation sink  effects + fixed-tick listeners
        present entity/effect rows once
      present local view                    camera/viewmodel placement (D-RORD-8)
      Terrain.render_frame                  compiled TerrainDrawList
      FoliageDispatcher.render_frame        compiled FoliageDrawList
      drive network session edges
      advance weather
      apply blink gates when a tick ran
      apply camera occlusion
      sample iris
      render material frame                 authored RLOD selection (individual models, then the
                                            placer's retained static instances), then the
                                            per-model ObjectModel advance
      sync_framefx_frame                    compile typed focused-Q3 draws after every producer published
      render slot shadows                   plan the 24/12 admission, publish the armed captures
                                            (drawn by the beauty compositor's PRE_OPAQUE pass)
      render particles
      mix mission audio
      update frame clear
      finish device frame
  local-view fallback only when the world leg was skipped (probe skip / no world)
  HUD compile/apply
```

A terminal `MissionFrameOutcome` stops catch-up and returns immediately from
the leg loop (`session` and `network` are the two stopping rows of `kFrameLegs`), so later device phases cannot run against a lost or failed
session. There is no callback lattice and no second legacy frame sequence.

## Ownership and seams

### Shared native assets

Each mounted resource source owns one `assets::AssetStore`. The Godot
`ResourceRoot` supplies that store to `MissionKernel`, `ObjectData` and
`SkeletalAnim`; a headless kernel owns a store bound to its own resource index.
Parsed 3DI models, ADM maps, BAD animations and compiled skeletal rigs are
immutable shared handles. Definition tables already retained by `ItemDatabase`
remain the mission's input instead of being loaded again.

`ResourceIndex` supplies mounted bytes and a source revision; `AssetStore`
owns parsed data. A remount, decode-policy change or explicit cache refresh
invalidates future lookups, including cached misses. Existing handles remain
valid snapshots. Active mission collision registrations and animation clocks
still belong to the mission; meshes, materials and skeleton nodes belong to
Godot. The shared store neither owns nor mutates those instances.

Animation loading and evaluation live in `runtime/anim`, entity setup in
`runtime/mission`, model collision geometry and attachment poses in
`runtime/world`, and first-person viewmodel rules in `runtime/renderer`.
[ADR 0044](adr/0044-shared-native-assets.md) records the ownership decision.


### Portable session

`engine/runtime/inmatch/session.*` owns:

- `inmatch::State` and the allowed transitions;
- role policy for single player, listen host, joiner, and dedicated host;
- fixed-step banking and the catch-up cap;
- held versus one-shot input retention across zero-tick and catch-up frames;
- reset, close, terminal error propagation, and frame timing;
- typed `FrameInput`, `TickInput`, `TickOutcome`, and `FrameOutcome` values.

Its one internal seam is the `inmatch::Role` the session binds (ADR 0043 d3: `LocalRole`, `HostRole`, `JoinerRole`). Both embedders — the Godot
`Simulation` binding and `apps/nw_server`'s dedicated host — embed the engine's
`mission::MissionKernel` and run `inmatch::HostRole::run_tick` (ADR 0042 d3,
PR #587), so boot, state and the no-net tick have one implementation and the
session interface provably does not depend on Godot. The target adds only
resource resolution, Godot value conversion and the device pipeline; none of
that leaks into the portable session state machine. The kernel also carries the
session facts the tooling and the shell flow used to re-derive: `session_open`
(retail's is_in_session, set by the net bring-ups; it gates the UseGun
null-slot rejection inside `toggle_mount`), the medic-call cooldown
(`tick_medic_cooldown` / `stamp_medic_request`), the local dead bit
(`local_player_dead`; a joiner reads its replica through
`inmatch::ClientRuntime::local_player_dead`), the water plane the occupant clamp
reads (`sync_water_plane` from `World::env.water_z`), and the per-tick
environment advance, which runs inside `HostRole::run_tick` and `LocalRole::run_tick`
rather than in each embedder. The headless embedders load their terrain
through the engine's one `terrain::terrain_field_store_load`.

That `World` is constructed in place and keeps one address for the mission.
It is deliberately non-copyable and non-movable because `EntityCommands`
binds it, its default network sink is an embedded member, and registered
systems retain mission-lifetime relationships. This prevents a returned or
relocated world from splitting registry state from the commands and systems
that operate on it; `tests/world/world_test.cpp` enforces the invariant.

### Godot adapter and presentation

`Simulation` converts the typed input once, advances its mission kernel, and
returns typed outcomes. A single temporary per-tick sink keeps catch-up
presentation synchronous without installing persistent string-named hooks.
While the profiling clocks run it also folds the session phase spans onto the
`FrameStats` board the shell hands it (ADR 0039).

The dev tools (`engine/runtime/devtools`, F3) sit outside this order: the
imgui-godot addon calls `ImGui::NewFrame` at the lowest process priority and
renders at the highest; the `DevTools` node runs the engine's layout pass in
a `_process` just under that render, after every game callback of the frame,
so the tool windows read the frame the game just produced. Closed tools cost
nothing: the Stats window arms the board's capture only while it is visible.

In a debug windowed run with the addon present, `GameRuntimeRoot`
(`godot/game/game_runtime_root.tscn`, the project's main scene) keeps
`MainGame` inside one always-updating `SubViewport`: with the tools closed the
`SubViewportContainer` composites it directly, and with them open the
container's composite is hidden while the same texture is drawn as the
workspace's Game window at the size that window requests (F3 through the Game
texture closes the workspace). Release, headless and addon-less runs hand
`MainGame` to the tree directly instead (ADR 0039; the `runtime_root_window`
and `window_fullscreen` probes pin both arrangements).

`MissionRoot` owns the placed and wire present passes, entity index,
effect drains, and fixed-tick presentation signals. It owns no cadence or
playing flag. Its deterministic `tick()` test/debug entry still goes through
`inmatch::Session::step_once`; it is not a second loop.

`GameWorld`'s static leg table (`kFrameLegs` in `godot/src/world/game_world_frame.cpp`, its literal order pinned by `frame_leg_names()`) owns the concrete Godot device order. It deliberately names
the renderer, audio, particle, environment, and presentation operations we
ship. We do not add a generic renderer interface for a hypothetical backend.

### Lifecycle

`inmatch::State` is authoritative for mission play:

- `Unloaded -> Connecting -> Loading -> Running` for joiners;
- `Unloaded -> Loading -> Running` for other roles;
- local single player alone may enter `Paused`, step, or reset;
- network roles continue running under overlays and reject pause/reset;
- terminal tick failure enters `Failed`;
- idempotent close crosses `Stopping` and ends at `Unloaded`.

`MainGame.State` is only shell/UI mode. `GameWorld._world_ready` is an
installation invariant for Godot render resources, not a competing play state.
The old `Simulation.loaded_/playing_`, presentation play flag, self-process
loops, and duck-typed legacy tick path are gone.

### Input

`PlayerInputRouter` samples movement, look, fire, and reload once per outer
frame into `MissionFrameInput`. `inmatch::Session` replaces held state with the
newest sample, accumulates look/pressed edges across zero-tick frames, consumes
those edges on the first successful tick, and reuses held state for every
catch-up tick. Camera/listener state travels in the same typed frame value.

### Shutdown

`WorldLoadCoordinator` owns each load's `WorldLoadOperation`, loading-screen
presentation, synchronous loader invocation, and separate `cancelled` and
`settled` edges. Cancellation stops preparation; settlement is deferred until
the coordinator's coroutine has returned and released the bound load Callable.
MainGame then closes the world/session, releases renderer resources while
RenderingServer is live, clears the mounted root, and quits. EXIT_TREE retains
an idempotent synchronous fallback. There is no reflective coordinator or
fixed-frame drain.

## Draw-list architecture

The proven deep render modules remain unchanged:

- `TerrainFrameCompiler -> Terrain.render_frame`;
- `FoliageFrameCompiler -> FoliageDispatcher.render_frame`;
- `HudFrameCompiler -> HudOverlay`;
- `MenuFrameCompiler -> MenuFrame`.

These modules own traversal, ordering, and typed draw records. Godot owns asset
upload and draw application. Particle rendering, per-model material eval, and
the local-player view placement are all explicitly invoked by
the leg table (`render_particle_frame`, `render_material_frame`, and the
`present_local_view_frame` leg); `ParticleRenderer`, `EffectWorld`,
`ObjectModel`, and `GameWorld` no longer run independent process loops. The
per-model advance is one static driver over a shared awake set
(`ObjectModel::advance_awake_frame`, per-frame-guarded); the menu shell and
its portrait models drive that same static advance from the game process loop
outside a live mission.

D-RORD-8 (fixed 2026-08-12): the one-frame visibility lag was the CAMERA, not
the occlusion-after-present order. The local-player camera/viewmodel placement
is now the `present_local_view_frame` leg, run inside the pipeline right after
the session tick and before terrain/foliage, so every camera-driven render leg
reads the render camera this frame's tick produced (`GameWorld` reads the live
camera, not the frame-entry stash). Occlusion staying after present is the
correct ownership order — present re-asserts base visibility, occlusion layers
hides, Godot renders after both.

ADR 0033 R3 remains closed not taken: visual parity did not justify an abstract
seven-pass render-command stream.

## Network shape

Single player is still an in-process listen server
([ADR 0011](adr/0011-single-player-in-process-listen-server.md),
[ADR 0012](adr/0012-player-is-host-side-server-entity.md)). Authority roles pump
`inmatch::host_session_pump`; joiners drive `ClientRuntime`. Both are reached
inside the role's tick (`HostRole::run_tick`, `JoinerRole::run_tick` — the
joiner frame owns its socket seam, the decoded-row asset resolution through
the kernel and the local-player pumps; the shell keeps only the loadout
profile seams, ADR 0043 d3). A joiner's decoded entities use the one
`ClientReplicaPipeline` path and `EntityPresenter`. The listen host presents
its own pools (its loopback 0x0A is retail's header-only frame, D-NET-140):
authored rows through the placed present pass, runtime-spawned rows through
`EntityPresenter`.

`npwire` is the retail compatibility boundary and `net/` is wire only (ADR
0043 d4). The in-match runtime lives above it in `runtime/inmatch` (the
session, the listen-host frame, the server/client state machines and frame
loops, the transports) and `runtime/replication` (the world<->wire seam and
the client replica state); neither is an additional public lifecycle layer.
The exact end-round exchange pushes
`0x61` then recipient-specific `0x1D`, followed by requester-only
`0x2B`/`0x56` pulls in at most 200-byte chunks. The client connection owns the
pull loop and folds the immutable board into client state.

## Match gameplay

`world::Match` is the authoritative gameplay owner shared by every session
role. It holds the configured game type, clock, 42-field retail player stats,
team rows, roster, and the ordered `score.ini` `FIELD` schema. Enemy kills,
deaths, teamkills, suicides, and zone captures mutate the witnessed signed stat
indices. One shared retail-default table feeds both gameplay and S2C 0x58 when
`score.ini` is absent; a VERSION 40 file overlays that table and may
intentionally materialize an all-zero row. At the 1 Hz authority pass Match
services hill occupancy and dropped-flag returns. Per tick it services flag
contacts drained from `CollisionWorld`'s exact successful MoveCB/non-Powerup
movement-collision stream; it does not perform a second radius/overlap query.
Mission promotion stamps a type-6006 hill's authored waypoint distance into
the same entity+0 Q16 radius that the authority reads, and the shared pool-3
load stream carries that raw field so joining clients materialize the same
marker. This remains one entity-data path rather than a KOTH-specific network
adapter. `[orig: Entity_SpawnFromBMSRecord @0x40F157..0x40F173;
Server_UpdateCaptureZoneProximity @0x5089E8..0x508A68;
serialize_entity_pool_to_packet @0x503593..0x5035A9;
NapiNPClientMsg_0x020 @0x425D07..0x425D1B]`
Its winner check implements the universal all-zones-owned rule and
every retail competitive branch: DM/TDM, KOTH/TKOTH, S&D/A&D, CTF, FlagBall,
Flag Me, and A&S/C&C. WAC/BMS co-op win/lose actions enter the same
`World::process_round_end` transaction; the host regression executes WAC
`Win` for stock Co-op and a BMS `RedWin` event for Objective Co-op before
checking the common 0x61/0x1D output. `[orig: WacAction_Win @0x4ED4A0;
EventAction_Dispatch EventAction_Dispatch @0x4542E0, the RedWin case @0x454495; GameEvent_ProcessScoring
@0x52F550; Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5;
GameType_CreateDefaultSettings @0x52DD00; ScoreConfig_LoadFile
@0x52D8A0; Server_CheckWinConditions @0x51AD40]`

The one typed host request carries the gameplay inputs needed by those
branches: game type, kill/flag/hill limits, KOTH delta, dropped-flag return
time, and two/four-side selection. Godot and the headless server install one
shared retail-default set before the request crosses into `GameConfig`; there
is no per-mode adapter or fallback rule layer. Authored BMS modes normally
select the game type. Flag Me is explicitly selectable because retail keeps
the task-12-to-type-8 launch case even though its BMS flag mapper cannot
produce task 12. `[orig: Config_SetDefaults @0x54D030;
AI_GetTaskTypeFromFlags @0x40DAE0; Game_StartMission @0x524360]`

The first finish freezes a `MatchResult`; later scoring and finish attempts are
inert. The network side resolves the frozen stats through retail's field-ID
accessor, omits columns that are zero for every player, serializes the remaining
field pairs in configuration order, announces once, and drains the retail
2790-tick multiplayer linger. Script-driven co-op outcomes consume their
originating tick because retail's linger drain precedes the automatic
win-condition pass; automatic multiplayer outcomes begin draining on the following tick. It
never recomputes the winner. `[orig: Server_TickUpdate @0x51D7E0;
CPlayerStats_GetFieldByIndex @0x52D630; Server_BuildEndOfRoundScoreboard
@0x508F30; Server_ProcessRoundEnd @0x5164F0]`

## Verification

Focused local coverage pins:

- lifecycle transitions, role restrictions, cadence, one-shot input retention,
  catch-up cancellation, reset, and idempotent close in
  `tests/frame/inmatch_session_test.cpp`;
- the same session interface in `apps/nw_server`;
- every retail game type, co-op, scoring, objectives, clocks, frozen results,
  and end-round wire flow in
  `tests/world/match_test.cpp`, `tests/npruntime/round_end_test.cpp`, and
  `tests/npruntime/client_runtime_test.cpp`;
- typed Godot session/presentation behavior in
  `godot/tests/mission_root_test.gd`;
- concrete device ordering and cancellation in
  `godot/tests/world_frame_order_test.gd` (the literal leg names, the replay list, the stopping legs), plus the real GameWorld stack's
  tick integration in `godot/tests/game_world_test.gd`;
- load cancellation and settlement in
  `godot/tests/game/main_game_lifecycle_test.gd`;
- the frame-driven parity joiner waiting on settlement rather than elapsed
  frames.

The PR CI matrix owns the exhaustive native, Godot, architecture-lint, web,
launcher, and packaging run.
