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

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.trn` | [`libs/trn`](../../../libs/trn) | terrain text config: name, sector grid, origin, wrap, asset paths, water |
| `.cpt` | [`libs/cpt`](../../../libs/cpt) | baked terrain mesh (collision and render) for the runtime |
| `.til` | [`libs/til`](../../../libs/til) | tile-overlay placement list |
| heightmap, colormap, detail maps | [`libs/terrain`](../../../libs/terrain) | the raster sources the map is built from |
| foliage map | [`libs/foliage`](../../../libs/foliage) | per-cell foliage placement |

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
