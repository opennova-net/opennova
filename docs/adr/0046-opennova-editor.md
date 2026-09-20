# ADR 0046: the OpenNova Editor, a project-based data editor

- **Status**: accepted (2026-09-20; maintainer directive, the plan reconciled by two
  architects)
- **Owners**: the editor (`engine/editor`, `godot/src/authoring`, `godot/editor`,
  `apps/project`), the Godot layer, packaging
- **Supersedes/updates**: supersedes the editor and packaging provisions of
  [ADR 0045](0045-cli-game-data-runtime-only.md) (ONED, its packer and "no integrated
  run/packaging utility") and [ADR 0037](0037-oned-runs-game-data.md) d6/d8 (no project
  file, import system, dependency graph or editor-owned database); updates
  [ADR 0039](0039-in-engine-dev-tools.md) d7 and its 2026-09-19 sole-consumer
  amendment (the editor composes its own window set on the ImGui pass),
  [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md) d6 (the ImGui include
  containment gains the editor UI trees), [ADR 0028](0028-engine-directory-and-shell-adapter.md)
  d1 and [ADR 0029](0029-engine-group-targets.md) (a fifth engine group and STATIC target),
  and [ADR 0015](0015-two-products-serve-mode.md) (two Godot products again, plus a Play
  export of the game). ADR 0045's CLI game-data rule for the runtime (`--resource-dir`
  required, no picker, no executable-adjacent discovery) stands unchanged.

## Context

ONED grew to thirteen workspaces without one coherent game-project workflow, was cut to
a run-only surface (ADR 0037) and removed with the bundled asset tree and the packer
(ADR 0045). The runtime is CLI-only. The maintainer wants the data editor back, slowly
and cleanly, as a separately exported Godot application with a real project model: loose
source files in a project directory, Godot-style import, PFF archives produced only for
Play or Export, the names the engine demands surfaced as a requirements checklist with
guided create-if-missing, a cross-asset reference graph, and registry-driven
extensibility. Modders must not need a Godot install. The retired editor is history to
mine for small utilities, not an architecture to copy.

Facts the decision rests on: `engine/base/gameprofile/required_resources` is the
witnessed manifest of every file the engine demands by name (80 rows with phase,
severity and failure text; `docs/required-resources.md`); boot archives are a fixed
table (`kBootArchiveTable`); logical names resolve flat and case-insensitively, PFF
names are 16 bytes; native writers exist for every format the first slices need except
`.def`; there is no reference index; release GDExtensions compile without Dear ImGui.

## Decision

1. **Product.** "OpenNova Editor", `opennova-editor.exe`, exported from the one `godot/`
   project through a second preset (custom feature `opennova_editor`; Godot reserves
   `editor`), its own main scene (`run/main_scene.opennova_editor`) and its own user
   directory (`use_custom_user_dir` + `custom_user_dir_name` overrides), so it never
   shares `user://` with the game. A third preset, "OpenNova Play Runtime", is the game
   with `game/mcp/*` included; it is the child the editor's Play launches. The ordinary
   runtime download is unchanged and excludes every editor file.
2. **Two GDExtension variants from one source tree.** Runtime-only (today's DLL) and
   editor-enabled (CMake `OPENNOVA_EDITOR`: adds `godot/src/authoring/`, the editor UI and
   links `opennova_editor`), with distinct filenames. Exported products select their
   variant through feature-keyed `.gdextension` library rows (`opennova_runtime`,
   `opennova_editor`; Godot picks the fully matching row with the most tags). The plain
   debug row, which source runs, the Godot editor and export scanning load, points at the
   editor-enabled debug DLL, the superset; nothing ships from a source run. The game and
   the Play child always ship the runtime variant: "nothing the game ships depends on the
   editor" is a link dependency, not a convention. A boot-time check logs the loaded
   variant.
3. **A fifth engine group.** `engine/editor/` builds the STATIC target `opennova_editor`,
   PUBLIC-linking `opennova_runtime`, so validators reuse the engine's own load paths
   (the WAC compiler, the item and weapon loaders, `MenuFrameCompiler`). Nothing under
   base/formats/runtime/net may include or link it; `include_graph_check.py` gains the
   group and `link_graph_check.py` the transitive prohibitions. Its libraries: `project`,
   `assets`, `requirements`, `blank`, `project_build` (no directory under `engine/` may
   start with `build`, which the lints skip), `run`, `model`, `documents`, `graph`,
   `import`, `ui`. Bindings live in `godot/src/authoring/` (the group-name lint forbids
   `godot/src/editor/`). These files are infrastructure, listed in the citation
   allowlist like `io` and `vfs`; nothing in them is a port.
4. **CLI parity.** `apps/project/` builds `opennova-project` (`new`, `status`,
   `validate`, `create-missing`, `build`, later `import`) over the same core: headless
   use for modders and CI, end-to-end ctests, and a real non-test consumer for every new
   header.
5. **The engine's names stay fixed.** The engine keeps its witnessed literals. The
   editor's requirements are the manifest rows (each gains a stable role token) plus
   project feature toggles; the build emits the canonical archive names and placement,
   so output boots on OpenNova unchanged and stays retail-bootable. A runtime-read
   deployment contract that would make names configurable (retail defaults, discovered
   from the build directory, explicit override, hard failure semantics, role-group
   migration) was designed and deferred by the maintainer; the role model, the
   requirements panel and the build keep that door open.
6. **Project model.** One source tree, Godot-style: `project.opennova` (versioned UTF-8
   JSON: schema version, project id, title, target game, feature toggles, export
   settings), native NovaLogic files anywhere in the tree (edited in place through
   native writers), importable sources each with a committed `<file>.import` sidecar,
   and a disposable self-ignored `.opennova/` (`local.json` for machine paths, imported
   outputs by content hash, index caches, `build/play/<build-id>/`). Flat-name uniqueness
   (case-insensitive) and the 16-byte PFF limit apply to runtime output names; discovery
   skips project metadata, sidecars, `.opennova/` and export directories; moving a source
   moves its sidecar; renaming a source never renames its outputs implicitly. Unknown
   schema versions are rejected and there are no migration readers pre-1.0. JSON is
   parsed by a strict portable reader/writer in `engine/base/io`, never Godot-side.
7. **Requirements.** A new project is metadata plus an unresolved checklist, not a hidden
   game. Rows come from the manifest (BOOT and MENU always, MISSION when the feature is
   on; fatal/dialog/required/soft rows are Required, optional rows fold away, archive-table
   rows are build outputs, pattern rows are hidden) and offer Create (a from-scratch
   factory through the native serializer), Assign (rename an existing file of the right
   kind) and later Import. Factories and typed validation cover every Required row of the
   default project; validation parses files and checks references. The startup menu a
   factory creates has a screen `Startup` and a working Exit; the bitmap font is drawn in
   code; no retail bytes are embedded anywhere.
8. **One build for Play and Export.** Refuse unsaved documents, import changed sources,
   validate, take every registered asset, route by kind into the canonical archives plus
   the mandatory loose files (`.sbf` streams by path, `earlyerr.txt` is read before any
   mount, `.bik`), write through the streamed PFF writer, reopen and verify through the
   VFS, publish an immutable build directory. Incremental by per-archive input hash; the
   last good build survives a failure; a directory a child runs from is never touched.
   Play always packs and launches `opennova.exe -- --resource-dir <build> --mcp-port <n>`
   through a `PlaySession` (one child, stop deadline, injected platform seam, log tail
   until the game MCP answers).
9. **Editing core and the reuse rule.** `EditableDocument`, a typed `FieldSchema`,
   `EditHistory` and `Diagnostic` are toolkit-neutral portable C++; per-format documents
   wrap the engine's OWN records and parsers (`def::DefItemDef` and siblings through
   `def_parse_*`, `mnu::Document`, `mns::Document`, the rtxt structs, `bms::File` with
   `bms_edit`) and validate through the engine's own loaders. The editor adds only what
   does not exist: writers, one property table per format where the knowledge lives in a
   witnessed parser branch chain (the table drives the writer and the schema and is
   pinned by a parser-equivalence test, never a second parser), commands and UI. A
   validator, loader, resolver or renderer that exists in the engine or the Godot layer
   is called, not copied; a missing one is added to the engine as a shared facility.
   Writers are built from scratch (ADR 0003); a raw retained line is never replayed.
10. **Asset graph and imports.** Typed edges (source asset, field, target name or symbol,
    resolution policy, required/optional/dynamic) from per-format extractors give find
    references, missing-reference diagnostics and transactional rename; pack closure
    pruning only where extraction is proven complete. Importers record importer id,
    version, options and outputs in the sidecar and cache outputs by content hash; image
    decoding is portable so the CLI and the editor share it. Retail data is either an
    explicit read-only dependency mount (project assets win, machine path only in
    `local.json`) or a source for importing selected effective files; referenced retail
    content is never redistributed.
11. **The UI is Dear ImGui in portable C++.** Editor panels are `engine/editor/ui`
    windows on the existing `ImGuiPass` (records in, typed requests out; ctest with the
    null backend), docked around Godot-native preview viewports, hosted by a C++
    `EditorApp` node that derives from a restored shared `ImGuiPassNode` (the game's
    `DevTools` derives from the same base). ImGui compiles when `OPENNOVA_DEVTOOLS` or
    `OPENNOVA_EDITOR` is on; the game's inspection windows keep their own gate; the
    ImGui include containment gains only `engine/editor/ui` and `tests/editor_ui`;
    `godot/src/authoring` reaches ImGui through the `imgui_abi.h` pointer seam like
    `godot/src/devtools`. The editor preset ships the imgui-godot addon in release. Native
    dialogs and OS file drops arrive as typed shell requests; a Godot `CodeEdit` window is
    the allowed device-side exception for script text.
12. **Order of work, one PR per slice.** Project core + JSON + CLI; blank factories and
    create-missing; build + `PlaySession` + the Play preset; the editor shell, the two DLL
    variants and packaging; the item / weapon / ammo catalog as the first editor feature
    (the structured `.def` writer and property tables land first, after promoting the
    weapon `ammoclass_max_carry` raw lines into typed parser fields); strings and menu
    editors with the shared `MenuFrame` preview; the asset graph; imports. Later: further
    def inspectors, missions, GLB/GLTF to 3DI under ADR 0038, fonts, audio, the editor
    MCP, expansion-type projects, the deferred deployment contract, plugins.

## Consequences

- Two Godot products again, three export presets and two GDExtension variants; the
  packaging script and CI build both variants, export all three presets and boot-smoke
  each; the editor zip carries an `editor/` and a `runtime/` product directory.
- The engine gains a group whose code is tooling, not a port: the citation allowlist
  grows by design and the ratchets stay non-increasing.
- `required_resources` rows carry a role token; the manifest stays the witness table
  and gains no editor dependency.
- Historical ONED records stay historical. ADR 0037 d6/d8's prohibitions no longer
  describe the product.

## Verification

- The `engine/editor` ctests (project document, registry and name rules, requirements,
  blank factories, build, play session, documents, UI windows on the null backend) and
  `opennova-project` end to end on a temporary project.
- GUT: the built minimal project boots the runtime to its menu; the editor boots from
  its main scene headless; the def catalog and the menu editor round trips reach the
  running game through the game MCP.
- Packaging: three presets exported and boot-smoked in debug and release, the loaded
  GDExtension variant asserted per product, the runtime zip byte-for-byte unaffected by
  editor files.
- Lints: `include_graph_check` (fifth group, ImGui trees), `link_graph_check` (both
  GDExtension graphs), `orphan_header_check`, `ratchet_counts`, `cite_census`,
  `env_lint`, `conventions_lint` all green on every slice.
