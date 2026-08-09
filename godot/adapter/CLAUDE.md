# godot/adapter/ — the shell adapter (GDExtension glue + shared GDScript)

The adapter (ADR 0016/0028) wires Godot to the engine. Two things live here, and both
must stay shell-neutral — consumable by the game shell (`godot/game/`) and ONED's
authoring/preview surfaces (`godot/modtools/`):

- **Native bindings (C++)**: `Nova*`-prefixed GDExtension classes binding `engine/` to
  Godot. Thin wrappers only — format/runtime logic belongs in `engine/`. Register new
  classes in `register_types.cpp`. What "thin" means is ADR 0031's five bands (binding
  glue, ONED document surface, presentation, res:// loaders, documented seam bridges);
  the `adapter_cpp_orig_cites` ratchet enforces it — a new `[orig:]` cite here is
  either a documented seam contract or code that belongs engine-side.
- **The shared shell-neutral GDScript layer** (~38.5k LOC across ~150 scripts) both shells run on:
  - `world/` — the runtime's DEVICE-HOST layer (~16.4k LOC). Since ADR 0033 R1 the
    loop itself is engine code (`engine/runtime/frame` FrameDriver): `game_world.gd`
    (the GameWorld scene) and `mission_runtime.gd` compose the sim + presenters,
    install their legs as frame hooks (`set_frame_shell_hooks` /
    `set_frame_world_hooks` — every hook binds a NODE, never a RefCounted
    presenter), and delegate `tick_realtime`/`tick` to
    `NovaSimulation.frame_realtime`/`frame_single`. Also here: `mission_present_pass.gd`,
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
- `NovaResourceRoot::set_root_dir` clears the dir index and texture caches — a 94s -> 2s
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
