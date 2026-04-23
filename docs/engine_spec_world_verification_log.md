# World Verification Log

Tracks terrain / foliage / tile code paths that were audited against the original
engine and notes where the port is exact, intentionally bounded, or still an extension.

## Status Legend

- `exact`: implemented to match the recovered engine logic for the audited slice.
- `bounded-deviation`: intentionally close to the engine, but still missing an upstream
  system or caller contract needed for full parity.
- `extension`: functionality that exists in the port but is outside the traced engine path.
- `deferred`: known engine behavior that is not implemented yet.

## Entries

| Repo Symbol / Path | Primary IDA Witness | Status | Note |
| --- | --- | --- | --- |
| `opennova::foliage::place_cell` `[libs/foliage/src/placement.cpp]` | `sub_5C0240@0x5C0240`, midpoint block `@0x5C05CA-0x5C0694` | `exact` | PRNG, candidate walk, L-infinity cull, FORCE_ON bypass, foliagemap mask, corner samples, and asymmetric midpoint pairing now match the recovered engine placement kernel. |
| `opennova::foliage::Dispatcher` `[libs/foliage/src/dispatcher.cpp]` | `sub_5C1940@0x5C1940` | `exact` | Shared core owns the 4-quadrant walk, 128-entry LRU, 8-frame stagger, and near-Z gate. |
| `NovaFoliageDispatcher` runtime path `[godot/engine/terrain/nova_foliage_dispatcher.cpp]` | `sub_5C1940@0x5C1940`, `sub_5C0240@0x5C0240` | `bounded-deviation` | Runtime now routes through four shared dispatchers, but still drives them from one supplied centre until the original visible-entity list exists in the port. |
| `NovaFoliageDispatcher` editor preview path `[godot/engine/terrain/nova_foliage_dispatcher.cpp]` | N/A (authoring-only) | `extension` | Wider cell-grid preview remains editor-only so artists can inspect foliage coverage without the engine's runtime entity loop. |
| `NovaTerrainData::{get_height_world,get_height_world_bilinear,get_foliage_index_world}` `[godot/engine/terrain/nova_terrain_data.cpp]` | `Terrain_SampleHeightBilinear@0x5C6770`, `sub_5C65E0@0x5C65E0` | `exact` | World-to-sector-to-source mapping is consolidated behind one helper so height and foliage lookups share the same sector-grid and quadrant math. |
| `opennova::til_build_entry_uv_quad` `[libs/til/include/til/til.h]` | `Terrain_DrawTileOverlays2D@0x5C79C0`, `sub_5C42B0@0x5C42B0` | `exact` | Flag ordering `FLIP_X -> FLIP_Y -> ROTATE_90`, atlas tile selection, and out-of-range fallback are shared in one helper now. |
| `NovaTerrainTileOverlay::build_entry_uvs` / `rebuild` `[godot/engine/terrain/nova_terrain_tile_overlay.cpp]` | `Terrain_DrawTileOverlays2D@0x5C79C0`, `sub_5C42B0@0x5C42B0` | `extension` | The 3D ground-quad renderer remains a port-side extension, but its UV math now delegates to the shared engine-faithful helper instead of duplicating it. |
| `TerrainTileOverlayPreview._build_entry_uvs` `[godot/modtools/terrain/terrain_tile_overlay_preview.gd]` | `Terrain_DrawTileOverlays2D@0x5C79C0`, `sub_5C42B0@0x5C42B0` | `extension` | Editor ghost tiles call the native shared helper, so authoring preview no longer owns a second copy of the tile flag transform logic. |
| `opennova::til::{load_til,save_til}` `[libs/til/src/til_io.cpp]` | `Terrain_LoadTileInfoFile@0x5CA730`, `sub_6081D0@0x6081D0`, `sub_6080F0@0x6080F0` | `exact` | On-disk / replicated 12-byte entry layout remains canonicalized in one place with direct engine anchors in comments. |
| `sub_5C6450` ambient-source avoidance | `sub_5C6450@0x5C6450` | `deferred` | The port still lacks the ambient-source registry required to reproduce the engine's AABB probe; non-FORCE_ON foliage therefore does not yet avoid those regions. |
| `FoliageDef.match` shared schema `[libs/foliage/include/foliage/foliage.h]` | foliage def record read by `sub_5C0240@0x5C0240` | `bounded-deviation` | Shared schema still exposes one authored match code even though the engine-side slot record can carry multiple match entries; widening the terrain/TRN wrappers is a follow-up. |
