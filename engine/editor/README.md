# engine/editor: the OpenNova Editor's portable core

The fifth engine group (ADR 0046 d3): the editor's portable core, the STATIC target
`opennova_editor`, which PUBLIC-links `opennova_runtime` so its validators reuse the engine's
own load paths. The rules that bind it (what may include or link it, the library ranks) are in
[`engine/CLAUDE.md`](../CLAUDE.md); the vocabulary is [`CONTEXT.md`](../../CONTEXT.md)'s
"Authoring" section. The Godot bindings and the Shell are `godot/src/authoring/`.

## The libraries

- `project`, `assets`, `requirements`: the project model, its files by kind and layout, the
  Required resources checklist.
- `blank`: the from-scratch factories of new files.
- `project_build`: the steppable build.
- `novaworld`: a project folder as a working copy of its game's project in NovaWorld
  (ADR 0069, ADR 0071), the project its project file names by key (`ProjectDocument::novaworld`).
  Its state under `.opennova/novaworld/` (each service's under its host: the base revision and
  its manifest; the hash cache and a pull's downloads every service's); its files hashed as the
  store keeps them, a few at a time (`novaworld_scan`, by the project walk's rule,
  `assets/project_walk`); its changes since its base and a pull planned path by path over the
  base, the folder and the revision, each conflict the author's to settle (`novaworld_plan`);
  and the plan applied, the files it replaces trashed first and the new base written last
  (`novaworld_apply`). The client: the account it signs in as (`novaworld_account`), the
  service's project routes (`/api/products/...`) as it speaks them (`novaworld_api`), and the
  check, the working copy's setup, the pull and the push as stepped jobs (`novaworld_jobs`), over
  the transport seam the host fills (`novaworld_transport`: the Shell's Godot HTTPClient, a
  test's fake).
  NovaWorld owns the lobby configuration and generates `gt.ssc`. Imports, project scans,
  build inputs and push manifests exclude that filename without case, including archived
  and nested copies. Historical manifests are verified before those files are omitted from
  Get/Pull and change comparison. Create/link/Get and normal synchronization cache the
  generated artifact under `.opennova/novaworld/`, isolated by service and project. Build/Play
  puts it only in build output, and its bytes participate in the build identity. Offline builds
  use the matching cache; a linked project without it requires a refresh. Unlinked projects
  keep the stock title default. Command-line builds and exports accept `--gate-tag-file`
  for the service-generated artifact, including automatic base and expansion exports.
- `run`: the Play session over the process seam.
- `model`: the neutral editing core (ADR 0046 d9); it names no format type.
  - `DocumentBase` (every document's lifecycle); `Document` (the record document over it, its
    row index keeping every record's path, `path_in`, and a type's `RecordShift` renumbering
    what names a collection's records by index when an edit moves them); `TextDocument` (a text
    whose spans its edits replace, its `TextHistory`).
  - `Node`, `Edit`, `EditHistory` (steps of row swaps under a byte budget), `StagedRows` (a
    batch's rows before it commits), `id_list` (the one list edit of a type's records and their
    identities), the `ChangeSet` a document answers since a state, `FieldSchema`.
  - The finding codes (`finding_code_row`): every finding is made from a row, the editor's own
    `CoreFinding` table's or its document type's `findings`, and keeps it (`Diagnostic::row`).
- `documents`: the document types over the engine's own records (every one a row of
  `document_types.cpp`), among them the def catalogs, string tables, menus, stylesheets,
  models (a `.3di`'s engine features over an immutable parsed base), missions, terrains,
  textures and sound banks, and the text types, a `TextDocument` each (a script through the
  WAC compiler, its operands as references with spans; a music script as its MUS text; credits
  as a CBIN file's ConfigFile text; a shader in its SCR form; a particle file, the HUD layout,
  a configuration or a text). `document_types` is
  the registry the session and the windows reach a type through: each type's `validate_file`
  (its file's own findings, from its document alone, kept per file by `validation_cache`) and
  its `project_check` (a check across the project's files that keeps state between
  validations, such as the menu type's render check; `project_checks`).
- `graph`: the asset graph: typed edges from the engine's own parsed records, a slot per file
  that an update patches; a file's record sets (its records a collection's references name by
  index); the one resolver behind the badges, the pickers and the Problems rows; the reference
  queries; a base layer's names; the rename transaction; the `use_checks` (one row per asset
  kind) and `style_value_use`; and `project_validation`, the one pass over the project's files
  (the graph's update, each file's own findings, the use checks, the graph's findings).
- `import`: the importers (a font set's glyph sheet to a `.fnt`, `font_import`; a PNG to the
  texture file its uses read, by the record's options; the terrain importer, `terrain_import`,
  baking a terrain set's images with TrnGen.exe's bake, `formats/cpt/trngen`; a wave to the
  form the game plays, `wave_source`), the `.import` sidecars and the import pass whose outputs
  the scan lists, and the one-shot converters (an `.o3d` to a `.3di`, an `.o3a` to its `.adm`
  and `.bad` files, a Black Hawk Down GP `.3di` migrated to the 3DI3 the game loads).
- `preview`: the viewports.
  - A viewport is one per (document, kind) from a compiled-in kind table (`viewport_kinds`),
    kept by the session (`viewports`, with the one preview clock), changed by a SetViewport
    request and the changes its follow derives (`viewport_follow`: from its document's change
    set, the files its picture read and its state, the action its device takes: Keep,
    Rebuild, Update or Clear; a gesture holds a Rebuild for a kind whose row says so).
  - Its envelope on the wire (`viewport_json`); the `viewport` query and the
    `edit_in_viewport` request, which serves a drag or a command the viewport plans.
  - The device seam (`viewport_device`): a device may build its picture over the Shell's
    frames, a unit a step within the budget, a newer build cancelling one in flight; a
    least-recently-used cache of four (`viewport_device_cache`; a kind's row may hold fewer).
    The Shell's devices are `godot/src/authoring`'s, over the runtime's `MenuFrame`,
    `ObjectModel` and, for a mission, the game's own environment, sky, water, terrain,
    placer, foliage, particle renderer and light director.
  - The kinds, twelve (`session/view/viewport_kind.h`: a menu, a model, a script, a mission
    and its map, a texture, an effect, a HUD, a definition, an environment, a terrain, a
    font), among them the menu's (`menu_viewport`: its screen compiled headless, what a drag
    of a window's handles and an arrange write); the model's (`model_viewport`: its orbit
    camera, the level the game draws, a clip on its rig); the mission's (`mission_viewport`,
    the Document tab's Main view: `mission_scene`, `mission_camera`, `mission_overlay`,
    `mission_handle_edit`, `mission_items`, `mission_effects`).
  - The render check, the menu type's project check: every menu screen compiled headless, its
    compiler notes as Problems rows, a menu rendered again only when it, a file it read or a
    variable it names changes (the `menu_tree`, `menu_findings` and `menu_render` queries).
  - The canvas's portable half: its one gesture machine, its overlay shapes, and each kind's
    half (`canvas_half`: what a press takes, the requests a drag raises, what is drawn).
- `session`: the one open project and everything the editor does to it, typed requests in and
  a view out.
  - `native_asset_catalog` is the export-only operation behind `opennova-project asset-catalog`.
    It reads a release's native export and its separately exported pinned base, follows the
    graph's dependencies through both layers, and writes a manifest plus content-addressed
    native bytes. Only the project's own models, animations, animation maps, terrains and
    textures are roots; unresolved required dependencies omit the root. It opens no editable
    project session and changes no source files.
  - `ProjectSession` is a facade over its parts, which call one another, never `handle()`:
    `SessionCore` (the open project, the operation slot, the view, the request's outcome),
    `DocumentSet` (the open documents, their edits, the clipboard), `ProblemsService` (the
    validation and the Problems rows), `PlayController`, `ImportController`,
    `RenameController`, `UnsavedGuard` (the unsaved-changes prompt) and `EditorPreferences`
    over the `PreferencesStore` its embedder owns.
  - The request table (`request_kinds`: one row per kind, its token, who serves it, its typed
    fields, `request_fields`, its policy; `request_factories.h` makes one of each) and the query
    table (`editor_queries`: one row per question, its params, the list it pages, the concern
    whose revision it answers with; `ProjectSession::query`).
  - The wire form (`session_json`: documents, records, a request with its edits in the batch
    form, `record_batch`, the findings; `view_json`: the view by section), the view the windows
    read (`session/view`: sub-views of the project, the documents, the findings, the activity
    and the dialogs, and the events a request posts for one window), the selection, and the
    finding codes' tables (`finding_codes`).
- `ui`: the Dear ImGui windows on the engine's pass, built only with `OPENNOVA_IMGUI`, the only
  tree besides `runtime/devtools` that may include an ImGui header. Each window reaches the
  session through the `Workspace` seam (`ui/workspace.h`); the Inspector is generic; a
  document's view in its Document tab is its type's row of `ui/document_views` (an outline
  over `ui/outline_model` as a tree, a list or master and detail, a view of its own, or a
  Main-role viewport beside the outline, `ui/main_viewport_view`); a viewport's view
  (`ui/viewport_view`, one per kind from `ui/viewport_views`) draws its canvas through the
  Workspace's devices.
