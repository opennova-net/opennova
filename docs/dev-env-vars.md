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
| `NW_REPLAY=<host:port>` | boot straight into the replay/spectate view dialing that source |
| `NW_REPLAY_DIR` / `NW_REPLAY_LOOSE` / `NW_REPLAY_ITEMS` | spectate resource dir / flat-extract flag / items.def override |

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
| `NOVA_VM_WEAPON=<WPN_*>` | viewmodel weapon override — the `body_holds_probe` / `game_world_test` A/B seam |
| `GODOT_BIN=<path>` | which Godot binary the sh scripts use (else `scripts/godot_bin.sh` resolves `.godot-bin/`) |

## Test-data gates

Owned entirely by [asset-gated-tests.md](asset-gated-tests.md) (`OPENNOVA_JO_DIR`,
`OPENNOVA_MODSUPEROED_DIR`, `NW_GOLDEN_*`, …) — machine paths go in
`.claude/settings.local.json` `env`, never tracked.

## Known debt (recorded, not yet unified)

The probe scripts under `godot/tests/` grew four prefix families
(`NOVA_*`/`NW_*`/`JO_*`/`OPENNOVA_*`), and the resource directory answers to
several names across them. Unifying those renames variables in maintainers'
local environments, so it is deliberately a dedicated slice with maintainer
sign-off rather than a drive-by.
