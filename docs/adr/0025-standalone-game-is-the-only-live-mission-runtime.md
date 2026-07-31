# ADR 0025: the standalone game is ONED's only live mission runtime

- **Status**: accepted (2026-07-29)
- **Supersedes/updates**: supersedes the embedded editor-preview, self-tick,
  editor Play/Stop, and rewind consequences of
  [ADR 0006](0006-unified-mission-runtime-present-pass.md). ADR 0006's
  consolidated game-side runtime, present sequence, and entity index remain
  accepted.

## Context

ONED historically offered embedded mission simulation and later a
play-in-editor local-player session. The editor toolbar also launched the game.
Even when both products reused `MissionRuntime`, `LocalPlayerPresenter`, and the debug
overlay, they entered different scene trees and owned different transport,
input, mouse, pause, and teardown state. Testing inside ONED therefore was not
literally the same operation as running the game.

The authoring loop is explicit: edit an asset, save its canonical loose file,
then run the game against that resource directory. Preserving unsaved editor
state in a second runtime is contrary to that boundary and creates lifecycle
and state machinery that the engine itself does not need.

## Decision

1. **ONED has no PIE or in-place mission simulation.** It does not create a
   `GameWorld`, drive `MissionRuntime`, or run a local player for live mission
   testing. Authoring previews may still reuse runtime nodes, public engine
   functions, and sampler seams, but those previews are not gameplay.
2. **`MainGame` / `GameWorld` is the sole live mission runtime.** The local
   player, mission tick, networking, presentation sequence, audio, HUD, F3
   debug UI, and runtime automation all use the standalone game's normal
   lifecycle.
3. **F5 runs the normal game.** ONED launches one managed standalone child
   over the resource directory it has mounted. Starting F5 again, or switching
   from F6 to F5, replaces that child rather than creating another runtime.
   Because ONED mounts loose files while the game's runtime mount is
   packed-archive-first, every managed launch also passes `--loose-root`: a
   directory holding none of the packed archives then falls back to the
   editor's loose mount instead of failing the boot, so play-testing works on
   the exact loose file set being authored. This is a tracked divergence from
   retail's fatal no-archives gate
   [orig: PFF_OpenAllArchives @ 0x4a4310; Game_InitSubsystems @ 0x4a6f44],
   scoped to editor-managed runs only — a standalone launch without the flag
   keeps the witnessed fatal error.
4. **F6 runs the current mission.** The mission must already be saved as an
   existing top-level loose `.bms` in the mounted resource directory. The
   standalone game boots that exact saved mission through its normal
   `MainGame` / `GameWorld` path.
5. **F8 stops the managed child.** Process ownership belongs to the editor
   shell, not to a mission workspace.
6. **Run is read-only with respect to authored data.** F5 and F6 never save,
   export, copy, or stage assets. They warn about every workspace with unsaved
   changes and launch from disk anyway. Saving remains an explicit author
   action.

## Consequences

- Mission testing, F3 diagnostics, player input, pause/teardown behavior, and
  runtime automation cannot drift between an editor-embedded runtime and the standalone game;
  there is only the standalone game.
- F6 retains the useful "current scene" workflow without inventing an
  in-memory mission format or state bridge. Its saved, top-level loose-file
  requirement is deliberate and visible.
- The editor no longer needs mission transport controls, simulation ownership
  locks, transform rewind, play-input routing, or a second debug-overlay
  lifecycle.
- Dirty editor state is intentionally absent from the running game. To test a
  change, save the canonical loose asset and relaunch.
- Standalone probes and integration tests enter through `MainGame` and a saved
  loose mission instead of an editor play controller.
