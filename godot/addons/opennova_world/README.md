# OpenNova world preview

Enable **OpenNova World** in Project Settings > Plugins (enabled in this project).
Choose **Project > Tools > OpenNova: Open example world** to open the example,
or open `res://examples/world_preview.tscn` from the FileSystem dock.

**Project > Tools > OpenNova: Open retail world** opens
`res://examples/retail_world.tscn`, configured for **Operation: Emerald Scorpion**
(`CP01.bms`) from stock Joint Operations. Copy `resource.pff`, `localres.pff`
and `language.pff` from your stock install to `local-data/jo/` at the repository
root. The scene's WorldSource uses Retail install and
`res://../local-data/jo`; its BMS selects `dvxg1.trn` and `full_01.env`.
The inherited GameWorld nodes resolve terrain, water, vegetation, sky and placed
models from those archives. `local-data/` is ignored by Git, sits outside the
Godot project, and is not included in exports. The synthetic example remains
available without retail data.

The retail scene includes a `PreviewView` Marker3D for an elevated editor view
over the riverside lumber mill.
Frame World uses that marker's position and rotation when present; otherwise
it frames the mission's starting area. Move or rotate the marker in the scene
to choose a different editor viewpoint.

The 3D editor shows native terrain, environment and placed models before Play.
Use ordinary Godot viewport navigation. Frame World returns to the starting area. World Source opens the saved selection
in the Inspector; Reload rereads files and clears source caches. Details lists
unavailable dependencies and explains partial previews.

To preview your own data, create an inherited scene from
`res://game/world/game_world.tscn` and assign a new **WorldSource**:

- `mission_name`: a top-level BMS filename.
- `source_kind`: Loose source for extracted data; Retail install for PFF data.
- `data_directory`: a project-relative directory, for example
  `res://../assets`. Plain relative paths are relative to the Godot project.
- `install_key`: optional local installation name. When set, Local Folder
  records this computer's directory in Godot's project metadata and overrides
  `data_directory`. Absolute local paths need not enter source control.
- `game_code` and `expansion`: the retail mount's title/decode key and expansion.
  Defaults are `jo` and no expansion.

The source is explicit: previews do not inherit game launch flags or saved game
installation settings. Retail mode loads the archived BMS; loose mode reads the
loose BMS and its loose dependencies.

Only the active scene's first GameWorld and the primary 3D viewport drive a
preview. Use a single 3D view. The preview shows authored starting state;
mission scripts, AI, networking, gameplay sound and effects do not run.
Individual mission-object editing and launching the selected mission are later
steps. Normal Play still starts the ordinary game.

Select the actual scene nodes to inspect their loaded configuration:

- **Terrain** exposes the decoded terrain, textures and mission tile information.
- **Terrain > FoliageDispatcher** shares that terrain and exposes its terrain and
  weather node links, foliage slot definitions, resolved meshes and textures.
- **MissionEnvironment** exposes the loaded environment and overcast documents
  and the mission's starting time. **Weather** uses that environment.
- **Water**, **Celestial** and **EnvironmentCubeCapture** share the loaded terrain.
  SkyDome, Water and Celestial use the scene's MissionEnvironment links.

Source-derived properties are read-only during preview and are excluded from
scene storage. Reload rereads the native files and updates these same nodes.
Editing native data through these Inspector fields is a later step.

Saving stores WorldSource and authored scene configuration such as node links,
terrain quality and PreviewView. Decoded documents, generated models and runtime
state remain transient. Scene switches, Reload and plugin shutdown clear the
loaded configuration and restore the original node properties, editor camera
environment, compositor, and shader globals. An edited source selection marks
the existing preview stale until Reload.

Native codecs and lookup policy remain in their current owners.
`GameWorld.load_preview` mounts and reads the selected data, shares environment,
terrain and placement operations, and stops before runtime startup.
`refresh_preview` calls shared render operations with a scoped editor camera
and fixed time. The existing DisplayDecode node supplies the color conversion
on the editor camera. The authored clear Environment stays unchanged.
It does not call the gameplay frame loop.

Validation: `res://tests/world_preview_test.gd` covers native preview loading,
source policy, missing dependencies, frozen time, and scene persistence through
GUT. In a disposable Godot editor session, open
`res://tests/tools/world_preview_editor_check.gd` in the Script editor and use
**File > Run**. This exercises the real menu and toolbar, checks rendered terrain
and models, writes an authored change and verifies it reached disk, then checks
Reload, failed-load recovery, scene switching, and plugin disable/enable.
It restores its sample changes and leaves the plugin enabled; its report and
screenshots go to `.scratch/world-preview-*`.
