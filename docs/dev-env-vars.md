# Dev environment variables and launch flags — the registry

The runtime, the tools and the tests read a closed set of environment
variables; everything else that used to be an env hook is a launch flag, an
argv option, a debug-catalog action or an MCP tool argument. If a variable is
not listed here it must not exist: `scripts/lint/env_lint.py --enforce` (CI)
fails on any other read, and a new hook lands here in the same PR that adds it.

## The two machine roots

Read only by the four resolvers — `tests/common/retail_paths.h` (ctests),
`godot/tests/support/retail_data.gd` (GUT), `scripts/net/lib.ps1` (the
PowerShell getters), `tests/blender/addon_harness.py` (the Blender tests'
`retail_asset`, `OPENNOVA_JO_ASSETS` only) — plus the `--resource-dir` default of
`scripts/mcp/game_mcp.py launch`. The test-runner preflight in
`scripts/ci/test_suites.py` checks that both roots name existing directories.
Machine paths go in
`.claude/settings.local.json` `env`, never in tracked files.

| Var | Points at | Who consumes it |
|---|---|---|
| `OPENNOVA_JO_DIR` | a packed retail JO install (the `.pff` set, expansions under `expansion/`) | the `JO_DIR`-gated ctests and GUT tests ([asset-gated-tests.md](asset-gated-tests.md)), the `game_mcp.py launch` default, the render/net scripts' defaults |
| `OPENNOVA_JO_ASSETS` | an extracted retail asset tree (`items.def`, `weapon.def`, models, `.adm`, the shipped `.bms` missions loose at its root, the reference fixture set under `fixtures/`) | the `JO_ASSETS`-gated ctests and GUT tests (including `mission_corpus` and the GUT corpus binding), the render fixture capture's loose mission |

`--suite core` unsets both roots. `--suite retail` requires both and fails on
any skipped compatibility test or leg. The default `--suite all` permits
explicit missing-data skips: exit 77 for a wholly gated ctest, or `SKIP-LEG:`
inside a mixed test's separately registered retail invocation. See the
[suite commands and coverage rules](asset-gated-tests.md).

## The scripts

| Var | Effect |
|---|---|
| `GODOT_BIN=<path>` | which Godot 4.6.1 binary `scripts/*.sh`, `scripts/**/*.ps1` and `scripts/mcp/game_mcp.py` use (else the main checkout's `.godot-bin/`) |

## The deployed service

`apps/novaworld_server/`, `deploy/`, `web/`, `backend/` and `infra/` keep
their own 12-factor configuration (`ONNET_*`, `DATABASE_PATH`, `ADMIN_*`, the
service's `NW_LOG_DEBUG`); see `DEPLOY.md`.
The lint treats those trees as a family, not a list.

## OS variables

`TEMP`/`TMP` (`tests/common/test_paths.h`),
`APPDATA`/`HOME`/`XDG_DATA_HOME` (`scripts/test_godot.sh`'s isolated
`user://`), `ProgramFiles`/`SystemRoot` (`scripts/net/lib.ps1` tool discovery).

## Launch flags (the game shell)

Game flags ride after `--` on the Godot command line; `LaunchFlags`
(`engine/base/resource_index/boot_policy.*`) parses them once for the shell
and the bindings. `scripts/mcp/game_mcp.py launch` forwards each as a named
option.

| Flag | Effect |
|---|---|
| `--resource-dir <dir>` | the game data root (a packed install or a loose tree); without it the game boots the bundled `assets/` menu (ADR 0048) |
| `/exp <name>` | mount that expansion on top of the base set |
| `/game <code>` | the game profile (SCR policy) |
| `/d` | loose files override archive entries |
| `--loose-root`, `--loose-mission <name.bms>` | treat the data directory as a loose tree (a bare flag) / boot the named loose mission from it |
| `--mission <name.bms>` | boot straight into a single-player mission (the world parks un-ticked under the start splash until the player is spawned) |
| `--lan-host <mission.bms>` | boot as a co-op LAN listen host |
| `--lan-join <ip[:port]>` | boot as a LAN joiner dialing that host (the mission comes from the wire, D-NET-194) |
| `--spectator` | with `--lan-join`, explicitly authenticate as a spectator instead of showing the Player/Spectator prompt |
| `--spectator-password <text>` | spectator password sent as ClientAuth `JSPP` (the retail host field is limited to 17 characters) |
| `--lan-port <1..65535>` | host bind port (default: the retail LAN range head, `npwire/net_ports.h`) |
| `--lan-gametype <n>` | numeric `g_GameType` (`0x` accepted; default Co-op) |
| `--lan-mode <1..4>` | host LAN rate mode (the witnessed holdoffs 12/6/4/3) |
| `--lan-max-players <1..64>` | listen-host capacity |
| `--callsign <name>` | the local player's callsign (15-char wire clamp) |
| `--integrity-profile <id>` | the integrity profile the host advertises / the joiner presents; applies to every joiner entry (`--lan-join` and NovaWorld browser joins). Default empty = the joiner leaves anti-cheat CRC challenges unanswered (the parity-safe posture, D-NET-181) |
| `--capture-pcap <path>` | write the session's UDP traffic to a pcap |
| `--mcp-port <1..65535>` | start the `opennova-game` MCP endpoint on that port ([mcp.md](mcp.md)) |

## Debug controls that replaced post-boot hooks

The typed debug-control table (`DebugControlTable`, driven as `game_debug` over MCP) carries the actions the old env
hooks performed after boot: `teleport_local_player {position, yaw_deg,
pitch_deg}` (the debug pose), `deploy_pick {zone}` (the auto-deploy hook),
`set_viewmodel_weapon {weapon}` / `clear_viewmodel_weapon` (the viewmodel
override), `kill_group {group}`, `crew_vehicle {occupant_ssn, vehicle_ssn}`,
`crew_local_player {vehicle_ssn}` (the AI console), `local_player_look
{dx_px, dy_px}`, the
`net_joiner_diagnostics` check (the joiner tripwire diagnostics) and
`third_person_on_foot` (the third-person override). `local_spectator`
switches an authority-owned local player onto the real spectator slot state;
it is confirmation-gated like the other authoritative mutations.

The same control is visible as **Spectator** in F3's Game window. Check it,
enter **Play**, then hold right mouse and use WASD/Q/E to free-fly; the match
continues ticking. Uncheck it to restore the player's team and respawn through
the normal map spawn selection.

## Argv options that replaced env knobs

| Tool | Option |
|---|---|
| `nw_pp` | `--hexcap-max <n>` |
| `opennova-extract` | `--game <dir> [/exp <name>] [/game <code>] [/d] --out <dir> <name>...` |
| `renderer_state_vectors_test`, `nw_codec_identity_test` | `--dump` (print the replacement vector table) |
| `nw_self_capture_test` | `--write-fixture` (regenerate `fixtures/novaworld/self-capture-session.pcap`) |
| `minimal_*_gen_test` | `--write` (regenerate that generator's minted fixtures) |
| `ai_path_conformance_test` | `--report`, `--ticks`, `--bms` |
| `mnu_compat_test` | extra loose menus as positional arguments |
| `wac_corpus_test` | extra corpus directories as positional arguments |
| `scripts/build.sh` | `--no-godot`, `--jobs N`, `--suite core\|retail\|all`; `scripts/build_godot.sh [Dev\|DebugFull\|Release] [--jobs N]`; `scripts/test_godot.sh --keep-user-dir --suite core\|retail\|all [--windowed]` |
| `scripts/ida/cite_sweep.py` | `--url` |

GUT-side regeneration is an uncollected script run alone:
`godot/tests/tools/env_vectors_regen.gd`
(`-gtest=res://tests/tools/env_vectors_regen.gd -gunit_test_name=test_regen -gexit`).
