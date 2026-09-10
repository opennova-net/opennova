---
name: editor-run
description: Runs the Godot world editor or OpenNova game with the native extension and game data correctly set up, then observes and stops the owned process. For a running game's state, captures and probes use game-mcp.
---

# Run the Godot editor or OpenNova

The Godot project is `godot/` (Godot 4.6.1). World authoring and run/staging/
packing controls live in the editor's OpenNova dock. There is no separate tools
executable. The game's main scene is `res://game/game_runtime_root.tscn`.

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
  (it installs GUT and the imgui-godot addon the game F3 tools draw through;
  `scripts/build_godot.sh` does NOT run it), then `bash scripts/build_godot.sh`.
  Nothing Godot-side works without the DLL, and without the addon `DevTools` has
  no ImGui context.
- If `godot/src/` behavior looks stale, rebuild and fully stop every running
  Godot process owned by this task. GDExtension registration does not hot-reload, and a live
  process can hold the DLL lock.
- After a fresh checkout or resource-heavy branch switch, run
  `"$GODOT_BIN" --headless --path godot --import` once.
- A stale or missing DLL usually appears as parse errors naming engine classes
  (`Simulation`, `Terrain`, `ResourceRoot`, `DevTools`, ...) or
  silently dropped GUT scripts.

## 3. Game data

The editor's WorldSource supplies its selected directory; local paths live in
Godot project metadata. Retail staging needs a JO installation with Jointops.exe,
Bink and game.cfg. See godot/tools/README.md for the supported source policies.

From source, the game takes its data as launch flags after `--`
(`docs/dev-env-vars.md`): `--resource-dir <dir>` (a packed install or a loose
tree), `/exp <name>`, `--mission <name.bms>` to boot straight into a mission,
`--lan-host`/`--lan-join` for a LAN role. The documented machine roots
(`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`) live in
`.claude/settings.local.json` `env`; they may point at copyrighted retail
assets. If a required value is unset, ask the user; never guess or commit a
local path.

## 4. Launch the requested product

- Game from source, windowed:
  `"$GODOT_BIN" --path godot -- --resource-dir "$OPENNOVA_JO_DIR" [--mission 00TRa.bms]`
- Game from source with its MCP endpoint (drive it with the `game-mcp` skill):
  `python scripts/mcp/game_mcp.py launch --windowed --resource-dir "$OPENNOVA_JO_DIR"`
- World editor: `"$GODOT_BIN" --editor --path godot res://examples/world_preview.tscn`.
  Use the OpenNova dock for Run Game / Stage & Run Retail / Stop, or the viewport
  toolbar's Play World. The editor owns one child; changing targets replaces it.
- Headless pack smoke:
  `"$GODOT_BIN" --headless --path godot --script res://tools/pack_game.gd -- --pack-game <src_dir> <game_dir>`.
  Success requires exit 0 and localres.pff in the destination.
- Observation, captures and runtime probes go through the game MCP
  (`game_probe`, `docs/mcp.md`); the former `godot/tests/*_probe.gd` scripts
  are gone (ADR 0041). Use raw `$GODOT_BIN` for CLI flags such as
  `--headless`, `--import`, and GUT.

## 5. Observe and finish

Watch output for `SCRIPT ERROR`, `ERROR:`, and GDExtension load complaints.
Done means the target launched cleanly, the requested behavior was observed,
and every process was stopped. Never leave a headless Godot or an editor-managed
game running.

Read `godot/tools/README.md` for editor run, retail staging and release packaging contracts.
