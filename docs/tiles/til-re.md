# Tile overlays (.til) — reverse-engineering record

Structure-mapping record for the original engine's **terrain tile overlays**
(the `.til` water/decal tile placements painted over the terrain surface) — the
12-byte overlay entry, the atlas UV mapping, and the flip/rotate transform. The
reimplementation surface is `libs/til` (`til.h` / `til_io.cpp` /
`til_overlay_bake.cpp`) and the Godot host. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-TIL-…` divergence catalog. Produced by a read-only
IDA audit (PAR-R3, 2026-07-05); no IDB renames were made. It converts the
**Tiles** system from `UNAUDITED` to tracked (divergence-ledger.md).

Note on binaries: `libs/til` was originally RE'd from `jodemo.exe`
(`Terrain_DrawTileOverlays2D @ 0x5C79C0`, `sub_5C42B0`); this audit re-confirms
the format and transforms against the **retail** render path
`PolyTrn_RenderTile @ 0x60df0d → render_water_quad @ 0x604700`.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Overlay entry (12 B: x, z, tile_index, flags) | **MATCHING** | `PolyTrn_RenderTile @ 0x60df0d` reads `g_TerrainTileArray` in 12-B strides — `x @+0`, `z @+4` (stored **negated**), `tile_index @+8` (byte), `flags @+9` (byte); our `TilOverlayEntry` is `int32 x_fixed / int32 z_fixed / u8 tile_index / u8 flags / u16 reserved` |
| Foliage exclusion AABB | **MATCHING** | `Foliage_PathBlockedByPlacedTile @ 0x606490` linearly scans this same array with an inclusive candidate-square/16x16-entry overlap; `til_blocks_foliage` pins boundary and stored-negated-Z vectors |
| Atlas UV mapping | **MATCHING** | retail `col = tile_index % dword_319F7B8`, `row = tile_index / dword_319F7B8`, `u = col·step_u (flt_319F7C0)`, `v = row·step_v (flt_319F7C4)`; our `til_build_entry_uv_quad` (`tile_index % tiles_x` / `/ tiles_x`, `·step_u`/`·step_v`) |
| Flip/rotate flags (0x01/0x02/0x04) | **MATCHING** | `render_water_quad @ 0x604700`: `flags & 1` swaps U, `& 2` swaps V, `& 4` rotates 90° — exactly `TIL_FLAG_FLIP_X`/`FLIP_Y`/`ROTATE_90` |
| Half-texel UV shift | **MATCHING** | retail `u += ±0.5·flt_319F7C8`, `v += ±0.5·flt_319F7CC` (sign by corner min/max), like `til_build_entry_render_uv_quad`'s half-texel |
| Z world-convention negation | **MATCHING** | retail stores `z` and reads `-z` (`waterOverlayCount = -*(v20-1)`); our `til_world_z_from_fixed` returns `-z_fixed/…` |
| 128-entry tile cache (LRU) | **MATCHING** | `dword_319A2E4` 128-slot cache, LRU eviction by `dword_319FC04 - age`, matching the reference note (128-LRU) |
| OUTLINE flag (0x08) | **FIXED (faithful) 2026-07-05** | D-TIL-1 — retail JO renders no outline; neither do we |

## The witnessed overlay + render (`PolyTrn_RenderTile @ 0x60df0d`)

`g_TerrainTileArray` (count `g_TerrainTileCount`) is the overlay array — the same
data streamed S2C by `serialize_terrain_tiles @ 0x6080F0` (§5.37, D-NET-83). Each
12-B entry, when its fixed-point AABB (`x .. x+0x100000`, `z .. z+0x100000`,
`0x100000` = one 16-unit cell) intersects the sector, is drawn as a
**water quad** via `render_water_quad(uv, pos, PolyTrn_TerrainTintHalf, flags)` —
the HALF terrain tint (env #19) applied under the terrain's MODULATE combine. The
render marches the tile-sized quad, resolves the atlas cell from `tile_index`, and
applies the flip/rotate flags to the UV corners.

`render_water_quad @ 0x604700`(`uv_coords`, `vertex_positions`, `height`,
`flip_flags`) — the flag transforms are exact:
- `flip_flags & 1` → swap U0↔U2 (mirror horizontal) = `TIL_FLAG_FLIP_X`.
- `flip_flags & 2` → swap V0↔V2 (mirror vertical) = `TIL_FLAG_FLIP_Y`.
- `flip_flags & 4` → rotate the UV quad 90° = `TIL_FLAG_ROTATE_90`.
- then a ±half-texel bias (`flt_319F7C8`/`flt_319F7CC`), sign chosen per corner.
- draws `GDynamicVB_DrawPrimitive(5 = TRIANGLESTRIP, 4 verts)`.

## Shared mission-tile foliage exclusion

`Foliage_PathBlockedByPlacedTile @ 0x606490` consumes the same
`g_TerrainTileArray` loaded from `<mission>.til` by
`Terrain_LoadFoliageFile @ 0x60a740`. It linearly scans every 12-byte entry.
After decoding `x_fixed` and stored-negated `z_fixed`, the entry owns the
inclusive world AABB `[min_x,min_x+16] x [min_z,min_z+16]`. A foliage
candidate with radius `r` is blocked exactly when:

`min_x <= x+r && min_z <= z+r && max_x >= x-r && max_z >= z-r`.

Both foliage generators call it with `r = 2.0`; their existing `FORCE_ON`
attribute gate bypasses the call. `Terrain_GetSurfaceTypeAtPosition @
0x606510` consults the same entries for terrain overrides, while
`PolyTrn_LoadTileData @ 0x6081d0` installs the network form of the same array.
The host therefore parses the co-named mission payload once before terrain
build and shares one resource/payload with overlay composition, foliage
exclusion, and listen-server initial state.

## D-TIL divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TIL-1 | B | **FIXED (faithful) 2026-07-05** | `TIL_FLAG_OUTLINE` (0x08): the LINELIST perimeter-outline pass is **jodemo-only** (`Terrain_DrawTileOverlays2D @ 0x5C79C0`). Retail JO's tile-overlay render `render_water_quad @ 0x604700` (via `PolyTrn_RenderTile @ 0x60df0d`) handles only bits 0/1/2 and draws a single TRIANGLESTRIP — no outline. Our code likewise **parses/preserves** the flag (in `TIL_FLAG_AUTHORED_MASK`, for round-trip) but renders no outline — so we already match retail JO (both omit it). Faithful, not a divergence; the flag is unconsumed-in-retail-JO (legitimately closed per the faithful-vs-open axis). |

Everything else (entry layout, atlas UV, flip/rotate, half-texel, Z negation,
the 128-LRU cache, and foliage AABB scan) is byte/behaviour-exact against retail.

## Cross-references

- Reimpl: `libs/til` (`til.h`/`til_io.cpp`/`til_foliage_blocker.cpp`/`til_overlay_bake.cpp`), consumed by
  the Godot terrain host.
- Wire form of the same array: [net/novaworld-net-re.md](../net/novaworld-net-re.md)
  §5.37 / D-NET-83 (`serialize_terrain_tiles @ 0x6080F0`).
- The HALF tint the overlay renders under is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`PolyTrn_TerrainTintHalf`, `tile_overlay_tint_factor`).
