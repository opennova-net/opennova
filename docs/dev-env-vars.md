# Dev environment variables — the registry

Every deliberate env hook the runtime, tools, and libraries honor. If a variable
is not listed here (or in [asset-gated-tests.md](asset-gated-tests.md), which
owns the test-data gates), it should not exist — the quality campaign removed
the strays, and new hooks land here in the same PR that adds them.

## Launch hooks (the game shell / NetSessionController)

| Var | Effect |
|---|---|
| `NW_SP_MISSION=<name.bms>` | boot straight into a single-player mission (skips menu navigation) |
| `NW_LAN_HOST=<mission.bms>` | boot as a co-op LAN listen host (the demo/smoke path) |
| `NW_LAN_JOIN=<ip[:port]>` | boot as a LAN joiner dialing that host |
| `NW_LAN_MISSION=<name.bms>` | joiner-side explicit mission override (debug; the normal path learns it from S2C 0x7B) |
| `NW_LAN_PORT=<port>` | host bind-port override (default: the retail LAN range head, `npwire/net_ports.h`) |
| `NW_LAN_GAMETYPE=<n>` | numeric g_GameType override (default Co-op `HostSessionConfig.GAME_TYPE_COOP`) |
| `NW_LAN_NAME=<callsign>` | callsign override for two-instance same-machine demos (15-char wire clamp applies) |
| `NW_LAN_MODE=<1..4>` | host LAN rate-mode override (the witnessed holdoffs 12/6/4/3; out-of-range values keep the default) |
| `NW_LAN_MAX_PLAYERS=<1..64>` | listen-host capacity override for the env-boot path |
| `NW_SP_DEBUG_POSE="x,y,z[,yaw_deg]"` | one-shot teleport of the local player beside `NW_SP_MISSION` — pose-matched retail captures |

## Headless `nw_server` host rules

| Var | Effect |
|---|---|
| `NW_GAME_TYPE=<n>` | exact decimal or `0x`-prefixed retail `g_GameType` override; otherwise the mission-authored mode wins |
| `NW_NUM_TEAMS=<1..255>` | active-team count for retail's multi-team modes (normally 2 or 4) |
| `NW_CAPTURE_DURATION_SECONDS=<i32>` | un-numbered takeover duration; retail default 15, values ≤0 select the instant branch `[orig: Config_SetDefaults @0x54D030; Server_UpdateCaptureZones @0x53B8F0]` |
| `NW_CAPTURE_SPEED_SETTING=<i32>` | numbered-zone secure-speed selector; retail default 1 (bases: fallback 12, 1→24, 2→48) `[orig: Config_SetDefaults @0x54D030; calculate_capture_zone_control_delta @0x501120]` |
| `NW_DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE=<u32>` | nonzero enables retail cfg `nodefaultspawnpoints`: target-less deployment is denied while the team owns an unnumbered or fully controlled numbered spawn zone `[orig: Server_ProcessClientRequestRespawn @0x519C8E; Entity_HasAliveEntityOfTeam @0x4FC7B0]` |

## Diagnostics (all off by default)

| Var | Effect |
|---|---|
| `OPENNOVA_NET_DIAGNOSTICS=1` | the joiner freeze-tripwire diagnostics (~1 Hz; renders via `print_verbose`, so run with `--verbose`) |
| `NW_LOG_DEBUG=1` | `nw_server` / `novaworld_server`: forward `io/log.h` kDebug messages (e.g. the per-tick burst trace) to the console sink |
| `OED_STRIPIFY_DEBUG` / `OED_STRIPIFY_SEED_DIAG=1` | 3DI stripify diagnostics through the `io/log.h` sink (`forceA`/`forceB` also select a pass) |
| `TRNGEN_TRACE=<path>` | terrain builder: write the TrnGen trace file |

## Probe/test seams (used by `godot/tests` probes; not runtime features)

| Var | Effect |
|---|---|
| `NOVA_VM_WEAPON=<WPN_*>` | viewmodel weapon override — the `game_world_test` A/B seam |
| `GODOT_BIN=<path>` | which Godot binary the sh scripts use (else `scripts/godot_bin.sh` resolves `.godot-bin/`) |
| `NW_RESOURCE_DIR=<dir>` | the retail install an `NW_SP_MISSION` probe mounts |
| `NOVA_RESOURCE_DIR=<dir>` | the loose or PFF-mounted retail game directory a probe hands to `ResourceRoot.mount_runtime`; the widest of the resource-dir names, read by ~30 `godot/tests/*_probe.gd` scripts including the four `00trg_*` probes (`00trg_briefing`, `00trg_rock_collision`, `00trg_world_state`, `00trg_zone_chain`), each of which fails fast when it is unset |
| `NW_PROBE_SHOTS=<dir>` | ladder probe: save action-shot PNGs there (non-headless run) |
| `NW_PROBE_BOTTOM=1` | ladder probe: full-span diagnostic — enter at the base |
| `SPLASH_CAPTURE_DIR=<dir>` + `OPENNOVA_JO_DIR` | the splash render probe: capture output dir + the retail install it mounts |
| `OPENNOVA_WAC_CORPUS_DIRS=<dir;dir;...>` | `wac_corpus_test`: extra corpus dirs (semicolon list) |
| `OPENNOVA_MNU_EXTRA=<path.mnu>` | `mnu_compat_test`: a developer-only loose menu to include |

## Test-data gates

Owned entirely by [asset-gated-tests.md](asset-gated-tests.md) (`OPENNOVA_JO_DIR`,
`OPENNOVA_MODSUPEROED_DIR`, `NW_GOLDEN_*`, …) — machine paths go in
`.claude/settings.local.json` `env`, never tracked. `OPENNOVA_JO_ASSETS` now also
gates ctest `root_motion` and the two sound GUT tests
(`sound_dialog_test.gd`, `sound_integration_test.gd`).

## Known debt (recorded, not yet unified)

The probe scripts under `godot/tests/` grew four prefix families
(`NOVA_*`/`NW_*`/`JO_*`/`OPENNOVA_*`), and the resource directory answers to
several names across them. Unifying those renames variables in maintainers'
local environments, so it is deliberately a dedicated slice with maintainer
sign-off rather than a drive-by.
