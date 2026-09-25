# Tile overlays (.til) — reverse-engineering record

Structure-mapping record for the original engine's **terrain tile overlays**
(the `.til` water/decal tile placements painted over the terrain surface) — the
12-byte overlay entry, the atlas UV mapping, and the flip/rotate transform. The
reimplementation surface is `engine/formats/til` (`til.h` / `til_io.cpp` /
`til_foliage_blocker.cpp` / `til_tsd.cpp`) and the terrain page composer
`engine/runtime/terrain/terrain_tile_composer` that draws the overlay into each
composed terrain page (the hosted 1024 overlay bake was retired 2026-09-24).
Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-TIL-…` divergence catalog. Produced by a read-only
IDA audit (PAR-R3, 2026-07-05); no IDB renames were made. It converts the
**Tiles** system from `UNAUDITED` to tracked (divergence-ledger.md).

Note on binaries: `engine/formats/til` was originally RE'd from `jodemo.exe`
(`Terrain_DrawTileOverlays2D @ 0x5C79C0`, `sub_5C42B0`); this audit re-confirms
the format and transforms against the **retail** render path
`PolyTrn_RenderTile @ 0x60df0d → PolyTrn_DrawTileOverlayQuad @ 0x604700`.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Overlay entry (12 B: x, z, tile_index, flags) | **MATCHING** | `PolyTrn_RenderTile @ 0x60df0d` reads `g_TerrainTileArray` in 12-B strides — `x @+0`, `z @+4` (stored **negated**), `tile_index @+8` (byte), `flags @+9` (byte); our `TilOverlayEntry` is `int32 x_fixed / int32 z_fixed / u8 tile_index / u8 flags / u16 reserved` |
| Foliage exclusion AABB | **MATCHING** | `Foliage_PathBlockedByPlacedTile @ 0x606490` linearly scans this same array with an inclusive candidate-square/16x16-entry overlap; `til_blocks_foliage` pins boundary and stored-negated-Z vectors |
| Atlas UV mapping | **MATCHING** | retail `col = tile_index % dword_319F7B8`, `row = tile_index / dword_319F7B8`, `u = col·step_u (flt_319F7C0)`, `v = row·step_v (flt_319F7C4)`; our `til_build_entry_uv_quad` (`tile_index % tiles_x` / `/ tiles_x`, `·step_u`/`·step_v`) |
| Flip/rotate flags (0x01/0x02/0x04) | **MATCHING (rotate direction corrected 2026-07-15, D-TIL-2; composition order corrected 2026-08-20, D-TIL-4)** | `PolyTrn_DrawTileOverlayQuad @ 0x604700`: `flags & 1` swaps U (@ 0x604782), `& 2` swaps V (@ 0x6047a9), `& 4` rotates the UV quad 90° **CCW** via the corner cycle `NW←NE, NE←SE, SE←SW, SW←NW` (@ 0x6047d4..0x604806) = per-corner `(u,v) → (1−v, u)`. The mirrors run FIRST and the rotate permutes the mirrored corner assignments — in sampling-function form: rotate, then flips (D-TIL-4). Full combo table: `0x04 → (1−z, x)`, `0x05 → (z, x)`, `0x06 → (1−z, 1−x)`, `0x07 → (z, 1−x)` |
| Half-texel UV shift | **MATCHING** | retail `u += ±0.5·flt_319F7C8`, `v += ±0.5·flt_319F7CC` (one uniform sign pair from the post-flag corner min/max comparison, applied to all four corners), like `til_build_entry_render_uv_quad`'s half-texel |
| Z world-convention negation | **MATCHING** | retail stores `z` and reads `-z` (`waterOverlayCount = -*(v20-1)`); our `til_world_z_from_fixed` returns `-z_fixed/…` |
| Tile-cache render-target alpha | **MATCHING (D-TIL-3 alpha facet REFUTED 2026-09-24)** | base, `.til` and scorch passes run under `COLORWRITEENABLE = 7` (`PolyTrn_RenderTile @ 0x60DD04..0x60DD12`, `@ 0x60DD6B..0x60DD73`; `0xF` restored `@ 0x60E0EA` / `@ 0x60E1B6`); page A is the Clear's 0 (`@ 0x60DC9D..0x60DCC5` -> `GTexRT_Select` Clear `@ 0x67FC32..0x67FC5B`) plus the additive DOT3 term. Overlays never write A. `compose_overlay_rgba` has written RGB only since #560 (2026-08-25); ctest `terrain_tile_composer` (the overlay oracles assert no alpha contribution) |
| Atlas sampling | **MATCHING 2026-09-24** | each page pass is an XYZRHW quad whose positions `fill_fullscreen_quad_vertices @ 0x678DFE..0x678E4B` copies unbiased, so page pixel x samples at position x; the atlas (flags `0x100203` @ 0x604B24) samples CLAMP + POINT min/mag/mip on its box level set, the level nearest the pixel footprint (a LOD-4 page reads level 0, a LOD-1 page level 3), and `PolyTrn_DrawTileOverlayQuad`'s half-texel bias (@ 0x604808..0x6048FD) lands each pixel on a texel centre. Commit "Compose terrain pages with the retail D3D9 raster and texture filters"; ctest `terrain_tile_composer` (1:1 `.til` line point-sampled, LOD-3 `.til` takes box level 1) |
| Atlas texture format | **PORTED 2026-09-24** | the atlas is created DXT5 on the reference adapter (`GTexture_CreateFromPixelData_0 @ 0x687717..0x687766`) through the statically linked D3DX9 codec, each level the box halving of the previous level's decoded blocks; the composer samples the DXT5 decode (`renderer/texture_dxt`, terrain-re texture creation). Commit "Encode the terrain atlas and detail layers with retail's D3DX DXT codec"; ctest `renderer_texture_dxt`, `terrain_tile_composer` |
| Mission tile-set atlas | **PORTED 2026-09-24** | a non-empty BMS tile-set name replaces the `.trn` `polytrn_tilestrip` (`Terrain_LoadEnvironmentConfig @ 0x6109C8..0x610A1C`); `trn_mission_tilestrip` feeds `TerrainData`'s tilestrip and `.TSD` table. Commit "Draw .til tiles from the mission's BMS tile set"; ctest `trn_config_roundtrip`, `mission_bms` |
| 128-entry tile cache (LRU) | **MATCHING** | `dword_319A2E4` 128-slot cache, LRU eviction by `dword_319FC04 - age`, matching the reference note (128-LRU). The record array is ported as `TerrainTileCompositionCache` (2026-09-24; claim rule, TOD cadence and lookup in [terrain-re.md](../terrain/terrain-re.md)) |
| OUTLINE flag (0x08) | **FIXED (faithful) 2026-07-05** | D-TIL-1 — retail JO renders no outline; neither do we |

## The witnessed overlay + render (`PolyTrn_RenderTile @ 0x60df0d`)

`g_TerrainTileArray` (count `g_TerrainTileCount`) is the overlay array — the same
data streamed S2C by `serialize_terrain_tiles @ 0x6080F0` (§5.37, D-NET-83). Each
12-B entry, when its fixed-point AABB (`x .. x+0x100000`, `z .. z+0x100000`,
`0x100000` = one 16-unit cell) intersects the sector, is drawn as a
**water quad** via `PolyTrn_DrawTileOverlayQuad(uv, pos, PolyTrn_TerrainTintHalf, flags)` —
the HALF terrain tint (env #19) copied into vertex diffuse and applied under the
terrain's MODULATE2X combine. The render marches the tile-sized quad, resolves
the atlas cell from `tile_index`, and
applies the flip/rotate flags to the UV corners.

`PolyTrn_DrawTileOverlayQuad @ 0x604700`(`uv_coords`, `vertex_positions`, `diffuse_packed`,
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

The overlays write RGB only. Overlay view mode `0x631` decodes to
`SRCALPHA/INVSRCALPHA` (`decode_blend_mode_to_d3d_states @ 0x680f2c..0x680f3a`)
with the stage alpha selecting texture A (`decode_mode_alpha_stage
@ 0x680c8a..0x680c95`), but the base pass and both ordered overlay loops (the
`.til` entries, then the scorches) run under `COLORWRITEENABLE = 7`
(`PolyTrn_RenderTile @ 0x60DD04..0x60DD12` before the base quad,
`@ 0x60DD6B..0x60DD73` before the overlay loops; `0xF` restored `@ 0x60E0EA` /
`@ 0x60E1B6` for the DOT3 and static-model alpha passes), so the source alpha
only weights the RGB blend. Page A is the page Clear's 0 (`@ 0x60DC9D..0x60DCC5`
-> `GTexRT_Select` Clear `@ 0x67FC32..0x67FC5B`) plus the tile DOT3 pass's
`ONE/ONE` light term (`PolyTrn_TileBakeDot3LightPass @ 0x60e385`). The exact
pass order is base RGB → ordered `.til` RGB → ordered scorch RGB → additive
DOT3 A. The 2026-08-17 reading that the device updates target alpha as
`srcA² + dstA·(1-srcA)` (D-TIL-3) missed the write mask and is refuted.

Each `.til` quad samples the DXT5 atlas CLAMP + POINT on the box level nearest
the page pixel's footprint, at integer page positions (the XYZRHW positions are
copied unbiased by `fill_fullscreen_quad_vertices @ 0x678DFE..0x678E4B`), so the
half-texel bias lands every pixel on a texel centre. The asset-gated
`terrain_tile_composer` oracles pin CP12 entries 53 (tile 42, flags 7) and 1013
(tile 41, flags 7) and 00TRa entries 761 (`0x05`) and 781 (`0x06`): every page
pixel against the point-sampled box-level texel of the decoded atlas, and a
page alpha untouched by the overlay.

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
| D-TIL-3 | A | **FIXED 2026-08-17 (ordered RGB); alpha facet REFUTED 2026-09-24** | The 2026-08-17 fix put the ordered `.til` source-over ahead of the additive DOT3 pass, which stands for RGB. Its alpha facet (retail blends all four target channels, so CP12's opaque road-marking tiles saturate cache A) is refuted: the overlay loops run under `COLORWRITEENABLE = 7` (`PolyTrn_RenderTile @ 0x60DD04..0x60DD12`, `@ 0x60DD6B..0x60DD73`; `0xF` restored `@ 0x60E0EA` / `@ 0x60E1B6`), so page A is the Clear's 0 plus the DOT3 term and overlays never write it. The composer has been RGB-only since #560 (2026-08-25); the record text was corrected in the 2026-09-24 rendering parity pass. |
| D-TIL-2 | A | **FIXED 2026-07-15** | `ROTATE_90` rotated the wrong way: the reimpl applied the CW transpose `(v, 1−u)` where retail's corner cycle @ `PolyTrn_DrawTileOverlayQuad 0x6047d4..0x604806` is the CCW `(1−v, u)` — every rotated tile rendered 180° off, scrambling multi-tile tire-track curves (user-reported on 00TRa). One shared helper (`til_transform_local_uv`) fixed; the overlay composer and GDScript binding inherit it. |
| D-TIL-1 | B | **FIXED (faithful) 2026-07-05** | `TIL_FLAG_OUTLINE` (0x08): the LINELIST perimeter-outline pass is **jodemo-only** (`Terrain_DrawTileOverlays2D @ 0x5C79C0`). Retail JO's tile-overlay render `PolyTrn_DrawTileOverlayQuad @ 0x604700` (via `PolyTrn_RenderTile @ 0x60df0d`) handles only bits 0/1/2 and draws a single TRIANGLESTRIP — no outline. Our code likewise **parses/preserves** the flag (in `TIL_FLAG_AUTHORED_MASK`, for round-trip) but renders no outline — so we already match retail JO (both omit it). Faithful, not a divergence; the flag is unconsumed-in-retail-JO (legitimately closed per the faithful-vs-open axis). |

Everything else (entry layout, atlas UV, flip/rotate, half-texel, Z negation,
the 128-LRU cache, and foliage AABB scan) is byte/behaviour-exact against retail.

## The tile-set atlas source (witnessed 2026-08-20)

The overlay pass samples the texture `Terrain_LoadTileSetAtlas @ 0x604a90`
loads — the **tile-set strip**, not a lightmap. `Terrain_LoadEnvironmentConfig @ 0x610940`
parses the environment/`.trn` config, which leaves the `.trn`'s `polytrn_tilestrip`
in the terrain-config string slot `configData+0xD00`; when `Bms_TileSetName`
(the BMS header's `+0x118` name, e.g. `trntile10`) is non-empty (@ 0x6109C8) it
is copied over that slot (@ 0x6109D2..0x6109E2) and its extension from the
first `.` replaced by, or else appended as, `TGA` (`Path_ReplaceOrAppendExtension
@ 0x53C780`, called @ 0x6109EE); the `.TSD` slot takes the same name with `TSD`
(@ 0x610A00..0x610A1C).
`PolyTrn_InitTextures @ 0x60aaa0` passes that slot (@ 0x60c5b9) to the loader,
which derives `Terrain_TileSetTilesX = width/64` and the `flt_319F7C0/C4`
UV steps plus the `flt_319F7C8/CC` half-texel factors — the exact atlas math
the overlay draw consumes (@ 0x60dec3..0x60dee5). The cluster's historical
`Terrain_Lightmap*` misnomer names were renamed to `Terrain_TileSet*` in the
IDB and every doc citation on 2026-08-20.

OpenNova ports the override (2026-09-24, "Draw .til tiles from the mission's
BMS tile set"): `MissionInfo.tile_set` (header `+0x118`) ->
`formats/trn` `trn_mission_tilestrip` -> `TerrainData`'s tilestrip, and so the
`.TSD` surface table the simulation resolves from it. Before the port every
mission drew from the `.trn` strip; in the JOX data 06TR and ASX_G13B (G13:
`trntile10` -> `trntilec1`), 07TR (Dvxc2: `trntile10` -> `trntilec1`), ASB_G11a
(G11: `trntileA1` -> `trntile10`) and TKX_G12A (g12: `trntileC1` ->
`trntile10`) change atlas. ctest `trn_config_roundtrip` (the asset-gated leg
resolves `trntilec1.TGA` for 06TR.bms over G13.trn) and `mission_bms`.

Two sibling systems witnessed while isolating the overlay producer, neither
of which contributes authored `.til` content:

- **Scorch decals**: the second overlay loop in `PolyTrn_RenderTile`
  (@ 0x60df71..0x60e0af) walks `dword_319A2D4` 20-byte rect records at
  `unk_31A1870` (x0,z0,x1,z1 in 16.16 + a pass index into `dword_319F910[]`,
  textures loaded by `Terrain_LoadScorchTextures @ 0x604ce0`) and draws each
  as a full-rect quad, diffuse white (gray `0xFF808080` when
  `g_TerrainAdapterCapsStorage.TexOpDisableOrArg2`). That field is
  `g_TerrainAdapterCapsStorage` (0x319FBA0) + 0x18, filled by
  `CGfxDevice_QueryAdapterCaps @ 0x67DF72..0x67DF8E` (TextureOpCaps & 5,
  cleared by the vendor quirk at device+0xE8) through `sub_676850` (called
  @ 0x60E4FE); it is normally set, so the scorch stage colour is
  MODULATE2X(texture, `0xFF808080`) = texture x 256/255 (@ 0x60E033..0x60E04C).
  The record quad puts UV (0,0) at (minimum X, maximum Z) (@ 0x60DFD1..0x60E02A,
  UV @ 0x60E06B..0x60E08E) and samples WRAP + LINEAR + MIPFILTER POINT (flags 0
  via `Texture_LoadByNameWithChannel @ 0x58B728`); the page alpha stays
  write-masked. These are runtime damage decals: the record array is empty in a
  fresh session, so registered render fixtures never see them. The composer
  ports the loop (`terrain_scorch`, terrain-re.md).
- **`<tileset>.TSD`**: `configData+0xE00` holds the tileset name with the
  `TSD` extension (@ 0x7DF3E4); `PolyTrn_InitTextures` parses it via
  `File_ParseASCIIFile` with callback `Terrain_ParseTsdRow @ 0x604c00`, mapping
  `INDEX_<n> <TSD_NAME>` lines into the 256-entry per-tile-index surface-type
  table `byte_319F7D8`. Surface classification only — no render contribution.

## 2026-09-24 rendering parity pass

- Page alpha: the D-TIL-3 alpha facet is refuted (the overlay loops are
  write-masked to RGB); the record's alpha-recurrence text is replaced above.
- Raster and filters: page pixels sample at integer positions and the atlas
  point-samples its nearest box level ("Compose terrain pages with the retail
  D3D9 raster and texture filters").
- Atlas format: DXT5 through the ported D3DX9 codec ("Encode the terrain atlas
  and detail layers with retail's D3DX DXT codec").
- Atlas source: the mission's BMS tile set overrides the `.trn` strip ("Draw
  .til tiles from the mission's BMS tile set").
- The hosted 1024 overlay bake is retired; the page composer is the only `.til`
  draw ("Retire the hosted 1024 .til overlay bake").

## Cross-references

- Reimpl: `engine/formats/til` (`til.h`/`til_io.cpp`/`til_foliage_blocker.cpp`/`til_tsd.cpp`),
  drawn into the terrain pages by `engine/runtime/terrain/terrain_tile_composer`
  (the port of the ordered entry loop `PolyTrn_RenderTile @ 0x60DDD4..0x60DF1B`),
  which `TerrainTileCacheDevice` drives. The hosted 1024 overlay bake
  (`til_overlay_bake`, `TerrainSurfaceInputs`' `tile_overlay` texture) was retired
  2026-09-24 ("Retire the hosted 1024 .til overlay bake"); nothing else read it.
- Page composition, the record cache and the DXT codec: [terrain/terrain-re.md](../terrain/terrain-re.md).
- Wire form of the same array: [net/novaworld-net-re.md](../net/novaworld-net-re.md)
  §5.37 / D-NET-83 (`serialize_terrain_tiles @ 0x6080F0`).
- The HALF tint the overlay renders under is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`PolyTrn_TerrainTintHalf`, `tile_overlay_tint_factor`).
