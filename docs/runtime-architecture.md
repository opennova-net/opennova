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

OpenNova keeps that shape with retail's own bank (`world::TickAccumulator`,
the one bank the game and the dedicated host both drive): a long
frame's backlog is low-passed over the following frames by the 7/8 frame-time
smoother instead of run as one burst of catch-up ticks, a bank over 500 ms
clamps, and the mission start banks neither the load nor the render time of
the first three frames drawn after it (`inmatch::Session::advance`, fed the
time since the last render by the shell's `frame_post_draw` stamp). Render
reads the latest state without interpolation, and WAC/BMS dividers remain
inside their own systems.

### One logic tick

Retail's `Game_ProcessMainFrame @0x5263F0` runs, in order:
`Client_ProcessNetworkFrame @0x42C180` (the call `@0x526692`),
`Sound_TickPendingSlots @0x529310` (`@0x526697`), then
`Server_TickUpdate @0x51D7E0` (the call `@0x5266B6`): its head timers, the
per-player walk (`@0x51D88B`), the receive pump (`@0x51D895`), the humans count
(`@0x51D89A`), the WAC tick (`@0x51D8BF`), the every-32 legs (the spawn-marker
assignment `@0x51D8D2` and the idle timers `@0x51D8D7`), the BMS quarter pass
(`@0x51D8F4`), the scoreboard legs (`@0x51D90D` / `@0x51D912`), the play-tick
walk with its per-slot scoring call (`@0x51D94B..0x51D9A3`, ahead of the
`is_in_session` test `@0x51D9A5`, so single player counts too), the periodic second (`@0x51DB6D..0x51E1B2`,
which also recounts the live groups), the per-slot 0x0A (`@0x51E3E4`) and the
send pump (`@0x51E487`). Then come the gated `Entity_UpdateAllEntities @0x4C2100`
(the call `@0x52674B`), the tracers (`@0x526758`), the weather (`@0x526774`), the
camera (`@0x526781`) and the WeaponAction pump (`@0x526786`). A 0x0A therefore
carries this frame's script and maintenance changes, and the entity update's
inline sends lead the NEXT frame's queue.

The port keeps that split. `Server_TickUpdate` routes the previous entity pass's
records at its head (`route_entity_pass_records`), runs `World::run_script_pass`
and its maintenance, builds and queues the 0x0A, then runs
`World::run_entity_pass` (the gated entity update and the tail that advances
`logic_tick`); `World::run_logic_tick` composes the script and entity passes for
the bare local role, a joiner and the tests. The weapon-action walk is one pass
per frame after the weather and the camera, outside the entity-update gate:
`World::pump_weapon_actions`, which `HostRole` and `LocalRole` call after their
weather and view legs whatever the phase (pool 0 in slot order, then each
unoccupied EWEAP row whose slot is still hot; a joiner walks its replica slots
instead) `[orig: WeaponAction_ProcessAllEntities @0x542690, the call @0x526786]`.
The live group recount (`EntityPool_RecountLiveByGroup @0x40E8D0`)
runs from the periodic second only, so the bare local role no longer recounts.
`World::update_all_entities` walks pool 1 once per row in slot order with each
row's parent chain first, then the phases retail runs after it (the HeliLift
slots, the facial state, precipitation, the death pieces, the timed AI events,
the projectiles, the explosion queue with the round-hit drain, pool 2, the doors,
pool 3, the proximity tables and the pool-0 walk); see the vehicle record,
section 26.1.

The whole entity update is admitted in `Game_ProcessMainFrame @0x5263F0`
(`@0x526703..0x526742`): a client skips to the second half; a playing host on its
death screen skips the humans test (`is_mp_session_peer`, `g_death_screen_active`);
no human and a started script clock skip the update (`wac_var_humans`,
`wac_var_ticks`); the retained pre-round byte skips it; and in session the
round-over latch `g_spawn_success_gate` skips it (raised by
`Server_ProcessRoundEnd @0x5164F0`, the S2C 0x1D handler `NapiNPClientMsg_0x01D
@0x430840` and `Cine_StartPlayback @0x577840`, cleared at
`Game_StartMission @0x524360`).
`wac_var_humans` counts live pool-0 rows with Flags 0x100 and not Flags 1
(`Server_BuildEntitySlotLists @0x4F97A0`, `@0x4F9815` / `@0x4F9820`): a dead
player counts, a hidden or not-yet-deployed one does not. Its definition test
(`@0x4F9809`) only tests for an allocated row, because every spawn links an
items.def row, row 0 for a type items.def lacks (`ItemList_FindIndexByTypeId
@0x49E100`, the miss `@0x49E131`).
Port: `World::entity_update_admitted`, with the death-screen exemption stamped by
`HostRole` (`CachedFrameState::peer_death_screen`). The update also returns at its head,
on every peer, while there is no local player entity (`cmp g_local_player_entity,0`
`@0x4C2110`, to the epilogue `@0x4C2642`), before the epilog branch: a retail world with no
local player never runs its entity update even when the frame admits it. A running retail
mission always has one (`Game_StartMission` calls `Player_InitPlayer` on every peer,
`@0x525BBC`), the port's dedicated hosts have none, and the port keeps the gate only in
front of the precipitation fall (world-wac-ai-re §38.9). While the epilog screen is up,
`Entity_UpdateAllEntities @0x4C2100` takes its epilog path (`@0x4C211D..0x4C2128` →
`@0x4C239A`): every pool-1 row copies its pose into savedLivePose, only a row whose
occupant carries Flags 0x100 is updated, and everything from the HeliLift slots
through pool 3 is skipped, so no projectile, explosion or death piece moves under
the MISSION FAILED screen; the pass then joins the proximity tables and the
pool-0 walk and tail-calls `Cinematic_EpilogUpdate @0x577950` in place of the entity-update
counter's add (`@0x4C2624` / `@0x4C2634`).

The round clock is not frozen by the round end: `g_round_time_remaining` is
decremented in `Game_ProcessMainFrame` (`@0x5265DA..0x526602`) under the authority,
no pause, no epilog, no pre-round delay and time left only, so it keeps counting
through the post-round linger. `tick` itself advances only while the in-game
menu pause flag is clear (`@0x5265A0..0x5265B4`); the tracers and the weather run
every frame, paused or not, while the camera and the WeaponAction pump do not.

Mission start: `Game_StartMission @0x524360` collects the spawn vehicles and
builds the spawn-marker list right after the mission load (the
`build_spawn_marker_budget_list @0x529B40` call `@0x5252C6`), ahead of
`Entity_InitAllFromModels @0x40E460` (`@0x52567F`), the authority-gated PreMission
pass (the `EventTrigger_UpdateAllWithFlag2 @0x454DC0` call `@0x525B86`) and the WAC's first
execution; `MissionKernel::boot` builds it once the definitions are attached and
before the organic init. Two boot-order differences remain open. The local
player spawns before the PreMission pass, where retail's
`Player_InitPlayer @0x4E15F0` call (`@0x525BBC`) follows the pass, and the later
boot legs (the `.adm` bind, the item traits, the loadout, the organic init) act
on that row, so moving the spawn reorders them too. The gunner attachments bind
in `initialize_mission_vehicles`, after the PreMission pass, the first WAC run
and the 255 weather ticks, where retail binds them in the class inits, before
the pass and on every peer (`Entity_InitVehicleAIFromDef @0x4686C0` and
`Entity_InitHelicopterAIFromDef @0x4683C0` call
`Entity_SetupGunnerAttachments @0x468100`, `@0x468964` / `@0x468692`); the port's
bind needs the gun points the collision boot step fills. No shipped JOX item
sets Parent. The PostMission pass runs once at teardown
(`MissionKernel::run_post_mission_pass` from `HostRole::close` and
`LocalRole::close`; `Game_TeardownMission @0x522350`, the call `@0x52266C`).

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
      present local view                    camera/viewmodel placement (D-RORD-8); while the
                                            NVG composite is up, the NVG scene raster
      scene environment                     the pass fog/ambient for this render eye, and the
                                            world pass's 0.2 near plane beside the far plane
      environment nodes                     weather smoothing, sun direction, sky dome, celestial
      Terrain.render_frame                  compiled TerrainDrawList
      water                                 the strip, the mirror and the wakes
      drive network session edges
      apply blink gates when a tick ran
      apply camera occlusion                the collector, section masks and sub-pixel floor
      FoliageDispatcher.render_frame        compiled FoliageDrawList; the MODEL anchors are the
                                            entities the occlusion walk just admitted
      sample iris
      sun veil
      point-light select
      render material frame                 authored RLOD selection (individual models, then the
                                            placer's retained static instances), then the
                                            per-model ObjectModel advance
      sync_framefx_frame                    compile typed focused-Q3 draws after every producer published
      render slot shadows                   plan the 24/12 admission, publish the armed captures
                                            (drawn by the beauty compositor's PRE_OPAQUE pass)
      render particles
      precipitation                         the streaks, published to the overlay stage
      scene overlay                         the post-particle tail (NVG laser beams, precipitation,
                                            coronas, water glint, underwater murk, sun glare) as
                                            one immutable frame for each view's overlay pass
      plan screen effects                   the FrameFX screen-effect plan (distortion, damage/death
                                            blur, bloom, thermal/monitor, the NVG view) from the
                                            local view's frame facts; after particles and
                                            precipitation so the distortion gate reads this frame
      mix mission audio
      update frame clear
      environment cube
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
session facts the tooling and the shell flow used to re-derive: retail's
`is_in_session` as `rules.mp_session`, which a listen host, a dedicated host and
a joiner set and single player never does. The in-session readers key on it,
among them `AiSystem::is_in_session`, stamped from it per entity update, for the death
event's in-session arm (`EntityAI_ProcessAirStateMachine @0x4581B0`,
`@0x458273`), the friendly-tag team mode (`HUD_DrawCrosshair @0x592640`,
`@0x5926C0..0x5926D0`), the zoom floor outside a session
(`Player_AdjustWeaponZoomLevel @0x4DBCC0`, `@0x4DBD0C`) and the unarmed UseGun
rejection (`Entity_AttachToUseGunSlot @0x546B80`, `@0x546BF6..0x546C0D`); the
former `session_open` flag, which every net bring-up set, the single-player
listen server included, is gone. The kernel also carries the
medic-call cooldown
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

`MissionRoot` leases one immutable native `SimulationPresentSnapshot` for
both placed and wire walks: float rows, door side table, and the exact layout
revision travel together. `Simulation` reuses its storage only when no reader
holds the previous snapshot. A nested callback that requests another snapshot
gets fresh storage, so the outer walk remains valid; reset also leaves an
outstanding lease valid. Row identity comparison reuses storage and compares
fields, not a hash. The bound `get_present_snapshot()` /
`get_present_door_phases()` script/tooling boundary copies into Godot packed
arrays, and two native cold paths also take that full build-and-copy route:
`MissionRoot::for_each_present_node` and
`LocalPlayerVisuals::prewarm_loaded_model_challenge_definitions`. Each such
call is an extra build, and on a joiner it also consumes that frame's
animation pulses: every path shares the same native builders and the
consume-once joiner animation pulses.

This lease is host implementation, not a reconstructed retail object. Retail
walks its native pool base, used count and stride directly
`[orig: collect_visible_entities_for_terrain @ 0x5C8C60]`
(`@ 0x5C8CAA`, `@ 0x5C8CB2`, `@ 0x5C8CB6`). The change removes the extra
packed-array allocation/copy between two native consumers. See
[03TR frame costs](perf/03tr-frame-costs.md) for measurement and the real
`MissionRoot` callback-reentrancy regression. An anchored entry comment at
`0x5C8C60` records this traversal and the host-code boundary in the saved IDB.

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

`MainGame.State` is only shell/UI mode. `GameWorld::world_ready_` (behind `is_loaded()`) is an
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

While the NVG composite is up, the local-view presenter renders the world into
its projection target at `world::nvg_view_projection`'s raster (the surface's
own 3D pass off), and GameWorld's clear leg switches to the NVG scene's
fog-colour clear (`EnvironmentState::nvg_scene_clear_color`). The scene
overlay stage and the FrameFX screen effects run inside each view's
compositor chain: the view's particle pair, then `SceneOverlayCompositorEffect`
(`renderer/scene_overlay.h`), then `FrameFxCompositorEffect`
([render-order-re.md](render/render-order-re.md), the 2026-09-24 rendering
parity pass).

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
Its winner check implements the all-zones-owned rule (which answers only in
A&S and C&C, `ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920`) and
every retail competitive branch: DM/TDM, KOTH/TKOTH, S&D/A&D, CTF, FlagBall,
Flag Me, and A&S/C&C. WAC/BMS co-op win/lose actions enter the same
`World::process_round_end` transaction; the host regression executes WAC
`Win` for stock Co-op and a BMS `RedWin` event for Objective Co-op before
checking the common 0x61/0x1D output. `[orig: WacAction_Win @0x4ED4A0;
EventAction_Dispatch @0x4542E0, the RedWin case @0x454495; GameEvent_ProcessScoring
@0x52F550; Entity_MovementCollisionResolver @0x4B2BD0 (@0x4B2F8D..0x4B2FF5);
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
