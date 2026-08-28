# ADR 0042: Godot is the permanent shell; one mission kernel; engine facts through engine functions

- **Status**: accepted (2026-08-28; maintainer directive — the boundary exploration)
- **Owners**: runtime architecture, the Godot layer, tooling (MCP/F3)
- **Supersedes/updates**: closes ADR 0033's R4 rung and supersedes its d1 device triad; updates
  ADR 0016 (fully historical), ADR 0020 d4 (the terrain-field provider), ADR 0035 (d5 made
  permanent), ADR 0036 d2 (the TickTarget implementer list), ADR 0039 d3 (the record/request
  channel), ADR 0040 d6 (the consumer list) and its ladder rows A3/B, ADR 0041 d5 (the rig
  reference). ADR 0028 d1 (four groups), ADR 0029 (five targets, the PUBLIC chain), ADR 0034 d6
  (MCP tooling stays GDScript) stand unchanged.

## Context

The project once kept a door open to a pure-DX12, no-Godot build of the game. The maintainer has
closed it: it will probably never happen, and the record should stop paying for it. At the same
time the one hard requirement on the tooling — MCP and F3 use core engine functions, never invent
parallel logic — has no mechanical form: the mission runtime is hand-assembled three to five
times (godot/src/simulation; tests/common/retail_mission_rig, whose 1,329 Godot-free lines copy
~15 Simulation methods verbatim; apps/nw_server, which boots no terrain, tables or collision; two
more boots in tests/mission), the engine has no inspection API (the ~50 Dictionary getters on
Simulation and a GDScript join in debug_entities.gd are the inspection API), and the witnessed
`Player_CanFireWeapon @0x5cf780` port's only `[orig:]` cite ships in the TEST rig while the
shipping copy in simulation_player.cpp is uncited — a live violation of the one-implementation
rule.

A greenfield review (every ADR treated as changeable) confirmed which constraints are real and
which were inherited: the four-group order and the "runtime never includes net" rule are
dependency-driven (the no-net tick has three headless consumers that must not link the wire
stack); a fifth engine group, ImGui windows in godot/src, and deleting nw_server were all argued
and rejected on evidence, not on precedent.

## Decision

1. **Godot is the permanent, sole shell; the pure-DX12/no-Godot build is dead.** ADR 0033 R4 is
   CLOSED (not "indefinitely optional"); d1's RenderBackend/InputSource/AudioSink triad — tokens
   that exist nowhere in code — is superseded by the real seams: `inmatch::TickTarget`,
   `GameFramePipeline`, the draw-list compilers + native appliers. ADR 0040 d6 loses "and any
   future non-Godot front-end". ADR 0016 is historical in full ("export-a-game future" no longer
   exists in GOALS.md; the "bypass sweep" instrument has no script). "Portable" means
   headless-runnable by the engine's real consumers: the ctest suite and the apps/ binaries.
   R3 stays CLOSED with its reopen clause, now decoupled from any backend: a reopened R3 would be
   for draw-order fidelity inside Godot, never a second renderer.

2. **The boundary rule.** A fact a headless ctest can reproduce without Godot is an engine fact:
   it is computed in exactly one engine function with one `[orig:]` cite, and Godot, MCP, F3 and
   probes reach it only through that function — a binding forwards and converts into a typed
   record; it never composes, re-derives, validates, or dispatches by name. Everything that
   exists only because a Godot node, server, viewport, window, socket, clock, input event or
   audio player exists is a device fact, written as typed Godot code beside its owner; it enters
   an engine window only as a VALUE slot or a typed record the embedder pushes, and a window's
   mutations leave as typed requests the embedder drains. `GameFramePipeline`'s `_world: Node` is
   the one sanctioned untyped seam (it exists for the pipeline's GUT double). This restates ADR
   0033 d3 and ADR 0034 d2 and replaces ADR 0031 §3's per-TU dispositions.

3. **One mission kernel, one listen-host frame.** `mission::MissionKernel`
   (`engine/runtime/mission/mission_kernel.*`) is the engine's one boot + state + no-net tick:
   the promoted body of the retail-mission rig — World, AiSystem, WacSystem, BmsEventSystem,
   CollisionWorld, OcclusionWorld, SimModelCache, AdmRootMotion, the terrain field store, seat
   specs, ai-profile defaults, the weapon/ammo/score tables, and the local-player frame (the
   cited CanFire verdict, the pre/post-tick pumps, the stance latch, the look accumulator). It is
   the ONE filler of `run_mission_boot`'s functor table (the table stays as the ctest-locked
   order). `inmatch::listen_host` (`engine/net/inmatch/listen_host.*`) is the host bring-up /
   request drain / per-tick frame over a kernel, an `IDatagramSocket&` and plain arguments — the
   one cited `Game_ProcessMainFrame` order. The embedders are `Simulation` (the Godot
   `TickTarget`, which OWNS a kernel), `apps/nw_server` (DedicatedHost), and the ctests
   (`tests/common/retail_mission_files` supplies retail paths only). No `MissionRuntime` class,
   no RuntimeDevices/ITickObserver/ISaveBytes interfaces: `host_session_pump` already takes the
   socket and plain arguments, and ADR 0036 d4 rejects mirror facades. The joiner role stays in
   the binding until a headless joiner assertion is wanted.

4. **The terrain-field provider is the engine.** The one cpt/trn(+charmap) → TerrainHeightField +
   SurfaceTypeMap builder lives in `engine/runtime/terrain_query/terrain_field_store.*` behind a
   format-free header on the ADR 0020 seam list; `TerrainData`, the kernel and the tests call it.
   (Amends ADR 0020 d4, which had named the Godot resource the provider.) The table builders
   (`weapon_table_build`, `ammo_table_build`, `score_rules_build`) live in `runtime/world` —
   nothing wire in them.

5. **Inspection and control are engine functions.** `world::inspect` (EntityRow, EntityCard,
   `entity_directory`) and the `EntityCommands` mutators are the only path a tool reads or
   mutates an engine fact; the binding exposes them as typed RefCounted records (the
   `MissionFrameOutcome` pattern) and `to_json_value()` exists only at the MCP boundary. The
   GDScript debug-control registry is a typed table (rows with read/write/invoke closures and an
   `owner: engine|device` field; authority from `Session::role()`); reflective
   `callv`/`call`/`get` dispatch by StringName is retired. The MCP transport stays GDScript
   (ADR 0034 d6): its only Godot needs — TCPServer and frame pacing — are exactly the device
   facts a Godot shell should own.

6. **F3 windows: records in, typed requests out.** An engine window reads a typed record the
   embedder pushes and emits typed requests the embedder drains (the `GameWindowRequest` /
   `OnedAction` pattern); the `DevTools` node holds the `Simulation` in C++ (set on world load,
   nulled on unload, exactly like the stats board) and does the push/drain with no GDScript
   relay. No feed framework is built ahead of a window: each window lands with its own record.
   Godot device facts still arrive as VALUE slots the shell samplers feed (ADR 0039 d3, now
   scoped to Godot facts). `imgui*.h` includes are linted to `engine/runtime/devtools` and
   `tests/devtools` (previously the containment was a single CMake PRIVATE keyword).

7. **One cite marker.** The `(retail: ...)` device-note convention is retired: a witness citation
   is `[orig: Name @0xADDR]` everywhere, and the counters (`adapter_cpp_orig_cites_*`,
   `gd_orig_cites` — now including `godot/probes`) are non-increasing. The marker rewrite that
   let 117 citations leave the pushdown gauge is no longer an exit: only code that moves banks a
   counter. `godot/game/mcp` carries a zero-cite floor (a converter that needs a witness cite is
   re-deriving).

8. **Named options, not slices** (a later maintainer may take them without re-arguing): retiring
   `apps/nw_server` once a headless `--lan-host` serve mode is proven; lifting netsim+npruntime
   out of `net/` so `net/` means wire (≈377 includes / 159 files; 63 protocol tests and nw_pp
   stop linking the sim); device-fact ImGui windows in godot/src if device windows ever pile up.

## Consequences

- The record stops contradicting itself: R4 vs "never will be", the phantom triad, the
  future-front-end clause, the ratchet's lifetime, and godot/game's double listing in CONTEXT.md
  are all resolved by this ADR.
- ADR 0040's ladder rows A3 and "B (remainder)" become the kernel/listen_host slices; the ladder
  survives as the on-touch queue for the remaining witnessed blocks (present rows, sun-visibility
  feed, scar gate, the menu/HUD/armory GDScript).
- The GUT doubles survive; `godot/tests` keeps driving the shell through typed seams
  (`MissionFrameInput`/`MissionFrameOutcome`, the pipeline's sanctioned `_world` double,
  the `ProbeShellSeams` verbs).
- The dead-code sweep (Slice 0 of the campaign) and the stop-inventing fixes (SP pause through
  `inmatch::State::Paused`, `session_role()` as the authority source, engine constants for the
  512/62.5 literals) land under this ADR's rule.

## Verification

- The campaign's slices each land green (full ctest, GUT with single-file isolation, the ten CI
  lints); the Simulation-embeds-kernel slice is additionally proven by the retail-interop recipe
  both directions and an `nw_pp` before/after decode of the 00TRg loopback pcap (byte-identical
  S2C stream over the first 2,000 ticks).
- Landed hashes are recorded here as the slices merge.
