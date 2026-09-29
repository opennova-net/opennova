---
name: game-mcp
description: Launches the OpenNova game runtime with its embedded MCP endpoint, drives it (state, debug catalog, menu, captures, logs) and runs the registered runtime probes through game_probe, then stops it cleanly. Use when asked to run, observe, capture or probe the game, or to reproduce a runtime-side finding.
---

# Drive the OpenNova game through its MCP

The game runtime embeds the `opennova-game` MCP server (`docs/mcp.md`, ADR
0041). One launch with `--mcp-port <n>` starts it; everything after launch is a
tool call. The retail game is driven separately through `onhook-mcp`
(`.agents/retail-lan-parity.md`).

## 1. Launch

```bash
python scripts/mcp/game_mcp.py launch --windowed --resolution 1280x720 \
    --resource-dir "$OPENNOVA_JO_DIR" [--exp revx02] [--mission 00TRa.bms] \
    --pid-file build/game.pid
```

- `--headless` when no window is needed (window probes then report
  `available: false`); `--windowed` otherwise (never rely on the project's
  fullscreen default under automation).
- The two machine roots come from `.claude/settings.local.json` `env`
  (`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`); never guess or commit a local
  path. `GODOT_BIN` names
  the binary (else the main checkout's `.godot-bin/`).
- A `--mission` launch parks under the start splash with the world un-ticked:
  every mission probe begins with `wait_for_local_player`.
- The console wrapper stalls from a shell without a console: launch from a
  terminal, or point `GODOT_BIN` at the plain runtime executable.
- Ports: 8975 single game (the tracked `.mcp.json`), 8975/8976 for a LAN pair, 8977 the editor (`scripts/mcp/editor_mcp.py`; docs/mcp.md "The editor MCP")
  (`scripts/net/run_lan_pair.ps1`). Claude Code sees a game started after the
  session began through `/mcp` reconnect, or through this script.

## 2. Drive

```bash
python scripts/mcp/game_mcp.py tools                       # the catalog
python scripts/mcp/game_mcp.py state                       # game_state
python scripts/mcp/game_mcp.py call game_debug '{"op":"invoke","action":"teleport_local_player","args":{"position":[x,y,z],"yaw_deg":90}}'
python scripts/mcp/game_mcp.py screenshot --out build/shot.png
python scripts/mcp/game_mcp.py entities --watch --interval 1 --out build/entities.jsonl
python scripts/mcp/game_mcp.py logs
```

`game_debug` drives the typed `DebugControlTable` (the C++ table F3 shares;
engine rows end in `Simulation`/`EntityCommands`, device rows in the shell;
ADR 0043 d12); entity
mutations take the `ai_index` from `game_entities` rows with `editable: true`.

## 3. Probe

```bash
python scripts/mcp/game_mcp.py probe list
python scripts/mcp/game_mcp.py probe run perf_sample '{"sample_ms":5000}' --wait
python scripts/mcp/game_mcp.py probe status <run_id> --cursor 0 --wait-ms 5000
python scripts/mcp/game_mcp.py probe cancel
```

One probe runs at a time; `--wait` polls status and exits 0 passed / 1 failed /
4 cancelled / 5 error. Artifacts land under `user://probe-runs/<run>/` and are
listed in the status. A probe whose verdict is engine behaviour is a ctest,
not a probe (`tests/<domain>/`, gated on the roots).

## 4. Stop

```bash
python scripts/mcp/game_mcp.py stop --pid-file build/game.pid
```

`stop` cancels an active probe, asks `game_control quit`, waits up to 20 s and
exits 6 if the process survives; it never kills. Done means the game launched,
the requested behaviour was observed through the tools, and no Godot process
remains.

Read `docs/mcp.md` for the tool and probe reference, `docs/dev-env-vars.md`
for the launch flags, and `godot/tests/CLAUDE.md` for where GUT and probes
diverge.
