# ADR 0037: ONED runs game data; it does not edit it

- **Status**: accepted (2026-08-24; hard cut)
- **Owners**: ONED product, game packaging
- **Supersedes/updates**: updates ADR 0015's second-product role; supersedes
  ADR 0025's F6/current-mission and unsaved-editor-state decisions; retires the
  ONED authoring consequences of ADRs 0016, 0033, and 0034; updates ADR 0029
  by removing the `opennova_oned_edit` interface leaf and editor-only `refs`
  subsystem. Their engine, runtime, and layering decisions remain in force.

## Context

ONED grew into thirteen asset workspaces, a project-like resource browser,
preview runtimes, cross-workspace navigation, an embedded MCP server, and
several save/export paths. That surface duplicated format tools without
providing one coherent game-project workflow. Maintaining it pulled effort
away from the engine and gameplay while the canonical game data already lived
as ordinary files under a source directory, while shipped builds use a packed
PFF game directory.

The useful product loop is much smaller: select the game-data tree, run the
OpenNova runtime against those exact loose or packed files, prove the same
files against retail, and stop the child. Release packaging also needs a deterministic
headless pack entry, but that is build infrastructure rather than an
interactive ONED feature. The separately distributed OpenNova Launcher is a
different product and is outside this decision.

## Decision

1. ONED remains the second Godot product, `opennova-modtools.exe`, under
   the existing `modtools` export feature. The executable and preset names are
   packaging contracts, not a promise that ONED contains asset editors.
2. ONED's user interface contains settings plus three operations: **Run
   OpenNova**, **Stage & Run Retail**, and **Stop**. It owns at most one
   child process; starting another mode replaces that child.
3. Settings persist the game-data directory (loose sources or a packed game),
   recent directories, game code/expansion, and retail install directory. A
   packaged dev build defaults to the sibling `assets/` tree without
   persisting that implicit choice.
4. Run OpenNova starts the sibling runtime through its normal
   standalone-game path with `/d`, `--resource-dir`, and `--loose-root` plus
   the selected game/expansion. Loose roots use the explicit fallback; existing
   boot PFFs mount normally with loose override. It never saves, imports,
   copies, or transforms source data.
5. Stage & Run Retail stages the selected files under `user://packed`, copies
   existing PFFs or adds the zero-entry boot PFF for a loose root, copies the
   required runtime files from the user's retail install, and runs
   `Jointops.exe /w /d /FRISK` with the staged directory as its working
   directory. Source data is never modified.
6. ONED has no workspaces, document editors, project file, import database,
   dependency graph, preview runtime, or embedded MCP server. The removal is a
   hard cut: no legacy interfaces, state migration, compatibility wrappers, or
   dormant authoring code remain. The cross-asset `refs` graph and the
   Godot-side `ReferenceIndex`/`ResourceIndex` bindings existed only for those
   authoring surfaces and are removed with them; the runtime's native resource
   lookup remains.
7. The hidden command
   `opennova-modtools.exe --headless -- --pack-game <src_dir> <game_dir>`
   remains release infrastructure. It builds the packed `localres.pff` game
   layout plus loose-by-contract files. It is intentionally different from
   the loose `/d` retail-test layout and is not exposed as a fourth UI action.
8. Asset creation belongs to source-controlled format tools and external DCC
   applications. ONED does not adopt Godot's import system or introduce an
   editor-owned project database.

## Consequences

- ONED's interface is small while its implementation still hides runtime
  discovery, process ownership, loose-run arguments, retail staging, working
  directory requirements, stop/restart behavior, and package errors.
- The dev zip continues to ship both Godot product executables, their
  GDExtension DLL, and the tracked loose `assets/` tree. The tagged release
  continues to ship the game executable and the package-time-built game tree.
- `OpenNova Mod Tools`, `opennova-modtools.exe`, `windows-apps`,
  `windows-game`, and `--pack-game` retain their existing names to avoid a
  packaging-only rename cascade.
- Historical ONED workspace documents remain useful as implementation history
  but are no longer current plans or product documentation.
- Retail format and wire compatibility remain product requirements. Removing
  the authoring UI does not remove format readers, writers, runtime bindings,
  or their parity tests when other consumers still use them.

## Verification

- ONED tests pin the settings defaults and the Run, Retail, Stop, restart,
  natural-exit, and failure paths through the application interface.
- Packer tests separately pin the packed release layout and loose retail-test
  layout, including output safety, root-only selection, loose-by-contract
  files, PFF entry limits, retail runtime staging, and child working directory.
- Windows packaging exports and boot-smokes both Godot products, runs ONED's
  hidden `--pack-game` command, requires `localres.pff`, and
  validates both deliverable manifests.
