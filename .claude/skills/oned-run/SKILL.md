---
name: oned-run
description: Runs this repo's ONED utility or OpenNova game runtime with the native extension and game data correctly set up, then watches and stops the process. Use when asked to run, launch, demo, or visually verify ONED/the runtime, or when either appears to load stale native code. For driving a running game (state, captures, probes) use the game-mcp skill.
---

# Run ONED / the OpenNova runtime

The Godot project is `godot/` (Godot 4.6.1). Its default main scene is the
game runtime root, `res://game/game_runtime_root.tscn` (on a debug windowed
run with the imgui-godot addon it embeds MainGame in a SubViewport for the F3
Game window; otherwise it hands off to `res://game/main_game.tscn` directly,
`GameRuntimeRoot.should_embed_game_view`). The ONED scene
`res://modtools/oned_main.tscn` is the `modtools` feature override for
the packaged `opennova-modtools.exe`; pass that scene explicitly when running
ONED from source. ONED is run-only (ADR 0037): Settings, Run OpenNova,
Stage & Run Retail, and Stop. It has no workspaces and no MCP server; the game
it runs carries the `opennova-game` endpoint (`docs/mcp.md`, the `game-mcp`
skill).

All commands below are Git Bash from the repository root.

## 1. Find the Godot binary

Use `GODOT_BIN` if set; otherwise use the main checkout's `.godot-bin/`
(linked worktrees do not have their own copy):

```bash
main="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
export GODOT_BIN="$main/.godot-bin/Godot_v4.6.1-stable_win64_console.exe"
```

Prefer the console build so stdout reaches the terminal. If neither exists,
ask the user; do not download one. The console wrapper stalls when launched
from a shell without a console (a hidden automation shell): launch from a
terminal, or point `GODOT_BIN` at the plain runtime executable.

## 2. Preflight the GDExtension

- Fresh worktree: initialize submodules, then run `bash scripts/bootstrap_godot.sh`
  (it installs GUT and the imgui-godot addon `OnedUi` draws through;
  `scripts/build_godot.sh` does NOT run it), then `bash scripts/build_godot.sh`.
  Nothing Godot-side works without the DLL, and without the addon `OnedUi` has
  no ImGui context.
- If `godot/src/` behavior looks stale, rebuild and fully stop every running
  Godot/ONED process. GDExtension registration does not hot-reload, and a live
  process can hold the DLL lock.
- After a fresh checkout or resource-heavy branch switch, run
  `"$GODOT_BIN" --headless --path godot --import` once.
- A stale or missing DLL usually appears as parse errors naming engine classes
  (`Simulation`, `Terrain`, `ResourceRoot`, `DevTools`, `OnedUi`, ...) or
  silently dropped GUT scripts.

## 3. Game data

ONED persists its selected loose or packed game-data directory in `user://oned.cfg`. A
packaged dev build with no explicit choice defaults to the `assets/` directory
beside its executable. Running retail also needs a user-selected JO install
containing `Jointops.exe`, Bink, and `game.cfg`.

From source, the game takes its data as launch flags after `--`
(`docs/dev-env-vars.md`): `--resource-dir <dir>` (a packed install or a loose
tree), `/exp <name>`, `--mission <name.bms>` to boot straight into a mission,
`--lan-host`/`--lan-join` for a LAN role. The documented machine roots
(`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`, `OPENNOVA_MISSION_CORPUS`) live in
`.claude/settings.local.json` `env`; they may point at copyrighted retail
assets. If a required value is unset, ask the user; never guess or commit a
local path.

## 4. Launch the requested product

- Game from source, windowed:
  `"$GODOT_BIN" --path godot -- --resource-dir "$OPENNOVA_JO_DIR" [--mission 00TRa.bms]`
- Game from source with its MCP endpoint (drive it with the `game-mcp` skill):
  `python scripts/mcp/game_mcp.py launch --windowed --resource-dir "$OPENNOVA_JO_DIR"`
- ONED from source, windowed:
  `"$GODOT_BIN" --path godot res://modtools/oned_main.tscn`
- Packaged dev build: run `opennova-modtools.exe`, select the loose source
  tree in Settings, then use Run OpenNova, Stage & Run Retail, or Stop.
  ONED owns one child; starting another mode replaces it.
- Headless pack smoke:
  `opennova-modtools.exe --headless -- --pack-game <src_dir> <game_dir>`.
  Success requires exit 0 and `localres.pff` in the destination.
- Observation, captures and runtime probes go through the game MCP
  (`game_probe`, `docs/mcp.md`); the former `godot/tests/*_probe.gd` scripts
  are gone (ADR 0041). Use raw `$GODOT_BIN` for CLI flags such as
  `--headless`, `--import`, and GUT.

## 5. Observe and finish

Watch output for `SCRIPT ERROR`, `ERROR:`, and GDExtension load complaints.
Done means the target launched cleanly, the requested behavior was observed,
and every process was stopped. Never leave a headless Godot or an ONED-managed
game running.

Read `godot/modtools/README.md` for ONED, retail-stage, and release-pack
contracts.
