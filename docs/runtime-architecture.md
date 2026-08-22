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
    Game_ProcessMainFrame                 one 62.5 Hz logic tick
  Render_ProcessMainSceneFrame           one variable-rate render
```

OpenNova keeps that shape. Catch-up is capped at 31 ticks, render reads the
latest state without interpolation, and WAC/BMS dividers remain inside their
own systems.

## Live OpenNova path

[ADR 0036](adr/0036-one-inmatch-session-wire-first.md) splits the frame
between one portable session module and one first-class Godot pipeline:

```text
MainGame._process
  sample player input once
  GameWorld.tick(camera, delta, MissionFrameInput)
    GameFramePipeline.advance
      begin device frame
      MissionPresentation.advance_session_frame
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
      render material frame                 per-model ObjectModel advance
      render particles
      mix mission audio
      update frame clear
      finish device frame
  local-view fallback only when the world leg was skipped (probe skip / no world)
  HUD compile/apply
```

A terminal `MissionFrameOutcome` stops catch-up and returns immediately from
`GameFramePipeline`, so later device phases cannot run against a lost or failed
session. There is no callback lattice and no second legacy frame sequence.

## Ownership and seams

### Portable session

`engine/net/inmatch/session.*` owns:

- `inmatch::State` and the allowed transitions;
- role policy for single player, listen host, joiner, and dedicated host;
- fixed-step banking and the catch-up cap;
- held versus one-shot input retention across zero-tick and catch-up frames;
- reset, close, terminal error propagation, and frame timing;
- typed `FrameInput`, `TickInput`, `TickOutcome`, and `FrameOutcome` values.

Its one internal seam is `inmatch::TickTarget`. The Godot `Simulation` adapter
and `apps/nw_server` each provide a target, which proves the session interface
does not depend on Godot. The target owns the concrete mission kernel it knows
how to construct. In the game that remains `Simulation`: one `World`, WAC,
BMS, AI, collision, and the selected network runtime. This is intentional
locality—resource resolution and Godot value conversion do not leak into the
portable session state machine.

### Godot adapter and presentation

`Simulation` converts the typed input once, advances its mission kernel, and
returns typed outcomes. A single temporary per-tick sink keeps catch-up
presentation synchronous without installing persistent string-named hooks.

`MissionPresentation` owns the placed and wire present passes, entity index,
effect drains, and fixed-tick presentation signals. It owns no cadence or
playing flag. Its deterministic `tick()` test/debug entry still goes through
`inmatch::Session::step_once`; it is not a second loop.

`GameFramePipeline` owns the concrete Godot device order. It deliberately names
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
`GameFramePipeline` (`render_particle_frame`, `render_material_frame`, and the
`present_local_view_frame` leg); `ParticleRenderer`, `EffectWorld`,
`ObjectModel`, and `GameWorld` no longer run independent process loops. The
per-model advance is one static driver over a shared awake set
(`ObjectModel::advance_awake_frame`, per-frame-guarded); the menu shell and
ONED drive that same static advance from their one process loop for portrait/
preview models outside a live mission.

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
`np::host_session_pump`; joiners drive `ClientRuntime`. Both are reached
inside the session target tick. Decoded entities use the one
`ClientReplicaPipeline` path and `WirePresentPass`; authored host/SP nodes
use the placed present pass.

`npwire` is the retail compatibility boundary. `npruntime` and `netsim` are
implementation directories used inside the concrete tick targets, not
additional public lifecycle layers. The exact end-round exchange pushes
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
evaluates the universal all-zones-owned rule, TDM score/time limits, and
Advance and Secure zone counts. WAC/BMS co-op win/lose actions enter the same
`World::process_round_end` transaction. `[orig: GameEvent_ProcessScoring
@0x52F550; GameType_CreateDefaultSettings @0x52DD00; ScoreConfig_LoadFile
@0x52D8A0; Server_CheckWinConditions @0x51AD40]`

The first finish freezes a `MatchResult`; later scoring and finish attempts are
inert. The network side resolves the frozen stats through retail's field-ID
accessor, omits columns that are zero for every player, serializes the remaining
field pairs in configuration order, announces once, and drains the retail
2790-tick multiplayer linger. Script-driven co-op outcomes consume their
originating tick because retail's linger drain precedes the automatic
win-condition pass; TDM/A&S outcomes begin draining on the following tick. It
never recomputes the winner. `[orig: Server_TickUpdate @0x51D7E0;
CPlayerStats_GetFieldByIndex @0x52D630; Server_BuildEndOfRoundScoreboard
@0x508F30; Server_ProcessRoundEnd @0x5164F0]`

## Verification

Focused local coverage pins:

- lifecycle transitions, role restrictions, cadence, one-shot input retention,
  catch-up cancellation, reset, and idempotent close in
  `tests/frame/inmatch_session_test.cpp`;
- the same session interface in `apps/nw_server`;
- TDM, A&S, co-op, scoring, clocks, frozen results, and end-round wire flow in
  `tests/world/match_test.cpp`, `tests/npruntime/round_end_test.cpp`, and
  `tests/npruntime/client_runtime_test.cpp`;
- typed Godot session/presentation behavior in
  `godot/tests/mission_presentation_test.gd`;
- concrete device ordering and cancellation in
  `godot/tests/game_frame_pipeline_test.gd`, plus the real GameWorld stack's
  tick integration in `godot/tests/game_world_test.gd`;
- load cancellation and settlement in
  `godot/tests/game/main_game_lifecycle_test.gd`;
- the frame-driven parity joiner waiting on settlement rather than elapsed
  frames.

The PR CI matrix owns the exhaustive native, Python, Godot, architecture-lint,
and packaging run.
