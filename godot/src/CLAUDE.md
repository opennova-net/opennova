# godot/src/ — pure C++ GDExtension bindings (part of the core engine)

The Godot layer's native half (ADR 0016/0028/0034 d6): C++ only — the
GDExtension classes binding `engine/` to Godot. Register new classes in
`register_types.cpp`. The standing rule is ADR 0033's one-line test: a line
here earns its place only as a device leg (marshalling, nodes, servers,
input, audio, draw-list appliers), a Resource-shaped ONED document surface,
or a documented seam bridge — format/runtime logic and every witnessed
behavior belong in `engine/`. Nova formats never touch Godot's resource
system (documents self-read/write via `load_from_path`/`save_to_path`). The
`adapter_cpp_orig_cites` ratchet is the transition gauge: a new `[orig:]`
cite here is either a documented seam contract or code that belongs
engine-side.

The game-level GDScript runtime (world, debug, environment, mission, ui,
avatar, mcp, strings, util) lives in `godot/game/` (ADR 0034 d6) — anything
there that is really engine behavior is the C++ rewrite queue.

Placement rule: no GDScript here, ever. Scripts go to `godot/game/` (game
level) or `godot/modtools/` (editor-only).

Error/diagnostic channels (ratcheted at zero — `gd_prints_outside_debug`,
`cpp_binding_console_writes`): a failure the caller already receives through the
return/error contract reports context via `push_warning` (negative-path tests
drive those legs; GUT counts engine errors as failures); `push_error` is for
invariant violations nothing recovers from. Load/lifecycle narration uses
`print_verbose` (visible under `--verbose`), live inspection goes through the
F3 overlay/stats system, and `engine/` diagnostics ride the `io/log.h` sink.
Never raw `print`/`printerr`/`print_line`/`WARN_PRINT`/`ERR_PRINT` in shipping
code; CLI tool drivers (screenshot_capture) are the allowlisted exception.

Gotchas:

- After any native change here: run `scripts/build_godot.sh` and fully restart the Godot
  editor — GDExtension registration does not hot-reload, and GDScript referencing an
  unregistered class fails to parse (GUT then silently drops those test scripts).
- The `build/Debug/opennova.dll`-shadows-Release trap is the PYTHON FFI loader's, not this
  layer's: the editor loads only `godot/bin/libopennova.*` (root CLAUDE.md has the note).
  A stale EDITOR means a stale `godot/bin` DLL — rebuild via `scripts/build_godot.sh` and
  fully restart.
- godot-cpp `Basis(axis, angle)` diverges from core Godot for negative-component axes.
  When porting GDScript Basis math to C++, add a parity test first.
- `ResourceRoot::set_root_dir` clears the dir index and texture caches — a 94s -> 2s
  mission-load regression hid here; do not call it casually. `list_files()` is
  kind-curated: load known filenames via `read_file`/`has_file`; don't expect them listed.
- The C ABI consumed here is shared with Python — keep exports flat and domain-prefixed
  (see `engine/CLAUDE.md`).
- Net bindings (`network/nova_world_client`, `network/nova_lan_session`) are thin pumps
  over the wire-compatible codecs — `engine/net/npwire` for the in-game codec + capture decode
  (ADR 0019), `engine/net/novaworld` for matchmaking — sockets and signals here, protocol and
  crypto in `engine/` (ADR 0010). Keep wire behavior in the portable libs so it stays
  unit-testable and interoperable; see `docs/net/novaworld-net-re.md`.
- Decoded in-match entities have one runtime fold and one presenter (ADR 0026):
  `ClientReplicaPipeline` owns `ClientState`, and `world/wire_present_pass.gd`
  renders it for live joiners. Nothing may grow another entity reducer or
  presenter. `NovaWorldClient` is matchmaking/handoff, not a
  gameplay-replication stack.
