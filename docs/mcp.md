# The game MCP — launching, driving and probing a running OpenNova

OpenNova is driven by two Model Context Protocol servers, one per side of the
parity workflow:

- **`opennova-game`** (this repository) is embedded in the game runtime. A launch
  with `--mcp-port <n>` starts it on `http://127.0.0.1:<n>/mcp`; it exposes the
  game's state, the typed debug-control table, the compiled menu, captures, logs and the
  registered runtime probes (`game_probe`). It never launches anything itself.
- **`onhook-mcp`** (the `opennova-int` repository) drives the retail
  `Jointops.exe` through onHook. The parity runner
  (`scripts/net/run_parity_topology.ps1`) shells out to its executable; it is not
  a registered MCP server in an agent's tool list.

Runbooks and scripts are clients of both. This page is the `opennova-game` side:
ADR 0041 records the decisions (a probe is a registered `GameProbe` run through
`game_probe`, launch behaviour is a launch flag, probes read typed arguments and
never the environment, `godot/probes/` is source-only, an assertion over the
portable engine is a ctest).

## Launching

The game reads its configuration from launch flags after `--`
(`docs/dev-env-vars.md` lists them); `--mcp-port` starts the endpoint. The two
client layers wrap the launch:

```bash
# Python (stdlib only): launch, wait for the endpoint, drive, stop.
python scripts/mcp/game_mcp.py launch --windowed --resolution 1280x720 \
    --resource-dir "$OPENNOVA_JO_DIR" --exp revx02 --mission 00TRa.bms --pid-file .scratch/game.pid
python scripts/mcp/game_mcp.py tools                      # the catalog
python scripts/mcp/game_mcp.py call game_state '{}'
python scripts/mcp/game_mcp.py probe list
python scripts/mcp/game_mcp.py probe run perf_sample '{"sample_ms": 5000}' --wait
python scripts/mcp/game_mcp.py stop --pid-file .scratch/game.pid
```

```powershell
# PowerShell 5.1 (scripts/mcp/game_mcp.ps1, dot-sourced by scripts/net/lib.ps1)
. scripts/mcp/game_mcp.ps1
$game = Start-OpenNovaProcess -GodotArguments @('--windowed') `
    -GameArguments @('--resource-dir', $env:OPENNOVA_JO_DIR) -McpPort 8975   # lib.ps1
Get-StructuredResult (Invoke-GameTool -Port 8975 -Name game_state)
$status = Invoke-GameProbe -Port 8975 -Name frame_stats -Arguments @{ window_ms = 3000 }
Stop-GameViaMcp -Port 8975 -Process $game; Stop-OpenNovaProcess -Process $game
```

`launch` flags: `--headless` (no window; tools that need one refuse) or
`--windowed`, `--resolution WxH`, `--log-file`, `--rendering-method`, `--port`
(default 8975), `--pid-file`, `--timeout`; game flags forwarded after `--`:
`--resource-dir` (default `OPENNOVA_JO_DIR`), `--exp`, `--game`, `--loose` (retail's
`/d`: loose files beside the archives override them), `--loose-root`,
`--loose-mission`, `--mission`, `--lan-host`, `--lan-join`, `--lan-port`,
`--callsign`, `--lan-gametype`, `--lan-mode`, `--lan-max-players`,
`--integrity-profile`, `--capture-pcap`. Exit codes: 0 ok, 2 tool error, 3
JSON-RPC error, 4 probe cancelled, 5 probe error, 6 the process outlived the
quit, 7 launch failed.

Port conventions: 8975 for a single game (the tracked `.mcp.json` points Claude
Code at it), host 8975 / joiner 8976 for a LAN pair (`scripts/net/run_lan_pair.ps1`,
`run_parity_topology.ps1 -HostMcpPort/-JoinerMcpPort`). Claude Code connects to
`.mcp.json` at session start; a game started later is reached with `/mcp`
reconnect, or through the script clients.

The `Godot_v4.6.1-stable_win64_console.exe` wrapper stalls when its launching
shell has no console (a hidden automation shell): launch from a real terminal
(or `cmd /c`), or point `GODOT_BIN` at the plain runtime executable. The
parity runner's ownership proofs require the wrapper.

`--log-file <path>` makes Godot's own log (script errors, prints) readable
through `game_logs` as source `godot`; engine io::log entries always arrive
from the in-process ring as source `engine`. A headless launch reports
`available: false` for probes that need a window.

## The tool catalog

| Tool | What it does |
|---|---|
| `game_state` | the shell, the in-match session (`session.state`/`session.role` from the portable inmatch session), mission, runtime (entity/brain counts, the network role: `host` with its port and peers, `joiner` with its phase and admission, or `local`) and player state |
| `game_entities` | `op=list` a bounded page of the rendered entity view; `op=inspect` one entity's public debug card (its `ai_index` is the edit target for `game_debug`) |
| `game_control` | `quit`, pause/step/resume, `open_ingame_menu`, `open_armory`, ...; `quit` cancels a running probe first |
| `game_debug` | the typed debug-control table (`DebugControls`, ADR 0042 d5) over MCP: `op=list/get/set/invoke/snapshot`; the automation actions `teleport_local_player`, `set_entity_health/position` (by `ai_index`), `set_entity_item_attrib` (both items.def attrib words on one entity by `wire_handle`), `deploy_pick`, `set_viewmodel_weapon`/`clear_viewmodel_weapon`, `kill_group`, `crew_vehicle`, `crew_local_player`, `local_player_look`, the audio bus actions, `runtime_transport`, `set_mission_variable`; the `net_joiner_diagnostics` check |
| `game_render_diagnostics` | one frame-correlated render snapshot (camera/projection, environment, lights, shadow config, pass counts) |
| `game_capture_bundle` | the next completed frame as a lossless PNG + diagnostics JSON under `user://render-captures` (`world_only` hides the canvas UI) |
| `game_menu` | drive the compiled menu: `state`, `press`, `press_at`, `click_at`, `key`, `screen`, `open` |
| `game_screenshot` | the game window, F3 included when open |
| `game_logs` | the MCP, probe, engine and Godot log entries (sources `server`, `script`, `probe`, `engine` — the native io::log ring — and `godot` — the tailed Godot log file) |
| `game_probe` | the registered runtime probes: `op=list/run/status/cancel` (below) |

Tools marked serial (captures, screenshots, control) run one at a time;
`game_probe run` is refused while a serial call is in flight or another probe
runs.

## Probes

A probe is a registered script under `godot/probes/<family>/<name>_probe.gd`
that extends `GameProbe` and implements `run(ctx: ProbeContext) -> ProbeVerdict`.
It runs inside the live game, reads typed arguments, logs through `ctx.log`,
publishes `ctx.progress(...)`, writes artifacts into its run directory and
returns `ProbeVerdict.passed(summary, data)` or `ProbeVerdict.failed(summary,
data)`. The catalog (`godot/game/mcp/probe_catalog.gd`, `ProbeCatalog.definitions()`)
is the single list: each `ProbeDef` carries the name, description, script path,
JSON-schema input, `needs_window`, `needs_mission` and the timeout. `godot/probes/`
is source-only — both export presets exclude it, so a shipped build lists every
probe as `available: false`.

`game_probe`:

- `op=list` -> `{probes: [{name, description, input_schema, needs_window,
  needs_mission, timeout_ms, available}]}`
- `op=run {name, args}` -> `{run_id, name, started_at, artifact_dir}` — args are
  validated against the schema (defaults injected, unknown keys rejected)
- `op=status {run_id?, cursor?, wait_ms?}` -> `{run_id, name, state: running |
  passed | failed | cancelled | error, started_at, elapsed_ms, lines: [{seq, t_ms,
  text}], next_cursor, progress, verdict: {ok, summary, data} | null, artifacts:
  [{label, path, kind, bytes, sha256}], error}`; `wait_ms` long-polls
- `op=cancel` -> `{run_id, state}`; the watchdog cancels a run past its timeout
  and reports `error("unresponsive")` five seconds later

`ProbeContext` gives a probe the live seams as functions (never cached across
awaits): `game() world() runtime() sim() presenter() hud_presenter()
menu_shell() armory_presenter() deploy_presenter() dev_tools() frame_stats()
viewport() camera() resource_root() effect_world() adapter()`; the waits
`wait_frames / wait_ms / wait_mission_seconds / wait_for_local_player /
wait_world_ready`; mission control `start_mission(bms)`,
`start_saved_mission(saved_path, bms, profile)`, `return_to_menu()`,
`load_saved_mission(...)`; the guarded mutations `set_time_scale`,
`freeze_shell / unfreeze_shell`, `set_menu_visible`, `set_window_size` and
`defer_restore(Callable)` — every mutation is undone by `finish()` on every exit
path; and `capture_png(label, viewport)`, `capture_bundle(args)`,
`artifact(label, path)`, `log(text)`, `progress(dict)`. A stage probe renders on a
`ProbeStage` (its own `SubViewport` + `World3D`) so the live world never bleeds
into a capture.

The probe contract test (`godot/tests/probes/probe_catalog_test.gd`) pins: unique
names, script paths under `res://probes/`, every script loads as a `GameProbe`,
every schema validates its own defaults, and no probe source contains
`print(`, `OS.get_environment(` / `OS.has_environment(` or `res://tests/`. A
probe whose assertion is engine behaviour (world, sim, collision, anim, WAC,
mission runtime, net codecs) is not a probe: it is a ctest under `tests/<domain>/`
(gated on the retail roots when it needs retail data, `docs/asset-gated-tests.md`).

### The registered probes

| Family | Probe | Purpose |
|---|---|---|
| perf | `perf_sample` | warm then sample the frame-stats board for `sample_ms` (counter list optional) |
| perf | `perf_fire` | the fire-path timing at a pose (`pose_json`, `look_dy`, `fire_seconds`, `weapon`; `baseline_only` on a `--lan-join` launch) |
| perf | `perf_sweep` | the phase sweep (`phase_ms`) |
| perf | `perf_mission_rows` | the mission-load / re-ground rows (`warmup_seconds`, `window_seconds`, `windows`, `label`) |
| perf | `frame_stats` | one settled frame-stats window (`settle_ms`, `window_ms`) |
| render | `render_fixture_capture` | the exact-pose render fixture publication (`id`, `catalog`, `mode`, `profile`, `output_dir`, `mission_resource_dir`, `source_commit`, `gdextension_binary`) |
| render | `render_swatch` | the material swatch A/B driver on its own stage (`mode`: capture, composite, lighting, channels, clip, projshadow, matchterrain, glow, calibrate, compare) |
| render | `foliage_spawn_capture` | the frozen-spawn retail comparison capture (`mission`, `mission_path`, `expansion`, `flicker`, `model_lighting_trace`) |
| render | `environment_cube_capture` | the environment-cube proof on Forward+ D3D12 |
| render | `loading_screen_render` | the loading screen for one mission |
| stage | `loading_screen_stage`, `effects_visual`, `firebarrel_visual`, `gore_set_visual`, `bridge_water_shock`, `retail_parity_visual`, `foliage_flicker_regression` | the visual stage scenes and their captures |
| runtime | `dialog_vs_ambient`, `mission_audio`, `fp_impact`, `hud_killfeed`, `vm_bone_dump`, `weapon_round` | audio, HUD and viewmodel witnesses in the live runtime |
| runtime | `runtime_root_window` | the embedded game view (ADR 0039's debug windowed startup): one always-updating SubViewport under `GameRuntimeRoot`, the ImGui context attached, the tools workspace hiding only the direct composite, a resize reaching the viewport, F3 through the Game texture closing the workspace |
| runtime | `window_fullscreen` | the F11 policy: windowed -> fullscreen -> windowed through `WindowState`, each state presenting a lit root frame and, in the embedded game view, the game viewport following the window size with its own frame lit |
| net | `parity_joiner_ready` | wait for `in_match` or `deploy_hold` readiness on a `--lan-join` launch (`auto_deploy` sends the default pick); the witness the parity runner classifies |
| net | `parity_joiner_motion` | the walk/strafe/turn exercise inside the runner's steady window |
| net | `parity_joiner_state` | one live readiness snapshot |

`python scripts/mcp/game_mcp.py probe list` prints the current catalog with
each schema; the table above is the map, the catalog is the truth.

## Writing a probe

1. Put the script under `godot/probes/<family>/<name>_probe.gd`, `extends GameProbe`,
   implement `run(ctx)`; read arguments from `ctx.args` only.
2. Register it in `ProbeCatalog.definitions()` with its schema, preconditions
   and timeout; the contract test picks it up.
3. Every mutation goes through a `ctx` setter or `ctx.defer_restore`; never hold
   a `Simulation`/`Node` reference across an `await`.
4. Log with `ctx.log`, never `print`; publish structured progress with
   `ctx.progress`; write files into `ctx.artifact_dir` and register them with
   `ctx.artifact`.
5. Smoke it: `python scripts/mcp/game_mcp.py launch --windowed --resource-dir
   "$OPENNOVA_JO_DIR" --mission <bms>` then `probe run <name> '<json>' --wait`;
   a `--mission` launch parks under the start splash with the world un-ticked,
   so a mission probe starts with `await ctx.wait_for_local_player()`.

## Recipes that replaced the retired scripts

- Looking around from a pose: `game_debug invoke teleport_local_player
  {position, yaw_deg, pitch_deg}` then `local_player_look {dx_px, dy_px}`, then
  `game_capture_bundle` (formerly the bend-capture and render-align probes).
- A time-of-day visual baseline: `game_debug set environment_time_of_day`
  four times with a `game_capture_bundle` each, compared with `render_swatch
  {mode: compare}` (formerly `env_visual_baseline_probe`).
- A LAN pair: `scripts/net/run_lan_pair.ps1 -Mission 01TR.bms` (host 8975,
  joiner 8976), then `game_state` on either port (formerly the `NW_LAN_*`
  environment launch).
- Bytes out of a mounted install: `opennova-extract --game <dir> [/exp x]
  weapon.def M82_1st.adm --out <dir>` (formerly the dump probes).
- The ONED boundary (ADR 0037) is unchanged: ONED has no MCP server; it runs
  the game, and the game carries the endpoint.
