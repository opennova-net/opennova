# Tile overlays (.til) — reverse-engineering record

Structure-mapping record for the original engine's **terrain tile overlays**
(the `.til` water/decal tile placements painted over the terrain surface) — the
12-byte overlay entry, the atlas UV mapping, and the flip/rotate transform. The
reimplementation surface is `engine/formats/til` (`til.h` / `til_io.cpp` /
`til_overlay_bake.cpp`) and the Godot layer. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-TIL-…` divergence catalog. Produced by a read-only
IDA audit (PAR-R3, 2026-07-05); no IDB renames were made. It converts the
**Tiles** system from `UNAUDITED` to tracked (divergence-ledger.md).

Note on binaries: `engine/formats/til` was originally RE'd from `jodemo.exe`
(`Terrain_DrawTileOverlays2D @ 0x5C79C0`, `sub_5C42B0`); this audit re-confirms
the format and transforms against the **retail** render path
`PolyTrn_RenderTile @ 0x60df0d → render_water_quad @ 0x604700`.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Overlay entry (12 B: x, z, tile_index, flags) | **MATCHING** | `PolyTrn_RenderTile @ 0x60df0d` reads `g_TerrainTileArray` in 12-B strides — `x @+0`, `z @+4` (stored **negated**), `tile_index @+8` (byte), `flags @+9` (byte); our `TilOverlayEntry` is `int32 x_fixed / int32 z_fixed / u8 tile_index / u8 flags / u16 reserved` |
| Foliage exclusion AABB | **MATCHING** | `Foliage_PathBlockedByPlacedTile @ 0x606490` linearly scans this same array with an inclusive candidate-square/16x16-entry overlap; `til_blocks_foliage` pins boundary and stored-negated-Z vectors |
| Atlas UV mapping | **MATCHING** | retail `col = tile_index % dword_319F7B8`, `row = tile_index / dword_319F7B8`, `u = col·step_u (flt_319F7C0)`, `v = row·step_v (flt_319F7C4)`; our `til_build_entry_uv_quad` (`tile_index % tiles_x` / `/ tiles_x`, `·step_u`/`·step_v`) |
| Flip/rotate flags (0x01/0x02/0x04) | **MATCHING (rotate direction corrected 2026-07-15, D-TIL-2; composition order corrected 2026-08-20, D-TIL-4)** | `render_water_quad @ 0x604700`: `flags & 1` swaps U (@ 0x604782), `& 2` swaps V (@ 0x6047a9), `& 4` rotates the UV quad 90° **CCW** via the corner cycle `NW←NE, NE←SE, SE←SW, SW←NW` (@ 0x6047d4..0x604806) = per-corner `(u,v) → (1−v, u)`. The mirrors run FIRST and the rotate permutes the mirrored corner assignments — in sampling-function form: rotate, then flips (D-TIL-4). Full combo table: `0x04 → (1−z, x)`, `0x05 → (z, x)`, `0x06 → (1−z, 1−x)`, `0x07 → (z, 1−x)` |
| Half-texel UV shift | **MATCHING** | retail `u += ±0.5·flt_319F7C8`, `v += ±0.5·flt_319F7CC` (one uniform sign pair from the post-flag corner min/max comparison, applied to all four corners), like `til_build_entry_render_uv_quad`'s half-texel |
| Z world-convention negation | **MATCHING** | retail stores `z` and reads `-z` (`waterOverlayCount = -*(v20-1)`); our `til_world_z_from_fixed` returns `-z_fixed/…` |
| Tile-cache render-target alpha | **FIXED 2026-08-17 (D-TIL-3)** | base pass clears A; mode `0x631` blends overlay RGBA with `SRCALPHA/INVSRCALPHA`; the later DOT3 pass is additive `ONE/ONE`. The page composer formerly blended RGB only, leaving opaque CP12 road tiles at terrain-light A instead of retail's saturated A |
| 128-entry tile cache (LRU) | **MATCHING** | `dword_319A2E4` 128-slot cache, LRU eviction by `dword_319FC04 - age`, matching the reference note (128-LRU) |
| OUTLINE flag (0x08) | **FIXED (faithful) 2026-07-05** | D-TIL-1 — retail JO renders no outline; neither do we |

## The witnessed overlay + render (`PolyTrn_RenderTile @ 0x60df0d`)

`g_TerrainTileArray` (count `g_TerrainTileCount`) is the overlay array — the same
data streamed S2C by `serialize_terrain_tiles @ 0x6080F0` (§5.37, D-NET-83). Each
12-B entry, when its fixed-point AABB (`x .. x+0x100000`, `z .. z+0x100000`,
`0x100000` = one 16-unit cell) intersects the sector, is drawn as a
**water quad** via `render_water_quad(uv, pos, PolyTrn_TerrainTintHalf, flags)` —
the HALF terrain tint (env #19) copied into vertex diffuse and applied under the
terrain's MODULATE2X combine. The render marches the tile-sized quad, resolves
the atlas cell from `tile_index`, and
applies the flip/rotate flags to the UV corners.

`render_water_quad @ 0x604700`(`uv_coords`, `vertex_positions`, `diffuse_packed`,
`flip_flags`) — the flag transforms are exact:
- `flip_flags & 1` → swap U0↔U2 (mirror horizontal) = `TIL_FLAG_FLIP_X`.
- `flip_flags & 2` → swap V0↔V2 (mirror vertical) = `TIL_FLAG_FLIP_Y`.
- `flip_flags & 4` → rotate the UV quad 90° **counter-clockwise**: the corner
  UVs permute `A←B, B←D, D←C, C←A` (@ `0x6047d4..0x604806`), i.e. per corner
  `(u,v) → (1−v, u)` = `TIL_FLAG_ROTATE_90` (reimpl corrected 2026-07-15;
  D-TIL-2).
- Composition order: the flag blocks run in bit order 1 → 2 → 4, so the
  mirrors rewrite the corner UV variables FIRST and the rotate then permutes
  the already-mirrored assignments. Corner-assignment permutation is the
  inverse mapping of a sampling-function transform, so the faithful
  sampling-function order is **rotate, then flips** — the orders differ
  exactly for rotate + a single flip (`0x05 → T(x,z)=(z,x)`,
  `0x06 → (1−z,1−x)`); rotate + both flips (`0x07 → (z,1−x)`) and all
  flag singletons agree under either order (D-TIL-4, reimpl corrected
  2026-08-20).
- then a ±half-texel bias (`flt_319F7C8`/`flt_319F7CC`): ONE sign pair,
  chosen from the post-flag corner min/max comparison, applied uniformly to
  all four corners.
- draws `GDynamicVB_DrawPrimitive(5 = TRIANGLESTRIP, 4 verts)` with the
  witnessed vertex↔UV pairing `A=(x0,z0)→(u_lo,v_lo)`, `B=(x1,z0)→(u_hi,v_lo)`,
  `C=(x0,z1)→(u_lo,v_hi)`, `D=(x1,z1)→(u_hi,v_hi)` (pre-flag), positions from
  the caller's entry AABB `[x, x+0x100000] × [−z, −z+0x100000]` in 16.16 bake
  space [`orig: PolyTrn_RenderTile overlay loop @ 0x60de23..0x60df1b`] — the
  stored `z` is negated on read, so the reimpl's `til_world_z_from_fixed` +16-unit
  span and north-edge `v_lo` both match retail exactly.

The cache target's alpha is part of the overlay result, not a DOT3-only side
channel. The base quad writes A=0 (`PolyTrn_RenderTile @ 0x60dce5`). Overlay
view mode `0x631` decodes to `SRCALPHA/INVSRCALPHA`
(`decode_blend_mode_to_d3d_states @ 0x680f2c..0x680f3a`), while its stage alpha
selects texture A (`decode_mode_alpha_stage @ 0x680c8a..0x680c95`). The device
applies no separate-alpha override (`GfxBlend_ApplyToDevice @ 0x6817d0`), so
each draw updates target alpha as
`srcA² + dstA·(1-srcA)`. Finally the tile DOT3 pass adds its light term with
`ONE/ONE` blending (`PolyTrn_TileBakeDot3LightPass @ 0x60e385`). The exact pass
order is therefore base RGB/A0 → ordered `.til` RGBA → additive DOT3 A. The
asset-gated page-composer oracle pins this on CP12 entries 53 (tile 42, flags 7)
and 1013 (tile 41, flags 7), whose sampled atlas alpha is fully opaque.

The host also distinguishes "no authored overlays" from a broken overlay
source. With tile overlays enabled, an unresolved TRN-declared tile-info source
or a resolved/override table with entries sets `tile_overlay_required`. If its
tilestrip cannot be converted, the page device rebuild fails instead of
publishing a ready base-only texture array; an absent or empty authored tile
table remains a valid base-only case. This is a fail-closed evidence invariant,
not a claim about an additional retail rendering rule.

## Shared mission-tile foliage exclusion

`Foliage_PathBlockedByPlacedTile @ 0x606490` consumes the same
`g_TerrainTileArray` loaded from `<mission>.til` by
`Terrain_LoadTileInfoFile @ 0x60a740`. It linearly scans every 12-byte entry.
After decoding `x_fixed` and stored-negated `z_fixed`, the entry owns the
inclusive world AABB `[min_x,min_x+16] x [min_z,min_z+16]`. A foliage
candidate with radius `r` is blocked exactly when:

`min_x <= x+r && min_z <= z+r && max_x >= x-r && max_z >= z-r`.

Both foliage generators call it with `r = 2.0`; their existing `FORCE_ON`
attribute gate bypasses the call. `Terrain_GetSurfaceTypeAtPosition @
0x606510` consults the same entries for terrain overrides, while
`PolyTrn_LoadTileData @ 0x6081d0` installs the network form of the same array.
The reimpl therefore parses the co-named mission payload once before terrain
build and shares one resource/payload with overlay composition, foliage
exclusion, and listen-server initial state.

## D-TIL divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TIL-4 | A | **FIXED 2026-08-20** | Flip/rotate **composition order**: retail mirrors the corner UV variables first (`&1` @ 0x604782, `&2` @ 0x6047a9) and then applies the rotate as a corner-**assignment** cycle over those already-mirrored values (@ 0x6047d4). Permuting corner assignments is the inverse of transforming the sampling function, so in sampling-function form the faithful order is **rotate, then flips**. The two orders agree for `0x04`, pure flips, and `0x07` — but for rotate + exactly one flip (`0x05` → `T(x,z)=(z,x)`, `0x06` → `(1−z,1−x)`) the reimpl's flip-then-rotate drew the art 180° off. Witnessed against 00TRa's driving-course fork (entries 731/746/761/781, tiles 40/42) which retail draws through the `00tra-tire-marks-retail` camera; the fix dropped that fixture's full-frame MAE 16.35 → 12.17. The CP12 oracles are flags-`0x07` and were blind to this; `terrain_tile_composer` now pins 00TRa entries 761 (`0x05`) and 781 (`0x06`), and `til_render_uv` pins the `0x05/0x06/0x07` transform functions. Same shared helper (`til_transform_local_uv`) — all consumers inherit. |
| D-TIL-3 | A | **FIXED 2026-08-17** | The runtime page composer reproduced `.til` source-over in RGB but preserved bare-ground DOT3 A. Retail blends all four target channels before additively drawing DOT3, so CP12's opaque road-marking tiles saturate cache A and receive the witnessed lighting; composing ordered overlay A before the DOT3 add restores that result. |
| D-TIL-2 | A | **FIXED 2026-07-15** | `ROTATE_90` rotated the wrong way: the reimpl applied the CW transpose `(v, 1−u)` where retail's corner cycle @ `render_water_quad 0x6047d4..0x604806` is the CCW `(1−v, u)` — every rotated tile rendered 180° off, scrambling multi-tile tire-track curves (user-reported on 00TRa). One shared helper (`til_transform_local_uv`) fixed; the overlay composer and GDScript binding inherit it. |
| D-TIL-1 | B | **FIXED (faithful) 2026-07-05** | `TIL_FLAG_OUTLINE` (0x08): the LINELIST perimeter-outline pass is **jodemo-only** (`Terrain_DrawTileOverlays2D @ 0x5C79C0`). Retail JO's tile-overlay render `render_water_quad @ 0x604700` (via `PolyTrn_RenderTile @ 0x60df0d`) handles only bits 0/1/2 and draws a single TRIANGLESTRIP — no outline. Our code likewise **parses/preserves** the flag (in `TIL_FLAG_AUTHORED_MASK`, for round-trip) but renders no outline — so we already match retail JO (both omit it). Faithful, not a divergence; the flag is unconsumed-in-retail-JO (legitimately closed per the faithful-vs-open axis). |

Everything else (entry layout, atlas UV, flip/rotate, half-texel, Z negation,
the 128-LRU cache, and foliage AABB scan) is byte/behaviour-exact against retail.

## The tile-set atlas source (witnessed 2026-08-20)

The overlay pass samples the texture `Terrain_LoadTileSetAtlas @ 0x604a90`
loads — the **tile-set strip**, not a lightmap: `Terrain_LoadEnvironmentConfig @ 0x610940` copies
`Bms_TileSetName` (the BMS header's `+0x118` name, e.g. `trntile10`) into the
terrain-config string slot `configData+0xD00` (@ 0x6109d8) and appends `.TGA`;
`PolyTrn_InitTextures @ 0x60aaa0` passes that slot (@ 0x60c5b9) to the loader,
which derives `Terrain_TileSetTilesX = width/64` and the `flt_319F7C0/C4`
UV steps plus the `flt_319F7C8/CC` half-texel factors — the exact atlas math
the overlay draw consumes (@ 0x60dec3..0x60dee5). The cluster's historical
`Terrain_Lightmap*` misnomer names were renamed to `Terrain_TileSet*` in the
IDB and every doc citation on 2026-08-20.

Two sibling systems witnessed while isolating the overlay producer, neither
of which contributes authored `.til` content:

- **Scorch decals**: the second overlay loop in `PolyTrn_RenderTile`
  (@ 0x60df71..0x60e0af) walks `dword_319A2D4` 20-byte rect records at
  `unk_31A1870` (x0,z0,x1,z1 in 16.16 + a pass index into `dword_319F910[]`,
  textures loaded by `Terrain_LoadScorchTextures @ 0x604ce0`) and draws each
  as a full-rect quad, diffuse white (gray `0xFF808080` when `dword_319FBB8`).
  These are runtime damage decals — the record array is empty in a fresh
  session, so registered render fixtures never see them (unhosted; noted at
  D-TERRAIN-7's ordered-contributions item).
- **`<tileset>.TSD`**: `configData+0xE00` holds the tileset name with the
  `TSD` extension (@ 0x7DF3E4); `PolyTrn_InitTextures` parses it via
  `File_ParseASCIIFile` with callback `Terrain_ParseTsdRow @ 0x604c00`, mapping
  `INDEX_<n> <TSD_NAME>` lines into the 256-entry per-tile-index surface-type
  table `byte_319F7D8`. Surface classification only — no render contribution.

## Cross-references

- Reimpl: `engine/formats/til` (`til.h`/`til_io.cpp`/`til_foliage_blocker.cpp`/`til_overlay_bake.cpp`), consumed by
  the Godot terrain layer.
- Wire form of the same array: [net/novaworld-net-re.md](../net/novaworld-net-re.md)
  §5.37 / D-NET-83 (`serialize_terrain_tiles @ 0x6080F0`).
- The HALF tint the overlay renders under is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`PolyTrn_TerrainTintHalf`, `tile_overlay_tint_factor`).
