# ADR 0044: Godot authors native worlds

- **Status**: accepted (2026-09-05)
- **Owners**: the Godot layer (the `opennova_world` editor plugin), world authoring
- **Supersedes/updates**: updates [ADR 0037](0037-oned-runs-game-data.md)'s
  authoring direction (decisions 6 and 8: no document editors, asset creation
  outside any OpenNova UI); its ONED run and packaging contracts were replaced by
  [ADR 0045](0045-godot-owns-development-workflows.md). ADR 0025 (the
  standalone game is the only live mission runtime), ADR 0038 (native runtime
  assets), ADR 0042 and ADR 0043 (Godot is the permanent shell; the world is
  C++ Nodes) stand unchanged and are what this record builds on.

## Context

ADR 0037 cut ONED to run-only after thirteen asset workspaces, a project-like
browser, preview runtimes and an embedded MCP server duplicated the format tools
without giving one coherent game-project workflow. ADR 0038 cut the asset
pipeline to native runtime assets, and ADR 0039 moved the tool UI into the
engine. Since then the project has been a runtime and an engine: the canonical
game data lives as ordinary files under `assets/`, and the minimal set makes a
fresh checkout run a playable mission from those tracked files.

What was missing was a way to look at a world and change it in place. ADR 0037
left that to "source-controlled format tools and external DCC applications",
which covers formats but not composition: choosing a mission's start time, its
environment, its foliage slots, and later its placed objects, against the
rendered result. ADR 0042 and ADR 0043 made the answer available for free: the
world is already C++ Godot Nodes driven by the engine, so the Godot editor can
show the same nodes the game renders.

Two shapes were on the table and are ruled out here rather than re-argued
later: a separate editor application (the retired ONED, `CONTEXT.md` lists it
under _Avoid_), and Godot scenes as the saved content model with an export step
back to native formats. The first re-creates the product ADR 0037 removed; the
second makes a second source of truth that the runtime, the retail-compatible
tools and the parity tests cannot read.

## Decision

1. Godot is the home for OpenNova world authoring. Native BMS, TRN, ENV, and
   mission sidecar files remain authoritative; Godot scenes describe composition
   and editor presentation. This keeps the authored result directly usable by
   the existing runtime and retail-compatible tools, while Godot supplies the
   Inspector, viewport, undo, save, and play workflow.
2. An editable world copy creates separately named native documents beside the
   selected game data. Existing archive-backed assets remain dependencies. Its
   explicit source mode uses the runtime's loose-override lookup policy;
   ordinary retail previews retain archive-only mission selection.
3. Generated scene nodes, resolved textures, and preview environments are
   transient projections of those documents and are never a second saved
   content model.
4. One world editing session owns the open native documents and their pending
   changes. Environment base values remain separate from the mission's effective
   overrides. Godot undo and save act on those documents, and Play starts the
   ordinary game against their saved files.
5. ADR 0045 completes the remaining launch and packaging migration and removes
   the separate modtools executable.

## Consequences

- The plugin lives in `godot/addons/opennova_world/`: `WorldSource` (a
  selection Resource on `GameWorld`: kind, data directory, install key,
  mission, game code, expansion), `WorldEditSession` (the open documents and
  their pending values), `WorldFileTransaction` (staged writes with backup and
  rollback), the Inspector plugin and the toolbar. `GameWorld.load_preview`
  binds the session's documents to the same rendering nodes the runtime uses.
- The preview shows the authored starting state only. Mission scripts, AI,
  networking, gameplay sound and effects do not run; the preview is not a
  running game and carries no dev tools.
- Source-derived node properties are read-only while a preview is active and
  are excluded from scene storage; releasing a preview restores the authored
  node values, the editor camera environment, the compositor and the shader
  globals. Editing a `WorldSource` marks its preview stale until Reload.
- An editable copy requires all three native documents (BMS, TRN, ENV) as loose
  files; an editable BMS never falls back to an archive. A copy of an
  archive-backed world keeps its retail archives as dependencies.
- Godot's external-save hook cannot veto an editor exit. When a save fails at
  exit the session keeps its edits in memory and writes recovery documents under
  `user://world-recovery/` with a `RECOVERY.txt`; these are recovery copies, not
  another authoring format.
- The editable fields in this slice are the mission start time, the base sky map
  filenames and sky height, and each existing foliage slot's graphic, map match
  and shadow flag. The foliage controls edit existing slots; they do not paint
  maps. Machine-local installation paths are Godot project metadata, never
  scene content.
- The retail example scene (`godot/examples/retail_world.tscn`) reads stock
  archives from the ignored `local-data/jo/` directory; the synthetic example
  (`examples/world_preview/`) is tracked and LFS-pulled by CI.
- The plugin is shipping GDScript and is counted by the maturity ratchets and
  the citation census beside `godot/game` and `godot/tools`; the vendored
  addons stay out.
- Placement authoring (the next slice: selection, transforms and undo over
  placed entities, with BMS save and reopen as the acceptance test) builds on
  the preview's entity projections: one `WorldEntityProxy` per mission record
  under the runtime root, `GameWorld.update_preview_entity` re-stamping a
  moved record's retained static instance or individual model in place, and
  `reload_preview_entities` re-placing the document for added or removed
  records (see `docs/runtime-architecture.md`, "Editor preview"). The editable
  fields are one typed `WorldField` table the session, the Inspector plugin
  and the property widget all read.

## Verification

- `godot/tests/world_edit_session_test.gd` drives the real session over a copy
  of `examples/world_preview/`: save and reopen preserving unexposed TRN fields,
  undo across a save, an archive copy with sidecars and shared assets, name
  collision and traversal rejection, refusal after an external change, writer
  failure rollback, settings updates keeping placed objects, base versus
  effective environment separation, and the recovery write.
- `godot/tests/world_preview_test.gd` pins the loose, retail and editable lookup
  policies, partial-preview diagnostics, the transient-property storage flags,
  a save and reopen of the scene, and that a game tick never activates a
  preview.
- The graphical editor checks `godot/tests/tools/world_preview_editor_check.gd`
  and `world_authoring_editor_check.gd` (run from the Script editor's File >
  Run in a disposable project) drive the real Inspector controls, undo, save,
  the close prompt, scene switching and Create Copy; their captures are
  `screenshots/editor/world-preview/preview-disabled.png`,
  `preview-enabled.png` and `screenshots/editor/world-authoring/inspector.png`.
- CI at the consolidation head: [PR checks](https://github.com/opennova-net/opennova/actions/runs/34004061770)
  and the [full Windows/Linux and release-packaging run](https://github.com/opennova-net/opennova/actions/runs/34004085024).
