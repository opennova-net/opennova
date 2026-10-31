# godot/src/ — the Godot GDExtension (a backend, ADR 0077)

The engine owns everything above platform and GPU (ADR 0077); Godot is the platform and GPU
backend. This tree is the shipped game's binding and it only shrinks: no new behavior lands
here. Every behavior, render and audio policy included (what to draw, order,
state, culling, skinning, transforms, mixing, panning, text layout, the game flow), belongs in
`engine/`, shaped like the retail runtime. A Godot class doing an engine's job (scene nodes,
Camera3D, Skeleton3D, SubViewport, Godot meshes/materials/shaders, Control UI, audio players,
Tween, HTTPRequest; `GODOT_HIGH_LEVEL_CLASSES` in `scripts/lint/ratchet_counts.py`) is never
added. A change here moves logic out to an engine home and leaves a thin call behind. Register
classes in `register_types.cpp`. The engine's formats never touch Godot's resource system
(documents self-read/write via `load_from_path`/`save_to_path`).
Names and includes (ADR 0040): there is no "Nova layer" — a file is named after the
type it declares, no `nova_`/`Nova` prefix anywhere (`NovaWorld*` is the service's
proper noun and stays); a binding may share its class name and directory with the
engine concept it exposes (`ScriptedInput` in `devtools/scripted_input.h` wraps
`opennova::devtools::ScriptedInput` from `<runtime/devtools/scripted_input.h>`). Binding
includes are quoted and root-relative (`#include "devtools/scripted_input.h"`, this
directory is the include root); engine includes are `<group/lib/file.h>`; no
subdirectory here may be named `base`, `formats`, `runtime` or `net`
(`include_graph_check.py` enforces all three).

Citations (ADR 0042 d7, ADR 0077): a witness citation is `[orig: Name @ 0xADDR]`
everywhere — there is no second marker form. A cite on the Godot side is engine
behavior still to move: `godot_orig_cites:<area>` is one non-increasing counter
per area (`src/<dir>`, `game/<dir>`, `editor/<dir>`, `probes/<dir>`), so a cite
never moves between Godot areas, only out to its engine home (or dies as
verified dead code); bank the decrease with `--write-baseline`.
`scripts/lint/cite_census.py` keeps the SET of cited addresses from losing a
member silently.

Size ratchet: no `.cpp`/`.h` here past 2500 lines — split by responsibility
(one type per TU pair), never by "leg"; `scripts/lint/ratchet_counts.py` fails
on any increase. Shipping GDScript has no size ratchet; it has
`gd_foreign_private_accesses` instead: a script that reaches into another
object's `_privates` is a method annex, not a class, and the count is an
absolute zero floor.

The game-level GDScript (world, ui, avatar, resource_index, strings, util and the
root scripts) lives in `godot/game/`; ADR 0077 ends shipping GDScript: every such
script moves to engine C++ (`godot_shipping_gd_lines` only falls), and only the dev
tooling (`mcp/`, `probe/`, `debug/`, the fly camera) stays GDScript. The debug-control
table itself is C++ here (`devtools/debug_control_table`, ADR 0043 d12): the Insert
dev tools' `ControlRequest`s and MCP's `game_debug` drive the one instance.

Placement rule: no GDScript here, ever. Game scripts go to `godot/game/`.

Error/diagnostic channels (ratcheted at zero — `gd_prints_outside_debug`,
`cpp_binding_console_writes`): a failure the caller already receives through the
return/error contract reports context via `push_warning` (negative-path tests
drive those legs; GUT counts engine errors as failures); `push_error` is for
invariant violations nothing recovers from. Load/lifecycle narration uses
`print_verbose` (visible under `--verbose`), live inspection goes through the
dev tools (Insert: engine-owned ImGui windows; `devtools/` here is the
`DevTools` node and the `FrameStats` board binding, ADR 0039), and `engine/` diagnostics ride the
`io/log.h` sink.
Never raw `print`/`printerr`/`print_line`/`WARN_PRINT`/`ERR_PRINT` in shipping
code.

Gotchas:

- After any native change here: run `scripts/build_godot.sh` and fully restart the Godot
  editor — GDExtension registration does not hot-reload, and GDScript referencing an
  unregistered class fails to parse (GUT then silently drops those test scripts).
- Two GDExtension variants come out of one build (ADR 0046 d2): the runtime-only one
  (the game and the editor's Play child; on Windows `opennovad.dll` debug and
  `opennova.dll` release, elsewhere `libopennova.*`) and the editor-enabled one (the
  same bindings plus `authoring/` and the editor core: what the OpenNova Editor
  ships; `oneditord.dll` debug and `oneditor.dll` release, elsewhere
  `libopennova_editor.*`). The Windows names stay short for the retail updater's
  15-byte 1SFS name field. A source run (the Godot editor, GUT, export scanning)
  loads the editor-enabled debug library, the superset (`godot/bin/oneditord.dll`
  on Windows), through the plain debug rows of `bin/opennova.gdextension`; the
  `opennova_runtime` / `opennova_editor` feature tags of the export presets pick the
  shipped one. A
  stale editor means a stale `godot/bin` DLL — rebuild via `scripts/build_godot.sh`
  and fully restart. `authoring/` is the editor's Godot seam (the `EditorApp`
  root node over the shared `ImGuiPassNode`, the OS process seam under Play):
  it compiles only into the editor variant, so nothing the game ships can
  reach it (`link_graph_check.py`), and it reaches ImGui only through the
  `imgui_abi.h` pointer seam like `devtools/`.
- godot-cpp compiles only the engine classes in `godot_cpp_profile.json`, derived from
  this tree. A binding that starts using a class the list lacks fails with a missing
  `godot_cpp/classes/*.hpp` (or a missing method): run
  `python scripts/lint/godot_cpp_profile.py --write` and commit the regenerated list.
- PR CI builds only the `template_debug` desktop GDExtension (both variants) and packages
  debug-mode exports (`-ExportMode debug`: the runtime zip and the editor zip); `template_release` + release-mode packaging run on master
  pushes/manual runs, so a release-flavour breakage surfaces after merge — build via
  `scripts/package_godot_windows.ps1` when touching build glue here. The engine test job
  runs Ninja + sccache (the VS generator is local-only).
- godot-cpp `Basis(axis, angle)` diverges from core Godot for negative-component axes.
  When porting GDScript Basis math to C++, add a parity test first.
- `ResourceRoot::set_root_dir` clears the dir index and texture caches — a 94s -> 2s
  mission-load regression hid here; do not call it casually. `list_files()` is
  kind-curated: load known filenames via `read_file`/`has_file`; don't expect them listed.
- The editor's mission device (`authoring/mission_viewport_applier`, ADR 0046 S14) draws with
  the game's process-wide render state (the environment's shader globals, the water plane: last
  writer wins, and a conflict is sticky), so a device says the scene state its picture renders
  with and the frame's arbitration renders one state at a time; such a global is written again in
  `publish_scene_state` each turn, never once at build (DI-31: the statics' light atlas is its light
  director's scene's, published there too). It draws single-sampled, as the game's view does: the particle
  renderer's passes bind the view's depth and draw nothing under MSAA. Its bytes are the project's alone
  (`ResourceRoot::mount_files` over the session's file source); a container it drops is renamed
  and `queue_free`d, never `remove_child`ed mid-frame.
- Bind native C++ engine APIs directly. Do not introduce a parallel flat FFI surface.
- Net bindings (`network/novaworld_client`, `network/lan_session`) are thin pumps
  over the wire-compatible codecs — `engine/net/npwire` for the in-game codec + capture decode
  (ADR 0019), `engine/net/novaworld` for matchmaking — sockets and signals here, protocol and
  crypto in `engine/` (ADR 0010). Keep wire behavior in the portable libs so it stays
  unit-testable and interoperable; see `docs/net/novaworld-net-re.md`.
- Decoded in-match entities have one runtime fold and one presenter (ADR 0026,
  ADR 0043 d9): `ClientReplicaPipeline` owns `ClientState`, and the one native
  `EntityPresenter` node (`simulation/entity_presenter.cpp` — the placed walk
  and the shared legs; `entity_presenter_wire.cpp` — the wire walk's cold path
  and hot rows) renders both the placed rows and the wire rows for live joiners.
  Nothing may grow another entity reducer or presenter. `NovaWorldClient` is matchmaking/handoff, not a
  gameplay-replication stack.
