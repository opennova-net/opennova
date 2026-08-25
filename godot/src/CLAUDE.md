# godot/src/ — pure C++ GDExtension bindings (part of the core engine)

The Godot layer's native half (ADR 0016/0028/0034 d6): C++ only — the
GDExtension classes binding `engine/` to Godot. Register new classes in
`register_types.cpp`. ADR 0035 supersedes ADR 0033's callback-bus design: a
line here earns its place only as a device leg (marshalling, nodes, servers,
input, audio, draw-list appliers), the typed bridge from `inmatch::Session` to
Godot's `GameFramePipeline`, or a documented seam bridge — format/runtime logic
and every witnessed behavior belong in `engine/`. Nova formats never touch
Godot's resource system
(documents self-read/write via `load_from_path`/`save_to_path`). The
two `adapter_cpp_orig_cites_*` ratchets are the transition gauge, split by
population (2026-08-11): `_pushdown` (simulation/, object/, mission/) is
witnessed engine behavior still living here — the burn-down class, and it
can legitimately reach zero; `_device` (env/, terrain/, hud/, mnu/,
particle/, network/, ...) is the retail-D3D→Godot device-leg mappings ADR
0035 sanctions — it must not grow, but its floor is NON-ZERO BY DESIGN:
deleting a device citation is a documentation regression, not a win. The
device counter counts both the `[orig:` marker and the adjudicated
`(retail:` form below, so converting a note never shrinks it; only a
deleted witness does.

Citation convention (adjudicated): a witness note in `godot/src` is written as
`(retail: Name @0xADDR, see docs/<record>)` — NEVER the literal `[orig:`
marker, which the `adapter_cpp_orig_cites_*` ratchets count. The
`(retail: ...)` form is for device-leg cross-references pointing at a
record-owned witness; genuinely witnessed engine behavior belongs in `engine/`
with a real `[orig:]` cite.

Size ratchets: no `.cpp` here past 2500 lines, no shipping `.gd` past 1200 —
split first; `scripts/lint/ratchet_counts.py` fails on any increase.

The game-level GDScript runtime (world, debug, mission, object, terrain,
ui, avatar, mcp, resource_index, strings, util) lives in `godot/game/` (ADR 0034 d6) — anything
there that is really engine behavior is the C++ rewrite queue.

Placement rule: no GDScript here, ever. Game scripts go to `godot/game/`;
ONED scripts go to `godot/modtools/`.

Error/diagnostic channels (ratcheted at zero — `gd_prints_outside_debug`,
`cpp_binding_console_writes`): a failure the caller already receives through the
return/error contract reports context via `push_warning` (negative-path tests
drive those legs; GUT counts engine errors as failures); `push_error` is for
invariant violations nothing recovers from. Load/lifecycle narration uses
`print_verbose` (visible under `--verbose`), live inspection goes through the
F3 overlay/stats system, and `engine/` diagnostics ride the `io/log.h` sink.
Never raw `print`/`printerr`/`print_line`/`WARN_PRINT`/`ERR_PRINT` in shipping
code.

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
  `ClientReplicaPipeline` owns `ClientState`, and the native `WirePresentPass`
  (`simulation/nova_wire_present_pass.cpp`; the hot row walk lives in
  `nova_present_applier_wire.cpp`) renders it for live joiners.
  Nothing may grow another entity reducer or presenter. `NovaWorldClient` is matchmaking/handoff, not a
  gameplay-replication stack.
