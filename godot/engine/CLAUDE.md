# godot/engine/ — the engine layer (GDExtension glue + shared GDScript)

Two things live here, and both must stay host-neutral — consumable by the game shell
(`godot/game/`) and by ONED play-in-editor (`godot/modtools/`) alike:

- **Native bindings (C++)**: `Nova*`-prefixed GDExtension classes binding `libs/` to
  Godot. Thin wrappers only — format/runtime logic belongs in `libs/`. Register new
  classes in `register_types.cpp`.
- **The shared GDScript engine layer** (~10.5k LOC) both hosts run on:
  - `world/` — THE runtime: `game_world.gd` (the GameWorld host scene), `mission_runtime.gd`,
    `mission_present_pass.gd`, `local_player_host.gd`, the net views, mission audio.
    ADR 0006/0011 territory — read `docs/runtime-architecture.md` and the ADRs first.
  - `mission/` (object placer + overlays), `object/` (`nova_object_model.gd`, collision
    hulls), `environment/` (time-of-day / water / sky / weather — heavily `[orig]`-cited,
    shared by the game and the Terrain/Object editors), `mcp/` (host-agnostic MCP server
    core, booted by `modtools/mcp/`), `debug/` (F3 overlays), `ui/` (HUD view helpers),
    `strings/` (the `NovaStrings` autoload, wired in `project.godot`), `fly_camera.gd`.

Placement rule: GDScript lands here only if both hosts can consume it. Game-shell-only
code goes in `godot/game/`; editor-only code goes in `godot/modtools/`.

Gotchas:

- After any native change here: run `scripts/build_godot.sh` and fully restart the Godot
  editor — GDExtension registration does not hot-reload, and GDScript referencing an
  unregistered class fails to parse (GUT then silently drops those test scripts).
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
  over the wire-compatible codecs — `libs/npwire` for the in-game codec/replay (ADR 0019),
  `libs/novaworld` for matchmaking — sockets and signals here, protocol and
  crypto in `libs/` (ADR 0010). Keep wire behavior in the portable libs so it stays
  unit-testable and interoperable; see `docs/net/novaworld-net-re.md`.
- Two net render paths coexist by design: the NovaNetClient replay/spectate path
  (`world/net_world_view.gd` / `net_event_view.gd`) and the NovaWorldClient/NovaSimulation
  listen-server path (`world/wire_present_pass.gd`) — ADR 0009/0011/0013. Don't unify
  them casually.
