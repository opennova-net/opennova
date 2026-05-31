# OpenNova Editor (ONED)

The OpenNova Editor (ONED) is the authoring side of OpenNova: a Godot-hosted,
code-first editor for NovaLogic Joint Operations (JO) and newer game data. Each
kind of asset gets its own *workspace*, and every workspace reads and writes the
game's canonical formats directly. Everything here is pre-1.0 and under active
development.

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
| World | Mission | placeholder (planned) | see below |
| Interface | Fonts | bitmap fonts (`.fnt`) | [fonts/](fonts/README.md) |
| Interface | Credits | rolling credits (`.kda`) | [credits/](credits/README.md) |
| Interface | Strings | localized string tables (RTXT) | [strings/](strings/README.md) |
| Atmosphere | Environment | weather, lighting, time of day (`.env`) | [environment/](environment/README.md) |

Mission is a registered placeholder
([`editor/mission_workspace.gd`](editor/mission_workspace.gd)): it holds a nav
slot in the World group and shows a "planned" message. Mission formats are not
implemented yet.

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
   the contract in `framework/editor_workspace.gd`; `editor/mission_workspace.gd`
   is a minimal example).
2. Add a `Workspace` enum entry in `editor/editor_workstation.gd`.
3. Append one `WorkspaceDef.make(Workspace.X, XWorkspaceAdapter, is_popup, &"Category")`
   row to `_workspace_defs()`.

### Add a workflow inspector
1. Write an inspector script: extend `WorkflowInspector` / `ListDetailInspector`
   (object style) or `TerrainInspector` (terrain style).
2. Append one `InspectorDef.make(id, "Label", "Tooltip", Script)` row to the
   workspace's `_build_inspector_defs()`.

Single-pane workspaces (Fonts, Credits, Strings, Environment) skip the workflow
rail and override `build_inspector(host)` instead.

## Folder map

| Path | Contents |
|---|---|
| `editor/` | the shell (`editor_workstation.gd`) plus the Terrain, Environment, Fonts, Credits, and Mission adapters |
| `framework/` | base classes and typed registries (`EditorWorkspace`, `WorkspaceDef`, `InspectorDef`) |
| `terrain/`, `object/`, `fonts/`, `credits/`, `strings/`, `environment/` | one workspace module each (editor model, UI, inspectors) |
| `tools/` | `screenshot_capture` automation helper (not a workspace) |

The editor binds to the shared C++ core through the GDExtension in
`godot/engine/`; on-disk formats are parsed by the libraries under
[`libs/`](../../libs). See the top-level [README](../../README.md) for the project
overview and build steps.
