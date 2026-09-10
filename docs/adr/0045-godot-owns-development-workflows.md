# ADR 0045: Godot owns development workflows

- **Status**: accepted (2026-09-05)
- **Owners**: the Godot layer, game packaging, CI
- **Supersedes/updates**: updates [ADR 0015](0015-two-products-serve-mode.md)'s
  two Godot products (one exported application remains; the serve-mode rule
  stands), replaces [ADR 0037](0037-oned-runs-game-data.md)'s ONED run,
  staging and packaging contract, updates [ADR 0039](0039-in-engine-dev-tools.md)
  (the `OnedUi` ImGui surface is removed; the game's F3 dev tools stand), and
  completes [ADR 0044](0044-godot-authors-native-worlds.md)'s remaining
  migration. ADR 0025, ADR 0041 and ADR 0043 stand unchanged.

## Context

ADR 0037 kept ONED as a second Godot product with three operations, Run
OpenNova, Stage & Run Retail and Stop, plus the hidden `--pack-game` release
command. With ADR 0044 the Godot editor already selects a world, saves its
native documents and needs to launch the game against them, so the same three
operations had to exist inside the editor. Keeping ONED beside it would have
meant two products, two settings stores (`user://oned.cfg` and the editor's
own), a second ImGui product node, a `modtools` export feature and a second
executable in every zip, all to run the same `GameRunSession` and `GamePacker`
code the editor dock runs.

The alternative was the ADR 0037 shape: ONED stays as a thin runner and the
editor only authors. That keeps the packaging names stable but leaves the
workflow split across two applications with no shared world selection, the
shape whose hidden implementation ADR 0037's own consequences section already
noted. This record takes the other branch: one source Godot project owns
authoring, launch, staging and packing; one game executable is exported.

## Decision

1. The source Godot project owns world authoring, game launch, retail staging
   and packing. OpenNova is the only exported Godot application. ONED's
   executable, scene, settings, native UI classes, export feature and consumers
   are removed. Historical ONED decisions remain records of the previous
   architecture.
2. The OpenNova editor dock uses the active `GameWorld`'s `WorldSource`. It
   does not create a second workspace, asset database or content format.
   Machine-local installation and output paths live in Godot project metadata.
   Native documents remain authoritative, and pending edits and disk conflicts
   are resolved through the existing `WorldEditSession` save seam before run or
   pack.
3. `GameRunSession` and `GamePacker` live under `godot/tools`. The editor and a
   headless source-project command share these workflow modules. Native format
   and packing policy remain in their engine owners.
4. OpenNova launches always use the current Godot executable and source
   project; the session owns exactly one child and waits for its exit before
   replacing or restaging it. Retail staging uses a project-local cache. Retail
   lookup policy follows the selected source kind.
5. Packing accepts loose sources and rejects archive-backed input instead of
   silently dropping its dependencies. Output directories cannot overlap
   sources; unmarked nonempty directories are preserved. The existing retail
   staging and release archive contracts do not change.
6. The release script runs the source command directly and keeps both
   established ZIP names, each with only the game executable. The debug game
   keeps F3 tools; release packaging omits the ImGui addon.

## Consequences

- Removed: `godot/modtools/` (the app, scene and settings), `OnedUi` on both
  sides of the seam (`engine/runtime/devtools/oned_ui.*`,
  `godot/src/devtools/oned_ui.*`) with `tests/devtools/oned_ui_test`, the
  second export preset and its `modtools` feature, and the GUT files
  `oned_app_test.gd`, `recent_resource_dirs_test.gd` and
  `shipped_game_defaults_test.gd`. Recent directories and the bundled-assets
  default were ONED features with no dock equivalent.
- The dock (`godot/addons/opennova_world/game_data_panel.gd`) is a view over
  `godot/tools`: Run Game, Stage & Run Retail, Stop and Pack Game Data, each
  saving pending native edits first. Native save conflicts or invalid Inspector
  values block run and pack.
- Packing and staging run synchronously on the editor thread; a large retail
  copy pauses the editor. Retail staging wipes and recopies
  `godot/.godot/opennova/retail/` on every run rather than invalidating a cache.
- A loose-only source is staged with a zero-entry `resource.pff` boot token and
  its loose `.fx` files are not archived, so retail validation of the minimal
  viewmodel still needs the shader-bearing install from
  `minimal_pff_package_test --install`.
- Archive-backed world copies keep their retail dependencies and cannot be
  repacked as complete loose-source releases; the packer refuses them.
- Machine paths (`retail_runtime_directory`, `pack_output_directory`) are
  `EditorSettings` project metadata under `.godot/editor/`, outside source
  control and the export.
- Closing the editor or disabling the plugin stops the child it started
  (forced termination with a bounded wait; a graceful-quit window is a
  `TODO.md` item). The launched game loads the same `godot/bin` extension the
  editor runs, so there is no stale sibling runtime to discover.
- Dear ImGui and the engine's ImGui pass still build in every flavour; the
  template_release extension compiles the dev-tool windows out and the release
  export strips the addon, as ADR 0039 already required.
- The editor has no MCP server; the game carries the endpoint (ADR 0041). The
  historical ONED records under `docs/oned/` stay as implementation history.
- The real dock check (`godot/tests/tools/game_workflow_editor_check.gd`) is a
  manual `EditorScript`; the GUT suite covers the workflow engine, not the
  editor controls.

## Verification

- `godot/tests/game_packer_test.gd` pins the loose-source requirement, the
  boot-archive refusal, output overlap in both directions including relative
  output paths, marked versus foreign nonempty output directories, the packed
  release layout and the CLI seam; `godot/tests/game_run_session_test.gd` pins
  single-child ownership, replacement after exit, stop failure keeping the
  owned process, per-source-kind lookup flags for Play World, and that a stale
  sibling runtime is ignored; `godot/tests/export_presets_test.gd` pins the one
  export preset with `imgui/debug=true, imgui/release=false`.
- `godot/tests/tools/game_workflow_editor_check.gd` (manual, File > Run)
  drives the dock's real save, pack and run controls;
  `screenshots/editor/game-workflow/dock.png` is its capture.
- `scripts/package_godot_windows.ps1` exports the one preset, runs
  `res://tools/pack_game.gd` headless, fails on script or extension load
  errors in that run, and stages the ImGui addon only for a debug export.
- CI at the consolidation head: [PR checks](https://github.com/opennova-net/opennova/actions/runs/34004061770)
  and the [full Windows/Linux and release-packaging run](https://github.com/opennova-net/opennova/actions/runs/34004085024),
  the latter building `template_release` and packaging in release mode.
