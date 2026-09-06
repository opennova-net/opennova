# OpenNova worlds in Godot

Enable **OpenNova World** in Project Settings > Plugins (enabled in this project).
Use **Project > Tools > OpenNova: Open example world** for the game's own
tracked source tree (`assets/mnml.bms`, the minimal playable set), or
**OpenNova: Open retail world** for Operation: Emerald Scorpion (`CP01.bms`).

For the retail example, copy `resource.pff`, `localres.pff`, and `language.pff`
from your stock Joint Operations install into `local-data/jo/` at the repository
root. This folder is ignored by Git and excluded from exports. The scene is
`res://examples/retail_world.tscn`; its WorldSource selects `CP01.bms`, which names
`dvxg1.trn` and `full_01.env`. Its native scene nodes load terrain, environment,
water, vegetation, and placed models from that data.

## Edit, save, and play

1. Open the world. Use ordinary Godot 3D navigation; **Frame** returns to the
   authored `PreviewView` Marker3D or the mission's starting area.
2. Choose **Create Copy** and enter a unique name of up to 12 ASCII characters.
   The plugin writes separately named BMS, TRN, and base ENV files alongside
   the game data, updates the mission's references, and copies its available
   TIL, WAC, BIN, and PCX sidecars. Supporting assets stay in their archives.
   The scene selects the new WorldSource. Undoing this selection leaves the
   created native files available on disk.
3. Select **GameWorld** to edit mission start time as `HH:MM`. Select
   **MissionEnvironment** to edit base sky map filenames and sky height. Select
   **Terrain > FoliageDispatcher** to edit each existing slot's graphic, map
   match, and shadow flag. The Inspector names the owning native file and
   distinguishes base environment values from mission overrides.
4. Use Godot undo/redo normally. Edits update the preview without replacing its
   placed objects. The toolbar reports pending native changes; changing scene
   tabs retains them. **Reload** offers Save, Discard, or Cancel when needed.
5. Use **Save**, Godot Save, or Save All. Changed native documents are staged
   before originals are replaced, with rollback on a replacement failure.
   Changes made by another editor are detected before writing. Closing a scene
   or quitting Godot includes pending native files in the save prompt.
6. **Play World** saves pending edits and starts the selected mission in the
   ordinary game with the same source policy as the preview. Save failures or
   disk conflicts prevent launch. **Stop** and plugin shutdown stop only the
   child this editor started. Ordinary Godot Play still starts the game shell.

Native BMS, TRN, and ENV files are authoritative. Saving the scene records its
WorldSource and authored node configuration, including node links, terrain
quality, and PreviewView. Parsed documents, generated models, resolved textures,
and preview rendering resources are excluded from scene storage.

Godot's external-save hook cannot veto an editor exit. If saving fails, the
plugin retains edits in memory and also writes native recovery documents under
`user://world-recovery/`. The error names that folder; `RECOVERY.txt` identifies
the original game data and the steps to restore those files. A failed recovery
write is reported explicitly. These are recovery copies, not imported assets or
another authoring format.

## WorldSource and lookup

Create an inherited scene from `res://game/world/game_world.tscn` and assign a
**WorldSource** to select your own world:

- `mission_name`: a top-level BMS filename.
- `source_kind`: **Loose source** reads loose files and ignores PFF archives.
  **Retail install** previews the archive-selected world without editing it.
  **Editable game data** requires a loose BMS and resolves dependencies through
  the runtime's loose-override policy over the archives. Create Copy selects
  this mode for a retail copy; a loose copy stays in Loose source mode.
- `data_directory`: a project-relative directory, such as `res://../assets`.
  Plain relative paths are relative to the Godot project.
- `local_install_name`: optional. Name a data folder that lives somewhere
  different on every computer (a stock JO install, say `jo`), then press
  **Folder** to choose that folder on this computer. The choice is kept in the
  editor's project metadata, never in the scene, so the scene stays shareable
  and machine paths never enter source control. While set it overrides
  `data_directory`.
- `game_code` and `expansion`: the mount's title/decode key and expansion;
  defaults are `jo` and no expansion.

The preview does not inherit game launch flags or saved installation settings.
Editing a WorldSource selection marks its preview stale until Reload. Pending
edits for the old selection remain part of the scene's native save state.
Loose or editable sources are writable only when all three native documents
exist as loose files. An editable BMS cannot silently fall back to an archive.

## Preview and scope

The active scene's first GameWorld and primary 3D viewport drive the preview.
It displays authored starting state: mission scripts, AI, networking, gameplay
sound, and effects do not run. **Details** lists missing dependencies and partial
preview diagnostics. Use a single 3D view.

The actual scene nodes expose their loaded configuration. Terrain, Water,
Celestial, EnvironmentCubeCapture, and FoliageDispatcher share the terrain;
SkyDome, Water, Weather, and Celestial use the scene's MissionEnvironment links.
Source-derived native properties remain read-only outside the explicit authoring
controls and are excluded from scene storage. Releasing a preview restores the
original node values, editor camera environment, compositor, and shader globals.

`WorldEditSession` owns pending native documents. `WorldSource` opens the selected
root and mission. `GameWorld.load_preview_documents` binds those documents to the
same rendering nodes as runtime; `update_preview_settings` applies time, sky,
and foliage changes. The effective environment is a separate view of the base
ENV plus mission overrides. Native codecs and format behavior remain in their
existing engine owners.

## The field table

`WorldField` (`world_field.gd`) is the one description of every editable
native value: its owning document (BMS, TRN or ENV), Inspector section and
caption, widget kind with range or placeholder, whether it repeats per foliage
slot, and the typed read and write of its value against the session's
documents. The session reads, validates, applies and snapshots through that
table; the Inspector plugin lays it out in table order under the node that
presents each document; the property widget builds itself from the spec; undo
actions take their names from it. Nothing else switches on a field id.

To add a field: append its id to `WorldField.Id`, add one `_row(...)` in
`WorldField._build` with its read and write functions, and, if the value needs
a new presentation, add a `Widget` kind to `world_property.gd` and its
validation to `WorldEditSession.validate_edit`. `world_edit_session_test.gd`
checks that the table covers every id once and names the right document.

The **OpenNova** dock in Godot's bottom panel supplies Run Game, Stage & Run
Retail, Stop, and Pack Game Data. All use this scene's selected source and save
pending native edits first. See [game data workflows](../../tools/README.md)
for installation, output and headless packaging details.

Next comes native mission placement editing through Godot selection and transforms.
More document fields, scene composition and asset tools follow in separate phases.
The current foliage controls edit existing slots; they do not paint foliage maps.

## Validation

The GUT tests `world_edit_session_test.gd`, `world_preview_test.gd`, and
`game_run_session_test.gd` cover native save/reopen, copy and lookup policy,
sidecars, undo across saves, failed writes and recovery, external conflicts,
base/effective environments, transient scene data, and selected-mission launch.

In a disposable graphical editor project, run
`res://tests/tools/world_authoring_editor_check.gd` from **File > Run** in the
Script editor. It drives the real Inspector controls, undo, save, close prompt,
scene switching, and Create Copy. It creates a temporary scene and synthetic
native data, then removes them after a successful run. Reports and a synthetic
Inspector screenshot go to `.scratch/` beside the Godot project.
`world_preview_editor_check.gd` retains the preview rendering and teardown checks.
