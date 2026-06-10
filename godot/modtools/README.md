# OpenNova Editor (ONED)

The OpenNova Editor (ONED) is the authoring side of OpenNova: a Godot-hosted,
code-first editor for NovaLogic Joint Operations (JO) and newer game data. Each
kind of asset gets its own *workspace*, and every workspace reads and writes the
game's canonical formats directly. Everything here is pre-1.0 and under active
development.

OpenNova reimplements the NovaLogic engine on a Godot host; ONED is where you author
the data that engine runs. See [GOALS.md](../../GOALS.md) for the project vision.

This folder holds the editor shell, the workspace framework, and one subfolder
per workspace.

## Workspaces

The left nav groups workspaces into three categories. The order and grouping come
from `EditorWorkstation._workspace_defs()` in
[`editor/editor_workstation.gd`](editor/editor_workstation.gd).

| Category | Workspace | Edits | Docs |
|---|---|---|---|
| World | Terrain | heightmaps, surface paint, foliage, tiles, layout (`.trn` / `.cpt` / `.til`) | [terrain/](terrain/README.md) |
| World | Object | 3D object projects (`.3di` / `.3dp`) | [object/](object/README.md) |
| World | Mission | missions: entities, waypoints, zones, BMS events, play in-editor (`.bms`) | [mission/](mission/README.md) |
| Interface | Fonts | bitmap fonts (`.fnt`) | [fonts/](fonts/README.md) |
| Interface | Credits | rolling credits (`.kda`) | [credits/](credits/README.md) |
| Interface | Strings | localized string tables (RTXT) | [strings/](strings/README.md) |
| Interface | Menu | menu screens (`.mnu` / `.mns`) | [mnu/](mnu/README.md) |
| Audio | Music | interactive music (`.sbf` + `.bin` script) | [music/](music/README.md) |
| Atmosphere | Sound | sound profiles (`.lwf`) | [sound/](sound/README.md) |
| Atmosphere | Environment | weather, lighting, time of day, sky/celestial (`.env`) | [environment/](environment/README.md) |

Mission edits `.bms` missions end to end: entities, waypoints, zones, and BMS
event scripting, with play-in-editor through the same mission runtime and present
pass the game host uses (one runtime; see
[`docs/runtime-architecture.md`](../../docs/runtime-architecture.md)).

Environment is a *popup* workspace: instead of swapping the main viewport it
overlays a panel on the active Terrain or Object view, so lighting changes are
visible on the scene you are editing.

## How it is built (code-first)

ONED has no `.tres` workspace resources, and the shell never switches on workspace
type. The shell reads capability hooks off each workspace, and workspaces and
inspectors are declared as typed registry rows:

- [`framework/editor_workspace.gd`](framework/editor_workspace.gd): the
  `EditorWorkspace` base class. It documents the full contract (identity,
  lifecycle, inspector, viewport, document actions, edit, asset dock) and gives
  every hook a safe default, so a workspace overrides only the tiers it needs.
- [`framework/workspace_def.gd`](framework/workspace_def.gd): `WorkspaceDef`, one
  registry row per workspace (id, adapter script, popup flag, nav category).
- [`framework/inspector_def.gd`](framework/inspector_def.gd): `InspectorDef`, one
  row per workflow inspector (id, label, tooltip, script).
- [`framework/inspector.gd`](framework/inspector.gd) and
  [`framework/list_detail_inspector.gd`](framework/list_detail_inspector.gd):
  `WorkflowInspector` and `ListDetailInspector`, base classes for multi-workflow
  inspectors.

### Add a workspace
1. Write an `EditorWorkspace` subclass implementing the hook tiers you need (see
   the contract in `framework/editor_workspace.gd`; `editor/fonts_workspace.gd`
   is a minimal single-pane example).
2. Add a `Workspace` enum entry in `editor/editor_workstation.gd`.
3. Append one `WorkspaceDef.make(Workspace.X, XWorkspaceAdapter, is_popup, &"Category")`
   row to `_workspace_defs()`.

### Add a workflow inspector
1. Write an inspector script: extend `WorkflowInspector` / `ListDetailInspector`
   (object style) or `TerrainInspector` (terrain style).
2. Append one `InspectorDef.make(id, "Label", "Tooltip", Script)` row to the
   workspace's `_build_inspector_defs()`.

Single-pane workspaces (Fonts, Credits, Strings, Sound, Menus, Environment) skip
the workflow rail and override `build_inspector(host)` instead.

## Folder map

| Path | Contents |
|---|---|
| `editor/` | the shell (`editor_workstation.gd`), the resource browser and library, the PFF archive tool, the export dialog, plus the Terrain, Environment, Fonts, Credits, Mission, and Music adapters |
| `framework/` | base classes and typed registries (`EditorWorkspace`, `WorkspaceDef`, `InspectorDef`) |
| `terrain/`, `object/`, `mission/`, `fonts/`, `credits/`, `strings/`, `mnu/`, `music/`, `sound/`, `environment/` | one workspace module each (editor model, UI, inspectors) |
| `tools/` | `screenshot_capture` automation helper (not a workspace) |

The editor binds to the shared C++ core through the GDExtension in
`godot/engine/`; on-disk formats are parsed by the libraries under
[`libs/`](../../libs). See the top-level [README](../../README.md) for the project
overview and build steps.
