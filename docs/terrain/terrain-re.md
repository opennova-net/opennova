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
old `PSShadow*` interpretation; the 2026-09-24 rendering parity pass ported the
page record cache, the D3D9 page raster and source filters, the DXT codec, the
BMS tile-set override and the lit-batch pool composite
(§2026-09-24 rendering parity pass); on 2026-09-26 the page's address mode
was witnessed CLAMP at every terrain pass (D-TERRAIN-7 closed) and the weapon
Inset pass's own terrain frame was ported.

**Status: PARTIAL.** Terrain is the largest system and the last of the seven
`UNAUDITED` systems; this record establishes the tracked surface — the module
map and the mixed-binary witness basis. The data/build path is byte-identical,
and the top-tier base-surface texture derivation and shader math are now closed.
The composed page is closed end to end (D-TERRAIN-7 closed 2026-09-26: the
page lifecycle, ordered contributions and raster ported 2026-09-24, and the
address mode at the terrain and foliage draws witnessed CLAMP, as the reimpl
samples it); what remains is CDEP/traversal documentation depth and the
single-detail binding. The material-animation leg is closed by the
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
| Flat stage-2 blend transform and independent detail/noise | MATCHING (instruction witness) | The shader collapses the blend coordinate independently of the live stage-1 detail and stage-3 detail/noise coordinates. Full-scene pixel equivalence remains unverified. |
| Godot mesh upload and shader parameter application | host code / not grillable | `Terrain` uploads the native vertex variants, selects the flat mesh by draw-list flag, and applies the native zero-primary-UV projection. |
| Water-mirror exclusion of the flat fallback | MATCHING (visual-layer split; instruction witness) | Flat draws ride `visual_layers::TERRAIN_FLAT_FALLBACK` alone; `REFLECTION_CULL_MASK` excludes it and the beauty camera admits it. |

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
@ 0x60DC08..0x60DC16, `.til` gate @ 0x60DD9A..0x60DD9E];
[orig: terrain_tile_cache_lookup @ 0x604140, masked-coordinate and sector
checks @ 0x6041A4..0x604206, projection @ 0x604215..0x604292].

Original-machine routing probes used the verified retail executable SHA-256
`b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`:
an all-zero grid produced 121 traversal calls with view `+100=0` and zero
calls with `+100=1`. The jo-c oracle fixture's `skip_empty` label writes `+96`,
so these probes explicitly wrote `+100`; its host preview was not used as a
visual oracle. IDA reads used `Jointops.exe.kong.i64` at image base `0x400000`.
No IDB changes were made. Full-scene retail pixel equivalence remains
separate verification work (the composition lifecycle closed with D-TERRAIN-7,
2026-09-26).

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
@ 0x8493e4` set with the tier and force-cleared WHEN the device word
`dword_32655B4` (device state `+0x4C`) is 80 or 73
(`PolyTrn_LoadTerrainConfig @ 0x60E565..0x60E584`: the 80/73 compare jumps
to the zero store). 0x50 and 0x49 match `D3DFMT_D16` and `D3DFMT_D15S1`, the
16-bit depth formats (identity plausible from the values; corrected
2026-09-24, the earlier "cleared unless" reading was inverted).

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
they carry box-filtered chains down to the 4×4 level (one level per halving
while the smaller side exceeds 2)
[`orig: PolyTrn_InitTextures @ 0x60b2c9; GTexture_CreateFromPixelData_0
@ 0x6877BA..0x6878BE (D3DXFilterTexture BOX)`]; the reimpl box-mips the
normalized blend, and since 2026-09-24 the page composer samples the
Colormap/TrnNMap quadrant level sets themselves (§2026-09-24 rendering
parity pass; the former box-mipped colormap tile-RT surrogate is gone). The
three splat layers are DXT1 textures on the reference adapter (same section).
The witnessed device supports texfilter
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
pairs. After the parse, a non-empty BMS tile-set name (`Bms_TileSetName`,
header `+0x118`) replaces the `.trn`'s tilestrip and `.TSD` slots
(`Terrain_LoadEnvironmentConfig @ 0x6109C8..0x610A1C`); OpenNova ports it as
`formats/trn` `trn_mission_tilestrip` (2026-09-24,
[tiles/til-re.md](../tiles/til-re.md) §The tile-set atlas source). For each quadrant/axis, a nonzero component selects mask 511 plus that
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
`TerrainData::_load_from_trn_text` rode an untracked planning note no tracked decision ever
carried (ONED is run-only, ADR 0037), so the polydata leg is ported and that
early-out removed. Foliage `match` lines carry up to four consumed codes per
definition (the parser stores seven), see
[foliage/foliage-re.md](../foliage/foliage-re.md) "Definition match codes"
(`Foliage_RemapPixelToDefMask @ 0x5FF4E0`, `Terrain_ParseConfigCallback
@ 0x60F330`).

`PolyTrn_InitTextures` splits the 1024 atlas into four 512 textures
`TrnNMap0..3` (creator calls from `0x60b3fa`; split loop from
`0x60b3eb`) with flags `0x100001`: CLAMP, LINEAR min/mag and MIPFILTER
POINT over the box level set (`apply_texture_stages @ 0x68084C..0x680870`
decodes flags bit 0 as CLAMP, bit 1 as POINT, bit 3 as the device mip/aniso
mode; see §2026-09-24 rendering parity pass). The page composer splits the
reimpl's shared atlas into the same four quadrant textures with their level
sets (`TerrainTileQuadrantSource`) and samples the page's level; the earlier
"no mip filter" reading and the single-atlas texel-centre clamp are
superseded (corrected 2026-09-24).

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
first-person NVG rebuilds the sky arg as bytes @ 0x610d16..0x610e22, the
first-person thermal view forces 0x101010/0xF0F0F0 @ 0x610e51..0x610e65)]`
(the NVG byte rebuild, corrected 2026-09-24: per channel
`trunc(sky_byte·0.25·f + modulator_byte·f·0.0015625·255)` with
`f = (level+1)·0.2` under a chop control word, the alpha byte kept from the
sky block, then `init_terrain_lighting_color_ramps(Env_LightBlock, nvgSky)
@ 0x610e36`; ported as `EnvironmentState::nvg_terrain_sky`, which
`build_terrain_uniforms` takes before the thermal ramps), so the shared shape is **terrain light =
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
[`orig: render_terrain_sector_batch @ 0x6092a0`]. That path's composite
(the pool lights, the DESTCOLOR/SRCCOLOR page multiply, then the fogged
ordinary pass) is ported since 2026-09-24; see §2026-09-24 rendering parity
pass.

The reimpl composes tile-overlay RGB into the reconstructed t0 base **before**
that lighting chain while retaining t0 alpha as the heightfield/light DOT3
term. Fog now follows the
retail mode split: exponential type 0 uses eye-space Z/depth, while linear
types 1/2/3 use radial camera distance [`orig: Render_SetFogState @ 0x58a950`
→ `CD3DDevice_SetFogParameters @ 0x677960`]. The start the terrain receives is
the device's, already folded: `Render_SetFogState` multiplies the type-2/3
start by `(1 - density)` (`@ 0x58a9c5..0x58a9fe`), and the terrain uses it
as supplied (2026-09-24, "Fog objects by their retail pass path and keep the
overcast fog start"; the terrain fog helper had re-derived the type-2/3 start
as a fraction of the end and dropped the overcast fold, and the unused `terrain_fog_start_for_type` /
`terrain_fog_factor_for_distance` helpers are deleted). Final mesh selection is the
eight-family mapping `floor(lod_sub × 8 / 16) = lod_sub / 2`, clamped to
families 0..7 [`orig: render_terrain_sector_batch @ 0x6092a0`; the family
select `Terrain_GetLodSlotFamily @ 0x60288E..0x6028B1` (`count × lod_sub / 16`), called
`@ 0x609581`].

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
`render_terrain_sector_batch @ 0x6092a0`, `Foliage_RenderDetailPatches
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

**The water-mirror terrain pass (2026-09-24).** The mirror context's
below-water word (`+0x74`) is 0 (`render_main_scene @ 0x5c153d`), so the
reflected terrain keeps the authored stage 3 (detail2) and never takes the
noise swap, whichever side of the plane the eye is on. The reflected pass fogs
with `Environment_ApplyFogAndAmbient(0, 0)` (`@ 0x5c1648..0x5c164c`), the dry
weather block, and the sky wrapper restores `Env_FogBlock` before the terrain
(`SkyDome_RenderWithSkyfog @ 0x579ce7..0x579cf6`). The terrain is clipped per pixel on both
sides of the plane: `render_main_scene @ 0x5c1561..0x5c1578` arms the plane
at `wh - 0.1` while the water height is nonzero, and
`render_terrain_sector_batch @ 0x6092c6..0x60935b` generates the clip-texture
coordinate `u = y + 0.45 - plane` (`flt_7C6FAC`) under AlphaRef 0x80, which
keeps `y >= wh - 0.05`. Ported in `terrain.gdshader` /
`terrain_lighting.gdshaderinc` ("Port the water mirror's below-water eye, dry
fog and clip planes"): the mirror pass is the one non-shadow camera whose mask
omits the water layer, fogs with `opennova_water_mirror_fog_color` /
`_range` (`EnvironmentState::build_water_mirror_fog`), skips the stage-3 swap
and discards below `wh - 0.05`; the terrain shader had no clip before. GUT
`terrain_underwater_modulation_test` pins the clip and its absence in cameras
that draw the water layer. The mirror itself is
[env/env-tod-re.md](../env/env-tod-re.md) #30.

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
  `0x4000` (`@ 0x60D315..0x60D32C`), and derives the per-axis slope
  `t = l_axis × 0x8000 / l_vertical` (imul/idiv, `@ 0x60D35D..0x60D386`) over
  the tile rectangle in mission 16.16 (`tileSize = 0x400 >> lod`, x
  `@ 0x60D286..0x60D2B5`, y `@ 0x60D299..0x60D2C5`);
- scans pool 2 and pool 1. Both reject BMS/runtime `NoShadow`
  (`entity flags & 0x01000000 @ 0x60D42F`) and ItemDef `NoShadow`
  (`ItemDef+0x54 & 0x04000000 @ 0x60D43E`). Pool-2 sector buildings otherwise
  cast by default; only pool-1 candidates require ItemDef attrib2
  `StaticShadow` (`ItemDef+0x58 & 0x20 @ 0x60D447..0x60D450`);
- extends the tile rectangle toward the light by `round16(t × r)` per axis
  (t = the 16.16 horizontal light component × 0.5 / the clamped vertical,
  r = the model sphere `+0x14`, the husk model's when `Flags & 4`,
  `@ 0x60D452..0x60D475`) and admits a caster whose sphere overlaps it,
  signed inclusive compares (`@ 0x60D465..0x60D54F`); a tile collects at most
  0x400 casters (`@ 0x60D390/0x60D40E`). A tall narrow caster at a grazing
  sun is therefore NOT drawn into far tiles its real shadow reaches: retail
  cuts it at the tile edge. Ported 2026-09-24 ("Port the static shadow tile
  collector's sphere test"): the candidate carries the entity position and
  the model sphere; the former swept-AABB broad phase onto each page's
  minimum receiver height, its receiver-height field and cache and the
  `bounds_exact` gate are gone. ctest `terrain_static_shadow_collector`;
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
candidate (`0x60E0C6..0x60E19D`). The temp target is twice the page at every
tier and the collector's viewport is the temp dimension (`@ 0x60D5E0`,
`@ 0x60D9E4`). The collector draws untransformed geometry through a plain
ortho with no half-pixel bias (`setup_shadow_cascade_matrices_0
@ 0x58D5CF..0x58D5EE`), so the temp raster samples at integer temp
coordinates (D3D9 pixel centres: temp pixel i at `page_u = i / temp_width`),
half a temp pixel before the page texel centres the composite reads; the
composite's MINFILTER LINEAR fetch over the `[-0.5, page - 0.5]` quad
(`@ 0x60E0D1..0x60E0E0`, `@ 0x60E152..0x60E166`) is the exact 2×2 box.
Ported 2026-09-24 ("Sample the static shadow temp target at D3D9 pixel
centres"; the raster had sampled at `i + 0.5`); ctest
`terrain_static_shadow_raster`. The final state is now closed rather than
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
  256×256 single-level RGBA page cache for quadtree levels 1–4. Each page
  composes as the D3D9 raster draws it: every pass is an XYZRHW quad whose
  positions `fill_fullscreen_quad_vertices @ 0x678DFE..0x678E4B` copies
  unbiased, so page pixel x samples at position x (not x + 0.5). The base and
  DOT3 passes sample the 512-unit quadrant textures
  (`Terrain_SplitTileIntoQuadrants @ 0x604E60`; Colormap0..3 / TrnNMap0..3
  flags 0x100001 `@ 0x60B51C` / `@ 0x60B3FA`) CLAMP + LINEAR + MIPFILTER
  POINT on their box level set (`GTexture_CreateFromPixelData_0
  @ 0x6877BA..0x6878BE`), so a LOD-1 page reads level 1; the ordered `.til`
  quads (`@ 0x60DE66..0x60DEBB`) point-sample the tile-set atlas (flags
  0x100203 `@ 0x604B24`: CLAMP, POINT min/mag/mip) with `PolyTrn_DrawTileOverlayQuad`'s
  half-texel bias landing each pixel on a texel centre of the nearest box
  level (LOD4 = level 0 ... LOD1 = level 3); the ordered scorch quads sample
  WRAP + LINEAR + MIPFILTER POINT (flags 0 via `Texture_LoadByNameWithChannel
  @ 0x58B728`); RGB only for base/`.til`/scorch (A=0 from the Clear), then
  the additive DOT3 alpha, then supported static model silhouettes as A-only
  projections (corrected 2026-09-24; the 2026-08-17 text had the `.til`
  quads blend render-target alpha too, see D-TIL-3). Terrain binds each
  patch's exact-key record; detail foliage and MATCHTERRAIN find pages through
  the record-order point lookup (§2026-09-24 rendering parity pass). A page
  samples the light and the casters when it composes; the former content
  identity (the truncated `(g2,g0,g1)` light-byte epoch plus per-caster
  revisions) no longer decides residency. Static caster admission,
  selected-LOD/all-ROBJ submission, destruction/husk/dynamic transforms, all
  diffuse-alpha flipbooks, and page-local unsupported attribution are
  explicit typed inputs rather than a global directional-light surrogate. The
  mission present pass republishes an admitted individual model's exact
  applied transform into that same source registry; exact-value gates keep
  repeated rows and repeated static-husk publications revision-stable, while
  a real pose, graphic, geometry identity, or caster-active change advances
  the snapshot once. A snapshot change never retires a resident page (retail
  keeps a page until its record is reclaimed, TOD-evicted or invalidated
  spatially).

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
  D-3DI-2 concern, not a guessed shadow-only evaluator. The final temporary-blue
  composite equation/state and per-technique pass admission,
  forced-opaque versus `_FFP` material blending, skinned rigid collapse,
  one-sided culling, and non-opaque inter-caster z ordering are closed. The
  gaps this list carried in 2026-08 (scorch damage updates, the ordered
  contributions, retail dirty cadence, RT edge/mip behavior) closed by
  2026-09-24: the scorch records landed with #560 (2026-08-25,
  `terrain_scorch`), and the 2026-09-24 pass ported the raster, the record
  cache and its cadence (§2026-09-24 rendering parity pass). The analytic
  cold-page fallback is retired: a patch whose page the cache could not claim
  draws with t0 unbound, which D3D9 samples as (0,0,0,1). D-TERRAIN-7's last
  facet, the page address mode at the draw, closed 2026-09-26: every terrain
  pass samples the page CLAMP (§2026-09-24 rendering parity pass, *Page
  sampling at the draw*).

  **Creation-time clear — WITNESSED 2026-09-14 (jo-c cross-check), PORTED.**
  `Terrain_CreateTileCacheTargets @ 0x604DD0` creates the 128 tile render targets (stride 8 dwords,
  `dword_319A2E0..0x319B2E0`, dimension `dword_31A00D4`) and clears each to
  D3DCOLOR `0xFFFF6060` (ARGB: R=FF G=60 B=60) with z `0.99994999`
  (`GTexRT_SelectThunk(0x12345678, rt, -40864, 0.99994999)`), stamping the
  slot key/UV sentinels `0x12345678` and indices −1; it then creates one extra
  square target `dword_319A2D8` of dimension `dword_31A00D0` (the model-shadow
  temp target: `PolyTrn_LoadTerrainConfig @ 0x60E41D..0x60E446` sets page
  `dword_31A00D4` = 0x100 and temp `dword_31A00D0` = 0x200 when config
  `+0x1740` is set, else 0x80 / 0x100, so the temp is always twice the page;
  the 0x80 / 0x100 tier needs the Terrain Tex Detail rung below 3, which
  OpenNova's locked video policy never offers (*The page tier*, §2026-09-24
  rendering parity pass);
  ported as `kTemporaryScale` = 2 with the exact 2×2 box resolve in
  `engine/runtime/terrain/terrain_static_shadow_raster.cpp`, corrected
  2026-09-24). Ported as `TerrainTileCompositionCache::kTileClearColorArgb` and the
  blank-layer fill in `godot/src/terrain/terrain_tile_cache_device.cpp`.

  **Retail refresh cadence: WITNESSED 2026-08-18, corrected 2026-08-22 and
  2026-09-24, PORTED 2026-09-24.**
  Retail's 128-slot hit compare keys ONLY on `(lod, packed source coordinate,
  routed sector z/x)` (`PolyTrn_RenderTile @ 0x60DAC0..0x60DAD1`; the earlier
  "quadrant" is the key's bits 9/25); no caster, light-epoch, or TOD input
  participates in the HIT. Slots stamp `Env_TodEpoch` at compose
  time (`@ 0x60DBC0`, `dword_319A2F8`), and that stamp is READ by the evictor:
  `Terrain_EvictOldestTodStaleTile @ 0x604600` walks the 128 slots four at a time and
  picks, among the slots whose stamp differs from the current
  `Env_TodEpoch` (`@ 0x60463B/0x60465E/0x604681/0x6046A3` — indexed
  reads through the slot base, which is why a plain xref of `dword_319A2F8`
  shows only the write), the OLDEST-composed one (compose frame
  `dword_319A2F4` vs the frame counter, compose age above 1: the running best
  starts at 1 and the compare is `jle`) and invalidates it (`lod =
  coord = -1`, last-use = frame − 0x10000 `@ 0x6046D2..0x6046EA`), one per
  call. There is no visibility check: a visible page can be evicted, and the
  re-sweep in the same frame recomposes it under the current light.
  `PolyTrn_RenderFrame @ 0x60EAC0` calls it only on a frame where NO
  tile was composed (`@ 0x60F0AB..0x60F0AD`) and then re-sweeps every visible
  patch (`@ 0x60F0CF`), so the evicted tile recomposes with the current sun
  that same frame. `Env_TodEpoch` steps every 311 logic ticks (~5 s)
  while the clock advances (`Environment_UpdateWeatherTick @ 0x57E9DA..
  0x57E9EF`). Net: retail re-bakes stale visible tiles one per all-hit frame
  after every ~5 s TOD epoch, oldest first — static shadows track the sun with
  a latency of roughly 5 s plus a frame per visible tile (the 2026-08-18
  "write-only stamp, stale until LRU turnover" reading missed the indexed
  reads). The reimpl ports the record array (`TerrainTileCompositionCache`,
  2026-09-24, "Port retail's terrain page record cache and compose pages
  before they draw"): identity = (level, packed source coordinate, routed
  sector); a miss claims the record with the largest last-use age, first in
  record order, only if the age exceeds 1 (`@ 0x60DAE4..0x60DB45`,
  empty-handed `@ 0x60DDC6`), so the first frame claims nothing and a frame
  with more pages than claimable records draws the rest with t0 unbound;
  claims stamp last use, compose frame and `Env_TodEpoch`
  (`@ 0x60DB56..0x60DBF0`; `WeatherState::tod_epoch`); the sweep
  composes every missing visible page synchronously before the draw (the
  device waits on the worker pool); an all-hit sweep evicts one TOD-stale
  record (`Terrain_EvictOldestTodStaleTile @ 0x604600`) and re-sweeps
  (`@ 0x60F0AD..0x60F0E3`). Casters and light are sampled when a page
  composes; nothing re-targets a page. The earlier reimpl (a content-stamped
  LRU fed by an asynchronous queue, stale-while-recompose, a DOT3-byte light
  epoch, and an analytic colormap fallback while a page was cold) is retired;
  the record-cache details are in §2026-09-24 rendering parity pass.

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
@ 0x5ff630` assembles the WIND-SWAY vs_1_1 (`Foliage_WindSwayVS @ 0x2c25e5c`:
a polynomial sine of the pre-wind vertex's render x (Godot Z) relative to
the patch's 512-unit sector origin, times `c24.y` = 1, plus the phase,
weighted by vertex RED and scaled by `c25.x` = 0.03 (`flt_7C9B90`), displacing
render z (Godot X); corrected 2026-09-24, the earlier "world.x, displacing Z"
reading swapped the axes, see foliage-re.md §Detail shader; constant diffuse
c6; lightmap UV = planar world projection
via c7/c8) and the per-patch GRID-PLACEMENT vs_1_1 (`Foliage_GridPlacementVS
@ 0x2c25e60` — a0-indexed per-patch constants, bilinear + quadratic height);
`Foliage_CreateLightmapBlendPS @ 0x5ff7a0` (`Foliage_LightmapBlendPS
@ 0x2c25e64`) is the fragment combine `rgb = t0 × (t1 × (t1.a·c1 + c0)) ×
v0 × 8, a = t0.a × v0.a` (t1 = the planar-projected lightmap).
`Foliage_SetupDetailSlotDraw @ 0x6007c0` (ex
`terrain_setup_display_adapter`; per model slot 0-3) picks the LOD entry,
binds the wind VS + FVF 338, fog mode 8 (VS fog, linear in eye depth for
every fog type; foliage-re.md) when the wind VS exists,
and **alpha-test ref 180 (high quality) / 8 (low)**. The c6.a ×0.1 scale in caller
`Foliage_RenderDetailPatches @ 0x60a4a8/0x60a16c` is gated on arg_8, which is
the THERMAL view flag, not a reflection flag: `Render_ProcessMainSceneFrame`
passes the held weapon's thermal byte (`@ 0x5ca2da..0x5ca2e3`) as the scene
core's fourth argument (`@ 0x5ca8e3`), which
`Terrain_RenderWorldScene` pushes to both detail passes
(`@ 0x5c95c1/0x5c9661`); under thermal each patch draws one primary LOW
(`@ 0x60a193..0x60a19c`) at fade ×0.1 and no secondary. Outside thermal the
near secondary LOW shares the unscaled fade (the 2026-07-15 grill). Corrected
2026-09-24 ("Draw new detail cells at once and thin them under thermal"; the
2026-07-15 reading tied arg_8 to the water reflection); foliage-re.md §Fade
and pass state. The setup branch at `0x6008fc..0x600912` passes value 2 to wrapper
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
(`init_view_effect_shaders_and_textures @ 0x5cf8e0`: the NVG view's tint
(ps 0x7DC3D8, state `dword_2BDFAB8` @ 0x5cfab6) and squared-tap glow (ps
0x7DC228, state `dword_2BDFABC` @ 0x5cfb60), both ported in
`renderer::frame_fx_effects` / `FrameFxCompositorEffect`; the 4x-luma ps
0x7DC5A8 (state `dword_2BDFAB0`) draws the 64 × 16 polar unwrap every NVG
scene ends with (ported, `renderer::nvg_polar_unwrap_passes`); the 2x2
lrp-average ps 0x7DC4A0 serves only the unreachable `dword_843480 == 0`
scope path), one in `Lighting_InitTextures
@ 0x5a94f0` (REN-5), and the FrameFX set (`CFrameFX_CreatePixelShaders
@ 0x5821d0`: six ps.1.1 stages plus the fixed-function scanline state and
the 64² `ffscan` texture, ported in `renderer::frame_fx_effects`). The
view-effect and FrameFX ports landed with the 2026-09-24 rendering parity
pass ("Port FrameFX's screen effects and the NVG render-to-texture view";
"Port the NVG view's scene raster, Scoped lens and Sighted card"); their
record is [render/render-order-re.md](../render/render-order-re.md).

## 2026-09-24 rendering parity pass

The rendering parity pass (PR #678) re-read the page producer, the lit batch
and the terrain texture creation in retail `Jointops.exe` (addresses are that
binary's, names the `Jointops.exe.kong.i64` IDB's) and ported the results.

| Component | Verdict | Evidence |
|---|---|---|
| Page raster and source filters | PORTED 2026-09-24 ("Compose terrain pages with the retail D3D9 raster and texture filters") | ctest `terrain_tile_composer` (integer pixel positions, box level per page LOD, point-sampled `.til` at 1:1 and at LOD 3), `terrain_tile_composition_worker`; the asset-gated CP12/00TRa `.til` oracles repinned ([tiles/til-re.md](../tiles/til-re.md)) |
| Scorch quads: orientation, filter, stage colour | PORTED 2026-09-24 (same commit) | ctest `terrain_scorch` |
| Page record cache: identity, claim, TOD eviction, lookup, spatial invalidation, unclaimed pages | PORTED 2026-09-24 ("Port retail's terrain page record cache and compose pages before they draw") | ctest `terrain_tile_composition_cache`, `terrain_tile_composition_worker`, `terrain_scorch`, `terrain_frame_compiler`, `destruction`; GUT `terrain_surface_inputs_test`, `foliage_tile_cache_runtime_test`, `terrain_underwater_modulation_test` |
| Patch emission order | PORTED 2026-09-24 ("Keep terrain patches in emission order and fix three terrain cites") | ctest `terrain_frame_compiler` |
| Lit-batch pool-light composite | PORTED 2026-09-24 ("Composite terrain pool lights with the page the way retail's lit batch does") | ctest `renderer_light_terrain_pass` |
| BMS tile-set override | PORTED 2026-09-24 ("Draw .til tiles from the mission's BMS tile set") | ctest `trn_config_roundtrip`, `mission_bms` |
| DXT `.til` atlas and splat detail layers | PORTED 2026-09-24 ("Encode the terrain atlas and detail layers with retail's D3DX DXT codec") | ctest `renderer_texture_dxt`, `terrain_tile_composer`, `terrain_texture_preprocess`, `terrain_scorch`; GUT `terrain_surface_inputs_test` |
| Page render-target sampling at the draw | MATCHING (read-only grill; the anisotropy lead is refuted) | witness below; `terrain.gdshader` `filter_linear` |
| Static shadow collector sphere test, temp raster pixel centres | PORTED 2026-09-24 ("Port the static shadow tile collector's sphere test"; "Sample the static shadow temp target at D3D9 pixel centres") | ctest `terrain_static_shadow_collector`, `terrain_static_shadow_planner`, `terrain_static_shadow_raster` (§Static sector/model sun shadows) |
| Water-mirror terrain pass: dry fog, authored stage 3, clip | PORTED 2026-09-24 ("Port the water mirror's below-water eye, dry fog and clip planes") | GUT `terrain_underwater_modulation_test`; ctest `environment_state` (§The water-mirror terrain pass) |
| NVG terrain sky bytes; overcast-folded fog start | PORTED 2026-09-24 ("Keep the env colour blocks raw under NVG; rebuild the terrain NVG sky as bytes"; "Fog objects by their retail pass path and keep the overcast fog start") | ctest `environment_state`, `terrain_lighting`; GUT `env_parity_vectors_test` |

**Texture flag decode.** `apply_texture_stages @ 0x68084C..0x680870` maps a
texture's creation flags (`+0x18`) to the per-stage device words: bit 0 →
CLAMP (`+0x190`), bit 1 → POINT (else LINEAR, `+0x160`), bit 3 → the device
texfilter mode (aniso / linear mip, `+0x178`, else MIPFILTER POINT);
`CGfxDevice_ApplyRenderStates` applies them (per-stage filters
`@ 0x67E463..0x67E4A7`); pass flags 0x1000000 / 0x2000000 add stage 0 / 1
CLAMP (`CGfxShader_ApplyPass @ 0x683265..0x683286`).

**Page raster.** Every `PolyTrn_RenderTile` pass is an XYZRHW quad whose
positions `fill_fullscreen_quad_vertices @ 0x678DFE..0x678E4B` copies
unbiased, so page pixel x samples at position x and a quad covers the pixels
whose position lies in `[start, end)`. Base and DOT3 sample the 512-unit
quadrant textures at page-local / 512 (`@ 0x60DB86..0x60DC02`, quad
`(0,0)-(dim,dim)` `@ 0x60DC1A..0x60DC7E`) on the box level the page's LOD
selects (a LOD-1 page reads level 1); the `.til` quads sit at
`(entry - origin) × dim / span` (`@ 0x60DE66..0x60DEBB`) and
`PolyTrn_DrawTileOverlayQuad`'s half-texel bias (`@ 0x604808..0x6048FD`) lands each
pixel on a texel centre of the nearest atlas box level (LOD4 = level 0 ...
LOD1 = level 3), the stage colour saturating before the blend. The old
composer sampled at x + 0.5 and kept the half-texel bias on top of that,
putting every overlay sample between two atlas texels (the faint, blurred
tire marks). `PolyTrn_RenderTile` runs base, `.til` and scorch under
`COLORWRITEENABLE = 7` (`@ 0x60DD04..0x60DD12`, `@ 0x60DD6B..0x60DD73`;
0xF restored `@ 0x60E0EA` / `@ 0x60E1B6`): page A is the Clear's 0
(`@ 0x60DC9D..0x60DCC5`) plus the DOT3 term and the static projections.
Ported in `engine/runtime/terrain/terrain_tile_composer`.

**Scorch records.** Retail keeps one append-only 4096-record list of
`{texture index, min x/z, max x/z}` rows (`Terrain_AddScorchRecord @ 0x605C90`,
cap `@ 0x605C9F`, row `@ 0x605CC7..0x605CEF`; producers
`Terrain_AddScorchForEffectKind @ 0x6060D0` and `Terrain_AddScorchSized
@ 0x606180`; textures `Terrain_LoadScorchTextures @ 0x604CE0`). An append
retires every resident page the record overlaps (`@ 0x605CFC..0x605D5F`), and
`PolyTrn_RenderTile` draws the overlapping records in insertion order after
the `.til` loop (`@ 0x60DF39..0x60E0AF`). The record quad puts UV (0,0) at
(minimum X, maximum Z) and UV (1,1) at (maximum X, minimum Z)
(`@ 0x60DFD1..0x60E02A`, UV `@ 0x60E06B..0x60E08E`), so texture rows run from
the record's maximum-Z edge; the pass flags 0x700000 carry 0x400000 =
CULLMODE NONE, so the upside-down quad draws. The textures load with flags 0
(`Texture_LoadByNameWithChannel @ 0x58B728`): WRAP, LINEAR, MIPFILTER POINT.
The stage colour is MODULATE2X(texture, `0xFF808080`) = texture × 256/255
saturated (`@ 0x60E033..0x60E04C`) before the DESTCOLOR/SRCCOLOR doubling;
the `0xFF808080` diffuse is chosen by
`g_TerrainAdapterCapsStorage.TexOpDisableOrArg2` (0x319FBA0 + 0x18), filled by
`CGfxDevice_QueryAdapterCaps @ 0x67DF72..0x67DF8E` (TextureOpCaps & 5,
cleared by the vendor quirk at device `+0xE8`) through `sub_676850` (called
`@ 0x60E4FE`) and normally set. The records and their page walk landed with
#560 (2026-08-25, `engine/runtime/terrain/terrain_scorch`,
`runtime/terrain_query/terrain_scorch_record.h`); the 2026-09-24 pass fixed
the orientation (the port ran texture rows from minimum Z), the filter (it
emulated anisotropy) and the 256/255 stage.

**Page record cache.** Retail keeps 128 32-byte records (`dword_319A2E0`:
level `+0x00`, packed coordinate `+0x04`, sector `+0x08/+0x0C`, last use
`+0x10`, compose frame `+0x14`, TOD epoch `+0x18`, render target `+0x1C`).
`TerrainTileCompositionCache` ports it:

- identity = (level, packed source coordinate, routed sector); a hit stamps
  last use (`PolyTrn_RenderTile @ 0x60DAC0..0x60DAD1`, `@ 0x60DDB8..0x60DDC0`);
- a miss claims the record with the largest last-use age, first in record
  order, only if the age exceeds 1 (`@ 0x60DAE4..0x60DB45`, empty-handed
  `@ 0x60DDC6`), so the first terrain frame claims nothing and a frame with
  more pages than claimable records draws the rest with t0 unbound; a claim
  stamps last use, compose frame and `Env_TodEpoch`
  (`@ 0x60DB56..0x60DBF0`); the frame counter advances once per terrain frame
  (`dword_319FC04 += 1 @ 0x60EAE8`) and never resets;
- `PolyTrn_RenderFrame` sweeps the visible list in emission order; a sweep
  that composed nothing evicts one TOD-stale record
  (`Terrain_EvictOldestTodStaleTile @ 0x604600`, compose age above 1, no visibility
  check) and sweeps again (`@ 0x60F080..0x60F0E3`); the device composes the
  claimed pages on the worker pool and waits before the batch draws;
- `PolyTrn_BindStageTextures @ 0x604330` clears t0 before the exact-key
  search (`@ 0x604356..0x60439C`); an unclaimed page draws with t0 unbound,
  which D3D9 samples as (0,0,0,1). The terrain shader's analytic cold-page
  fallback, its raw colormap/heightfield/overlay uniforms and its
  `u_tile_overlay_tint` uniform are retired (the page composer reads the
  environment's tint when a page composes);
- `terrain_tile_cache_lookup @ 0x604140` (MATCHTERRAIN; probe
  `@ 0x6041A4..0x604206`, last-use stamp `@ 0x60423E..0x604243`) and
  `Terrain_FindSectorPatchRT @ 0x6042B0..0x60430B` (detail foliage) walk
  granularity 32..512 and take the first resident record in record order
  whose masked coordinate matches in the point's sector: no LOD preference,
  no same-frame restriction, and a coarse match can return a page that does
  not contain the point (`TerrainTileCompositionCache::lookup`);
- `Terrain_InvalidateTileCacheRegion @ 0x605C10` (ex `CVertexBuffer_RemoveFromList`,
  renamed 2026-09-25) retires every resident page the
  destroyed entity's footprint (position ± 2 bound radii) overlaps
  (coordinate = -1, last use = 0, walk `@ 0x605C21..0x605C7F`), from
  `Entity_ProcessDestructibleDeath @ 0x43FC12..0x43FC5E`,
  `Entity_ProcessBld2Destruction @ 0x43F192..0x43F1DA` (the bld2 collapse),
  the crane collapse's twin block
  `Entity_ProcessCraneCollapse @ 0x440036..0x44007E` (the `cran` class think,
  defined in the IDB 2026-09-25) and `Entity_SpawnSectionEntity @ 0x44062E..0x440670`; ported as
  `world::TerrainScorchEvents::emit_page_invalidation` →
  `Terrain.invalidate_tile_cache_region`. A mission reset empties every record
  (`Terrain_ResetTileCache @ 0x605FC0..0x605FC5`).

Casters and light are sampled when a page composes; nothing re-targets a
page. A static-shadow planner state change therefore never retires a page.

**Emission order.** Retail draws and composes in the traversal's emission
order: the batch bubble sort (`@ 0x6093C0..0x609550`) keys on entry `+0x14`,
which `Terrain_TraverseQuadtreeNode` never writes (stores
`+0/+4/+8/+0xC/+0x10/+0x18` `@ 0x608FDA..0x609006`), so it never reorders.
The frame compiler's front-to-back sort is removed.

**Lit-batch pool composite.** A batch with pool lights never draws its
ordinary pass alone. `render_terrain_sector_batch` draws each light unfogged,
the first drawn light with blending off (`dword_2732DC0` on the PS path /
`dword_2732DC8` mode 0x600 on the fixed-function path), later ones ONE/ONE
(`dword_2732DBC` / `dword_2732DC4` mode 0x602); then it multiplies the target
by the page with `dword_319F934` (`sub_6791A0(1, 0x1000628)`
`@ 0x60C499..0x60C4AD`: MODULATE2X(page, lit white diffuse),
DESTCOLOR/SRCCOLOR, unfogged; drawn `@ 0x609960..0x609A19`), then adds the
fogged ordinary pass (`terrain_setup_lighting_and_shader(2)` →
`dword_319F930` mode 0x1020002; `@ 0x609A49..0x609AB6`). Pixel =
`sat(sat(2 × sat(2 × page) × pool) + fogged lit)`
(`renderer::terrain_light_pool_composite`, mirrored per channel in
`terrain.gdshader`); the shader had added the pool to the lit colour and
fogged the sum. On the PS path (`caps+0x34 & 0x100 @ 0x609890`) the per-light
setup is `Light_SetupTerrainProjectedPassPS @ 0x5AAB30` with the
cube-normalize map bound (`@ 0x5AAEA2..0x5AAEB7`); the port serves that
`ps.1.1` pass since 2026-09-26, with the detail coefficient map as its t1; the
light values are
[render/render-lighting-re.md](../render/render-lighting-re.md)'s.

**DXT textures.** The `.til` atlas (flags 0x100203 `@ 0x604B24`) is created
DXT5 and the three splat detail layers (0x400200 | terrain-detail flags | 8,
`@ 0x60AB95..0x60ABE5`) DXT1 on a current adapter: 0x300 asks for DXT5 and
0x400000 falls back to DXT1 unless the NVIDIA GeForce 256..GeForce4 Ti
device-id quirk (`+0x138`, set by `CGfxDevice_DetectHardwareCapabilities`
`@ 0x67D7A6..0x67D853`) keeps DXT5 (`GTexture_CreateFromPixelData_0
@ 0x687717..0x687766`). The colormap quadrants (0x100001 at full texture
quality; 0x500201 below it) are uncompressed at full quality; detail2 is never
compressed. The pixels go through the D3DX9 codec statically linked in
`Jointops.exe` (`D3DXTex::CCodecDXT`; block coders `@ 0x72170F` / `0x720AB7`
/ `0x720537` and `0x721962` / `0x720210`, decoders `@ 0x72140A` /
`0x7215D1`). Each mip level is the box halving of the previous level's
DECODED blocks (`D3DXFilterTexture`, `@ 0x6912AD..0x6912F9`). The device runs
without `D3DCREATE_FPU_PRESERVE` (`CGfxDevice_CreateDevice @ 0x67EA17`), so
the codec's x87 math is single precision and its conversions run under
round-toward-zero. Retail quirks kept: the eight-alpha weight table ends at
8/7 (`g_D3DXTex_AlphaD8`), the alpha Newton step uses texel minus step, and the
colour refinement has no lower index guard. Reimplemented in
`engine/runtime/renderer/texture_dxt`; the page composer samples the decoded
DXT5 atlas levels and `TerrainSurfaceInputs` uploads the detail layers as
`FORMAT_DXT1` blocks. Microsoft's later `D3DX9_43` is not the retail encoder
for alpha (its eight-alpha table ends at 1.0 and it keeps index 1 for
alpha 1.0).

**Page sampling at the draw (the anisotropy lead is REFUTED).** The page
render target is sampled LINEAR / MIPFILTER POINT (one level) with no
anisotropy: the render-target resource's flags word (`+0x18`) is zeroed at
construction (`GTexRT_Construct @ 0x680106`), and `apply_texture_stages`
(`@ 0x68084C..0x680870`) derives the per-stage words from it (bit 3 clear, so
no aniso). `terrain.gdshader`'s `filter_linear` is retail; no code change.
The page's address mode is CLAMP at every terrain pass (witnessed 2026-09-26,
closing D-TERRAIN-7): the pass states' intrinsic words (0x1020000 ordinary,
0x1020002 pool light, 0x1000628 page multiply; `PolyTrn_InitTextures
@ 0x60C3B0 / 0x60C433 / 0x60C499`) carry the stage-0 clamp bit 0x1000000.
`CGfxShader_ApplyPass` ORs the intrinsic word into the pass flags
(`@ 0x683221`) and applies the clamp after the page is bound
(`@ 0x683265..0x683279`), and the device sets ADDRESSU/V/W from it
(`CGfxDevice_ApplyRenderStates @ 0x67E4A9..0x67E4FE`);
`terrain_setup_lighting_and_shader @ 0x604420` applies the states with pass
flags 0, so the intrinsic word decides. `terrain.gdshader`'s clamp-to-edge
sampling is that mode (D3D9 CLAMP under LINEAR is Godot's CLAMP_TO_EDGE, and
the texel-centre clamp is the same under bilinear).

**The page tier.** The 128-page / 256-temp tier is selected only when the
Terrain Tex Detail rung (`0x24D2044`, labelled "Terrain Tex Detail" by
`Debug_DrawRenderSettings @ 0x44BC76`, IDB `g_TerrainTexDetail`) is below 3
(`Terrain_LoadEnvironmentConfig @ 0x610981..0x61099F`;
`PolyTrn_LoadTerrainConfig @ 0x60E41D..0x60E446`). OpenNova's locked video
policy pins TERRAINTEX = 3 (`engine/runtime/menu/options_policy.h`), so the
tier is unreachable and only the 256/512 tier is ported.

**The weapon Inset pass's own terrain frame (2026-09-26).** The Inset pass
runs its own PolyTrn frame over the shared page cache:
`Render_WeaponInsetScene @ 0x5C9740` (called `@ 0x5CA949`) → `sub_60FF50`
(the call `@ 0x5C9A2F`, its context `@ 0x60FF6F`) →
`PolyTrn_ResetFrameStatsAndRender` (the call `@ 0x610009`) → `PolyTrn_RenderFrame`
(`add dword_319FC04 @ 0x60EAE8`: one page-frame advance per traversal), then
its own sector pass (`@ 0x5C9D65`), both gated on the main frame's indoors
value (render-occlusion-re.md §4); the main traversal is `@ 0x5CA654`. Ported
as `Terrain::render_inset_frame`: the Inset draws its own patch pool (its own
light rows and `u_below_water`) on INSET_VIEW while the main pool moves to
MAIN_VIEW, and the Inset's detail foliage cells come from its own traversal
([foliage/foliage-re.md](../foliage/foliage-re.md)). Its environment and fog
uniforms follow the main eye, which the Inset's eye coincides with in
practice. GUT `terrain_inset_frame_test.gd`.

### Open after the 2026-09-24 pass

- None.

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
  glare-occlusion path `godot/src/env/celestial.cpp` `_glare_ray_clear` stands in for
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
| D-TERRAIN-3 | C | **FIXED (REN-7, 2026-07-07)** | **Below-horizon fill**: retail fills the below-rim region with the frame clear alone — the env #21 horizon-blended skyfog `[orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]`; no skirt/ring geometry exists in the frame walk (`Terrain_RenderMainSectorPass @ 0x610ac0` is the terrain surface pass and draws no sky: `Terrain_RenderSectorBatchLit @ 0x60c670`, the plain fogged sector batch, called `@ 0x610c34`, then `RenderSlot_DrawAllDrapes` `@ 0x610c47`; corrected 2026-09-24), the seam hidden by fog convergence at the 1024 fog reference (= the dome rim radius). The reimpl's clear consumer was swallowed by a `BG_SKY`(null-sky) Environment rendering BLACK; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` in `game_world.tscn`, GUT-pinned — and `get_frame_clear_color()` corrected to the post-blend DOUBLED skyfog (the modulate2x-path Clear takes it verbatim; the "undoubled" 07-05 reasoning was the non-modulate2x fallback, no reimpl analog). The former ONED far-environment preview residual disappeared with the preview. |
| D-TERRAIN-4 | C | **PERMANENT (runtime safety boundary)** | **Safe terrain-query bounds** (ENG-3 B1): beyond-extent = no-terrain/no-hit vs retail's clamp-to-edge `[orig: @ 0x31a0010/0x319fc0c]`; no-data = clear/NAN vs retail's return-HIT `[orig: @ 0x60ccf7]`; contiguous-atlas bilinear vs the per-quadrant seam flags `[orig: @ 0x31a17f0..]`. Game consumers use these guards. Returning no result outside valid data avoids inventing an edge hit; §Runtime terrain queries carries the retail forms for any consumer that specifically requires them. |
| D-TERRAIN-5 | A | **FIXED (2026-07-13)** | **Top-tier texture/shader source mismatch**: the reimpl incorrectly used its heightmap normal as the t3 detail coefficient, camera-crossfaded near/far textures, float-normalized DBlend, and multiplied an extra terrain tint. the separate heightfield-normal atlas feeds cached-tile alpha; DBlend, paired mip chains, and literal t0..t5 ps.1.4 math are ported. **Corrected 2026-07-15**: the fix's own first reading (t3 = the generated authored-detail B-channel coefficient) was also wrong — t3 is the authored second detail pair (`polytrn_detailmap2` ⊕ `dist2`) at density2; the generated coefficient belongs to the ps.1.1 tiers at stage 7 [`orig: Texture_GenerateNormalMap @ 0x58c070`; `Terrain_GenerateNormalMap @ 0x603210`; `PolyTrn_InitTextures @ 0x60aaa0`; `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`; `PolyTrn_PS14SplatNormalMap source aPs14TexldR0T0T_0 @ 0x7dece0`]. |
| D-TERRAIN-6 | A | **FIXED (2026-07-13)** | **LOD/fog/overlay base-pass semantics**: both raw `lod_sub / 2` sites use the exact clamped eight-family selector; exponential fog uses eye-space depth while linear types use radial distance; ordered `.til` color is composed before terrain lighting. The render-target-alpha recurrence the 2026-08-17 D-TIL-3 fix added within that order was refuted 2026-09-24: the overlay loops run under `COLORWRITEENABLE = 7` and never write page alpha (tiles/til-re.md D-TIL-3) [`orig: render_terrain_sector_batch @ 0x6092a0`; `Render_SetFogState @ 0x58a950`; `PolyTrn_RenderTile @ 0x60da70`]. |
| D-TERRAIN-7 | A | **FIXED (MATCHING, 2026-09-26: the last facet, the page address mode, is the CLAMP the reimpl already sampled)** | **Tile-composition RT/update parity**: the runtime ports retail's 128-record page cache (`TerrainTileCompositionCache`: identity = level, packed source coordinate and routed sector; the age-above-1 LRU claim, first in record order; the TOD-stale eviction and same-frame re-sweep; the record-order point lookup for MATCHTERRAIN and detail foliage; spatial invalidation on destruction and scorch appends; t0 unbound for an unclaimed page) and composes every claimed page before the batch draws with the D3D9 raster: integer pixel positions, the quadrant box levels CLAMP/LINEAR/MIPFILTER POINT, the point-sampled DXT5 `.til` atlas, the WRAP/LINEAR scorch quads, RGB-only overlays, then the DOT3 alpha and the static A-only projections. The 2026-09-24 rendering parity pass closed the refresh cadence and page identity (the content-stamp LRU, stale-while-recompose, the DOT3-byte light epoch and the analytic cold fallback are retired), the raster/filter/mip behavior, the static collector's sphere test and the temp raster's pixel centres; the ordered contributions are the `.til` and scorch loops, both ported (scorch records since #560). Resolved earlier and kept: the max-quality c7/c8 `TerrainTilePageProjection`; DOT3 before silhouettes (`@ 0x60D794..0x60D7C0`, skip gate `@ 0x60E1CE`); the live-probed temp-blue composite state; the PROJSHAD admission/blending/culling/z audit and the skinned rigid collapse; the shared material-animation evaluator (whole-process CTRL/RNG ordering is D-3DI-2). `g_TerrainAdapterCapsStorage.TexOpDisableOrArg2` (0x319FBB8) is the adapter TextureOpCaps & 5 flag (normally set) that picks the scorch stage's `0xFF808080` diffuse, not a sun-angle control, so the 2026-08-23 rejection of a low-sun "density mechanism" stands. The last facet, the page render target's address mode at the terrain and foliage draws, closed 2026-09-26: the zero flags word would select WRAP, but every terrain pass state's intrinsic word carries the stage-0 CLAMP bit (0x1020000 ordinary, 0x1020002 pool light, 0x1000628 page multiply), which `CGfxShader_ApplyPass` ORs into the pass flags and applies after the page is bound, so the page samples CLAMP, the reimpl's clamp-to-edge (the detail foliage pass's stage-1 CLAMP is D-FOLIAGE-7's closure). §2026-09-24 rendering parity pass carries the witnesses `[orig: PolyTrn_RenderTile @ 0x60DA70 (hit @ 0x60DAC0..0x60DAD1, claim @ 0x60DAE4..0x60DB45, stamps @ 0x60DB56..0x60DBF0); Terrain_EvictOldestTodStaleTile @ 0x604600; PolyTrn_RenderFrame @ 0x60EAC0 (sweep @ 0x60F080..0x60F0E3); PolyTrn_BindStageTextures @ 0x604330; terrain_tile_cache_lookup @ 0x604140; Terrain_CollectAndRenderTileModels @ 0x60D250; apply_texture_stages @ 0x68084C..0x680870; PolyTrn_InitTextures @ 0x60C3B0 / 0x60C433 / 0x60C499; CGfxShader_ApplyPass @ 0x683221, @ 0x683265..0x683279; CGfxDevice_ApplyRenderStates @ 0x67E4A9..0x67E4FE]`. |
| D-TERRAIN-8 | A | **FIXED (2026-08-13; coordinate corrected 2026-08-17)** | **Underwater terrain water-noise modulation**: the engine terrain frame stamps `below_water` from the render eye vs the live water height (the bare unguarded strict `<` `@ 0x60fea5` — NO zero sentinel; the water height is plumbed unconditionally), and the shared surface include swaps the ps.1.4 stage-3 dp3 INPUT to the water module's per-frame regenerated noise texture at the swapped `source × 8/512` (`colormap_uv × 16` for the normalized 1024 atlas) texcoord — the witnessed TOP-TIER behavior (the 2026-08-13 selector decode above): the noise rides the PS14SplatNormalMap dp3 on detail2-authored maps, detail2-less splat maps faithfully render NO underwater modulation, and the `saturate(4·t3²)·t0.a` PSShadow pair belongs to the unported ps.1.1 tiers `[orig: below-water flag @ 0x60FEE0 → dword_319FB3C @ 0x60915F; live t3 slot swap @ 0x6043f2; selector @ 0x6044b1..0x604556; texcoord @ 0x609786..0x6097D6]`. Tests: ctest `terrain_frame_compiler` (flag pins) + GUT `terrain_underwater_modulation_test` (the Dvxi5 flip drive). |
| D-TERRAIN-10 | A | **FIXED (2026-07-14)** | **Terrain light-vector coordinate basis**: EnvFile preserves the direct retail getter tuple `g`, not Godot/world XYZ. Retail's D3DCOLOR pack writes GPU diffuse RGB `(g2,g0,g1)`, matching normal-map RGB `(grid X slope, grid Y slope, up)`; the old reimpl `(x,z,y)` pack swapped the horizontal DOT3 axes. Terrain and analytic foliage now pack `(z,x,y)`. Flat 06:00/12:00/18:00 checks could not distinguish the swap, so a non-flat 08:00 oracle pins light bytes `(231,83,187)` and slope alphas `0.8987774/0.0794002` [`orig: Environment_GetLightDirectionFloat @ 0x57d870; Terrain_GenerateNormalMap pack @ 0x603470..0x6034eb; PolyTrn light pack @ 0x60e201..0x60e331; PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e`]. |
| D-TERRAIN-11 | A | **FIXED (2026-08-17)** | **Terrain detail coordinate scale**: retail constructs mesh UV1 as `source × polytrn_detaildensity / 512`; OpenNova had multiplied normalized 1024-atlas UV by density, halving every detail frequency. Runtime mesh UVs, authored detail2, and the underwater stage-3 swap now share the exact source-grid conversion. Deterministic 00TRa A/B probes select 2× with the existing axis at high correlation and reject the UV-swap alternative [`orig: parser @ 0x60f993..0x60f9b3; config load @ 0x60e634..0x60e63b; density/512 write @ 0x6029a0..0x6029aa; UV1 @ 0x602db5..0x602dbe; stage-3 transforms @ 0x609786..0x609810`]. |

| D-TERRAIN-12 | A | **FIXED (2026-09-13)** | **Missing empty-sector flat fallback**: zero sector-grid entries now traverse quadrant 1 unless view `+100` skips them (the live draw pass writes 0; the water-mirror prerender and the PCX screenshot scene write `Env_WaterHeightFixed != 0`, so the reimpl's flat draws ride a mirror-excluded visual layer). The draw carries the high-bit mode through flat mesh selection and zero primary/blend UVs while preserving independent detail/noise coordinates and raw tracked source heights. Every flat draw shares the canonical LOD-0 page, whose base/DOT3 source is UV zero and whose `.til` overlay loop is suppressed; the origin sector can borrow it with the ordinary geometric projection. `terrain_frame_compiler`, `terrain_tile_composer`, `terrain_tile_composition_cache`, and the Godot terrain shader contract pin the bounded behavior. [orig: PolyTrn_RenderFrame @ 0x60EAC0; terrain_render_visible_sectors @ 0x6090C0; decode_terrain_tile_vertices @ 0x602AA0; PolyTrn_RenderTile @ 0x60DA70]. |

The completed passes close D-TERRAIN-5/6/7/8/10/11/12 (D-TERRAIN-7 narrowed
to the page address mode by the 2026-09-24 rendering parity pass and closed
2026-09-26). The terrain data path remains the byte-identical TrnGen port; the
pending grill below is documentation depth around CDEP/traversal and the
single-detail binding.

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

The CDEP/traversal item is documentation depth; no terrain divergence row is
open (D-TERRAIN-7's page address mode closed 2026-09-26; D-TERRAIN-8's
underwater modulation FIXED 2026-08-13). D-TERRAIN-1 and
D-TERRAIN-9 were retired with the ONED terrain preview.

## Cross-references

- Reimpl: `engine/runtime/terrain`, `engine/runtime/terrain_query` (ADR 0020, the world→height seam),
  formerly `engine/formats/tpm` (the TPM1 tile mesh) + `engine/formats/dep` (the depth intermediate), placed under ADR 0030 and removed with ADR 0037 (d57608b3d).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env
  far-colormap bake (#19).
