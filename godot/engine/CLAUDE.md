# godot/engine/ — the engine layer (GDExtension glue + shared GDScript)

Two things live here, and both must stay shell-neutral — consumable by the game
shell (`godot/game/`) and ONED's authoring/preview surfaces (`godot/modtools/`):

- **Native bindings (C++)**: `Nova*`-prefixed GDExtension classes binding `libs/` to
  Godot. Thin wrappers only — format/runtime logic belongs in `libs/`. Register new
  classes in `register_types.cpp`.
- **The shared GDScript engine layer** (~36k LOC across ~150 scripts) both shells run on:
  - `world/` — THE runtime, and by far the largest slice (~15k LOC): `game_world.gd` (the
    GameWorld scene), `mission_runtime.gd`, `mission_present_pass.gd`,
    `local_player_presenter.gd`, the per-system present passes (fire, throwable, destruction,
    aim overlay, emplaced weapon, player-view effects), the net views, mission audio.
    ADR 0006/0011/0012 territory — read `docs/runtime-architecture.md` and the ADRs first.
  - `debug/` (the F3 overlay — a NovaDebugPage-per-system framework: sidebar shell,
    NovaDebugOptions registry, the pick/snapshot stack; add pages per
    `debug/pages/README.md` — plus the UI-free session layer under it:
    `NovaDebugSession` + `NovaDebugCatalog`, the one control catalog both F3 and
    the runtime `game_debug` MCP tool consume), `environment/` (time-of-day /
    water / sky / weather — heavily `[orig]`-cited, shared by the game and the
    Terrain/Object editors),
    `mission/` (object placer + model resolver; the editor-only authoring overlays
    live in `modtools/mission/`), `object/` (`nova_object_model.gd`, collision
    hulls), `mcp/` (shell-agnostic MCP server core: booted by ONED's
    `modtools/mcp/` on the stable editor port, and by the game's
    `game/game_mcp_service.gd` as the arg-gated ephemeral runtime endpoint),
    `ui/` (HUD view helpers), `avatar/` (avatar composition), `terrain/`,
    `resource_index/`, `util/`, `strings/` (the `NovaStrings` autoload, wired in
    `project.godot`), `fly_camera.gd`.
  - The remaining subdirectories here (`audio/`, `cbin/`, `dbf/`, `env/`, `fnt/`, `hud/`,
    `lwf/`, `mnu/`, `network/`, `particle/`, `pff/`, `refs/`, `rtxt/`, `simulation/`,
    `wac/`, `editor/`, `build/`) are native C++ binding code, not GDScript.
    `simulation/` (`nova_simulation.cpp`) is the biggest of them: the World binding,
    the present snapshot, and the shell-side asset resolution the portable systems consume.

Placement rule: GDScript lands here only when it is engine-level and shell-neutral.
Game-shell-only code goes in `godot/game/`; editor-only code goes in `godot/modtools/`.

Error/diagnostic channels (ratcheted at zero — `gd_prints_outside_debug`,
`cpp_binding_console_writes`): a failure the caller already receives through the
return/error contract reports context via `push_warning` (negative-path tests
drive those legs; GUT counts engine errors as failures); `push_error` is for
invariant violations nothing recovers from. Load/lifecycle narration uses
`print_verbose` (visible under `--verbose`), live inspection goes through the
F3 overlay/stats system, and `libs/` diagnostics ride the `io/log.h` sink.
Never raw `print`/`printerr`/`print_line`/`WARN_PRINT`/`ERR_PRINT` in shipping
code; CLI tool drivers (screenshot_capture) are the allowlisted exception.

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
