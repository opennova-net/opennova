# OpenNova Editor (ONED)

The OpenNova Editor (ONED) is the authoring side of OpenNova: a Godot-based,
code-first editor for NovaLogic Joint Operations (JO) and newer game data. Each
kind of asset gets its own *workspace*, and every workspace reads and writes the
game's canonical formats directly. Everything here is pre-1.0 and under active
development.

OpenNova reimplements the NovaLogic engine on a Godot-based runtime; ONED is where you author
the data that engine runs. See [GOALS.md](../../GOALS.md) for the project vision.

This folder holds the editor shell, the workspace framework, and one subfolder
per workspace.

## Workspaces

The left nav groups workspaces into four categories. The order and grouping come
from `EditorWorkstation._workspace_defs()` in
[`editor/editor_workstation.gd`](editor/editor_workstation.gd).

| Category | Workspace | Edits | Docs |
|---|---|---|---|
| World | Mission | missions: entities, waypoints, zones, BMS events (`.bms`) | [mission/](mission/README.md) |
| World | Terrain | heightmaps, surface paint, foliage, tiles, layout (`.trn` / `.cpt` / `.til`) | [terrain/](terrain/README.md) |
| World | Object | 3D object projects (`.3di` / `.3dp`) | [object/](object/README.md) |
| World | Avatars | playable characters: head/body/arms combos by nationality and division, with a 3D preview (`Avatars.def`) | [avatar/](avatar/README.md) |
| Interface | Fonts | bitmap fonts (`.fnt`) | [fonts/](fonts/README.md) |
| Interface | Credits | rolling credits (`.kda`) | [credits/](credits/README.md) |
| Interface | Strings | localized string tables (RTXT) | [strings/](strings/README.md) |
| Interface | Menu | menu screens (`.mnu`) and the shared menu stylesheet (`.mns`): named colors, fonts, pictures | [mnu/](mnu/README.md) |
| Interface | HUD | in-game HUD layout preview (`hudpos.def`, read-only) | [hud/](hud/README.md) |
| Audio | Music | interactive music (`.sbf` + `.bin` script) | [music/](music/README.md) |
| Atmosphere | Particles | particle effects: explosions, smoke, muzzle flashes, water spray (`.ptl`) | [particle/](particle/README.md) |
| Atmosphere | Sound | sound profiles (`.lwf`) | [sound/](sound/README.md) |
| Atmosphere | Environment | weather, lighting, time of day, sky/celestial (`.env`) | [environment/](environment/README.md) |

Mission edits `.bms` missions end to end: entities, waypoints, zones, and BMS
event scripting. Testing always happens in the standalone game runtime.

Environment is a *popup* workspace: instead of swapping the main viewport it
overlays a panel on the active Terrain or Object view, so lighting changes are
visible on the scene you are editing.

## Run the game

The top bar exposes **Run game** (F5), **Run current mission** (F6), and
**Stop game** (F8) over one managed standalone process. The child mounts the
resource directory the editor has selected; loose files next to packed
archives win through the retail `/d` flag. F6 requires the Mission workspace's
current saved, top-level loose `.bms`. Starting F5 or F6 again restarts the one
managed process.

Running never saves, exports, copies, or stages editor data. Unsaved changes are
deliberately excluded, and F6 warns when it is launching an older saved version.
A resource directory that is a game install (PFFs present) plays with saved
loose files overlaying it exactly as the original tools did; a loose authoring
directory with no packed archives plays as the loose file set directly (the
managed launch passes `--loose-root` — ADR 0025's tracked divergence from the
game's packed-only boot).

Run warnings name every workspace with unsaved changes. Save whichever loose
asset you want the standalone process to consume, then relaunch; the editor
never mutates the resource directory on your behalf.

What to look at, per workspace:

| Workspace | After launching |
|---|---|
| Mission | start your mission from the game's mission list |
| Terrain | load a mission on your terrain and walk the surface, tiles, and foliage |
| Object | place the object in a mission first, then find it in the world |
| Avatars | the player screen lists your characters |
| Fonts | any menu or HUD text using your font draws with it |
| Credits | the credits screen plays your roll |
| Strings | menu and HUD copy pulls from your table |
| Menu | the menu set boots your screens |
| HUD | spawn into any mission; the HUD reads your layout |
| Music | menu music plays on boot, mission music in game |
| Particles | trigger the effect in game (the weapon, explosion, or scripted event that spawns it) |
| Sound | trigger the sound in game (the weapon or action that uses it) |
| Environment | time of day, sky, and fog follow your settings in missions that use them |

## How it is built (code-first)

ONED has no `.tres` workspace resources, and the shell never switches on workspace
type. The shell reads capability hooks off each workspace, and workspaces and
inspectors are declared as typed registry rows:

- [`framework/editor_workspace.gd`](framework/editor_workspace.gd): the
  `EditorWorkspace` base class. It documents the full contract (identity,
  lifecycle, inspector, viewport, document actions, edit, asset dock) and gives
  every hook a safe default, so a workspace overrides only the tiers it needs.
  A workspace returns its domain document/controller from
  `get_editor_document()` and the base derives undo/redo and dirty state from
  it; the base also owns the shared resource-root and VFS-open helpers.
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
   the contract in `framework/editor_workspace.gd`; `fonts/fonts_workspace.gd`
   is a minimal single-pane example).
2. Add a `Workspace` enum entry in `editor/editor_workstation.gd`.
3. Append one `WorkspaceDef.make(Workspace.X, XWorkspaceAdapter, is_popup, &"Category", &"icon-id")`
   row to `_workspace_defs()`.

### Add a workflow inspector
1. Write an inspector script: extend `WorkflowInspector` / `ListDetailInspector`
   (object style) or `TerrainInspector` (terrain style).
2. Append one `InspectorDef.make(id, "Label", "Tooltip", Script)` row to the
   workspace's `_build_inspector_defs()`.

Single-pane workspaces (Fonts, Credits, Strings, Sound, Menu, HUD,
Environment) skip the workflow rail and override `build_inspector(host)` instead.

## Folder map

| Path | Contents |
|---|---|
| `editor/` | the app root (`editor_app.gd` — boot wiring, window sizing, MCP service), the shell (`editor_workstation.gd`), the resource browser and library, the PFF archive tool, the export dialog |
| `framework/` | base classes and typed registries (`EditorWorkspace`, `WorkspaceDef`, `InspectorDef`) |
| `terrain/`, `object/`, `avatar/`, `mission/`, `fonts/`, `credits/`, `strings/`, `mnu/`, `hud/`, `music/`, `particle/`, `sound/`, `environment/` | one workspace module each (adapter, editor model, UI, inspectors) |
| `mcp/` | the embedded agent server's ONED side: built-in tool catalog, asset describe serializers, and the service that boots `godot/engine/mcp/` |
| `tools/` | `screenshot_capture` automation helper (not a workspace) |

## Agent server (MCP)

ONED embeds an [MCP](https://modelcontextprotocol.io) server so AI agents can
inspect and drive the editor through a **fixed, curated tool catalog** that
routes every operation through the editor's own code paths: list/describe game
assets (through loose dirs and PFF archives), `analyze_mission` for studying a
mission's composition, grounded mission authoring (`place_entities`,
`edit_mission_entity`, `edit_waypoint_path` — the tools sample the terrain and
bake ground anchors exactly like click-placement, so agents can never float or
sink objects by guessing heights), `set_mission_header` (environment changes
re-apply the preview, mission fog/water overrides included), `reground_mission`
repair, camera + screenshot, undo/redo, and `save_mission` (only ever on
explicit request).
Menu authoring is first-class too: `get_menu` / `analyze_menu` to study,
`add_menu_widgets` / `edit_menu_widget` / `set_widget_actions` /
`edit_widget_items` to build (snapshot-undo batches, validated Action wiring),
`menu_screenshot` for the WYSIWYG board, and `preview_menu` — the Interactive
preview where pressing a widget walks the real navigation, sandboxed. The menu
tools encode the conventions the original NovaLogic engine requires — new
screens get a game-shaped root, `set_widget_actions` auto-fills the `file=` the
engine demands on screen jumps, widgets carry their state appearance rows — and
`analyze_menu` reports a `game_safety` audit (the crash-class and render-class
rules learned by debugging an authored menu against the real game), so a file
that previews cleanly also runs in the shipped engine. There is **no script or
code execution** on this surface.

The stable ONED endpoint also owns the standalone run lifecycle:
`run_game` mirrors F5/F6/F8, while `game_state`, `game_control`, `game_debug`,
`game_screenshot`, and `game_logs` proxy into an ephemeral loopback MCP server
inside that exact child process. F3 and `game_debug` consume the same UI-free
debug-session catalog; there is no editor simulation.

It speaks MCP Streamable HTTP on `http://127.0.0.1:8975/mcp` and starts with
the editor by default (never in headless runs). The repo's `.mcp.json` points
Claude Code at it; other clients connect with their HTTP transport, and
stdio-only clients can bridge via `npx mcp-remote`. Controls:

- Settings popup (gear icon) → *Agent server (MCP)*: enable/disable + port,
  with a live status line. The choice persists.
- Launch flags: `--mcp-port N` (use a different port this launch),
  `--mcp-off` (don't start it).
- `user://oned_mcp/server.json` records the live URL while running.

Security note: the catalog is fixed and code-execution-free, but any local
process that can reach the loopback port can still drive the editor's
documents (non-local `Origin`/`Host` headers are rejected). Disable it in
Settings on shared machines. The Environment workspace's time-of-day remains
an authoring control; while Mission is active, its preview uses the BMS
`start_time` without mutating the saveable `.env` document.

Code: transport/protocol core in `godot/engine/mcp/` (shell-agnostic), ONED
tool catalog + service in `mcp/` (`editor_mcp_tools.gd` editor-wide,
`editor_mcp_mission_tools.gd` mission authoring).

The editor binds to the shared C++ core through the GDExtension in
`godot/engine/`; on-disk formats are parsed by the libraries under
[`libs/`](../../libs). How the editor and the runtime share rendering and
simulation code (the `edit_mode` runtime-node pattern, sampler seams, the one
mission runtime) is documented in
[docs/oned/editor-runtime-parity.md](../../docs/oned/editor-runtime-parity.md).
See the top-level [README](../../README.md) for the project overview and build
steps.
