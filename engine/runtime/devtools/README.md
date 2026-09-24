# engine/runtime/devtools — the ImGui pass and its surfaces (ADR 0039)

The engine's Dear ImGui tool UI, Godot-free. `GameDevTools` composes the game's
F3 windows onto an `ImGuiPass`. The Godot `DevTools` node supplies the addon's
ImGui context and calls `draw_frame` once per frame between `NewFrame` and
`Render`, then applies the windows' requests and updates their records.

The Game viewport and Stats window start open; the other inspection windows
open through the Windows menu (grouped World / Simulation / Render / Network /
Tools, ImGui's demo under Help) or a world pick. Dear ImGui, the pass and the
windows compile only with `OPENNOVA_DEVTOOLS`, which is off for release
GDExtensions. The frame-stats board remains available in every flavour.

What is here:

- The Game window: the game image, Play/Interact, the spectator switch, and a
  transport toolbar (Pause / Step / Resume, the script pause, Leave...) with a
  status readout (frame rate, logic tick, session role and state).
- World: Entities, Entity Properties, AI, Player, Environment. Simulation:
  Weapon, Rays, Physics, Script. Render: Render, Particles, Audio. Network:
  Net. Tools: Stats, Log.
- The menu bar's status line: every control request's verdict ("id: ok" /
  "id: failed (reason)"), the last sixteen one hover away; the Log window
  keeps them with a read's full payload beside the engine log ring.
- The Overlays menu: world-space layers drawn over the Game view (entity
  selection and labels, the AI's labels / routes / targets / rings, rays,
  contacts, hit meshes).

## The control board

`control_board.h` is the read side of the debug-control table: the embedder
pushes the table's catalog once (label, tooltip, kind, range, enum choices,
authority) and, every 0.25 s, the live state (value, writable, the refusal
reason) of the rows the visible windows declare through
`Window::wanted_controls`. A window draws a row with `draw_control(board, id,
queue)` — a checkbox, a slider queued on release, an enum combo, or an
argument-less action button, disabled with the table's reason when it is not
writable — and never hand-copies a row's range or choices. Rows with
arguments (a variable index and value, a bus name and volume) are the
window's own widgets queuing a `ControlRequest`.

## Adding an overlay layer

1. Subclass `OverlayLayer` (`overlay_canvas.h`): `group()` (the Overlays menu
   section), `label()`, `tooltip()`, `draw_priority()` (lower draws
   underneath), the line/text budgets, and `draw(OverlayCanvas &)`, which
   paints mission-frame primitives (lines, crosses, ground circles, boxes,
   sphere outlines, labels). The canvas projects them through the pushed
   `OverlayCamera` (`overlay_camera.h`: one mission-frame view-projection the
   shell composes from the camera the game image is drawn through, the
   presenter's stretched-frame camera while its target is live) and clips
   them to the game image.
   There is no depth test.
2. The window owns the layer and the record it draws (declare the record
   first; the layer holds a reference), and `GameDevTools` registers it with
   `pass_.register_overlay`. Layers draw only while the tools are open and
   are toggled independently of their window, so the window's `needs_*`
   becomes "pass open && (window open || layer on)", and a window keeps the
   record its layer reads through its own close.
3. The embedder pushes the layer's record in `push_overlay_frame`
   (`godot/src/devtools/dev_tools_overlay.cpp`) BEFORE the layout pass, per
   logic tick (or on its own cadence when the read is heavy), from the ONE
   engine function behind it; `clear_overlay_records` drops it on close and
   unload.
4. Pin it in `tests/devtools/devtools_overlay_test.cpp`: a north-facing
   hand-built camera, a fake image (`FakeGameViewport` emits an item of the
   requested size), settling frames, then the layer's `last_stats()`.

## Adding a window to the game's dev tools

1. Subclass `Window` (`imgui_pass.h`): `title()` and `draw(ImGuiPass &, uint64_t frame_index)`;
   override `on_visibility(bool)` to arm/disarm any data capture on the
   (pass open && window open) edge; declare close, collapse, scroll, and
   initial-placement policy through the explicit virtuals; override
   `owns_frame()` only for a window that issues its own `ImGui::Begin/End`, and
   `preferred_size()` for a surface the cascade default is too small for (a
   timeline) — it applies `ImGuiCond_FirstUseEver`, so the user's own sizing and
   ImGui's ini always win afterwards.
2. Register it in `GameDevTools::GameDevTools()` (`pass_.register_window(std::make_unique<MyWindow>())`);
   it appears in the "Windows" menu under its `menu_group()`. Keep `open`
   false unless the window is the default surface. A window that reads
   debug-control rows takes the `ControlBoard &` and declares the rows in
   `wanted_controls`; its request queue joins `GameDevTools::take_control_request`.
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
5. Add it to the registry pin (`kExpectedWindows` in
   `tests/devtools/devtools_test.cpp`: title, group, default), and pin the
   window itself in `tests/devtools/devtools_windows_test.cpp` with the null
   backend (`devtools_test_support.h`): push a record, assert what it
   formats, run a layout pass; a request window also pins the queue
   round-trip directly (clicking its buttons needs a real backend).
6. Row labels or comments say "host" only for the game host (CONTEXT.md "Host /
   Joiner"); identifiers never contain the word. Vocabulary is a review concern, not
   a lint (ADR 0043 retired `host_lint.py`).

## Adding a slot

One `X(NAME, "description")` line in `frame_stats_slots.h`, in the group it
belongs to. Feed it from the shell (`FrameStats.add(FrameStats.NAME, us)`) or
from `Simulation::fold_frame_stats`, then add its row to
`stats_window_rows.h` (SPAN/GROUP/HEADER/RESIDUAL, parented by depth).
