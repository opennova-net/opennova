# ONED

ONED is OpenNova's developer-facing game-run utility, shipped as
`opennova-modtools.exe`. Its complete user-facing scope is:

- store the game-data directory (loose sources or a packed game), recent directories, game/expansion,
  and retail Joint Operations install;
- **Run OpenNova** against the selected loose or packed game;
- **Stage & Run Retail** against a staged copy of that same tree; and
- **Stop** the one child process ONED owns.

[ADR 0037](../../docs/adr/0037-oned-runs-game-data.md) records the hard cut
from the former workspace UI. There is no project file, import database,
asset browser, document model, preview runtime, workspace framework, or
embedded MCP server.

The surface is drawn by the engine: `OnedUi`
(`engine/runtime/devtools/oned_ui.*`, [ADR 0039](../../docs/adr/0039-in-engine-dev-tools.md))
is one Dear ImGui window rendered through the imgui-godot addon; `oned_app.gd`
seeds its fields, pushes recents/readiness/status, and executes the typed
requests it reports (run, stage & run, stop, browse, apply). Process
spawning, the native directory dialogs and `user://oned.cfg` stay in GDScript.

## Loose OpenNova runs

Run starts the sibling `opennova` executable through the standalone game's
normal scene. The selected directory is passed with `/d`, `--resource-dir`,
and `--loose-root`, plus the configured game code and optional expansion. A
directory with boot PFFs mounts as a packed game; otherwise it mounts loose.
ONED does not save, copy, import, or transform the selected files.

A packaged dev build defaults to the `assets/` directory beside ONED when no
explicit directory is saved. That implicit default is not persisted, so an
explicit Settings choice always wins.

Starting another run replaces the one managed child. Stop terminates that
owned process, waits for the bounded exit result, and releases the handle. The
handle-backed path prevents a recycled PID from being mistaken for ONED's
child.

## Retail compatibility runs

Stage & Run Retail requires a configured JO install containing
`Jointops.exe`, Bink, and `game.cfg`. ONED stages the selected data under
`user://packed`, preserving existing PFFs or adding the zero-entry
`resource.pff` boot token for loose roots. It copies the required retail
runtime files, then runs:

```text
Jointops.exe /w /d /FRISK
```

The child working directory is the staged directory because retail opens its
boot archives relative to the process working directory. The source tree is
never modified. Repacking clears only output carrying ONED's own marker, and
refuses to wipe an unrelated non-empty directory.

## Release pack command

The exported ONED executable also carries one hidden build command:

```text
opennova-modtools.exe --headless -- --pack-game <src_dir> <game_dir>
```

CI uses it to build the tagged-release game tree with the same shipped code.
This path writes `localres.pff` plus files that must remain loose, notably
`.sbf` music banks and `earlyerr.txt`. It is deliberately different from the
loose `/d` retail-test stage and is not exposed as an ONED action.

The executable name, the `OpenNova Mod Tools` export preset, the `modtools`
feature tag, and the `--pack-game` flag are packaging contracts and remain
stable across the ONED hard cut.

## Source ownership

ONED treats the selected directory as an external, source-controlled game-data
tree. Asset creation belongs to format-specific tools and external DCC
applications such as Blender. Nova formats continue to use their direct
`load_from_path`/`save_to_path` interfaces; ONED does not integrate them with
Godot's resource import system or create sidecar metadata.
