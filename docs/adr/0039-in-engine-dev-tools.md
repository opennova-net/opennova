# ADR 0039: The tool UI lives in the engine, drawn with Dear ImGui

- **Status**: accepted (2026-08-27; hard cut)
- **Owners**: engine runtime, the game shell
- **Supersedes/updates**: updates ADR 0034 d6 (the F3 debug pages are no longer
  game-level GDScript) and ADR 0031's seam-bridge note (the "F3 clocks" are
  device legs feeding an engine-owned board); ADR 0025's "the standalone game is
  the only live runtime" and ADR 0037's "ONED is run-only" stand unchanged.
  Updated by ADR 0041 (the env-var probes of decision 6 and the verification
  bullet's probe are `game_probe` tools). Updated by
  [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md) (2026-08-28),
  which scopes decision 3: Godot device facts arrive as VALUE slots or pushed
  records; ENGINE facts arrive as typed records the embedder pushes, and
  window mutations leave as typed requests the embedder drains; `DevTools`
  holds the `Simulation` natively; the imgui include lint is added by the
  campaign.

## Context

With ONED reduced to run-only (ADR 0037) and the asset pipeline cut to the
native runtime (ADR 0038), the project is a runtime and an engine. Its one
live-inspection surface was the F3 overlay: ~6 k lines of GDScript under
`godot/game/debug/` — a `CanvasLayer` shell, fifteen `Control`-tree pages, a
190-slot frame-stats accumulator and the samplers feeding it — docked inside
the game viewport. That surface was Godot-shaped end to end: every readout was
a `Tree`/`Label` rebuild, its cost was measurable in the frame (the Stats page
alone cost ~1.5 ms mean / 3.8 ms peak on its refresh frame), it could not leave
the game window, and it grew page by page in the layer ADR 0033/0034 reserve
for device legs and thin typed seams.

The retail side of the parity workflow (onHook, in `opennova-int`) already
uses a Dear ImGui docking overlay. One toolkit on both sides, owned by the
portable engine, is the smaller system — and ONED's run surface (ADR 0037:
settings plus Run / Stage & Run Retail / Stop), until now a `Control` tree,
is the same kind of tool UI, so it moves onto the same pass and proves the
integration is not a one-off.

## Decision

1. **The tool UI is engine code.** `engine/runtime/devtools/` (inside
   `opennova_runtime`, namespace `opennova::devtools`) owns the ImGui pass —
   `ImGuiPass`: the window registry, the dockspace and menu bar, the open/closed
   state, the ImGui context binding — and the window sets each product composes
   onto it. The game's dev tools (`GameDevTools`): `FrameStatsBoard` (the
   fixed-slot per-frame accumulator, transcribed from the retired GDScript board
   with the same semantics and slot names), `GameWindow` (the mandatory embedded
   runtime surface plus typed Play/Interact requests), `StatsWindow` (the
   retired Stats page's row tree over the board) and `DemoWindow` (ImGui's
   demo, the docking/multi-viewport smoke test). ONED's run surface (`OnedUi`):
   the one window over the fields it owns, the state the app pushes and a typed
   request queue the app drains. New windows are `Window` subclasses registered
   on a pass — see the directory README. Nothing here is a port of retail
   behaviour; it is infrastructure, listed in the citation allowlist like `io`
   and `vfs`.
2. **Dear ImGui is vendored at the addon's exact commit.**
   `third_party/imgui/` fetches the docking-branch commit that imgui-godot
   6.3.2 bundles (`v1.91.6-docking`, hash-pinned) and compiles it with
   `opennova_imconfig.h` = `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, the one
   layout-affecting define the addon's `imconfig-godot.h` sets (at 1.91.6 it
   removes three `ImGuiIO` members). The addon and the engine copy share one
   `ImGuiContext`; the addon fingerprints `IMGUI_VERSION` and
   `sizeof(ImGuiIO/ImDrawVert/ImDrawIdx/ImWchar)` before handing it over, the
   engine `static_assert`s the version and pins `sizeof(ImGuiIO)` in its test.
   The pin lives in two files that move together: `third_party/imgui/CMakeLists.txt`
   and `scripts/bootstrap_imgui_godot.sh`.
3. **The Godot side is one node per product and one addon.** The imgui-godot addon
   (installed by the bootstrap script into gitignored `godot/addons/imgui-godot/`,
   enabled as a plugin with its `ImGuiRoot` autoload) creates the context,
   forwards input and renders the draw lists through the RenderingDevice; its
   multi-viewport support turns undocked tool windows into OS windows, which
   is why `display/window/subwindows/embed_subwindows` is off project-wide.
   `ImGuiPassNode` (`godot/src/devtools/`) hands the context to the engine once
   (`ImGuiGD.GetImGuiPtrs`) and runs the engine's layout pass in a `_process`
   just under the addon's render pass; `DevTools` (the game) adds the open and
   input-mode state, the `SubViewport` adapter, its signals, and the Stats-row
   read seam, `OnedUi` (ONED)
   the field seeds/reads, the pushed state and `take_request`/`push_request`.
   `FrameStats` is the board's binding: the slot enum constants, the hot-path
   `add`, the capture edge and `drain()` into a typed `FrameStatsWindow` record.
   A debug-windowed `GameRuntimeRoot` keeps the complete `MainGame` scene in one
   always-updating `SubViewport`; it composites that viewport directly while
   F3 is closed and hands the same texture to `GameWindow` while F3 is open.
   The game shell owns the Play/Interact input gate and debug-pick policy;
   ONED's app
   (`godot/modtools/oned_app.gd`) seeds
   the surface and executes its requests — process spawning, the native
   directory dialogs and settings persistence stay in GDScript. `Simulation`
   folds its session phase spans onto the board natively; `get_session_perf()`
   stays as the probes' Dictionary edge.
4. **The game's dev tools are debug-only; the ImGui core is not.** Dear
   ImGui, the pass, the frame-stats board and ONED's surface compile in every
   flavour (ONED ships in the dev zip that master exports in release mode).
   `OPENNOVA_DEVTOOLS` is on in every build except the `template_release`
   GDExtension flavour, where the game's windows are compiled out and the
   `DevTools` node is an inert stub (the classes still register so scripts
   parse). The game's release export strips the addon through its own export
   plugin (`imgui/release=false`, `imgui/debug=true` on the runtime preset in
   `export_presets.cfg`), so the shipped game never loads ImGui; ONED's preset
   keeps it in both modes. The packaging script stages the addon library into
   the dev zip always and into the game zip only for a debug export.
5. **Docking and multi-viewport are required.** `attach_imgui` sets
   `ImGuiConfigFlags_DockingEnable | ViewportsEnable`; the game layout is an
   opaque application workspace. On first use, Game owns the center and Stats
   starts in a roughly 30% right dock; an existing ImGui layout is preserved.
   Game cannot close or collapse, but it can undock like the other tools and
   become its own OS window while the root window is windowed.
   *Amended 2026-08-28:* multi-viewport is suspended while the root window is
   in either fullscreen mode. With `ViewportsEnable` set, a borderless
   fullscreen main window presents black through imgui-godot 6.3.2 on Godot
   4.6.1 / D3D12 (the game SubViewport keeps rendering; the root frame does
   not), and the flag must already be clear at the NewFrame that first sees
   the fullscreen size. So `ImGuiPass::set_platform_windows_enabled` carries
   the flag, `ImGuiPassNode` applies it at attach and every frame from the
   window mode, and the shell's F11 handler withdraws it before the switch
   (`MainGame._toggle_fullscreen`); undocked tool windows merge back into the
   main viewport while fullscreen. Pinned by `tests/devtools/devtools_test`,
   the lifecycle GUT test and the `window_fullscreen` game probe.
6. **Hard cut.** The GDScript overlay, its page framework, the pages, the
   typed resolvers, the snapshot writer and the GDScript board are deleted;
   F3 opens the ImGui tools. What survives under `godot/game/debug/` has other
   owners: the MCP `game_debug` control plane (`DebugSession` and its catalog
   [since ADR 0042 d5 the typed `DebugControls` table,
   `godot/game/debug/debug_controls.gd` (since ADR 0043 slice G12: the C++ `godot/src/devtools/debug_control_table`); the `DebugSession` family is gone]),
   the world-owned debug views and pick nodes (`DebugViewSet` itself is at
   `godot/game/world/debug_view_set.gd`) and the three
   samplers (retyped to `FrameStats`); the env-var probes that survived this
   cut went with ADR 0041 (2026-08-27) as `game_probe` tools under
   `godot/probes/` or gated ctests. Retired page features return as engine
   windows when wanted; `TODO.md` carries the list.
   *Amended 2026-09-02:* the GDScript world-space debug views and their
   `DebugViewSet` owner were retired too, in favour of the ImGui windows (the
   AI, Rays and Physics windows keep their data panes and capture controls;
   their "show world overlay" toggles and the shell-mirrored view state went
   with the views). The windows have no world-space overlay yet; under this
   item's rule an overlay returns as an engine window when wanted, and
   `TODO.md` lists the overlays with no ImGui home.
7. **Not an editor.** The tools inspect and, later, control a running game.
   ADR 0037 stands: no authoring, no project state, no asset database.

## Consequences

- Every future window — dev tool or ONED control — is C++ in the engine and
  testable under ctest with a null ImGui backend (`tests/devtools/`), without
  Godot. ONED's `Control` scene, its LineEdit/Button wiring and the
  node-path assertions in its test are gone; the app is ~30 lines shorter and
  its tests drive the same typed seam the mouse does.
- The release GDExtension carries ImGui's code for ONED; the shipped game never
  reaches it (no addon, no `DevTools` body). Release, headless, and missing-addon
  starts hand `MainGame` directly to the scene tree, so they incur no persistent
  `SubViewport` composite.
- Adding a slot is one line in `frame_stats_slots.h`; the engine enum, the
  `FrameStats` constants and the Stats rows follow.
- The addon is a build-time download like the sqlite amalgamation and GUT:
  fresh checkouts run `scripts/bootstrap_godot.sh` before any Godot command,
  and the packaging job installs it for both export modes (the exporter that
  strips ImGui from a release export is the addon's own).
- The Stats window's info cells that read Godot `Performance` monitors and the
  occlusion/effect/present stats off Godot objects are not carried over yet;
  they return as VALUE slots the shell samplers feed.
- The perf probe's overlay mode feeds the window its own drains
  (`DevTools.feed_stats_window`) instead of rendering a page.
- `FRAME_DEBUG_DRAW` (the overlay's draw-sentinel row) is retired;
  `FRAME_DEBUG_REFRESH` is the dev tools' own layout cost, which now lands
  inside the process-callbacks span rather than the deferred flush.

## Verification

- `tests/devtools/frame_stats_board_test` pins the board's capture edges,
  atomic drains and per-frame peaks; `tests/devtools/devtools_test` pins the
  ABI fingerprint and the `ImGuiIO` size, the docking/viewport policy on
  attach, the mandatory Game window and first-use layout, responsive viewport
  sizing, typed input requests, the Stats window's capture gating and
  formatting, and the external feed.
- `godot/tests/game/main_game_lifecycle_test.gd` pins the F3 edge: input
  suspended in Interact, mouse freed, capture armed, world still ticking, pick
  clicks on, Play recapturing normal gameplay, Escape returning to Interact
  before closing, and teardown clearing the mode.
- `godot/tests/game/game_runtime_root_test.gd` pins the debug/direct startup
  decision; the `runtime_root_window` game probe
  (`godot/probes/runtime/runtime_root_window_probe.gd`) exercises the real
  addon, shared texture, first layout, responsive resize, and F3 routing in a
  window, and the `window_fullscreen` probe pins the F11 toggle presenting a
  lit frame in both window modes.
- `tests/devtools/oned_ui_test` pins ONED's surface: open from construction,
  a layout pass from the seeded state, the FIFO request queue, the field
  round trip; `godot/tests/oned_app_test.gd` drives the app over the seam.
- `scripts/package_godot_windows.ps1` requires the addon library beside the
  exports (ONED) and stages it into the game zip only for a debug export; the
  boot smoke covers both products in both modes.
- the `frame_stats` runtime probe (`godot/probes/perf/frame_stats_probe.gd`,
  run windowed through the `game_probe` MCP tool) opens the tools and reads
  the Stats rows through the `DevTools` seam.
