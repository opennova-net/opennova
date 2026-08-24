# In-match Session and Godot Frame Pipeline master plan

> **Status:** COMPLETE
>
> **Delivery:** one coordinated cutover PR; no compatibility period and no
> second production frame path.
>
> **2026-08-22 amendment:** ADR 0036 deepened and renamed the portable owner
> to `opennova::inmatch::Session`, removed the old API outright, and placed
> gameplay rules/results in `world::Match`. The text below names the final
> architecture rather than preserving obsolete API spelling.

## Goal

Replace the callback-driven frame facade with two deep modules:

```text
MainGame._process
  -> sample player input once
  -> GameFramePipeline                     Godot device ordering
       -> stamp camera onto sampled input
       -> terrain + foliage
       -> inmatch::Session.advance()   portable lifecycle, cadence, sim, net
       -> typed tick/frame outcomes
       -> presentation, particles, environment, audio
  -> local-player post-present + HUD        shell/UI ordering
```

`inmatch::Session` owns one match's lifecycle, network-role policy, fixed
cadence, input deposit, reset, and teardown. Its one internal
`inmatch::TickTarget` seam owns the concrete mission kernel. `GameFramePipeline` is the
first-class Godot owner of rendering and the other device phases. The proven
draw-list compilers remain. ADR 0033's R3 render-command stream remains closed
NOT TAKEN.

## Target interface

The portable module lives in `engine/net/inmatch/session.*`, the
lowest existing layer that may depend on both the runtime world and the
network runtime without reversing the engine link graph.

It exposes typed values for:

- `inmatch::State`: `Unloaded`, `Connecting`, `Loading`, `Running`, `Paused`,
  `Stopping`, `Failed`;
- `inmatch::Role`: single-player, listen host, joiner, dedicated host;
- one sampled `InputPacket` and `FrameInput` per outer frame;
- immutable `TickOutcome`, `FrameOutcome`, `SessionError`, and
  `TransitionResult` values.

One-shot input executes at most once on the first catch-up tick and remains
pending across a zero-tick frame. A terminal frame result stops catch-up and
suppresses every later Godot device phase. `close()` is idempotent. The module
is direct-C++ only and adds no flat-C export.

## The six changes

### 1. Deep portable session

- Move lifecycle, role policy, cadence, input retention, terminal errors, and
  teardown orchestration into `inmatch::Session`.
- Keep mission-kernel construction behind the single `inmatch::TickTarget` seam;
  retain the existing portable `runtime_boot` module rather than leaking
  Godot resource resolution into the session state machine.
- Make `apps/nw_server` use `inmatch::Session` in dedicated-host mode, proving
  that the interface is portable and not a Godot-shaped extraction.

### 2. Typed frame values

- Delete `FrameHooks`, `FrameDriver`, Callable registration, and frame
  Dictionaries.
- Make `inmatch::Session::advance(FrameInput)` bank wall clock, run zero to 31
  fixed ticks, and return one `FrameOutcome` with immutable per-tick packets.
- Replace `_frame_aborted` with a terminal outcome.
- Sample input/camera once. Order terrain, foliage, session advance, per-tick
  presentation, final presentation, weather, blink, occlusion, iris, particles,
  and audio in `GameFramePipeline`; keep local-player post-present and HUD as
  the explicit shell/UI tail in `MainGame`.

### 3. One lifecycle owner

- Make `inmatch::State` authoritative; remove the loaded/playing flags in
  `Simulation` and `MissionRuntime`. Keep only GameWorld's render-resource
  installation invariant, renamed `_world_ready`.
- Keep MainGame's state only as a shell-mode enum for menus and overlays.
- Allow pause, manual step, and baseline reset only for local single-player.
  Network overlays submit neutral input while the session keeps running.
- Route connect, load, session loss, reload, stop, and close through typed
  transitions and signals.

### 4. First-class Godot pipeline

- Add `GameFramePipeline` under `godot/game/world`; MainGame performs shell work
  and calls it once per frame.
- Delete `mission_runtime.gd`; move portable behavior into the session and
  keep the renamed `MissionPresentation` as the Godot-only present owner
  invoked by the frame pipeline.
- Disable the particle/effect/GameWorld self-driven frame loops and invoke
  them explicitly from the pipeline.
- Keep the terrain, foliage, HUD, menu, entity, and particle draw-list seams.
  Do not add a generic renderer abstraction.

### 5. Event-driven shutdown

- Give each world load a typed, cancellable operation with `cancelled` and
  `settled` signals.
- Make loading-screen preparation complete or cancel through one signal.
- Delete the reflective shutdown coordinator. MainGame cancels the load,
  closes the session, awaits settlement, releases renderer resources while
  RenderingServer is alive, clears the mounted root, and quits.
- Retain an idempotent EXIT_TREE fallback; remove the fixed four-frame drain.

### 6. Documentation and residue

- Add ADR 0035 and rewrite the runtime architecture around the actual split.
- Correct the stale Godot layout, hook names, WAC placement, and instruction
  files.
- Remove the duplicate `hud_math.cpp` CMake entry and the completed shutdown
  TODO.
- Mark this file as this completed-effort record.

## Cutover and verification

The implementation branch may use ordered internal commits, but only one
cutover PR lands. The final tree has no aliases, compatibility shims, callback
frame, or duplicate lifecycle path.

Focused local checks cover session lifecycle/cadence, headless hosting, the
real Godot session/frame-pipeline integration, cancellation during loading, and
the affected draw-list paths. The PR CI matrix owns the final exhaustive
native, Python, Godot, architecture-lint, and packaging run.

Completion searches must find no live `FrameHooks`, `FrameDriver`, frame
registration methods, `DRAIN_FRAMES`, `MissionRuntime`, `Simulation.playing_`,
or reflective shutdown access.
