---
name: oned-run
description: Launches this repo's Godot project — the ONED editor or the game runtime — with the native extension and game assets correctly set up, and watches its output. Use when asked to run, launch, demo, screenshot, or visually verify the editor/runtime, when the generic run/verify skills need this project's launch procedure, or when the editor seems to be running stale native code.
---

# Run ONED / the OpenNova runtime

The Godot project is `godot/` (Godot 4.6.1). The default scene is
`res://modtools/editor/editor_main.tscn` — the EditorApp root hosting the whole
ONED shell with all twelve workspaces. The game runtime is
`res://game/main_game.tscn`. All commands are Git Bash, from the repo root.

## 1. Find the Godot binary

Use `GODOT_BIN` if set; otherwise the main checkout's `.godot-bin/` (worktrees
do NOT have their own copy):

    main="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
    export GODOT_BIN="$main/.godot-bin/Godot_v4.6.1-stable_win64_console.exe"

Prefer the `_console.exe` build so stdout reaches the terminal. If neither
exists, ask the user — do not download one.

## 2. Preflight the GDExtension (most failures start here)

- Fresh worktree: `git submodule update --init --recursive` (third_party/ ships
  empty), then `bash scripts/build_godot.sh` — `godot/bin/` contains only
  `opennova.gdextension` until you build, and nothing Godot-side works without
  the DLL. The editor and any `--path godot` run load `template_debug`.
- Stale code: if behavior doesn't reflect new `godot/engine/` C++, compare the
  timestamp of `godot/bin/libopennova.windows.template_debug.x86_64.dll`
  against the source change, rebuild, and FULLY restart Godot — GDExtension
  registration does not hot-reload, and a running editor holds the DLL lock.
- Also check for a stale `build/Debug/opennova.dll` shadowing fresh native
  code; delete it.
- Failure signature of a stale/missing DLL: parse errors naming `Nova*`
  classes, or `Ignoring script ... does not extend GutTest` in test runs.
- Fresh worktree or a branch switch that adds resources: run once
  `"$GODOT_BIN" --headless --path godot --import`

## 3. Assets

ONED and the runtime share one external "resource root" persisted in
`user://terrain_editor_state.cfg` (see
`godot/engine/resource_index/resource_dir_settings.gd`) — an interactive launch
on a machine that has used ONED before usually just works. Env vars are for
automation only:

- `NOVA_RESOURCE_DIR` — screenshot driver's asset dir (falls back to the
  persisted root). See `scripts/capture_screenshots.sh` header for contents.
- `JO_ASSETS_DIR` — retail loose-asset dir for the headless perf probes.

These point at copyrighted retail assets and are machine-specific: if unset and
needed, ask the user for the path; never guess or commit one.

## 4. Launch — pick the mode

- Interactive / visual check (needs a real window): prefer the MCP godot
  server — `mcp__godot__run_project` with the absolute path to `godot/`
  (optional `scene` for `res://game/main_game.tscn`), then poll
  `mcp__godot__get_debug_output`, and finish with `mcp__godot__stop_project`.
  Raw fallback (run in background): `"$GODOT_BIN" --path godot [scene]`.
- Game-under-ONED (the managed F5/F6 child, ADR 0025): when ONED is running
  with its MCP server, drive the game through the repo's own `oned` server
  instead — `run_game(op="start", mode="game"|"mission")` mirrors F5/F6,
  `game_state`/`game_debug`/`game_screenshot` observe and mutate the live
  runtime, `run_game(op="stop")` mirrors F8. The child mounts the editor's
  resource dir (`--resource-dir` + `--loose-root`: a loose authoring dir with
  no PFFs plays as-is).
- Headless scripted observation: the probe pattern — `SceneTree` scripts named
  `*_probe.gd` under `godot/tests/` (not collected by GUT), e.g.
  `"$GODOT_BIN" --headless --path godot -s res://tests/runtime_scene_probe.gd`.
  For a new one-off check, copy an existing probe's shape.
- README screenshots: `bash scripts/capture_screenshots.sh` (desktop session,
  NOT headless; validates every PNG was rewritten).
- `mcp__godot__launch_editor` only when you need the Godot editor UI itself
  (scene/inspector authoring) — it is not how you run ONED.
- Anything needing CLI flags (`--headless`, `--import`, `-s`, GUT) → raw
  `$GODOT_BIN`, since `run_project` only takes a project path and scene.

## 5. Observe and finish

Watch stdout / `get_debug_output` for `SCRIPT ERROR`, `ERROR:`, and
GDExtension load complaints (go back to step 2 if seen). Done = launched
cleanly, the target behavior was observed and described (including any
errors), and the process is stopped (`stop_project` or kill the backgrounded
PID) — never leave a headless Godot running.

Read `godot/modtools/README.md` for the workspace map when deciding where to
look for a feature.
