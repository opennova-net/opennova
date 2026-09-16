# Terrain — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **terrain** pipeline — the
heightmap/mesh build, the quadtree LOD, CDEP, lighting/modulation, mesh
simplification, byte packing, and (REN-4) the runtime surface-shading resource
set. ADR 0037 removed OpenNova's legacy editor-side terrain builder, DEP/TPM
intermediates, and bake tests. Their analysis remains below as historical
research; the live reimplementation surface is the runtime terrain/query code
and Godot terrain layer. Binaries: **both** `jodemo.exe` (the accessible LOD/quadtree/mip renderer)
and retail **Jointops.exe** (lighting/modulation/fog/shading). This file is the
committed home for the `D-TERRAIN-…` catalog. Produced 2026-07-05 (PAR-R1);
the runtime shading section landed 2026-07-06 (maturity REN-4); the runtime
terrain-query section (height samplers + segment raycast) 2026-07-07
(ENG-3 B0); a fresh retail runtime re-grill on 2026-07-13 closed the live
LOD-family selector, texture preprocessing, top-tier bindings/math, tile-overlay
ordering, and fog-distance semantics recorded below; the 2026-07-29 model-shadow
audit pinned the static projected-silhouette tile producer and corrected the
old `PSShadow*` interpretation.

**Status: PARTIAL.** Terrain is the largest system and the last of the seven
`UNAUDITED` systems; this record establishes the tracked surface — the module
map and the mixed-binary witness basis. The data/build path is byte-identical,
and the top-tier base-surface texture derivation and shader math are now closed.
The bounded runtime gap is the remaining tile-composition ordered contributors,
refresh cadence, and RT edge/address/mip detail enumerated by D-TERRAIN-7, plus
CDEP/traversal documentation depth. The material-animation leg is closed by the
shared live AlphaGen/UV/flipbook evaluator. The underwater water-noise modulation is FIXED by
D-TERRAIN-8, including the 2026-08-17 coordinate correction.
Like [mission/mis-format-re.md](../mission/mis-format-re.md), this remains a
partial; it converts terrain from `UNAUDITED` to *tracked (partial)*.

## Historical build module map and live runtime witness basis

| Module | Role | Witness |
|---|---|---|
| `builder` | heightmap → terrain mesh (the build pipeline entry) | the TrnGen byte-identical data path (canonical reference) |
| `quadtree` / `build_quadtree` / `lod` | quadtree LOD traversal, frustum culling, height mipchain, final mesh-family selection | **jodemo** `Terrain_TraverseQuadTreeNode @ 0x5C89C0`, `Terrain_CollectVisibleSectors @ 0x5C9120`, `Terrain_BuildHeightMipChain @ 0x5C5310`; **retail** `render_terrain_sector_batch @ 0x6092a0` (eight families, `clamp(lod_sub, 0, 15) / 2`) |
| `cdep_constraint` | quantized [min,max] of the 256 pixels of a block (CDEP depth constraint) | documented in-code; full CDEP bitstream grill pending |
| `lighting` | terrain lighting colors + per-position modulation | **retail** `terrain_sector_compute_lighting @ 0x5c7550`; `Terrain_SetLightingColors @ 0x5C4B10` / `Terrain_GetModulatedColorAtPos @ 0x5C5FE0` are jodemo-era names with no kong function at those addresses (verified 2026-08-29: the addresses sit inside `render_visibility_portal_traversal @ 0x5c4ae0` and `Terrain_RenderSectorModels` respectively — do not cite; env-tod-re's retired rows); fog via `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` |
| `texture_preprocess` | byte-faithful detail coefficient map, DBlend normalization, and paired near/far mip chains | **retail** `Texture_GenerateNormalMap @ 0x58c070`, `PolyTrn_InitTextures @ 0x60aaa0`, `GTexture_Downsample2x2_RGBA8 @ 0x687000`, `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270` |
| `mesh_simp` | mesh simplification (edge-collapse) | **BYTE-IDENTICAL — verified** (2026-06) by the since-retired `dvd4_parity` (canonical `.cpt`) + `parametric_parity` sweeps over the Sample/Gradient/Checker64/Perlin TrnGen corpus (4.6–6.8 MB CPTs each, no longer carried in `fixtures/`); the tracked gate today is `tests/cpt/cpt_roundtrip_test` over the committed `assets/mnml.cpt` plus the gated retail `.cpt` sweep. The in-code "divergence point / vertex 1223" logging is leftover debug scaffolding from when parity was being achieved, now inert |
| `tpm` (formerly `engine/formats/tpm`, removed with ADR 0037 in d57608b3d; ex `mesh_data` here) | the TPM1 tile-mesh container (.tml/.tms) read/write | **TrnGen.exe** `MeshData_LoadFromFile @ 0x404100`, `MeshData_WriteToFile @ 0x403FE0` (the addresses are TrnGen's — kong retail holds `CAdminServer_*` there); §The TPM1 tile-mesh format below |
| `packing` (formerly `engine/formats/tpm/src`, removed with ADR 0037; ex here) | the TPM1 on-disk index codecs | **TrnGen.exe** `pack_words_to_bytes @ 0x403CD0` (low byte of each u16, 3 bytes/group) + the unpack/10-bit pair `@ 0x403DD0/0x403E70/0x403EF0` |
| `tristrip` (incl. the ex-`mesh_data` remap pass) | strip conversion + the cache-order vertex remap the bake runs before writing .tms | **TrnGen.exe** `sub_4068E0` (strips), `sub_404480` via thunk `sub_404610` (remap) |
| `depthmap` | depth/height map storage (the raw `.dep` intermediate's read/write formerly lived in `engine/formats/dep`, removed with ADR 0037) | in-code |
| `terrain_query` raycast (ENG-3 B1, ported with #209) | world-space height samplers + the segment raycast used by runtime consumers | **retail** §Runtime terrain queries below (`Terrain_SampleHeightBilinear @ 0x6067b0`, `Terrain_RaycastHeightmapLoRes @ 0x60cb80`, `Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`) |

The tile overlay and foliage that render over the terrain surface have their own
now-landed records: [tiles/til-re.md](../tiles/til-re.md) (PAR-R3),
[foliage/foliage-re.md](../foliage/foliage-re.md) (PAR-R2). Env #19's
`terrain_rgb` is confined to the dead far-colormap bake described below; the
live top-tier terrain shader has no terrain-tint multiplier
([env/env-tod-re.md](../env/env-tod-re.md) #19).

## Empty-sector flat fallback (2026-09-13, D-TERRAIN-12)

| Component | Verdict | Evidence |
|---|---|---|
| Empty-sector routing, geometry and primary UVs | MATCHING (bounded behavioral proof) | `terrain_frame_compiler` covers mixed and all-empty grids, both skip policies, nonflat source geometry, zero primary UVs, shared page identity, and above/below-water classification; retail instruction witnesses below. |
| Flat page base/DOT3 source and mission-overlay gate | MATCHING (bounded behavioral proof) | `terrain_tile_composer` checks every output texel against a distinct source-corner color/normal with an overlapping opaque `.til` entry; `terrain_tile_composition_cache` covers the LOD-0 projection and origin-sector borrower. |
| Flat stage-2 blend transform and independent detail/noise | MATCHING (instruction witness; shader contract) | `terrain_shader_contract_test.gd` pins the collapsed blend coordinate independently of the live stage-1 detail and stage-3 detail/noise coordinates. Full-scene pixel equivalence remains unverified. |
| Godot mesh upload and shader parameter application | host code / not grillable | `Terrain` uploads the native vertex variants, selects the flat mesh by draw-list flag, and applies the native zero-primary-UV projection. |
| Water-mirror exclusion of the flat fallback | MATCHING (visual-layer split; instruction witness) | Flat draws ride `visual_layers::TERRAIN_FLAT_FALLBACK` alone; `REFLECTION_CULL_MASK` excludes it and the beauty camera admits it. `terrain_shader_contract_test.gd` pins the mask split. |

A zero entry in the 16x16 `.trn` sector grid is not an unconditional hole.
Both the main draw collector and the visible-bounds collector select quadrant
1's existing quadtree with packed tile-key bit `0x80000000`, unless the view's
word at byte offset `+100` is nonzero. That word is wider than an
empty-sector switch: `PolyTrn_RenderFrame` copies it into `dword_319FB84`,
the water-plane view mode, whose readers are the three empty-sector gates
plus the sector batch (its per-light terrain pool re-draw loop is skipped
when nonzero), the terrain scene renderer, and the lighting/shader setup.
Its writers: the live draw pass writes zero, the bounds pass zeroes
`+92..+100`, `sub_610420` writes zero, and both the water-mirror prerender
and the PCX screenshot scene write `Env_WaterHeightFixed != 0`. The
ordinary main view therefore includes the flat fallback, and the 11x11
sector window continues to supply flat geometry beyond the authored land
instead of exposing the scene clear, while every mirror rendered for a
mission with water contains no empty-sector geometry (the prerender only
runs while the water pass is active). OpenNova's shared compile models only
the live draw pass: `TerrainViewInput::skip_empty_sectors` defaults to
false and the bounds pass shares that value; the mirror exclusion is the
visual-layer split described below, and the screenshot scene and the
temporal reprojection (overlay) routing are not modeled. Witnesses:
[orig: PolyTrn_RenderFrame @ 0x60EAC0, `+100` read @ 0x60EB3C, routing
@ 0x60EC94..0x60ECBD, gate @ 0x60ECB0]; [orig: terrain_render_visible_sectors
@ 0x6090C0, reads @ 0x609122 and @ 0x609244, routing @ 0x609238..0x609263];
[orig: render_terrain_sector_batch @ 0x6092A0, reads @ 0x6092C6,
@ 0x60983F..0x609846, @ 0x60997A and @ 0x609B31]; [orig: terrain_render_scene
@ 0x60E850, reads @ 0x60E8B1 and @ 0x60E9EE]; [orig:
terrain_setup_lighting_and_shader @ 0x604420, read @ 0x604444]; [orig:
sub_6040A0 @ 0x6040A0, read @ 0x6040F3]; [orig: sub_60FF50 @ 0x60FF50, zero
store @ 0x60FFCE]; [orig: terrain_setup_view_and_lighting @ 0x60FE40, zeroed
view words @ 0x60FEBA]; [orig: sub_610420 @ 0x610420]; [orig:
render_main_scene @ 0x5C1240, `+100` = 1 @ 0x5C1567 and = 0 @ 0x5C1583];
[orig: render_scene_with_water_reflection @ 0x5D7EA0, stores @ 0x5D8061 and
@ 0x5D8079]; [orig: Water_ReflectionPrerender @ 0x5C2780]; [orig:
Render_TerrainScene @ 0x610C80].

The water-mirror exclusion: retail's mirrored terrain never contains flat
fallback geometry when the mission has water. OpenNova's mirror is a Godot
camera over the same scene (env #30), so the flat draws ride their own
visual layer alone (`visual_layers::TERRAIN_FLAT_FALLBACK`, written on the
patch instance together with the zero-height flag): the beauty camera mask
admits it, `REFLECTION_CULL_MASK` excludes it above and below water, and the
mirror only renders for a nonzero water height, the same condition the
retail word encodes. Ordinary draws keep the world layer.

The view cull (ported 2026-09-14, jo-c cross-check): the terrain walk never
reads the projection. `PolyTrn_RenderFrame` rebuilds four clip planes through
the eye every frame from the horizontal FOV alone (`sub_603DA0`: `halfH = fov *
0.5`, `halfV = 0.5 * (fov * 0.83333331)` — the vertical half-angle is a fixed
5/6 of the horizontal one whatever the display aspect — deg→rad literal
`0.01745327777777778`, planes `(±cos H, 0, sin H)` and `(0, ±cos V, sin V)`
with `d = 0`) and cuts depth with the scalar `flt_8493E8` (static 2000.0,
overwritten by context float `[6]`, the frame's view distance, whenever that is
positive). Per node, in order: the sphere center (node `+0x30/+0x34/+0x38`
plus the sector origin, Y zeroed for flat sectors) is transformed into view
space; `depth - r > far` and `depth + r < 0` reject; a level-0 node with
`depth > far` and a level-1 node with `depth - far > 0.25 r` (`flt_7C333C`) are
forced to subdivide; each plane's signed distance `< -r` rejects; a distance
below `-0.33 r` (`flt_7C59B4`) runs the eight-corner AABB test
(`Terrain_TestAABBOutsideFrustumPlane`, inside = any corner strictly positive)
which rejects outright or, below level 3, forces subdivision. The LOD
multiplier `flt_319FB2C` is the context's quality scale times the
settings-derived `flt_8493D8 = clamp((polygonDetail + 1) * 0.25, 0, 1) * 0.8 +
0.2` (`Terrain_Init` → `sub_605D70`; detail 3 = 1.0, detail 0 = 0.4). The port
is `TerrainViewCull`/`make_terrain_view_cull`/`terrain_lod_quality_scale` in
`engine/runtime/terrain/quadtree.*`, fed by `TerrainViewInput::fov_deg`
(the Godot embedder derives it from the projection's first column),
`far_distance` (the integer part of the live fog distance) and
`polygon_detail`; ctest `terrain_view_cull`. The earlier port extracted six
Gribb/Hartmann planes from `proj * view`, which admitted a different vertical
band (0.75 h at 4:3, 0.5625 h at 16:9) and used the projection's own far
plane. Witnesses: [orig: sub_603DA0 @ 0x603DA0]; [orig: PolyTrn_RenderFrame
@ 0x60EAC0, fov @ 0x60EAF6, far override @ 0x60EB7E..0x60EB8D, quality product
@ 0x60EB4A]; [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, depth slab
@ 0x608AAA..0x608ACB, straddle arms @ 0x608AD1..0x608B03, sphere planes
@ 0x608B08..0x608BD7, AABB refinement @ 0x608BDD..0x608C97]; [orig:
Terrain_TestAABBOutsideFrustumPlane @ 0x6086C0]; [orig: sub_605D70 @ 0x605D70;
Terrain_Init @ 0x60FC33].

Flat mode zeros the traversal center Y and the AABB test's Y extent while
retaining the source node's radius. The vertex decoder zeros position Y and
both primary texture coordinates, preserving X/Z topology and the independent
secondary detail stream. The tracked-bounds accumulator still reads the raw
source node height range: changing that range to zero would introduce another
behavior difference. Witnesses: [orig: Terrain_TraverseQuadtreeNode @ 0x608A00,
flat center @ 0x608A50 and raw tracked heights @ 0x608E04..0x608E5F];
[orig: Terrain_TestAABBOutsideFrustumPlane @ 0x6086C0, flat Y/extent
@ 0x6086DF/0x608772]; [orig: decode_terrain_tile_vertices @ 0x602AA0,
zero stores @ 0x602DC9..0x602DCF].

The sector batch also collapses the stage-2 blend-map transform: flat mode
zeros transform 8 except its homogeneous bottom-right element, so splat
weights sample the source corner. Stage-1 detail coordinates remain live,
and stage 3 keeps the separate transform 9 for authored detail2 or underwater
noise. The shader passes a distinct blend coordinate so collapsing the
weights does not collapse those detail inputs. Ordinary/editor draws retain
the existing coordinates through the flat flag's false default. Witnesses:
[orig: render_terrain_sector_batch @ 0x6092A0, flat transform
@ 0x60973A..0x609750, publication @ 0x609799..0x6097A0];
[orig: PolyTrn_InitTextures @ 0x60AAA0, stage-2/3 binds
@ 0x60C400..0x60C411 and @ 0x60C483..0x60C494].

`PolyTrn_RenderTile` canonicalizes every flagged request to one shared page:
LOD 0, zero coordinates/origins, with the high-bit identity retained. Base
colormap and heightfield-normal DOT3 sample source UV `(0,0)` across the page.
The mission `.til` loop is skipped; the following scorch and static-projection
paths retain their ordinary order. OpenNova reserves page LOD 0 for this flat
identity and keeps its geometric 1024-unit projection separate from terrain's
zero primary texture-coordinate projection. Spatial object/foliage borrowing
can select the ready flat page in its canonical origin sector `(0,0)`; the
ordinary sector-origin comparison rejects other routed sectors. The retail
lookup masks packed coordinates without rejecting the high bit, and a flat
borrower receives the ordinary `1/1024` projection rather than the terrain
mesh's zero UVs. Witnesses: [orig: PolyTrn_RenderTile @ 0x60DA70,
canonicalization @ 0x60DA98..0x60DAA8, zero source UVs
@ 0x60DC08..0x60DC16, `.til` gate @ 0x60DDA7];
[orig: terrain_tile_cache_lookup @ 0x604140, masked-coordinate and sector
checks @ 0x6041A4..0x6041E1, projection @ 0x604215..0x604292].

Original-machine routing probes used the verified retail executable SHA-256
`b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`:
an all-zero grid produced 121 traversal calls with view `+100=0` and zero
calls with `+100=1`. The jo-c oracle fixture's `skip_empty` label writes `+96`,
so these probes explicitly wrote `+100`; its host preview was not used as a
visual oracle. IDA reads used `Jointops.exe.kong.i64` at image base `0x400000`.
No IDB changes were made. Full-scene retail pixel equivalence and the broader
D-TERRAIN-7 composition lifecycle remain separate verification work.

## The TPM1 tile-mesh format (.tml/.tms — TrnGen.exe witness map)

The tile-mesh container the bake writes and the CPT export re-reads. Reimpl (historical,
removed with ADR 0037 in d57608b3d): `engine/formats/tpm` (read/write + the index codecs; magic-shaped lib name,
precedent bfc1, since the two extensions share one format), with the
strip/remap bake pass staying in `engine/runtime/terrain` (`tristrip`).
The retail `S0_00_00.tml` sample and its `tpm1_roundtrip` byte gate were
retired with the retail terrain corpus; the format record below stands.

- **Header (12 bytes)** [orig: `MeshData_LoadFromFile @ 0x404100`, TrnGen.exe]: magic
  `TPM1` (0x314D5054), `tile_x u16`, `tile_y u16`, `vertex_count u16`,
  `reserved u16`. Vertex data follows: 4 bytes per vertex (packed x,y
  offsets), rotate-crypted.
- **LOD sections**: the in-memory MeshData carries 16 12-byte LOD entries;
  the FILE carries 8 — every OTHER entry (indices 0,2,…,14). Per-section
  12-byte header: a pointer placeholder u32 (written as 0; the reader's
  32-byte roundtrip tolerance, pinned by the retired `tpm1_roundtrip`, covers
  stale writer pointers in retail files), `face_count u32`, `max_index u16`, `flags u16`
  (bit 1 = triangle strip).
- **Three index-codec tiers by `max_index`** [orig: the pack/unpack family
  `@ 0x403CD0/0x403DD0/0x403E70/0x403EF0`]: ≤256 → byte-packed (3 bytes per
  3-index group), ≤1024 → 10-bit packed (4 bytes per group), else raw u16.
  Every payload (vertices + indices) goes through the cpt `rotate_crypt`.
- **Pipeline roles**: `.tml` is written by the quadtree bake
  (`build_quadtree`), `.tms` by the builder's remap pass — the same
  container, post `remap_vertex_ordering` (`sub_404480`: strip conversion,
  first-seen vertex sort with the exact quicksort/insertion-sort hybrid,
  forward/inverse remap, `max_index = max + 1`). Retail treats existing tile
  files as a cache (the cache-trust semantics tracked in TODO.md).
- The raw `Output.dep` depth intermediate the bake hands to the CPT export was
  its own minimal lib (`engine/formats/dep`, removed with ADR 0037; the format
  record stands): headerless 1024×1024 u16,
  short/missing file reads zero-filled.

## Runtime surface shading (REN-4, retail Jointops.exe — witness map)

The terrain surface's runtime shader/material resource set, decoded at REN-4
against the device layer ([ADR 0023](../adr/0023-render-visual-parity.md):
witness source, never a port target). The state-struct decoder key is the ptl
record's `RenderState_ApplyToDevice @ 0x681920` layout (stage 0 at +16,
stages 1..5 at stride 36 from +52 — see the errata note in
[particles/ptl-format-re.md](../particles/ptl-format-re.md) §5.2); the
engine-wide render-mode word and pass-flag bits are decoded in
[render/render-material-re.md](../render/render-material-re.md).

**Capability tiers** (`PolyTrn_LoadTerrainConfig @ 0x60e3d0`): caps bit 3
required; caps bit 10 + ≥2 simultaneous textures → `PolyTrn_ShaderTier
@ 0x31a181c` = 2, else caps bit 8 → tier 1, else 0;
`PolyTrn_UsePixelShaderPath @ 0x8493e0` + `PolyTrn_UseMultiTexturePath
@ 0x8493e4` set with the tier and force-cleared unless `dword_32655B4` ∈
{80, 73} (an unidentified device/format code — open question in the render
record).

**Texture build — fresh re-grill (2026-07-13)** (`PolyTrn_InitTextures @ 0x60aaa0`):
Tier ≥ 2 loads authored detail textures c1/c2/c3 plus
`polytrn_detailmapdist`; the second detail `polytrn_detailmap2` pairs with
`polytrn_detailmapdist2` through the same builder and feeds the ps.1.4
splat's stage 3 (see the top-tier closure below)
[`orig: PolyTrn_InitTextures @ 0x60af97/0x60aff9`]. A coefficient normal
texture is additionally generated for the **ps.1.1 tiers** (bound at stage
7, not consumed by the ps.1.4 splat) from the authored `polytrn_detailmap`
**B channel**, not the terrain heightmap: wrapped `(coord ± 1) & (dimension - 1)` neighbor samples form X/Y,
fixed scale 1/32 (`bumpScale @ 0x7DBFAC`) is applied, **Z is the retail
`fld1` unit 1.0** (corrected 2026-07-15; the earlier `Z = 2` reading halved
every slope response), the vector is normalized, and each RGB
component is packed by `trunc((component + 1) × 127.5)`; the
source center alpha is preserved [`orig: Texture_GenerateNormalMap @
0x58c070`, `fld1 @ 0x58c1fa`, paired diffs `@ 0x58c26d..0x58c2b0`; call at
`PolyTrn_InitTextures @ 0x60b14d`]. The heightfield-normal generator uses the
same shape: paired one-sided diffs equal to the central difference at scale
1/256 (`@ 0x7C6950`) per axis, unit Z (`fld1 @ 0x603248`), encode 127.5
(`@ 0x7D8B48`), and packed alpha `0x80`
[`orig: Terrain_GenerateNormalMap @ 0x603210, pack @ 0x6034eb`]. DBlend is normalized
with the recovered integer path: for a nonzero RGB sum, coefficient =
`floor(65535 / sum)` and each channel becomes
`(coefficient × channel) >> 8`; a zero sum becomes pure R and alpha is
preserved [`orig: PolyTrn_InitTextures @ 0x60b1f0..0x60b24d`]. The result is
quadrant-split into `DBlendmap0..3`; the authored-detail coefficient texture is
stored separately at `dword_319F994`. It is **not** `TrnNMap0..3`: those four
textures are the heightfield-normal quadrants used by the tile-cache alpha bake
described below.

Each authored c1/c2/c3 texture is independently paired with the same far
`polytrn_detailmapdist` through the retail custom mip builder
[`orig: GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`]. Base and
far inputs are independently 2×2 box-downsampled per level with per-channel
floor averaging [`orig: GTexture_Downsample2x2_RGBA8 @ 0x687000`]. At level
`i` of `mip_count`, far weight is
`min(256, floor(320 × i / mip_count))`, base weight is its complement, RGB
is the fixed-point weighted blend, and alpha is copied from the base mip
unchanged. Base/far mixing is therefore a mip-chain construction, not a
camera-distance normal crossfade. Retail stops after the 4×4 level; the Godot
adapter appends the API-required 2×2/1×1 tail after preserving every
retail-visible level byte-for-byte. The DBlendmap/Colormap/TrnNMap quadrant
textures are created with flags `0x100001` — no mip-suppression bit — so
they carry full box-filtered auto chains
[`orig: PolyTrn_InitTextures @ 0x60b2c9; GTexture_CreateFromPixelData
@ 0x6877c7..0x6878be (D3DXFilterTexture BOX)`]; the reimpl box-mips the
normalized blend and uses a box-mipped colormap as the per-LOD tile-RT
surrogate. The witnessed device supports texfilter
modes 0–4: 0/1 = `MAG/MIN LINEAR` + `MIPFILTER POINT`, 2 = trilinear, 3/4 =
`MINFILTER ANISOTROPIC` + `MAXANISOTROPY` [`orig: per-stage filter select
@ 0x67e38a..0x67e45b; device init @ 0x679c28..0x679c9c,
0x677f9e..0x677ff9`]. The 2026-07-15 screenshot adjudication (00TRa
courtyard, 03tr distant slopes) shows the retail reference sampling ground
detail at minor-axis LOD — near-mip-0 grain at grazing angles and textured
far slopes — i.e. the ANISOTROPIC mode is active on the reference machine,
and the exact capture-machine `game.cfg` reads `texfilter_level = 3`. The
parser feeds the terrain-device state, whose cfg-3 branch selects filter mode
4 (`MINFILTER=ANISOTROPIC`, linear mip filtering, and the device capability's
`MAXANISOTROPY`) [`orig: config parse @ 0x551239; mode map
@ 0x610be8..0x610c1d`]. The captured D3D9 capability is 16. Point-mip,
trilinear, or 8× sampling of the gray-converging paired chains measurably
washes mid-distance terrain that retail renders grainy. The reimpl therefore
samples the detail layers anisotropically
(`filter_linear_mipmap_anisotropic`, project anisotropy 16×) and scales the
gradients back onto the 4×4 terminal so the synthetic Godot tail is never
selected.

Implemented in `engine/runtime/terrain/texture_preprocess.h` and
`Terrain::_load_textures`;
`terrain_texture_preprocess_test` pins recovered byte vectors, while
`terrain_lod_family_test` pins all 16 sublevels against the eight-family
selector. The same preprocessor now builds the separate heightfield-normal
atlas from CPT depth and the four parsed quadrant-lock pairs.

The average detail color → `flt_319F9D0/D4/D8` (0.50196 = 128/255 constants
when the blendmap path is active). The 1024×1024 colormap is checksummed and
quadrant-split into `Colormap0..3` (tier path raw, non-tier path
alpha-PREMULTIPLIED). The authored-detail coefficient texture, the
heightfield-normal quadrants, and the tile-cache render target are three
different resources; the 256×256 far colormap (`"PolyTrn colormap2"
@ 0x319f798`) box-downsampled 4×4 from the premultiplied colormap with the
UNDERWATER TINT baked (texels at/below `Env_WaterHeightFixed >> 15`:
`color/4 + (48,32,32) BGR`) and multiplied by `PolyTrn_TerrainTintFull
@ 0x31a1824` `>> 12` — this loop is env #19's dead bake site, not a live
top-tier shader multiplier (its readers are
zero-xref, confirming the record's dead-code disposition); the `"depthspin"`
256×256 shore texture (4-tap height sums `<< 14` in alpha over white); the
4×4 `"PolyTrnClip"` pattern (gray, alternating alpha rows); the 128×128
`"PolyTrnNoise"` detail noise (PRNG ±4 offsets around 0x80 per channel).

**Tile-cache t0 producer — fresh alpha correction (2026-07-13).** The terrain
pixel shaders do not sample raw colormap RGBA as `t0`; they sample the cached
tile render target made by `PolyTrn_RenderTile @ 0x60da70`. Its base draw
(`0x60dcd2..0x60dd5a`) uses diffuse `0x00808080`: RGB `MODULATE2X`
produces `clamp(rawColormap.rgb × 256/255)`, while alpha `MODULATE` with
diffuse A=0 clears authored colormap alpha. The bare base therefore leaves
`t0.a = 0`, not a baked sun mask.

**Overlay loops never write RT alpha (2026-08-23).** `PolyTrn_RenderTile`
masks the alpha channel off for the whole overlay window:
`SetRenderState(D3DRS_COLORWRITEENABLE, 7)` before the base draw
(`0x60dd04..0x60dd12`) and again before the ordered `.til`/scorch loops
(`0x60dd6b..0x60dd73`), restoring `0xF` only for the depth-alpha/DOT3 alpha
passes (`0x60e0ea..0x60e0f2`, `0x60e1b6..0x60e1be`). The `.til` tile quads
(view mode `0x631` = SRCALPHA/INVSRCALPHA with stage alpha SELECTARG1
TEXTURE) and the scorch quads therefore blend RGB only; the final page alpha
is exclusively the DOT3 light term (plus the static-projection passes). The
reimpl composer briefly wrote `src.a x src.a` tile alpha here, which
saturated heavily-tiled pages (03TR airfield ~0.85 mean alpha) and re-lit
them with full sun ambient at dawn — lit terrain measured ~2x retail at
06:30 while noon stayed within noise.

`Terrain_GenerateNormalMap @ 0x603210` then supplies the alpha-lighting
source. For uint16 height `H`, its loop (`0x603326..0x6034eb`) computes
`N = normalize(Hwest-Heast, Hnorth-Hsouth, 256)` (paired one-sided diffs at
scale 1/256 per axis with the `fld1` unit Z — the 2026-07-15 correction; the
earlier `..., 512` reading was the retracted `Z = 2` form), packs each
component as
`trunc((N+1) × 127.5)` clamped to a byte (`0x603470..0x6034eb`), and stores
A=128. Its A8R8G8B8 RGB semantics are `(grid X slope, grid Y slope, up)`, not
a world-space XYZ normal. The reimpl's `FORMAT_RGBA8` upload preserves those
semantic RGB channels; there is no upload-time channel swap.

**Hosted light-vector basis correction (D-TERRAIN-10, fixed 2026-07-14).**
`Environment_GetLightDirectionFloat @ 0x57d870` supplies a direct getter tuple
`g=(g0,g1,g2)`, which EnvFile preserves unchanged; it is **not** a Godot/world
XYZ vector. `PolyTrn_RenderTile @ 0x60da70` receives that tuple in stack fields
`outDir/var_28/var_24`, then its packer (`0x60e201..0x60e331`) writes D3DCOLOR
`BYTE2←g2`, `BYTE1←g0`, and `BYTE0←g1`. GPU diffuse RGB is therefore
`(g2,g0,g1)`, so the reimpl shader must quantize the direct tuple as `(z,x,y)`
before `PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e`. The old reimpl
`(x,z,y)` mapping swapped the two horizontal DOT3 axes; flat normals could not
distinguish that permutation, so the 06:00/12:00/18:00 flat checks all missed
it. The non-flat 08:00 oracle now pins packed light RGB bytes `(231,83,187)`:
normal bytes `(217,127,217)` and `(127,217,217)` produce alpha `0.8987774` and
`0.0794002`, respectively.

The four `.trn` lock pairs control only those neighbor taps. The parser
`Terrain_ParseConfigCallback @ 0x60f330` recognizes:

- `lock_topleft`: compare `0x60faf7`, stores `0x31be250 @ 0x60fb0c`
  and `0x31be254 @ 0x60fb1f`, returns at `0x60fb27`;
- `lock_topright`: compare `0x60fb31`, stores `0x31be258 @ 0x60fb46`
  and `0x31be25c @ 0x60fb59`, returns at `0x60fb61`;
- `lock_bottomleft`: compare `0x60fb6b`, stores `0x31be260 @ 0x60fb80`
  and `0x31be264 @ 0x60fb93`, returns at `0x60fb9b`;
- `lock_bottomright`: compare `0x60fba5`, stores `0x31be268 @ 0x60fbba`
  and `0x31be26c @ 0x60fbc8`.

Each component is `atol(argv[1/2])`. Defaults are zero because
`Terrain_LoadEnvironmentConfig @ 0x610940` clears the containing config
block at `0x61094c..0x610959`; the runtime pushes the parser callback at
`0x6109ad`, and `Environment_LoadTimeOfDayConfig` rewrites the extension
to literal `trn` at `0x57dbb6`; shipped `DVD4.TRN` contains all four
pairs. For each quadrant/axis, a nonzero component selects mask 511 plus that
quadrant's base, while zero selects mask 1023/base zero and crosses the internal
seam (`0x60327a..0x6032ec`). These values are not `polytrn_wrapx/y`.
`TrnConfig`, the TPJ build flow, and `TerrainData` preserve all four pairs;
at runtime `height_field_apply_trn` stamps them onto every `TerrainHeightField`
(the `TerrainData` samplers/raycast and `Simulation`'s grounding field),
and `build_heightfield_normal_map` plus the `terrain.cpp` render mesh and
seam normals tap the same `CoordsTaps` kernel. The former Godot-physics
heightfield consumer was removed (#330; range-find rides the ported raycast).

The admission gate (ported 2026-09-14): the tail of
`Terrain_LoadEnvironmentConfig @ 0x610940` returns 0 (terrain config rejected)
when `polytrn_colormap` (+256), `polytrn_detailmap` (+512) or
`polytrn_polydata` (+3072) is empty, when the `polytrn_sectors` row count
(+5960) or `polytrn_sectorcount` (+5956) exceeds 16, or when either is not a
power of two (`((n - 1) & n) != 0`; n = 0 passes), and when
`shift_terrain_heightmap_rows @ 0x60f190` fails. `load_trn`
(`engine/formats/trn/trn_io.cpp`) now returns false with the reason for the
text legs; the heightmap-rows leg runs over the loaded .cpt, not the text, and
is NOT ported. ctest `trn_admission_gate` (replacing `trn_optional_polydata`)
pins every leg. The former "empty polydata = editor project mode" early-out in
`TerrainData::_load_from_trn_text` rode a `plan/` note no tracked decision ever
carried (ONED is run-only, ADR 0037), so the polydata leg is ported and that
early-out removed. Foliage `match` lines carry up to four consumed codes per
definition (the parser stores seven), see
[foliage/foliage-re.md](../foliage/foliage-re.md) "Definition match codes"
(`Foliage_RemapPixelToDefMask @ 0x5FF4E0`, `Terrain_ParseConfigCallback
@ 0x60F330`).

`PolyTrn_InitTextures` splits the 1024 atlas into four 512 textures
`TrnNMap0..3` (creator calls from `0x60b3fa`; split loop from
`0x60b3eb`) with flags `0x100001`: CLAMP U/V/W, linear min/mag, and no
mip filter. The reimpl keeps one shared atlas and reproduces four independent
CLAMP samplers by restricting each selected quadrant to its texel-center
bounds.

`Environment_GetLightDirectionFloat @ 0x57d870` returns the direct tuple
`g=(-fixed[1], fixed[2], fixed[0]) / 65536` (stores
`0x57d8be..0x57d8cd`). `PolyTrn_RenderTile` does not normalize it; the D3DCOLOR
packer at `0x60e201..0x60e331` permutes it to GPU RGB `(g2,g0,g1)` while
applying `trunc((component+1) × 127.5)`. The additive
`PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e` preserves RGB and writes
`t0.a = saturate(4 × dot(normalByte−0.5, lightByte−0.5))`. The generated authored-detail
B-channel coefficient is a separate texture (stage 7, ps.1.1 tiers — see the
top-tier closure) and does not supply tile alpha; the ps.1.4 `t3` is the
authored second detail pair.

**Terrain pixel shaders** (`compile_terrain_pixel_shaders @ 0x605260`, gate
caps bit 8 = ps1.1; handles at `PolyTrn_PS*`): all share the lighting shape
`r0 = ((t0.a·c1 + c0)/2) ×2 t0 [× detail term]` —
`PolyTrn_PSBasic` (×4 t1 detail), `PolyTrn_PSNormalMap` (×4 `dp3(t1
normalmap, t2 light-direction texture)`), `PolyTrn_PSDualNormalMap` (two
dp3 bump terms ×2/×4), `PolyTrn_PS14Splat` (ps.1.4: three detail textures
sampled at t1 by samplers 1/4/5, blended by the t2 blendmap's RGB — the
3-way splat — then the colormap lighting chain ×4),
`PolyTrn_PS14SplatNormalMap` (splat + dp3), `PolyTrn_PSShadowBasic` /
`PolyTrn_PSShadowNormalMap` (the legacy name is misleading: underwater
variants for the PARTIALLY-authored stage sets — light scale
`saturate(4·t3²)·t0.a` with t3 = `Water_NoiseColorTexture`; on the full
splat tier the underwater noise rides the PS14 dp3 slot instead — full
sources + selector decode 2026-08-13, below),
`PolyTrn_PSDepthAlpha` (alpha = t0.b via `dp3 c5=(0,0,1)`, rgb = 0 — the
depth/alpha extract pass). The c0/c1 lighting constants are CLOSED (REN-5):
**c0 = the SKY block, c1 = the LIGHT block** (both `[0]` render colors ÷255)
pushed per draw `[orig: terrain_setup_lighting_and_shader @ 0x604420 —
SetPixelShaderConstantF(0, PolyTrn_PSConstC0_Sky @ 0x31a183c) / (1,
PolyTrn_PSConstC1_Light @ 0x31a182c); values init_terrain_lighting_color_ramps
@ 0x604ee0 ← Render_TerrainScene @ 0x610c80 (Env_LightBlock/Env_SkyBlock;
NVG blends the sky arg toward the modulator, the vehicle scope forces
0x101010/0xF0F0F0)]` — so the shared shape is **terrain light =
tileAlpha × light + sky** (tile alpha is the byte-quantized heightfield/light
DOT3 term; the ÷2 and MODULATE2X cancel). The foliage/sector-model blend PS inherits the same
device constants. Ported: `terrain_lighting.gdshaderinc` (the prior
combined/fill pairing was a gobj-era stand-in) +
`renderer::terrain_surface_light` (T1 section 5). Full chain:
[render/render-lighting-re.md](../render/render-lighting-re.md).
**Top-tier binding/math closure (fresh re-grill, 2026-07-13; stage-3
corrected 2026-07-15)**: the recovered `t0..t5` contract is now literal in
the shared reimpl include. The stage binder
[`orig: PolyTrn_BindStageTextures @ 0x604330, walk @ 0x604392..0x604415`
(renamed 2026-07-15 from kong's `CD3DDevice_FindBestTexturePermutation` —
PolyTrn code that binds stages, not a device search)] resolves: stage 0 = the cached
per-patch tile render target (not source colormap RGBA; on bare ground its
RGB is the MODULATE2X colormap result and its alpha is the heightfield/light
DOT3 result); stages 1/4/5 = the three authored splat layers sampled with
the shared repeating detail UV; stage 2 = the normalized DBlend quadrant
(selected by two key bits) at the terrain-map UV; **stage 3 = the authored
SECOND detail (`polytrn_detailmap2`, paired with `polytrn_detailmapdist2`)
at its own `polytrn_detaildensity2` coordinate** — the texture-matrix pair
in the sector batch scales the mesh detail UV by `1/density` for stage 2 and
`density2/density` for stage 3 [`orig: render_terrain_sector_batch
@ 0x60976f..0x609810 (dword_31A1810/dword_31A1814, parsed by
Terrain_ParseConfigCallback @ 0x60f993/0x60f9bf)`]. The earlier reading of
t3 as the generated detailmap-B coefficient map is RETRACTED for this tier:
that generated map binds at stage 7 for the ps.1.1 tiers, and on the ps.1.4
splat it produced blend-weighted darkening spots retail does not render.
When the camera is below water, the LIVE stage-3 slot swaps to the water
noise texture with an `8/density` transform (the underwater section below;
the pre-2026-08-13 "projected-shadow texture" reading of `0x6043f2` was
stale) [`orig: 0x6043f2; 0x6097ca`].

**Detail-coordinate scale closure (2026-08-17; D-TERRAIN-11).** Retail does
not use a fixed `0.125` detail scale. `configData` begins at `0x31BCB38`, so
the `polytrn_detaildensity` parser store at `0x31BE270` is exactly field
`+0x1738` [`orig: Terrain_ParseConfigCallback @ 0x60f993..0x60f9b3`].
`PolyTrn_LoadTerrainConfig` passes that field to the four-instruction scale
writer [`orig: PolyTrn_SetDetailDensityScale @ 0x6029a0..0x6029aa` — renamed
2026-08-18 from the misnomer `GMaterial_SetAllChannelDefaults`; call site
`0x60e634..0x60e63b`], which converts the integer to float, multiplies it by
`0.001953125` (`1/512`, constant `0x7D83A4`), and stores the result in
`flt_8493C0`. The vertex decoder uses
that live value for UV1 [`orig: 0x602db5..0x602dbe`], therefore:

`UV1 = source_position × polytrn_detaildensity / 512`.

For 00TRa's `polytrn_detaildensity 128`, retail repeats at
`source_position × 0.25`; the old reimplementation used normalized full-atlas
UV (`source_position / 1024`) times 128, only `source_position × 0.125`.
Runtime uses `atlas_uv × density × 2`; the same source-grid
conversion makes authored stage 3 `atlas_uv × density2 × 2` and the
underwater `8/density` swap `atlas_uv × 16`. Deterministic A/B probes
adjudicated the axis as well as the scale: 2× with the existing axis reached
00TRa high-pass texture correlations `0.795..0.910` (fire-barrel) and
`0.780..0.861` (courtyard), while the swapped-axis variants remained near
zero. The two ROIs were C1-only and the independently reconstructed paired
mip chain matched the live upload byte-for-byte, ruling out source selection,
DBlend, and mip construction as the cause.

The detail splat is `t1×t2.r + t4×t2.g + t5×t2.b`; the `mul_x2` modulation
is `2×dot(t3.rgb, t2.rgb)` — with the normalized blend this is a
blend-weighted scalar of the second detail — and it exists ONLY in
`PS14SplatNormalMap`, selected when `detailmap2` is present; maps without a
second detail run `PS14Splat`, which has no such stage (factor exactly 1)
[`orig: terrain_setup_lighting_and_shader @ 0x604544/0x6044e8`]. Lighting
starts from `(t0.a×c1 + c0)/2`, then follows the witnessed ×2 colormap,
×2 dot-product, and ×4 splat stages
[`orig: PolyTrn_PS14SplatNormalMap source aPs14TexldR0T0T_0 @ 0x7dece0, assembled with the full
eight-variant family @ 0x605260 — PSBasic/PSNormalMap/PSDualNormalMap/
PS14Splat/PS14SplatNormalMap/PSShadowBasic/PSShadowNormalMap/PSDepthAlpha`].
There is no camera-distance normal mix and no extra terrain tint, and there
is **no distance-based tier downgrade**: every sector at every distance
draws through `terrain_setup_lighting_and_shader(0)`; the quality-1/2
passes (`0x319F934`, and the ONE/ONE additive `0x319F930`) belong to the
local-light multi-pass path, not distance
[`orig: render_terrain_sector_batch @ 0x6092a0`].

The reimpl composes tile-overlay RGB into the reconstructed t0 base **before**
that lighting chain while retaining t0 alpha as the heightfield/light DOT3
term. Fog now follows the
retail mode split: exponential type 0 uses eye-space Z/depth, while linear
types 1/2/3 use radial camera distance [`orig: Render_SetFogState @ 0x58a950`
→ `CD3DDevice_SetFogParameters @ 0x677960`]. Final mesh selection is the
eight-family mapping `floor(lod_sub × 8 / 16) = lod_sub / 2`, clamped to
families 0..7 [`orig: render_terrain_sector_batch @ 0x6092a0`].

**Include correction (2026-07-06, D-TERRAIN-2)**: `terrain_lighting.gdshaderinc`
had stacked TWO ×2 detail-normal factors on the splat (the gobj-era "v23
dual-normal" chimera; under the gamma-faithful pipeline it clipped regions
to white). The recovered top-tier path applies exactly one coefficient factor;
the splat-only variant omits it [`orig: PolyTrn_PS14SplatNormalMap @
0x7dece0`; `PolyTrn_PS14Splat source aPs14TexldR0T0T @ 0x7dee18`]. The fresh re-grill establishes the paired
base/far mip chain as the only near/far transition; its initial reading of t3
as the authored-detail B-channel coefficient was RETRACTED 2026-07-15 — t3 is
the authored second detail pair (see the top-tier closure).

**Surface materials** (same function): the `"depthspin"` shore material
(`dword_319f904`, alpha-tested: stage 0 alpha `ADDSIGNED(COMPLEMENT
texture, DIFFUSE)`, stage 1 alpha `ADD(TFACTOR, CURRENT)` — the water-edge
cutout); DOT3 additive lightmap materials (`dword_319f8f8/8fc` — stage 0
`DOTPRODUCT3` both channels, ONE/ONE additive or opaque single-stage);
per-quadrant colormap materials (`dword_319f940..94c`, tier path 2-stage:
s0 `MODULATE2X(tex, DIFFUSE)` → s1 `MODULATE2X(CURRENT,
CURRENT|ALPHAREPLICATE)`; non-tier single-stage `MODULATE4X(tex,
DIFFUSE)`); the complement-noise material (`dword_319f8f4`: two stages of
`MODULATE2X(COMPLEMENT texture, DIFFUSE/CURRENT)`); the main detail
materials (`dword_319f938/930`, tier-2 3-stage with the
`CURRENT × CURRENT.aaa × 2` self-modulation tail and per-slot
TEXCOORDINDEX/texture-matrix selectors (resource fields `+0x4C/+0x52`,
consumed by `sub_681720`; these are not mip-LOD-bias writes), opaque +
additive variants; the four texture slot ids 1-4
= the terrain's dynamic texture registry) and the framebuffer-mod2x overlay
(`dword_319f934`, mode 0x628 — DESTCOLOR/SRCCOLOR). Draw-time consumers:
`render_terrain_sector_batch @ 0x6092a0`, `Foliage_RenderFarPatches
@ 0x609de0` (renamed 2026-08-15, ex render_terrain_lightmaps; REN-5), `Terrain_CollectAndRenderTileModels @ 0x60d250`,
`PolyTrn_RenderTile @ 0x60da70`.

**Underwater selector correction (2026-07-29).** `dword_319FB3C` is not a
scene-shadow enable or a loaded shadow-map handle. `terrain_setup_view_and_lighting
@ 0x60FE40` writes view field `+0x74 = cameraY < Env_WaterHeightFixed`
(`@ 0x60FEE0`); `terrain_render_visible_sectors @ 0x6090C0` copies that field
to `dword_319FB3C` (`@ 0x60915F`; the alternate scene path does the same at
`@ 0x60E8EE`). When set, `PolyTrn_BindStageTextures @ 0x604330` calls
`sub_5C0190 @ 0x6043ED`, whose return is
`Water_NoiseColorTexture @ 0x28EE8B8`, and binds it at stage 3. The same flag
selects `PolyTrn_PSShadowBasic` / `PolyTrn_PSShadowNormalMap`
(`terrain_setup_lighting_and_shader @ 0x6044B1`) and changes stage-3 UV scale
(`render_terrain_sector_batch @ 0x609786..0x6097D6`). These are therefore
underwater animated water-noise/wave-shadow variants, not receivers for static
or dynamic model shadows.

**PSShadow\* sources + the full underwater selector decode (2026-08-13;
strcpy sites @ 0x605315 / @ 0x605329 inside `compile_terrain_pixel_shaders
@ 0x605260`; selector disasm @ 0x6044b1..0x604556).** The earlier "the flag
selects PSShadowBasic/PSShadowNormalMap" one-liner was an oversimplification.
The selector keys on the AUTHORED stage handles (`dword_319F998` detail2 /
`dword_319F99C`+`dword_319F9A0` splat layers 2-3), while the BINDER swaps only
the LIVE stage-3 slot (`dword_3266E98` ← `sub_5C0190()` =
`Water_NoiseColorTexture` when `dword_319FB3C` is set @ 0x6043f2, else the
authored `dword_319F998` @ 0x6043ff). Full table (blendmap present and
shader tier ≥ 1; otherwise PSBasic above / PSShadowBasic below):

| authored stages | above water | below water |
|---|---|---|
| splat 2+3 + detail2 | PS14SplatNormalMap | PS14SplatNormalMap — **t3 = the noise** |
| splat 2+3, no detail2 | PS14Splat | PS14Splat — **no t3 consumer, NO underwater modulation** |
| partial splat + detail2 | PSDualNormalMap | PSDualNormalMap — its t3 dp3 reads the noise |
| partial splat, no detail2 | PSNormalMap | **PSShadowNormalMap** |
| no blendmap / tier < 1 | PSBasic | **PSShadowBasic** |

So on the TOP-TIER (ps.1.4 splat) path the underwater effect is the stage-3
dp3 modulation reading the live noise at the swapped `8/density` texcoord
(`source × 8/512`, or normalized 1024-atlas `colormap_uv × 16`) —
`2·dot(noise, blend)`,
neutral at mid-gray noise — and detail2-less splat maps faithfully render
NO underwater terrain modulation. The PSShadow pair serves only the
partially-authored stage sets. Their transcribed ps.1.1 sources:
`PolyTrn_PSShadowBasic` = `mul_x4 r1.rgb, t3, t3` (noise², ×4, **saturated
by the ps.1.1 register clamp** — the faithful scale is `min(4·t3², 1)`,
mid-gray neutral), `mul r1.rgb, r1, t0.a`, `mad_d2 r0.rgb, r1, c1, c0`,
`mul_x2 r0.rgb, r0, t1`, `mul_x4 r0.rgb, r0, t0`, `+mov r0.a, t0.a`;
`PolyTrn_PSShadowNormalMap` inserts `dp3 r1.rgb, t1, t2` / `mul_x2 r0.rgb,
r0, r1` before the t0 multiply. (Their tail multiplies t1 ×2 then t0 ×4 —
the reverse of PSBasic; same product, different intermediate clamp points.)

`dword_319FB8C` is also not shadow state. Its complete writer set stores one:
`sub_6040A0 @ 0x6040DD`, `terrain_render_visible_sectors @ 0x60910C`,
`terrain_render_scene @ 0x60E89B`, and `PolyTrn_RenderFrame @ 0x60EB25`
(EAX is seeded to one at `0x60EAC0`). `render_terrain_sector_batch` itself
calls `sub_6040A0 @ 0x6092BE` before reading the value at `0x6095D7`, making
its zero leg dead on the live path. The true leg admits terrain drawing and
the nearby point/spot-light collection; the other observed consumer is the
foliage/lightmap path at `0x60A3F8`. The most specific supported description
is an always-on terrain-render/lighting latch, not a user shadow toggle.

**Static sector/model sun shadows (witnessed 2026-07-29).** These live in the
tile composer, completely separate from the underwater shaders above. With
the model-shadow graphics gate `dword_8493DC` enabled,
`PolyTrn_RenderTile @ 0x60DA70` calls
`Terrain_CollectAndRenderTileModels @ 0x60D250` (`@ 0x60DC83..0x60DC96`).
The collector:

- gets the fixed and float sun directions from
  `Environment_GetLightDirectionFixed @ 0x57D8E0` and
  `Environment_GetLightDirectionFloat @ 0x57D870`
  (`@ 0x60D2F5/0x60D2FF`), clamps the vertical fixed component to at least
  `0x4000`, and derives the horizontal projection extent
  (`@ 0x60D315..0x60D386`);
- scans pool 2 and pool 1. Both reject BMS/runtime `NoShadow`
  (`entity flags & 0x01000000 @ 0x60D42F`) and ItemDef `NoShadow`
  (`ItemDef+0x54 & 0x04000000 @ 0x60D43E`). Pool-2 sector buildings otherwise
  cast by default; only pool-1 candidates require ItemDef attrib2
  `StaticShadow` (`ItemDef+0x58 & 0x20 @ 0x60D447..0x60D450`);
- expands each model bound along the sun projection and intersects it with the
  requested terrain-tile extent (`@ 0x60D465..0x60D54F`);
- chooses the current model/husk render data, using its second render LOD for
  coarse tile levels `<= 3` when available (`@ 0x60D881..0x60D894`), then
  writes the same nonzero entity transform into every ROBJ matrix slot
  (`renderObjectCount + 1` copies at `0x60D926..0x60D95E`) and submits
  `Render_SubmitEntity @ 0x5DAD80` with flag `0x2`, the PROJSHAD technique
  (`@ 0x60D960..0x60D971`).

The complete shipped PROJSHAD source audit fixes the per-material draw law.
`_FFP.fx::TBoringFFPProjShad` is the sole material-blend declaration: its
`BLEND_NONE/ALPHA/ADD/MULT` variants select ONE/ZERO,
SRCALPHA/INVSRCALPHA, ONE/ONE, and DESTCOLOR/SRCCOLOR respectively. The other
15 declarations all force blending off with ONE/ZERO. Tracer, Flag, Glass,
and SkGlass expose no PROJSHAD declaration and therefore cast no fallback
silhouette. All live declarations retain normal z mode, so LESSEQUAL and z
writes order overlapping surfaces even when an `_FFP` additive draw changes
no color. Ordinary material culling also remains live: material flag bit 2
selects CULLMODE NONE, otherwise the pass uses CULLMODE CCW
[`orig: CRenderBatchQueue_FlushBatches @ 0x5DA3A9..0x5DA401`].

Skinned selected LODs are exact in this pass rather than unsupported. The
collector copies the same entity transform into all `renderObjectCount + 1`
matrix slots (`@ 0x60D926..0x60D95E`); `_vsSkPost.fx::vsSkinPostBlackT1`
calls `_BaseInc.fx::CalcSkinWorldPosAndNormal`, whose explicit weights plus
computed final weight sum to one. Applying identical matrices therefore
collapses exactly to the rigid source vertex, with no PANM pose input.

That direct render-data submission includes every opaque and alpha strip in
every ROBJ of the selected LOD. It does not consume
`g_BuildingSectionVisMask` or `g_HiddenSectionMask @ 0xB7965C`; those masks
belong to the live portal-building draw in
`Terrain_RenderSectorModels @ 0x5C5D30`. A loose-data audit of `IHQ01.3di`
corroborates the call path: all five LODs contain three ROBJ; the two LODs used
by this pass carry opaque strip counts 5/3/3 and 3/3/3, with no alpha strips.
Ihq01 is pool-2 building index 40 and has neither BMS nor ItemDef `NoShadow`,
so all three of its ROBJ enter the static caster pass even though its ItemDef
does not author `StaticShadow`.

The result is receiver-scoped rather than ordinary model self-shadowing.
`Terrain_CollectAndRenderTileModels` selects the dedicated temporary render
target `dword_319A2D8` at `0x60D5BE` (cleared to depth 0.99995), rasterizes
black PROJSHAD silhouettes, and restores it at `0x60DA4F`. The projection is
the view/ortho pair `setup_shadow_cascade_matrices_0 @ 0x58D4B0` builds from
the negated, vertical-clamped float direction (`@ 0x60D7FC..0x60D800`,
ortho half-size `tileSize/2`): the direction is divided by its vertical
(`@ 0x58D4F1..0x58D50E`) and the view rows `[0,−1,0] / [dz,−dx,1] /
[1,0,0]` (`@ 0x58D523..0x58D583`) slide every vertex along the light until it
meets the horizontal plane through `Terrain_GetHeightAtPosition(entity x, y)`
(the caster's floor-cell ground height, subtracted from the entity's vertical
`@ 0x60D8FA`); the ortho depth row is `z = h·0.0005 − 0.00005` with `w = 1`
(`P[10] @ 0x58D5F8`, `P[14] @ 0x58D602`, `P[15] @ 0x58D60C`), so the D3D clip
volume's near face removes every vertex lower than 0.1 u above that plane —
buried skirts, foundations and half-sunk decorations cast nothing, and a
triangle straddling the plane casts only its part above it. (The 00TRa
sandbag walls are the visible case: `SBag02.3di` carries a 2.5 u skirt below
its Ground anchor — 88 of its 787 LOD0 vertices lie under the anchor — which
must never reach the page.) Whether the PROJSHAD pass touches
`D3DRS_CLIPPING` is not witnessed (the technique state lives in the packed
.fx); the −0.00005 bias exists to put h = 0 outside the volume, so the default
clip is taken as the law. `PolyTrn_RenderTile` then selects the destination
tile-cache RT at `0x60DCC5`, binds that temporary texture at
`0x60E10A..0x60E112`, and composites it only when the collector returned a
candidate (`0x60E0C6..0x60E19D`). The final state is now closed rather than
inferred from the packed mode id. A targeted D3D9 vtable probe on the pinned
retail executable (`b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`)
matched the live pixel-shader handle to `PolyTrn_PSDepthAlpha` at the shared
quad draw return `0x679005` and read
`COLORWRITEENABLE=0xF`, alpha blend enabled, `SRCBLEND=ONE`,
`DESTBLEND=ONE`, separate-alpha blend disabled, and Z/Z-write disabled. The
last source/destination writes came from `GfxBlend_ApplyToDevice @
0x6818E5/0x6818FB`; therefore the creator's nominal mode-6 decode must not be
used as the draw-time state. `PSDepthAlpha` emits `(0,0,0,tempBlue)`, so the
RGBA ONE/ONE draw is output-equivalent to an A-only copy because every prior
destination-tile pass used RGB-only writes and left A=0. Eight live 256x256
readbacks proved it: RGB sums were identical before/after every target draw;
on the first page they remained `4479113/4532023/3283970` while A changed
from sum 0 to `2278029`. The portable
`composite_terrain_static_shadow_pixel` pins the general saturating equation,
and the collapsed two-RT handoff supplies the witnessed zero destination A.
The final cache is sampled as t0 by terrain
and as t1 by `Foliage_LightmapBlendPS`; therefore terrain and foliage receive
the projected/draped shadow, while an Ihq01 courtyard/interior floor mesh does
not. Dynamic person/`DynamicShadow` render slots are a separate system
(`Entity_InitFromModel @ 0x40E1BC..0x40E1F7`).

**Bounded runtime gaps after the 2026-08-17 page-composer slice**:

- **Tile-composition RT/update gap** — runtime now hosts a 128-layer,
  256×256 single-level RGBA page cache for quadtree levels 1–4. Each ready
  page composes the quadrant-clamped colormap over A=0, ordered mission `.til`
  quads with retail source-over RGB/**and render-target alpha**, the additive
  heightfield/DOT3 alpha pass, and supported static model silhouettes as
  A-only projections. Terrain and detail foliage borrow the same current-frame
  page binding. Base DOT3 and static-projection content identity share the same
  truncated `(g2,g0,g1)` light-byte epoch: sub-byte TOD movement retains ready
  pages, while crossing an epoch boundary recomposes with the then-current raw
  projection vector. Static caster admission, selected-LOD/all-ROBJ submission,
  destruction/husk/dynamic transforms, all diffuse-alpha flipbooks, and
  page-local unsupported attribution are explicit typed inputs rather than a
  global directional-light surrogate. The mission present pass republishes an
  admitted individual model's exact applied transform into that same source
  registry; exact-value gates keep repeated rows and repeated static-husk
  publications revision-stable, while a real pose, graphic, geometry identity,
  or caster-active change advances the snapshot once. Inactive/rejected sources
  may retain their latest pose without invalidating resident pages.

  Geometry resolution separately counts each selected ROBJ's authored and
  decoded surfaces. An authored zero-surface hierarchy ROBJ remains an exact
  no-op, but partial surface loss, malformed indices, or UV loss required by an
  alpha-dependent material is explicitly unsupported; if malformed authored
  ranges cannot supply conservative bounds, planning fails closed globally.
  Mandatory colormap and height-normal sources also require both dimensions to
  be at least two before quadrant sampling. Overlay source readiness fails
  closed: with overlays enabled, an unresolved TRN-declared tile-info source or
  a nonempty resolved/override tile table sets `tile_overlay_required`; without
  a convertible tilestrip the device does not build a base-only cache. A
  mission with no authored entries remains a valid base-only case. The
  max-quality c7/c8 projection is exact: the packed cache record and required
  `Foliage_WindSwayVS` branch reduce to the shared presentation-world
  `TerrainTilePageProjection`, used by every page consumer and the shadow
  raster with no failed-VS fallback API. The projected-shadow material path
  now calls the same signed CTRL-aware AlphaGen/full-matrix UV evaluator and
  diffuse-frame selector as ordinary object rendering. It samples the
  frame-shared millisecond tick, retains authored model-local CTRL names, and
  resolves every diffuse alpha frame; selected missing frames still fail
  closed. A mounted-game census covered all 2,501 `.3di` files (2,497 parsed;
  the four known malformed/temp files were reported), including the 34
  alpha-coverage rows with dynamic UV or non-team/team flipbooks. All four
  dynamic-AlphaGen rows were additive and untested, so their alpha is correctly
  unobservable in PROJSHAD. Retail's submission stamp and four-register batch
  snapshot/restore are cited at `Render_SubmitEntity @ 0x5DAD9D`,
  `collect_render_objects_for_batch @ 0x5D91AB..0x5D91DE`, and
  `@ 0x5DA1B8..0x5DA1FD`; `apply_shader_parameters @ 0x58DB80` is the shared
  consumer. Whole-process inherited CTRL/RNG ordering remains the renderer-wide
  D-3DI-2 concern, not a guessed shadow-only evaluator. The remaining gaps are
  scorch damage updates, remaining ordered tile-model contributions, retail
  dirty cadence, and final RT edge/address/mip behavior. The final temporary-blue
  composite equation/state and per-technique pass admission,
  forced-opaque versus `_FFP` material blending, skinned rigid collapse,
  one-sided culling, and non-opaque inter-caster z ordering are closed. The analytic cold
  fallback remains a lower-fidelity global overlay.

  **Creation-time clear — WITNESSED 2026-09-14 (jo-c cross-check), PORTED.**
  `sub_604DD0 @ 0x604DD0` creates the 128 tile render targets (stride 8 dwords,
  `dword_319A2E0..0x319B2E0`, dimension `dword_31A00D4`) and clears each to
  D3DCOLOR `0xFFFF6060` (ARGB: R=FF G=60 B=60) with z `0.99994999`
  (`GTexRT_SelectThunk(0x12345678, rt, -40864, 0.99994999)`), stamping the
  slot key/UV sentinels `0x12345678` and indices −1; it then creates one extra
  square target `dword_319A2D8` of dimension `dword_31A00D0` (the model-shadow
  target; jo-c reads 512 at `0x60E426/0x60E430`, unverified here and not
  ported). Ported as `TerrainTileCompositionCache::kTileClearColorArgb` and the
  blank-layer fill in `godot/src/terrain/terrain_tile_cache_device.cpp`.

  **Retail refresh cadence — WITNESSED 2026-08-18, corrected 2026-08-22.**
  Retail's 128-slot hit compare keys ONLY on `(lod, tileCoord, tileRow,
  quadrant)` (`@ 0x60DAD1..0x60DAD7`); no caster, light-epoch, or TOD input
  participates in the HIT. Slots stamp `Env_TodMinutesElapsed` at compose
  time (`@ 0x60DBC0`, `dword_319A2F8`), and that stamp is READ by the evictor:
  `terrain_cache_evict_lru @ 0x604600` walks the 128 slots four at a time and
  picks, among the slots whose stamp differs from the current
  `Env_TodMinutesElapsed` (`@ 0x60463B/0x60465E/0x604681/0x6046A3` — indexed
  reads through the slot base, which is why a plain xref of `dword_319A2F8`
  shows only the write), the OLDEST-composed one (compose frame
  `dword_319A2F4` vs the frame counter, min age 1) and invalidates it (`lod =
  coord = -1`, last-use = frame − 0x10000 `@ 0x6046D2..0x6046EA`), one per
  call. `PolyTrn_RenderFrame @ 0x60EAC0` calls it only on a frame where NO
  tile was composed (`@ 0x60F0AB..0x60F0AD`) and then re-sweeps every visible
  patch (`@ 0x60F0CF`), so the evicted tile recomposes with the current sun
  that same frame. `Env_TodMinutesElapsed` steps every 311 logic ticks (~5 s)
  while the clock advances (`Environment_UpdateWeatherTick @ 0x57E9DA..
  0x57E9EF`). Net: retail re-bakes stale visible tiles one per all-hit frame
  after every ~5 s TOD epoch, oldest first — static shadows track the sun with
  a latency of roughly 5 s plus a frame per visible tile (the 2026-08-18
  "write-only stamp, stale until LRU turnover" reading missed the indexed
  reads). The reimpl's page identity is a content stamp (per-intersecting-
  caster transform/team/ground revision, the quantized light epoch, and the
  enable/suppression config), so exactly the affected pages recompose on a
  real change, and during the asynchronous recompose the last-published page
  keeps serving (stale-while-recompose) while explicit control changes
  (attach/enable/suppression/terrain swap) still drop payloads outright. Its
  light epoch is the DOT3 byte triple (one step ≈ 2–3 mission minutes near
  mid-afternoon), coarser than retail's ~5 s TOD epoch but re-baking every
  affected page at once rather than one tile per frame; neither window is
  visibly wider at ordinary clock rates.

  Fixture-output publication (not comparison registration) fails closed unless
  the post-freeze refresh has available cache/shadow devices, every required
  `.til` source, zero recorded upload/raster failures, one output page per
  compose job, all requests accounted for by current jobs or ready hits,
  selected current-frame pages, and no capacity, plan, or RGB-integrity
  failure. Static-enabled variants additionally require an exact nonempty
  resolved-caster snapshot with actual projected pages/draws and no unsupported
  or truncated attribution; static-disabled variants require no plan, raster,
  or alpha change. Registration and hash-bound retail pairing remain a separate
  evidence-builder step
  [`orig: Terrain_CollectAndRenderTileModels @ 0x60d250`;
  `PolyTrn_RenderTile @ 0x60da70`].
- **Underwater water-noise modulation — CLOSED 2026-08-13** (D-TERRAIN-8
  FIXED; see the underwater selector section above): the reimpl feeds the
  water module's noise texture into the top-tier ps.1.4 dp3 when the camera
  is below water; the misleadingly named `PolyTrn_PSShadowBasic` /
  `PolyTrn_PSShadowNormalMap` (`4×t3²×t0.a`) serve only the partially
  authored ps.1.1 tiers. Unrelated to the static tile shadow above; the
  wider record is
  [render/render-lighting-re.md](../render/render-lighting-re.md).

**Foliage / sector models** (the four terrain-attached model slots):
`Foliage_LoadDefAssets @ 0x601260` loads the models and replaces each source
alpha with the wrapped 9-tap kernel `(4C + cardinals + 2×diagonals) >> 4`.
`GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270` then keeps authored
RGB at mip 0 and blends each recursively box-downsampled later mip toward
`0x808080` by `min(256,floor(320×mip/N))`, preserving the downsampled smoothed
alpha. The reimpl now supplies that complete packed custom chain directly to
Godot without generic mip regeneration, fixing D-FOLIAGE-5. The loader also
creates TWO materials per
model — pass flags 0x2560000 (fog + alpha-test + z-write-OFF + cull-none +
stage-1 clamp) and 0x2460000 (same, z-write ON) — whose stage tables are
3-stage on the shader path (s0 `MODULATE(tex, DIFFUSE)` → TEMP; s1
`MODULATE4X(tex, TEMP)`; s2 `MODULATE2X(CURRENT, CURRENT|ALPHAREPLICATE)`)
or 2-stage `MODULATE2X` chains without it. `Terrain_CreateFoliageVertexShaders
@ 0x5ff630` assembles the WIND-SWAY vs_1_1 (`Foliage_WindSwayVS @ 0x2c25e5c`
— a polynomial sine of `world.x · c24.y + time`, weighted by vertex RED,
displacing Z; constant diffuse c6; lightmap UV = planar world projection
via c7/c8) and the per-patch GRID-PLACEMENT vs_1_1 (`Foliage_GridPlacementVS
@ 0x2c25e60` — a0-indexed per-patch constants, bilinear + quadratic height);
`Foliage_CreateLightmapBlendPS @ 0x5ff7a0` (`Foliage_LightmapBlendPS
@ 0x2c25e64`) is the fragment combine `rgb = t0 × (t1 × (t1.a·c1 + c0)) ×
v0 × 8, a = t0.a × v0.a` (t1 = the planar-projected lightmap).
`Foliage_SetupFarSlotDraw @ 0x6007c0` (ex-misnomer
`terrain_setup_display_adapter`; per model slot 0-3) picks the LOD entry,
binds the wind VS + FVF 338, fog mode 8 (VS fog) when the wind VS exists,
and **alpha-test ref 180 (high quality) / 8 (low)**. The c6.a ×0.1 scale in caller
`Foliage_RenderFarPatches @ 0x60a4a8/0x60a16c` is gated on the
water-REFLECTION invocation (arg_8 = reflectionEnabled) and never applies to
the main scene's near secondary LOW, which shares the unscaled fade
(corrected in the 2026-07-15 grill; foliage-re.md §Fade and pass state). The setup branch at `0x6008fc..0x600912` passes value 2 to wrapper
`0x67cac0..0x67caea`, which writes render state 0x17 (`D3DRS_ZFUNC`): this is
strict `D3DCMP_LESS`, not wireframe/fill mode; the normal value 4 is
`D3DCMP_LESSEQUAL`. The reimpl's shared detail include receives the exact runtime
alpha reference and fade rather than the former fixed-cutoff stand-in. The old
"0x005BF064 pixel shader" / "sub_5C1790" anchors in
that shader were BOGUS (a stale note: 0x5BF064 is inside
`draw_death_screen_overlay`; 0x5c1790 is not a function) — corrected at
REN-4.

**Embedded-shader census (REN-4)**: every `D3DXAssembleShader` caller in the
retail image is now witnessed — the sky dome pair
(`terrain_init_rendering_resources @ 0x5789e0`, env record), the water
surface set (`Water_InitSurfaceShaders @ 0x5c19b0`, env record §Water), the
particle water/distort trio (`create_water_shaders @ 0x5dfc30` →
`EffectWorld_Water*` — EffectWorld domain, consumed by
`CParticleTexture_InitTextureAndChannels` type 7), the foliage pair + blend
PS (above), the eight terrain pixel shaders (above), the view-effect family
(`init_view_effect_shaders_and_textures @ 0x5cf8e0` — binocular/NVG
grayscale, 4-frame lrp accumulate, green tint, glow-squared; screen-space
overlay scope, not a REN port target), one in `Lighting_InitTextures
@ 0x5a94f0` (REN-5), and the FrameFX set (`CFrameFX_CreatePixelShaders
@ 0x5821d0` — out of REN scope).

## Runtime terrain queries (ENG-3 B0, retail Jointops.exe — witness map)

The engine's world-space terrain query family — the height samplers and the
segment raycast chain — witnessed 2026-07-07 for the ENG-3 B1 port (the
`engine/runtime/terrain_query` raycast; [ADR 0020](../adr/0020-world-terrain-query-seam.md)
§5 growth). All coordinates 16.16 fixed-point world units; the heightmap V axis
runs opposite world y (samplers negate y internally).

### Shared data substrate (renames applied this session)

| Global | Address | Role |
|---|---|---|
| `Terrain_HeightAtlasPtr` (ex `tileMask`) | `@ 0x31a00cc` | base of the 1024×1024 `u16` raw16 height atlas, row stride 1024; `sample << 8` = 16.16 height (raw16/256 units). 512×512 quadrant windows — the same atlas model as `terrain_query/coords.h` |
| `Terrain_SectorGrid` | `@ 0x319fc10` | 16×16 `int` cell grid, sector ids 0..4 (0 = empty). Quadrant offsets by `id-1`: bit 0 → +512 V, bit 1 → +512 U — exactly `coords_quadrant_offset_z` (ids 2,4) / `_x` (ids 3,4) |
| `Terrain_SectorOriginX` / `Y` | `@ 0x319b2e4` / `@ 0x319b2e0` | world sector origin, subtracted from `coord >> 25` to form the grid cell |
| `Terrain_CellOOBMaskX` / `Y` | `@ 0x31a0010` / `@ 0x319fc0c` | out-of-bounds detectors: `(cell & mask) != 0` → clamp to 0/15 via the sign trick (`~(cell >> 31)` low byte, then `& 0xF`). Written at terrain load `[orig: PolyTrn_LoadTerrainConfig @ 0x60e3d0, store @ 0x60e4c2]` |
| `Terrain_SeamFlags_X0Y0/X1Y0/X0Y1/X1Y1` | `@ 0x31a17f0/-f8/-1800/-08` | per-quadrant `{+X, +Y}` dword pairs: the bilinear `+1` neighbor policy — nonzero → wrap within the own 512 half (`& 0x1FF` + half base), zero → cross the seam / wrap the full 1024 (`& 0x3FF`). Reader-witnessed (sampler + `Terrain_GetHeightGradient @ 0x606330`); the writer is a follow-up |
| `Terrain_LastRayStepX/Y/Z` | `@ 0x319a298/-9c/-a0` | the per-sample step vector stored by the LoRes raycast on hit (y in the remapped −y axis); consumed by the HiRes_0 refine |

### Height samplers

- **`Terrain_SampleHeightBilinear @ 0x6067b0`** (ex `null_stub` — the IDB had
  it mistyped `void()` with a "confirmed correct" comment, so its ~40 callers
  all decompiled as no-op calls; retyped
  `int __cdecl(world_x_1616, world_y_1616)` this session). THE canonical
  ground-height function (entity/projectile physics, camera, weapon raycasts,
  foliage, HUD compass all call it). Behavior: `y → −y`; cell =
  `coord >> 25` − origin, OOB-clamped; empty cell → **0** (the height-0
  plane); texel = `floor(coord >> 16) & 0x1FF` + quadrant offset; `+1`
  neighbor per the seam flags; four `u16` taps `<< 8`; bilinear weights
  `(1−fx)(1−fy) …` with **each product individually rounded**
  (`+0x8000 >> 16`), then summed.
- **`Terrain_GetHeightAtPosition @ 0x606720`** — the point-sample variant:
  same substrate, single floor-texel tap `<< 8`, no interpolation.
- Same-substrate siblings (own records/scopes):
  `Terrain_GetHeightGradient @ 0x606330`,
  `Terrain_GetSurfaceTypeAtPosition @ 0x606510`,
  `Terrain_GetColorMapBilinear @ 0x606d80`,
  `sample_foliage_density_bilinear @ 0x606200`.

### Segment raycast chain

- **`Terrain_RaycastHeightmapLoRes @ 0x60cb80`** —
  `int __cdecl(start[3], end[3], hit[3]|NULL)`; **returns 0 = HIT, 1 =
  CLEAR**. Remaps `x += 0x8000`, `y → 0x8000 − y` (half-texel bias + V flip).
  - *Null atlas*: no terrain loaded (`Terrain_HeightAtlasPtr` null) → returns
    0 = HIT immediately, no hit write `[orig: @ 0x60ccf7]` — "blocked" is the
    no-data default.
  - *Column shortcut*: when `|dx| < 4096` AND `|dy| < 4096` (both under 1/16
    unit): ONE bilinear sample at the start x/y; HIT iff the segment
    **crosses** the surface — a fully-buried segment returns CLEAR
    (**witnessed asymmetry**: the march path hits at its first sample when
    starting below ground). The hit out = (start x, start y, terrain height)
    and the ZEROED step globals are written **even on the CLEAR outcomes**
    `[orig: @ 0x60cc12..0x60cc2d]`.
  - *March*: per-sample step = `delta · (2^32 / max(|dx|, |dy_r|)) >> 16`
    (rounded per component, `+0x8000`) — ~1.0 world unit along the major
    axis; the sample budget is the major-axis extent (`remaining 0x10000 −=
    floor(2^32/maxΔ)` per sample; ≤ 0 → CLEAR). Order per iteration:
    **sample → decrement budget → advance** — an N-unit ray gets exactly N
    samples (the floored divide under-fills the budget, so e.g. a 3-unit
    extent gets a 4th sample). Per sample: coarse **point sample** of the
    atlas ≥ ray z → confirm with `Terrain_SampleHeightBilinear` at the
    reconstructed world coords → HIT iff bilinear ≥ ray z. Cell re-resolve
    on a 512 crossing (`frac & 0xFE000000`; base `>> 25`; OOB
    clamp-to-edge). An EMPTY cell (null tile) marches until ray z ≤ 0 → HIT
    on the height-0 floor, through the SAME hit epilogue (step globals
    stored).
  - On HIT with a hit pointer: hit = reconstructed world x/y and the **ray z**
    at the hit sample (not the terrain height — HiRes_0 refines it), and the
    step vector lands in `Terrain_LastRayStep*`.
- **`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`** — LoRes, then refine on
  hit. Guard (witnessed odd form): refine is skipped iff
  `step_x == 0 && step_y != 0 && step_z != 0`. Refine steps =
  `Terrain_LastRayStep*/4` (arithmetic `>> 2`; y **negated** back to the
  world axis): back-steps while the ray point is below the bilinear height,
  then forward-steps while above — each walk's counter test is on the OLD
  value (postfix `count--`), so a counter-terminated walk moves up to **9**
  times with 8 resamples — then an 8-iteration bisection (above → +step,
  below → −step, steps halve after each move) — final precision ~(1/4)/2⁸
  unit along the ray. Callers:
  `raycast_entity_collision @ 0x413760`, `Entity_FindNearestByRay
  @ 0x413af0`, and via the thunk `@ 0x610890`:
  `Entity_ProcessProjectileTravel`, `Weapon_RaycastAndSpawnImpact`,
  `Projectile_UpdatePhysics`, `Entity_BuildCameraView`,
  `Entity_BuildCameraFromWeaponView`, `Entity_UpdateInfantryPlayerBody`,
  `HUD_DrawScopeOverlayDetails`, `Debug_DrawAICrosshairInfo`.
- **`Terrain_RaycastLoResNoNormal @ 0x610860`** — the LoRes core with
  `hit = NULL` (pure boolean clear test). Callers:
  `render_skybox_sun_glow @ 0x5acd00`, `update_sun_glare @ 0x5ad130` — the
  glare-occlusion path `celestial.gd::_glare_ray_clear` stands in for
  (env #14; the stand-in adopts the B1 port).
- **`Terrain_RaycastHeightmapHiRes @ 0x60c760`** — a SIBLING full
  implementation, **witnessed 2026-07-16** (the occlusion slice) and ported as
  `terrain_raycast_los_clear`: END-point bilinear precheck, a 2.0u
  short-segment two-endpoint test, then a 4.0u-per-texel POINT-sample march
  (0.5u-quantized cache byte, no bilinear confirm, no refine), the height-0
  null-tile floor, budget CLEAR — the cheap LOS variant
  ([render-occlusion-re.md](../render/render-occlusion-re.md)). Callers:
  `Physics_RaycastTerrainAndSectors @ 0x539910` (`CollisionWorld::raycast_clear`,
  now on this port), `Physics_CheckTerrainLineOfSight @ 0x53b080` (the sound
  occlusion LOS), `HUD_RenderAllOverlays`, and
  `terrain_occlusion_check_three_rays @ 0x610ed0` (the camera-to-entity
  terrain visibility latch). This last caller is not D-RLIT's directional
  lighting query: per-entity sun visibility instead uses collision-entity
  candidate slices through `Entity_ComputeSunVisibility @ 0x5c6800` →
  `raycast_find_collision_entity @ 0x539a70`; see
  [render-lighting-re.md](../render/render-lighting-re.md#d-rlit-divergence-catalog).

### B1 port (landed 2026-07-07)

The B1a/B1b slices landed the LoRes march + HiRes_0 refine as
`engine/runtime/terrain_query/terrain_raycast.{h,cpp}` (`terrain_raycast_march` /
`terrain_raycast_refined`, 16.16 structural translations with a
point+bilinear sampler seam; ~60 pinned checks in the `terrain_raycast`
ctest incl. the step-math exactness, the crossing-rule asymmetry, the
height-0 floor, and the odd refine guard's zero-step no-op-walk interplay),
bound as `TerrainData.raycast_terrain(from, to)` over both reimpl
substrates (an optional height image or baked CPT — the existing slice-A
sampler cores reused). The surviving adopter is the celestial glare ray
(`celestial._glare_ray_clear`, the 32-unit stand-in retired); the former
ONED mission-picker adopter was removed by ADR 0037. The safe-query guard
divergences (OOB no-terrain vs retail clamp-to-edge,
no-data NAN vs retail return-HIT, contiguous-atlas bilinear vs the seam
flags) are **D-TERRAIN-4** (class C, runtime safety boundary).

Open follow-ups from this pass: the seam-flag WRITER (load-time adjacency
derivation) and the rationale (if any) behind HiRes_0's odd skip-refine guard.
The `@ 0x60c760` internals follow-up closed 2026-07-16 — witnessed and ported
as `terrain_raycast_los_clear` on the occlusion slice (the AI LOS
`los_terrain_blocked` stand-in upgraded to it in the same change).

## D-TERRAIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TERRAIN-1 | C | **RETIRED (2026-08-24)** | **Former terrain-shader edit/runtime split**: the deleted ONED terrain preview used a live-sculpt shader while the runtime used the baked shader. ADR 0037 removed the preview and therefore removed the divergence; this row remains as history only. |
| D-TERRAIN-2 | A | **FIXED (2026-07-06)** | **Doubled detail-normal factor** (the gobj-era chimera): `terrain_lighting.gdshaderinc` stacked TWO ×2 `dp3(normalmap, blendmap)` factors on the 3-way splat; the witnessed top-tier ps.1.4 applies exactly ONE `[orig: PolyTrn_PS14SplatNormalMap source @ 0x7dece0; PolyTrn_PS14Splat source aPs14TexldR0T0T @ 0x7dee18; compile_terrain_pixel_shaders @ 0x605260]` (the dual-normal product belongs to the separate non-splat ps.1.1 tier). Post-gamma (D-RMAT-7) the squared factor clipped whole regions to white. See §Include correction above; ledger row carries the full witness. |
| D-TERRAIN-3 | C | **FIXED (REN-7, 2026-07-07)** | **Below-horizon fill**: retail fills the below-rim region with the frame clear alone — the env #21 horizon-blended skyfog `[orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]`; no skirt/ring geometry exists in the frame walk (the sky-pass terrain leg `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670` is the plain fogged sector batch), the seam hidden by fog convergence at the 1024 fog reference (= the dome rim radius). The reimpl's clear consumer was swallowed by a `BG_SKY`(null-sky) Environment rendering BLACK; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` in `game_world.tscn`, GUT-pinned — and `get_frame_clear_color()` corrected to the post-blend DOUBLED skyfog (the modulate2x-path Clear takes it verbatim; the "undoubled" 07-05 reasoning was the non-modulate2x fallback, no reimpl analog). The former ONED far-environment preview residual disappeared with the preview. |
| D-TERRAIN-4 | C | **PERMANENT (runtime safety boundary)** | **Safe terrain-query bounds** (ENG-3 B1): beyond-extent = no-terrain/no-hit vs retail's clamp-to-edge `[orig: @ 0x31a0010/0x319fc0c]`; no-data = clear/NAN vs retail's return-HIT `[orig: @ 0x60ccf7]`; contiguous-atlas bilinear vs the per-quadrant seam flags `[orig: @ 0x31a17f0..]`. Game consumers use these guards. Returning no result outside valid data avoids inventing an edge hit; §Runtime terrain queries carries the retail forms for any consumer that specifically requires them. |
| D-TERRAIN-5 | A | **FIXED (2026-07-13)** | **Top-tier texture/shader source mismatch**: the reimpl incorrectly used its heightmap normal as the t3 detail coefficient, camera-crossfaded near/far textures, float-normalized DBlend, and multiplied an extra terrain tint. the separate heightfield-normal atlas feeds cached-tile alpha; DBlend, paired mip chains, and literal t0..t5 ps.1.4 math are ported. **Corrected 2026-07-15**: the fix's own first reading (t3 = the generated authored-detail B-channel coefficient) was also wrong — t3 is the authored second detail pair (`polytrn_detailmap2` ⊕ `dist2`) at density2; the generated coefficient belongs to the ps.1.1 tiers at stage 7 [`orig: Texture_GenerateNormalMap @ 0x58c070`; `Terrain_GenerateNormalMap @ 0x603210`; `PolyTrn_InitTextures @ 0x60aaa0`; `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`; `PolyTrn_PS14SplatNormalMap source aPs14TexldR0T0T_0 @ 0x7dece0`]. |
| D-TERRAIN-6 | A | **FIXED (2026-07-13)** | **LOD/fog/overlay base-pass semantics**: both raw `lod_sub / 2` sites use the exact clamped eight-family selector; exponential fog uses eye-space depth while linear types use radial distance; ordered `.til` color is composed before terrain lighting. The render-target-alpha recurrence within that order is cataloged separately as D-TIL-3 [`orig: render_terrain_sector_batch @ 0x6092a0`; `Render_SetFogState @ 0x58a950`; `PolyTrn_RenderTile @ 0x60da70`]. |
| D-TERRAIN-7 | A | **OPEN (narrowed 2026-08-23)** | **Tile-composition RT/update parity**: runtime hosts a current-frame 128-layer 256×256 page cache shared by terrain and foliage; exact bare RGBA, ordered `.til` RGBA, DOT3 alpha, and static selected-LOD/all-ROBJ A-only projections are composed per page. The former global directional static-shadow surrogate is retired. Static source lifecycle, every diffuse-alpha animation frame, content stamps, LRU/generation safety, required-overlay source readiness, and page-local unsupported attribution are pinned. The fixture-output gate rejects incomplete/currently unavailable page results and inexact static-source realization; comparison registration is separate. The policy half is portable (`engine/runtime/terrain/terrain_static_shadow_{geometry,planner,raster}`), with page plans memoized under a state epoch and the full captured CTRL array in caster identity. The max-quality c7/c8 projection is RESOLVED: the packed cache record, D3D `(world Z,Y,world X)` foliage transform, and required `Foliage_WindSwayVS` branch reduce exactly to `((world X-origin X),(world Z-origin Z))/span`; one `TerrainTilePageProjection` now drives terrain, foliage, MATCHTERRAIN, and shadow raster consumers, and the old per-consumer uniform names are removed. The composite ORDER is RESOLVED: on tiles where the static collector ran, `PolyTrn_RenderTile` skips its own DOT3 add (`@ 0x60E1CE`) because the collector already drew `PolyTrn_TileBakeDot3LightPassAlt` (`@ 0x60D794..0x60D7C0`) before silhouettes, matching the composer. The final composite STATE is also RESOLVED by a live D3D9 state/readback probe: RGBA writes plus ONE/ONE blend and the `(0,0,0,tempBlue)` shader preserve RGB exactly and add temp blue to the zero destination alpha; `composite_terrain_static_shadow_pixel` pins that equation. The 2026-08-23 exhaustive PROJSHAD audit closes four false sources of low-sun divergence: only `_FFP.fx::TBoringFFPProjShad` honors material blend state; all 15 shader declarations force ONE/ZERO; Tracer/Flag/Glass/SkGlass have no pass; and skinned inputs reduce exactly to rigid geometry because the collector copies one transform into all matrix slots before the unit-sum skin blend. The portable raster also applies retail CULLMODE CCW/two-sided override and per-fragment LESSEQUAL z writes, including blended/no-op occluders. The material-animation leg is RESOLVED: the shared evaluator consumes the submission tick plus loader-mapped CTRL values for AlphaGen and the complete row-vector UV matrix, and the shared selector handles time- and control-driven diffuse frames (`Render_SubmitEntity @ 0x5DAD9D`; batch CTRL snapshot/restore `@ 0x5D91AB..0x5D91DE`, `@ 0x5DA1B8..0x5DA1FD`; consumer `apply_shader_parameters @ 0x58DB80`). Time updates publish immutable worker state without manufacturing a resident spatial-page miss, matching retail's sample-on-recompose behavior. Whole-process CTRL/RNG ordering is tracked once by D-3DI-2. No shipped PROJSHAD source has sun-angle opacity, `PolyTrn_SunToBlendRatioColor` belongs to the lower non-multitexture branch, and `dword_319FBB8` has no live writer, so the earlier speculative "density mechanism" is rejected; the old 03tr screenshot delta must be re-evaluated after these corrections, not preserved as an expected residual. The D-TIL-4 grill identified the second overlay loop (`@ 0x60DF71..0x60E0AF`) as runtime scorch decals and the overlay atlas as the tile-set strip loaded by `Terrain_LoadTileSetAtlas @ 0x604A90`. Scorch damage updates, remaining ordered contributions, retail dirty cadence, and final RT edge/address/mip behavior remain open `[orig: cache record @ 0x60DB67..0x60DC02; c7/c8 build @ 0x60A220..0x60A34F; uploads @ 0x6006AB..0x600704; Terrain_CollectAndRenderTileModels @ 0x60D250; PolyTrn_RenderTile @ 0x60DA70]`. |
| D-TERRAIN-8 | A | **FIXED (2026-08-13; coordinate corrected 2026-08-17)** | **Underwater terrain water-noise modulation**: the engine terrain frame stamps `below_water` from the render eye vs the live water height (the bare unguarded strict `<` `@ 0x60fea5` — NO zero sentinel; the water height is plumbed unconditionally), and the shared surface include swaps the ps.1.4 stage-3 dp3 INPUT to the water module's per-frame regenerated noise texture at the swapped `source × 8/512` (`colormap_uv × 16` for the normalized 1024 atlas) texcoord — the witnessed TOP-TIER behavior (the 2026-08-13 selector decode above): the noise rides the PS14SplatNormalMap dp3 on detail2-authored maps, detail2-less splat maps faithfully render NO underwater modulation, and the `saturate(4·t3²)·t0.a` PSShadow pair belongs to the unported ps.1.1 tiers `[orig: below-water flag @ 0x60FEE0 → dword_319FB3C @ 0x60915F; live t3 slot swap @ 0x6043f2; selector @ 0x6044b1..0x604556; texcoord @ 0x609786..0x6097D6]`. Tests: ctest `terrain_frame_compiler` (flag pins), GUT `terrain_shader_contract_test` (formula pins) + `terrain_underwater_modulation_test` (the Dvxi5 flip drive). |
| D-TERRAIN-10 | A | **FIXED (2026-07-14)** | **Terrain light-vector coordinate basis**: EnvFile preserves the direct retail getter tuple `g`, not Godot/world XYZ. Retail's D3DCOLOR pack writes GPU diffuse RGB `(g2,g0,g1)`, matching normal-map RGB `(grid X slope, grid Y slope, up)`; the old reimpl `(x,z,y)` pack swapped the horizontal DOT3 axes. Terrain and analytic foliage now pack `(z,x,y)`. Flat 06:00/12:00/18:00 checks could not distinguish the swap, so a non-flat 08:00 oracle pins light bytes `(231,83,187)` and slope alphas `0.8987774/0.0794002` [`orig: Environment_GetLightDirectionFloat @ 0x57d870; Terrain_GenerateNormalMap pack @ 0x603470..0x6034eb; PolyTrn light pack @ 0x60e201..0x60e331; PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e`]. |
| D-TERRAIN-11 | A | **FIXED (2026-08-17)** | **Terrain detail coordinate scale**: retail constructs mesh UV1 as `source × polytrn_detaildensity / 512`; OpenNova had multiplied normalized 1024-atlas UV by density, halving every detail frequency. Runtime mesh UVs, authored detail2, and the underwater stage-3 swap now share the exact source-grid conversion. Deterministic 00TRa A/B probes select 2× with the existing axis at high correlation and reject the UV-swap alternative [`orig: parser @ 0x60f993..0x60f9b3; config load @ 0x60e634..0x60e63b; density/512 write @ 0x6029a0..0x6029aa; UV1 @ 0x602db5..0x602dbe; stage-3 transforms @ 0x609786..0x609810`]. GUT `terrain_shader_contract_test` pins the surviving consumers. |

| D-TERRAIN-12 | A | **FIXED (2026-09-13)** | **Missing empty-sector flat fallback**: zero sector-grid entries now traverse quadrant 1 unless view `+100` skips them (the live draw pass writes 0; the water-mirror prerender and the PCX screenshot scene write `Env_WaterHeightFixed != 0`, so the reimpl's flat draws ride a mirror-excluded visual layer). The draw carries the high-bit mode through flat mesh selection and zero primary/blend UVs while preserving independent detail/noise coordinates and raw tracked source heights. Every flat draw shares the canonical LOD-0 page, whose base/DOT3 source is UV zero and whose `.til` overlay loop is suppressed; the origin sector can borrow it with the ordinary geometric projection. `terrain_frame_compiler`, `terrain_tile_composer`, `terrain_tile_composition_cache`, and the Godot terrain shader contract pin the bounded behavior. [orig: PolyTrn_RenderFrame @ 0x60EAC0; terrain_render_visible_sectors @ 0x6090C0; decode_terrain_tile_vertices @ 0x602AA0; PolyTrn_RenderTile @ 0x60DA70]. |

The completed passes close D-TERRAIN-5/6/8/10/11/12 and bound D-TERRAIN-7.
The terrain data path remains the byte-identical TrnGen port; the pending grill
below is documentation depth around CDEP/traversal plus those two gaps.

## Pending (the deep grill, to complete R1)

The **data path is proven byte-identical** — the build → mesh-simplify → CPT
export chain reproduces the canonical output exactly across 5 fixtures (above),
so `mesh_simp` needs no further witness (it was the concern; it is closed). What
remains for a *full* (vs partial) R1 record:

- **CDEP / quadtree witness depth** — finish the on-disk block-encoding record
  and retail traversal/threshold re-confirmation. The final eight-family
  `lod_sub / 2` selector is closed; this item no longer includes shader
  binding or final mesh-family selection. `cdep_read` / `cdep_roundtrip`
  already pin the header and encode/decode round-trip against the synthetic
  `fixtures/terrain/tmap/Tmap.cpt`, and `cpt_jo_assets_sweep` (behind
  `OPENNOVA_JO_ASSETS`) parses every retail `.cpt` including `Dvxi5.cpt`'s header.
- **Tile-composition mechanics** — close D-TERRAIN-7 by matching the remaining
  scorch updates, retail refresh cadence, remaining ordered draws, and final
  RT edge/address/mip policy. The hosted
  128-page lifecycle, exact base/`.til`/DOT3 RGBA order, current-frame
  terrain/foliage binding, and supported static A-only projection are closed.
- **Single-detail (BHD-era) `.trn` binding** — a pre-JO terrain authors only
  `polytrn_detailmap` (no `_c1..c3`, no `detailblendmap`, no `detailmapdist`;
  DPTH depth). The tier table above says the missing blend map selects
  `PSBasic` (`colormap ×4 t1`), but WHAT `PolyTrn_InitTextures @ 0x60aaa0` /
  `PolyTrn_BindStageTextures @ 0x604330` put in stage 1 when no splat layer
  is authored — the single detail, or nothing — is unwitnessed, and the
  runtime port (`terrain_surface_inputs.cpp`) is splat-only. Until it is
  witnessed, the former ONED terrain workspace promoted such a map on open
  (Detail A/B/C + far target seeded from the single detail, blend map all-A,
  name from the file stem) so a JO/DFX export resolved to `colormap × detail`
  on the splat tier. ADR 0037 removed that authoring-only mitigation.
  Hazard while this stays open: the shared splat shader has no
  no-blend-map tier — a null `u_blendmap` samples Godot's WHITE default, so
  `c1+c2+c3` (×3, then the ×4 stage) blows the terrain out white/yellow. The
  retired workspace seeded an all-A blend map into `TerrainData` for such
  maps; the runtime (`terrain_surface_inputs.cpp` `get_blend_texture`)
  still binds null for a raw blend-map-less `.trn`, so loading a BHD terrain
  straight into the game renders the blow-out until the
  PSBasic tier is witnessed and ported.

The CDEP/traversal item is documentation depth; D-TERRAIN-7's exact
tile-shadow/cache mechanics are the one bounded open runtime parity surface
(D-TERRAIN-8's underwater modulation FIXED 2026-08-13). D-TERRAIN-1 and
D-TERRAIN-9 were retired with the ONED terrain preview.

## Cross-references

- Reimpl: `engine/runtime/terrain`, `engine/runtime/terrain_query` (ADR 0020, the world→height seam),
  formerly `engine/formats/tpm` (the TPM1 tile mesh) + `engine/formats/dep` (the depth intermediate), placed under ADR 0030 and removed with ADR 0037 (d57608b3d).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env
  far-colormap bake (#19).
