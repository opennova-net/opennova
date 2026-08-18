# Terrain workspace

Sculpt, paint, and dress NovaLogic terrain, then bake it to the game's data
formats. Part of the [OpenNova Editor (ONED)](../README.md).

## What you do here

The Terrain workspace opens a 3D view of the map with a fly camera. The mode rail
switches between five workflows:

- **Sculpt**: raise, lower, smooth, and flatten the ground with a brush (radius,
  strength, hardness).
- **Paint**: paint detail layers, vertex color, clone color from one spot to
  another, and tag surface types (sand, rock, grass, and so on).
- **Foliage**: set up foliage types (graphic, color blending, attributes) and
  paint where they grow.
- **Tile**: place tile overlays from a tile set, with flip and rotate, and edit a
  placed tile.
- **Layout**: set the sector grid and map size, the map origin, water height, and
  edge wrapping.

An asset dock on the right manages the map's shared assets: the detail-layer
terrain textures and shading maps, and the data maps that drive surface paint,
foliage placement, and tile placement. Save round-trips through the `.trn` text
config plus its binary assets; Export bakes the runtime terrain.

## Opening older (Black Hawk Down era) terrains

Terrains from the earlier titles open like any other `.trn`, and Export can
target either family: **BHD** writes the raw depth format those games read,
**JO/DFX** writes the compressed depth format Joint Operations and Delta Force
Xtreme read. Promoting an older map to JO/DFX means three things, and the
workspace does them for you on open:

- **Steepness.** JO/DFX limit how much height a 256-pixel run of the heightmap
  may span. The workspace flags any area over the limit when the map opens
  (with a one-click flatten) and blocks a JO/DFX export until they are fixed;
  BHD exports have no such limit.
- **Detail textures.** Older maps carry ONE detail texture drawn over the
  colormap. JO/DFX draw three blend-layer detail textures (Detail A/B/C) mixed
  by the blend map, plus a far target, and read the old single-detail key as a
  shading source instead. When a map has no blend layers authored, the
  workspace seeds Detail A/B/C and the far target from that single detail
  (blend map all-A), so the export looks the way the map did before; repaint
  the layers afterwards if you want a real splat.
- **Name.** Older maps ship an empty terrain name (the game used the file
  name). The workspace names the map after the `.trn` file, so the exported
  set is `<name>.trn/.cpt/_c.tga/...`, which is what missions reference.

The map opens dirty when any of these applied; the status bar says which.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.trn` | [`engine/formats/trn`](../../../engine/formats/trn) | terrain text config: name, sector grid, origin, wrap, asset paths, water |
| `.cpt` | [`engine/formats/cpt`](../../../engine/formats/cpt) | baked terrain mesh (collision and render) for the runtime |
| `.til` | [`engine/formats/til`](../../../engine/formats/til) | tile-overlay placement list |
| heightmap, colormap, detail maps | [`engine/runtime/terrain`](../../../engine/runtime/terrain) | the raster sources the map is built from |
| foliage map | [`engine/formats/foliage`](../../../engine/formats/foliage) | per-cell foliage placement |

## How it is built

A multi-workflow workspace (see the framework in [`../README.md`](../README.md)).
The adapter [`terrain_workspace.gd`](terrain_workspace.gd)
declares the five workflows in `_build_inspector_defs()` and opts into a 3D
viewport, the asset dock, and the in-world tile gizmo. (All five inspectors are
code-first under `ui/inspectors/`.)

| File | Role |
|---|---|
| [`terrain_workspace.gd`](terrain_workspace.gd) | workspace adapter: workflow rows, file actions, export |
| `terrain_editor.gd` | editor state: active tool, brush, document, viewport |
| `terrain_editor_document.gd` | document model: heightmap, colormap, blend, foliage and tile data |
| `terrain_edit_history.gd` | undo / redo history |
| `terrain_viewport.gd`, `terrain_viewport_input_router.gd` | 3D viewport and brush / tile input |
| `editor_terrain_mesh.gd` | builds the preview mesh |
| `ui/terrain_inspector.gd` | base class for the per-workflow inspectors |
| `ui/editor_asset_dock.gd` | texture, foliage, and tile-set asset dock |

## Related

- Editor framework and how to add a workflow: [`../README.md`](../README.md);
  contract in [`../framework/editor_workspace.gd`](../framework/editor_workspace.gd).
- Project overview and builds: [top-level README](../../../README.md).
