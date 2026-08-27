---
name: oned-run
description: Runs this repo's ONED utility or OpenNova game runtime with the native extension and loose game data correctly set up, then watches and stops the process. Use when asked to run, launch, demo, or visually verify ONED/the runtime, or when either appears to load stale native code.
---

# Run ONED / the OpenNova runtime

The Godot project is `godot/` (Godot 4.6.1). Its default main scene is the
game runtime, `res://game/main_game.tscn`. The ONED scene
`res://modtools/oned_main.tscn` is the `modtools` feature override for
the packaged `opennova-modtools.exe`; pass that scene explicitly when running
ONED from source. ONED is run-only (ADR 0037): Settings, Run OpenNova,
Stage & Run Retail, and Stop. It has no workspaces or MCP server.

All commands below are Git Bash from the repository root.

## 1. Find the Godot binary

Use `GODOT_BIN` if set; otherwise use the main checkout's `.godot-bin/`
(linked worktrees do not have their own copy):

```bash
main="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
export GODOT_BIN="$main/.godot-bin/Godot_v4.6.1-stable_win64_console.exe"
```

Prefer the console build so stdout reaches the terminal. If neither exists,
ask the user; do not download one.

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

Probe environment variables remain machine-specific:

- `JO_ASSETS_DIR` is the retail loose-asset directory for headless perf probes.
- `NW_RESOURCE_DIR` is the retail install mounted by a standalone mission probe.
- `NW_SP_MISSION=<bms>` selects that probe's mission.
- `NW_SP_DEBUG_POSE="x,y,z[,yaw_deg]"` pins its player pose.

These may point at copyrighted retail assets. If a required value is unset,
ask the user; never guess or commit a local path.

## 4. Launch the requested product

- Game from source, windowed:
  `"$GODOT_BIN" --path godot`
- ONED from source, windowed:
  `"$GODOT_BIN" --path godot res://modtools/oned_main.tscn`
- Packaged dev build: run `opennova-modtools.exe`, select the loose source
  tree in Settings, then use Run OpenNova, Stage & Run Retail, or Stop.
  ONED owns one child; starting another mode replaces it.
- Headless pack smoke:
  `opennova-modtools.exe --headless -- --pack-game <src_dir> <game_dir>`.
  Success requires exit 0 and `localres.pff` in the destination.
- Headless observation: run an existing `*_probe.gd` SceneTree script under
  `godot/tests/`, for example
  `"$GODOT_BIN" --headless --path godot -s res://tests/runtime_scene_probe.gd`.
- Standalone game against retail data: set `NW_RESOURCE_DIR` and
  `NW_SP_MISSION`; use `godot/tests/ladder_climb_probe.gd` as the exemplar.

There is no ONED MCP automation path. Drive runtime behavior through the
standalone game/probe interfaces, and use raw `$GODOT_BIN` for CLI flags such
as `--headless`, `--import`, `-s`, and GUT.

## 5. Observe and finish

Watch output for `SCRIPT ERROR`, `ERROR:`, and GDExtension load complaints.
Done means the target launched cleanly, the requested behavior was observed,
and every process was stopped. Never leave a headless Godot or an ONED-managed
game running.

Read `godot/modtools/README.md` for ONED, retail-stage, and release-pack
contracts.
