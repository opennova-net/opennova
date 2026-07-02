# godot/engine/ — GDExtension glue

- Thin wrappers only: format/runtime logic belongs in `libs/`; classes here
  (`Nova*`-prefixed) bind it to Godot. Register new classes in `register_types.cpp`.
- After any change here: run `scripts/build_godot.sh` and fully restart the Godot editor —
  GDExtension registration does not hot-reload, and GDScript referencing an unregistered
  class fails to parse (GUT then silently drops those test scripts).
- A stale `build/Debug/opennova.dll` can shadow the freshly built DLL; delete it if the
  editor keeps loading old native code.
- godot-cpp `Basis(axis, angle)` diverges from core Godot for negative-component axes.
  When porting GDScript Basis math to C++, add a parity test first.
- `NovaResourceRoot::set_root_dir` clears the dir index and texture caches — a 94s -> 2s
  mission-load regression hid here; do not call it casually. `list_files()` is
  kind-curated: load known filenames via `read_file`/`has_file`; don't expect them listed.
- The C ABI consumed here is shared with Python — keep exports flat and domain-prefixed
  (see `libs/CLAUDE.md`).
- Net bindings (`network/nova_net_client`, `network/nova_world_client`) are thin pumps
  over the wire-compatible `libs/novaworld` codecs — sockets and signals here, protocol and
  crypto in `libs/` (ADR 0010). Keep wire behavior in the portable libs so it stays
  unit-testable and interoperable; see `docs/net/novaworld-net-re.md`.
