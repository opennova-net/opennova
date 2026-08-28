# ADR 0035: MissionSession with a first-class Godot frame pipeline

- **Status**: superseded in part by ADR 0036 (2026-08-22). Decisions 2-6
  remain; decision 1's name, namespace, and location are replaced by the
  full-cutover `opennova::inmatch::Session`.
- **Amended**: PR #468 (2026-08-11) moved local-player camera/viewmodel
  placement into the pipeline as `present_local_view_frame`; decision 2's
  "MainGame retains the explicit local-player and HUD shell/UI tail" now
  covers only a fallback for frames that skip the world leg, plus the HUD/UI
  tail.
- **Updated**: [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (2026-08-28) makes decision 5 permanent — no alternate shell, ever — and
  its kernel slices supply the concrete mission kernel
  (`mission::MissionKernel`) the session drives.
- **Owners**: runtime architecture
- **Amends**: ADR 0033 decisions 1-2 and ADR 0034 decision 3

## Context

ADR 0033 correctly moved fixed cadence and draw-list compilation out of
GDScript, but its `FrameDriver` interface is seventeen semantic callbacks.
The only production caller rebuilds those callbacks from Godot Callables each
frame, while Godot still owns camera/input sampling, mission state, abort
handling, and several self-driven frame loops. Deleting `FrameDriver` would
put nearly the same order back into `GameWorld`, so it is a shallow module.

The portable engine already has a real cross-embedder seam: `npruntime` owns the
host/client protocol state and both Godot and `apps/nw_server` provide concrete
tick targets. Session orchestration belongs there, above the tick target and
below its two real embedders.

## Decision

1. `opennova::np::MissionSession` is the portable owner of one mission's
   lifecycle, network-role policy, fixed cadence, input deposit, and typed
   tick/frame results. Its single internal seam, `MissionTickTarget`, owns the
   concrete mission kernel; Godot's target owns `World`/WAC/BMS/AI/network
   resources and the dedicated server supplies its own headless target.
2. Godot's `GameFramePipeline` owns world-device ordering directly through typed
   Godot references. It accepts one sampled input record, stamps the camera,
   calls the session once, then applies draw lists, effects, environment,
   particles, and audio. MainGame retains the explicit local-player and HUD
   shell/UI tail after the world pipeline returns.
3. The portable interface returns values. It does not accept a bundle of
   semantic callbacks and does not expose internal seams for tests.
4. Session lifecycle has one state machine. Shell menu/overlay mode remains a
   separate Godot concern and never substitutes for session state.
5. Godot is still the sole renderer. No generic renderer interface,
   alternate shell, or ordered render-command stream is introduced. ADR
   0033's R3 result remains NOT TAKEN and retains its existing reopen
   condition.
6. Process shutdown waits for explicit load/session completion signals, not a
   guessed number of SceneTree frames.

## Consequences

- `FrameDriver`, its hook registration, `MissionRuntime`, and the duplicate
  loaded/playing flags are deleted in the cutover.
- `apps/nw_server` consumes the same session interface as Godot, which proves
  the portable seam without inventing a second renderer.
- Tests move to the session and frame-pipeline interfaces. Hook-order tests are
  replaced rather than layered beneath the new interface.
- Existing typed draw-list compilers and Godot appliers remain the rendering
  seam and keep their visual-parity coverage.
