# engine/runtime/devtools — the ImGui pass and its surfaces (ADR 0039)

The engine's Dear ImGui tool UI, Godot-free. A product composes its windows
onto an `ImGuiPass`; the embedding shell only hands over the ImGui context the
imgui-godot addon created and calls `draw_frame` once per frame between the
addon's `NewFrame` and `Render` (the `ImGuiPassNode` seam in `godot/src/devtools/`).

Two products compose it:

- the **game's dev tools** (`game_dev_tools.*`, F3): the Stats window over the
  frame-stats board plus ImGui's demo window. Debug builds only
  (`OPENNOVA_DEVTOOLS`; off for the release GDExtension flavour).
- **ONED's run surface** (`oned_ui.*`): the one window with the game-data
  directory, its recents, the game/expansion profile, the retail install and
  the Run / Stage & Run Retail / Stop actions. Every flavour — ONED ships in
  the dev zip that master exports in release mode.

| File | What it is |
|---|---|
| `imgui_pass.h/.cpp` | The pass: context binding (`attach_imgui`), the window registry, open/closed state, the per-frame layout (dockspace, "Windows" menu, each open window, Escape), the ABI fingerprint |
| `imgui_abi.h` | The version + struct-size fingerprint the shell passes to `ImGuiGD.GetImGuiPtrs` (no ImGui include) |
| `frame_stats_slots.h` | The slot table (X-macro): one entry per measured span or VALUE counter; the engine enum, the `FrameStats` constants and the Stats rows all derive from it |
| `frame_stats_board.h/.cpp` | The fixed-slot per-frame accumulator (sums, worst-frame peaks, sampled-frame counts) with the capture edge and the atomic drain |
| `game_dev_tools.h/.cpp` | The game's window set on a pass, with the board hand-off |
| `stats_window.h/.cpp`, `stats_window_rows.h` | The Stats window: the row tree over a drained window, refreshed every 0.5 s |
| `demo_window.h/.cpp` | ImGui's demo window, the docking/multi-viewport smoke test |
| `oned_ui.h/.cpp` | ONED's surface: the fields it owns, the state the app pushes, the typed request queue the app drains |

## Adding a window to the game's dev tools

1. Subclass `Window` (`imgui_pass.h`): `title()` and `draw(ImGuiPass &, uint64_t frame_index)`;
   override `on_visibility(bool)` to arm/disarm any data capture on the
   (pass open && window open) edge; override `owns_frame()` only for a window
   that issues its own `ImGui::Begin/End`.
2. Register it in `GameDevTools::GameDevTools()` (`pass_.register_window(std::make_unique<MyWindow>())`);
   it appears in the "Windows" menu. Keep `open` false unless the window is
   the default surface.
3. Read engine data through the board or through typed engine records the
   embedder pushes; a window never reaches into Godot. Data the shell alone
   has (RenderingServer counters, viewport stats) arrives as VALUE slots the
   shell samplers feed.
4. Pin it in `tests/devtools/devtools_test.cpp` with the null backend: open the
   window, run a layout pass, assert what it formats.
5. Row labels or comments that must say "host" (the game host) go through
   `scripts/lint/host_allowlist.json`; identifiers never contain the word.

## Adding a slot

One `X(NAME, "description")` line in `frame_stats_slots.h`, in the group it
belongs to. Feed it from the shell (`FrameStats.add(FrameStats.NAME, us)`) or
from `Simulation::fold_frame_stats`, then add its row to
`stats_window_rows.h` (SPAN/GROUP/HEADER/RESIDUAL, parented by depth).

## Changing ONED's surface

`OnedUi` owns the text fields; the app seeds them and reads them back on the
`APPLY_*`/`COMMIT_*` requests. Anything else the surface shows is pushed by the
app (`set_recent_dirs`, `set_readiness`, `set_status`). A new control is a new
`OnedAction` the app handles in `oned_app.gd` — the surface never spawns a
process, opens a dialog or writes a setting. `tests/devtools/oned_ui_test.cpp`
pins the surface, `godot/tests/oned_app_test.gd` the app over its seam.
