# engine/runtime/devtools — the ImGui pass and its surfaces (ADR 0039)

The engine's Dear ImGui tool UI, Godot-free. A product composes its windows
onto an `ImGuiPass`; the embedding shell only hands over the ImGui context the
imgui-godot addon created and calls `draw_frame` once per frame between the
addon's `NewFrame` and `Render` (the `ImGuiPassNode` seam in `godot/src/devtools/`).

Two products compose it:

- the **game's dev tools** (`game_dev_tools.*`, F3): an opaque workspace with
  a mandatory Game viewport, the frame-stats window, the Entities and Entity
  Properties windows (closed by default; a world pick opens them), the Weapon
  window (closed by default), and ImGui's demo window.
  Debug builds only
  (`OPENNOVA_DEVTOOLS`; off for the release GDExtension flavour).
## Adding a window to the game's dev tools

1. Subclass `Window` (`imgui_pass.h`): `title()` and `draw(ImGuiPass &, uint64_t frame_index)`;
   override `on_visibility(bool)` to arm/disarm any data capture on the
   (pass open && window open) edge; declare close, dock, collapse, scroll, and
   initial-placement policy through the explicit virtuals; override
   `owns_frame()` only for a window that issues its own `ImGui::Begin/End`, and
   `preferred_size()` for a surface the cascade default is too small for (a
   timeline) — it applies `ImGuiCond_FirstUseEver`, so the user's own sizing and
   ImGui's ini always win afterwards.
2. Register it in `GameDevTools::GameDevTools()` (`pass_.register_window(std::make_unique<MyWindow>())`);
   it appears in the "Windows" menu. Keep `open` false unless the window is
   the default surface.
3. Read engine data through the board or through typed engine records the
   embedder pushes; a window never reaches into Godot. Data the shell alone
   has (RenderingServer counters, viewport stats) arrives as VALUE slots the
   shell samplers feed.
4. Engine facts flow records-in / requests-out (ADR 0042 d6; the Entities
   window is the template). No feed framework ahead of a window — each window
   lands with its own record: a plain value struct beside the window
   (`entity_directory_snapshot.h`), pushed by value through a `GameDevTools::set_*`
   (an invalid record clears), gated by a `GameDevTools::needs_*()`
   (pass open && window open) so the embedder skips building records nobody
   shows, and refreshed on the window's 0.5 s cadence — unless the window is a
   scope on a fast signal, which the Weapon window is: it pushes every frame and
   drains the pump's 62.5 Hz trace ring incrementally, because a 1-tick action
   or a zero-length tail falls between two display frames. Mutations leave as a
   typed control request (`control_request.h`: the wire id + args the C++ `DebugControlTable` invokes, ADR 0043 slice G12) the window queues and
   `GameDevTools::take_*_request` drains. The `DevTools` node
   (`godot/src/devtools/dev_tools.cpp`) holds the `Simulation` in C++ (set on
   world load, nulled on unload — the stats-board pattern) and does the
   push/drain against the ONE engine function per fact
   (`world::inspect::entity_directory` and `build_entity_card`, the
   `EntityCommands` mutators MCP uses); no GDScript relay. A device event
   that names an engine fact (the shell's world pick) crosses as a typed
   request carrying only the engine handle (`DevTools.select_entity`); the
   window reads everything else from the pushed records.
5. Pin it in `tests/devtools/devtools_test.cpp` with the null backend: open the
   window, run a layout pass, assert what it formats; a record/request window
   also pins the push/clear and the queue round-trip directly (clicking its
   buttons needs a real backend).
6. Row labels or comments say "host" only for the game host (CONTEXT.md "Host /
   Joiner"); identifiers never contain the word. Vocabulary is a review concern, not
   a lint (ADR 0043 retired `host_lint.py`).

## Adding a slot

One `X(NAME, "description")` line in `frame_stats_slots.h`, in the group it
belongs to. Feed it from the shell (`FrameStats.add(FrameStats.NAME, us)`) or
from `Simulation::fold_frame_stats`, then add its row to
`stats_window_rows.h` (SPAN/GROUP/HEADER/RESIDUAL, parented by depth).
