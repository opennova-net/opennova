# ADR 0048: Authoring inside the Godot editor

- **Status**: proposed (2026-09-26; the alternative to the standalone editor on PR #665)
- **Owners**: the authoring layer (`godot/src/authoring`), the portable authoring core
  (`engine/editor`), the content tree mount (`engine/base/vfs`, `godot/src/resource_index`)
- **Supersedes/updates**: supersedes [ADR 0032](0032-direct-document-io.md) decision 1 (as
  restated by [ADR 0033](0033-engine-owned-loops-device-shells.md) decision 3 and carried by
  [ADR 0043](0043-canonical-cpp-and-godot-hard-cut.md)) for the authoring layer only: the runtime
  still never hands a NovaLogic file to `ResourceLoader`. Amends [ADR 0045](0045-cli-game-data-runtime-only.md)
  (a `res://` or `user://` resource directory, and exports that carry their content),
  [ADR 0025](0025-standalone-game-is-the-only-live-mission-runtime.md) (F5 in the Godot editor runs the real game on
  the project's content), [ADR 0028](0028-engine-directory-and-shell-adapter.md) and
  [ADR 0029](0029-engine-group-targets.md) (a fifth engine group), and [ADR 0015](0015-two-products-serve-mode.md)
  (an export may carry content). PR #665's ADR 0046 (never merged) is the competing design; only
  one of the two lands.

## Context

The data editor is coming back. PR #665 builds it as a second exported Godot product: Dear
ImGui panels in portable C++, its own project file, sidecars, undo history, PFF build and a
spawned Play child. That is a second editor application beside the one every Godot user
already has.

The maintainer asked for the opposite approach: make the Godot editor itself the editor.
NovaLogic files become Godot resources, so the FileSystem dock, the Inspector, the 2D editor,
undo and redo, Save, the Play button and Godot's own export work on them directly.

ADR 0032 banned exactly that integration in 2026-08: the 24 loaders and savers of the time had
no consumer (no `.tscn` or `.tres` referenced a NovaLogic file, `res://` held no game data,
nothing called `get_dependencies`), so they were a second, dead loading idiom. This record
gives the layer real consumers (the editor, F5 and export), keeps it out of the runtime, and
keeps the runtime's own loading path as the only one the game uses.

## Decision

1. **The Godot editor is the editor.** Authoring runs in the Godot editor on the repository's
   `godot/` project. There is no separate editor product, no project file and no mod layer:
   the Godot project is the unit. Game content lives under one content root inside it
   (project setting `opennova/content/root`, default `res://data`). The folder is git-ignored;
   retail data is never committed.
2. **NovaLogic formats are Godot resources in the editor, and only there.** C++
   `ResourceFormatLoader`/`ResourceFormatSaver` pairs (and import plugins where Godot's own
   importers already claim an extension) live in `godot/src/authoring`. They are registered at
   the EDITOR initialization level and compiled only when `OPENNOVA_AUTHORING` is on (every
   flavour but `template_release`), so no export template and no release library contains
   them. Loaders recognize only paths under the content root, report no UID (so Godot writes
   no `.uid` sidecars), and wrap the engine's own parsers and records; savers write from
   scratch through the engine's writers ([ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)).
   Each format has one property table (`def_schema`, `mnu_schema`, ...) that drives both its
   writer and the properties the Inspector edits. Editable resources store one `source` byte
   property whose getter is the engine writer and whose setter is the engine parser, so reload,
   duplicate and revert all go through the engine. The runtime never loads a NovaLogic file
   through `ResourceLoader`; `ResourceRoot` and the Vfs stay its only path (ADR 0043).
3. **The runtime reads the content root through a tree mount (D-VFS-12).**
   `engine/base/vfs/vfs_tree.h` declares `VfsTree`, a file tree the embedder provides;
   `Vfs::mount_tree` mounts it as the only layer. Files are found by flat, case-insensitive
   name at any depth; the first path in sorted order wins and every duplicate is reported;
   lookups behave like a packed install (no loose preference, no loose mark in the mission
   list, SCR and BFC1 payloads decoded, no expansions, no boot-archive count). The Godot side
   (`ContentTree`) walks and reads with `DirAccess`/`FileAccess` and never globalizes a path,
   so the same code reads the project folder in the editor and the `.pck` in an export.
   `--resource-dir` accepts `res://` and `user://` paths, which select the tree mount; an
   operating-system path keeps today's retail mount byte for byte. On a tree mount
   `weapon.sav` lives under `user://`.
4. **F5 plays the content.** The plugin's `_run_scene` adds `-- --resource-dir <content root>`
   when the project's main scene runs (never for addons or tests). `_build` mounts the content
   root exactly as the game will and cancels the run when a fatal boot file is missing, a name
   is duplicated or a finding blocks. A machine-local "play with game install" path lives in
   the editor's untracked project metadata.
5. **Godot's export packs the content.** An export plugin re-adds every content-root file raw
   at its `res://` path, so no import artifact, remap or scene customization ever replaces a
   game file. The per-preset option `opennova/pack_content` adds the `opennova_content`
   feature, which boots the packed content with no arguments. The "OpenNova Runtime" preset
   excludes `data/*` and keeps ADR 0045's contract (no arguments: usage, exit 2).
6. **A fifth engine group, `engine/editor`.** The portable authoring core (requirements
   evaluation, blank factories, classification, validators, the reference graph, the menu
   layout planner) is the STATIC target `opennova_editor`, above `opennova_runtime`. Nothing
   in base, formats, runtime or net includes or links it; only `godot/src/authoring`, tests
   and apps include `<editor/...>`, and nothing outside `godot/src/authoring` includes it.
7. **Format coverage.** String tables (`.bin` RTXT) and stylesheets (`.mns`) are editable
   resources. The item, weapon and ammo catalogs are a `DefCatalog` resource with record views
   and a catalog panel. Menus (`.mnu`) are scenes: a `MenuFile` root lays its screens out side
   by side, each `MenuScreen` draws the retail look through the runtime's `MenuFrame`, and each
   window is a `MenuWindow` Control that the stock 2D editor moves, resizes, selects, copies
   and undoes; the saver rebuilds the `.mnu` through the retail-parity writer. `.pcx` loads as
   a texture. `.3di` is view and place only: a read-only scene drawn by the runtime's model
   renderer; authoring stays in Blender ([ADR 0047](0047-blender-3di-exporter.md)). Content files
   whose extension a stock Godot importer claims (`.tga`, `.dds`, `.wav`, `.fnt`, ...) get
   `keep` import sidecars so they stay raw.
8. **Testing.** GUT covers loaders, savers and scene conversion (EDITOR-level classes exist
   in every editor-binary process: the editor, GUT and the F5 game). An editor smoke harness
   (`--opennova-editor-smoke=<case>`) drives editor behaviour headless. The game MCP checks
   what an F5-equivalent run shows.
9. **Shared with #665.** #665's editor-independent work (menu, def and stylesheet parity, the
   def writers, blank factories, requirements) is ported and shared. Its application shell,
   session, undo core, project file, sidecars, PFF build and Play child are not.

## Consequences

- Modders and maintainers author in the stock Godot 4.6.1 editor; there is no second editor
  to install or maintain.
- The runtime gains one mount kind (D-VFS-12, ratified here) and nothing else; retail
  installs load exactly as before.
- `CLAUDE.md` and `godot/src/CLAUDE.md` state the resource-system rule as: the runtime never
  loads NovaLogic files through Godot's resource system; the authoring layer may, under
  decision 2.
- CI is unchanged: the PR build is `template_debug`, which carries the authoring layer, and
  GUT runs against it. Release libraries build without it.

## Verification

Slice 0 ran throwaway spikes against the stock Godot 4.6.1 editor (headless, driven by a
C++ `EditorPlugin` registered with `EditorPlugins::add_by_type` at the EDITOR level and a
`@tool` case script) before any of this record's code was written:

- **A loader-provided `PackedScene` is a real scene.** A toy format whose loader returns a
  `PackedScene` rooted in a custom `Control` reports its type as `PackedScene`, opens through
  `open_scene_from_path` like a double-click, and `save_scene` routes it to the matching
  `ResourceFormatSaver`. A loader that overrides `_get_resource_uid` to return -1 gets no
  `.uid` sidecar.
- **The 2D editor's own undo round-trips.** The action the canvas editor records for a drag
  (`_edit_get_state`/`_edit_set_state` on the Control) fires `item_rect_changed` once, which
  the node maps back to its authored rectangle; undoing every step and saving again writes
  the original bytes.
- **Dynamic properties behave.** Flattened arrays (a count property with
  `PROPERTY_USAGE_ARRAY` and `name_N/field` elements) and `PROPERTY_USAGE_CHECKABLE` presence
  (NIL clears, a set restores the kept value) work through `EditorUndoRedoManager` actions.
- **EDITOR-level classes are everywhere the editor binary runs.** Classes and loaders
  registered at the EDITOR level exist in a `-s` script run (GUT) and in the game process the
  editor launches; export templates never initialize that level.
- **Saving edited resources.** An undo-recorded edit marks a resource edited; saving a scene
  also saves edited external resources, but "Save All" with no scene open does not. The plugin
  therefore implements `_save_external_data` and its panels offer Save. An external change to
  a resource with unsaved edits does not overwrite them.
- **Keep sidecars win the first scan.** Plugins enter the tree while the first scan is
  already running, yet `keep` sidecars written in `_enter_tree` for 140 retail `.tga`, `.fnt`,
  `.wav` and `.png` files were honored: nothing was imported and the files stayed untyped.
  `.dds` is claimed by Godot's own DDS loader and needs no sidecar. Retail extensions are
  often upper case, so every extension test is case-insensitive.
- **F5 runs through the plugin.** Playing a scene calls `_build` and then `_run_scene`; the
  arguments `_run_scene` returns reach the launched game as user arguments. The editor saves
  open scenes (content scenes included, through their savers) before it runs.

Left for the slices that need them: the Save As filter, a FileSystem-dock rename reaching
`_rename_dependencies` (slice 8), `.3di` thumbnails (slice 9), and a real export
(slice 10).
