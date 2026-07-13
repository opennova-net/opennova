# Foliage placement — reverse-engineering record

Structure-mapping record for the original engine's two **procedural foliage
tiers** and their distinct render passes. The reimplementation surface is
`libs/foliage` (`placement.cpp`, `far_mesh_emitter.cpp`, `model_placement.cpp`,
and the dispatchers) and the Godot host `NovaFoliageDispatcher`
(`godot/engine/terrain`). Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-FOLIAGE-…` divergence catalog.

The original PAR-R2 audit was recorded on 2026-07-05. A second read-only IDA
audit on 2026-07-09 retracted the central FAR-tier interpretation: retail does
not synthesize a square, quad, billboard, or ground patch. It copies the full
source 3DI mesh for every accepted FAR candidate. The apparent corner-height
and terrain-color work in the decompilation does not describe the emitted
geometry/color: the source-vertex loop and its unconditional final color write
do. FAR and MODEL therefore share assets and PRNG ancestry, but they do **not**
share geometry, accepted-count caps, terrain fitting, wind shaders, or material
passes.

Note on binaries: `libs/foliage/placement.cpp` was originally ported from
`jodemo.exe` (it cites `sub_5C0240`/`sub_5C6450`/`sub_5C65E0`). Retail confirms
the candidate loop inside `generate_foliage_instances_0 @ 0x5ffdd0`
(`@ 0x600197` is an interior address), including the seed, PRNG, and gates.
A third audit (2026-07-10) witnessed the retail FAR *driver*: the jodemo-era
per-entity quadrant dispatcher (`sub_5C1940` - 4-quadrant walk, 128-LRU,
38.0 near-Z, L-infinity view cull) does not exist in Jointops' FAR path.
Retail collects visible leaf cells during terrain traversal and bakes each
cell ONCE into a persistent per-def slot pool; `generate_foliage_instances_0`
takes `(key, VB, IB, &idx_count, &vtx_count)` - no view center, no radius.
The jodemo dispatcher port and `place_cell`'s view-cull parameters were
deleted with that witness.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| FAR candidate placement | **MATCHING algorithm; key provenance pending** | `placement.cpp` preserves the 36-candidate ROL-hash sequence and gates from `generate_foliage_instances_0 @ 0x5ffdd0`; accepted count is 0..36, not capped at 21; D-FOLIAGE-8 tracks the host's render-keyed rather than native-keyed PRNG seed input |
| FAR feed + slot pool | **MATCHING mechanics; exact traversal inputs pending** | Traversal collects leaf cells within **42.0** of the camera (<=128) `[orig: Terrain_TraverseQuadtreeNode @ 0x60905c -> Terrain_CollectNearFoliagePatches @ 0x603e60]`; per-def slot pools bake each new key ONCE and LRU-evict by frame stamp `[orig: Foliage_UpdateFarCellSlots @ 0x601b30]`; the host runs that pool over a direct 42u enumeration and approximates retained leaf Y bounds from footprint heights; exact traversal/residency inputs remain D-FOLIAGE-7 |
| FAR geometry and vertex payload | **MATCHING (corrected 2026-07-09)** | `far_mesh_emitter.cpp` copies the complete source mesh per accepted candidate, transforms XZ at 1.0 scale, halves source Y, samples terrain below every transformed source vertex, preserves UV/index topology, and emits the witnessed red wind weight `[orig: @ 0x6002DB..0x60030A]` |
| FAR wind and fragment pass | **MATCHING core; exact input/pass split pending** | `foliage_far.gdshader` ports the `Foliage_WindSwayVS` polynomial/axis and `Foliage_LightmapBlendPS`, the witnessed c24.x phase expression, fade knee/slope, refs 180/8, and strict `GREATER` alpha test. The fold-input alpha stand-in, omitted optional patch-cache RGB composites, and second LOW resubmit remain D-FOLIAGE-7; pinned by foliage_shader_contract_test.gd |
| MODEL tier geometry/placement | **MATCHING mechanics; exact terrain driver pending** | Full source mesh, cap 21, 0.75 XZ / 0.5 Y, eight-sample biquadratic fit, per-anchor alpha, and per-tile draw/wind cadence are hosted; D-FOLIAGE-7 tracks the exact visible/occlusion-tested sector-entity feed |
| The `:fd` foliage texture (both tiers) | **MATCHING (ported 2026-07-08)** | D-FOLIAGE-5 FIXED — `bake_fd_rgba` (exact kernel + wrap + `0x808080` fold) via `VegAssets.resolve_slot_fd_textures`; BOTH tiers bind it `[orig: Foliage_LoadDefAssets @ 0x601260]` |
| MODEL material pass | **MATCHING (fog witnessed 2026-07-10)** | Pass table[16] selects the black c6 diffuse for RGB and multiplies `:fd.a × diffuse.a`; the draw runs FOGENABLE-OFF (combined word 0x00440000, unconditional fog latch) so the silhouettes stay unfogged black `[orig: Foliage_DrawModelTileSlot @ 0x601d90 / 0x601e33; CGfxShader_ApplyPass @ 0x68324f]`; `foliage_model.gdshader` is deliberately separate from FAR; duplicate submissions dedup per D-FOLIAGE-10 |

## The FAR tier (`generate_foliage_instances_0 @ 0x5ffdd0`)

### Candidate placement

- **Cell key and empty marker**: the packed signed key supplies the two tile
  coordinates; `0x80000000` marks an empty cell.
- **PRNG seed**: `state = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F)`.
- **PRNG step**: `state = ROL32(state + ROL32(state, 11), 4) ^ 1`; the low
  16 bits drive each jitter/yaw draw.
- **Grid and cap**: 36 candidates in a 6×6 grid, three PRNG draws per
  candidate. The loop ends after candidate 35. There is **no accepted-count
  early-out at 21**: an all-accept cell emits 36 complete source meshes.
- **Range/foliage-map gate**: candidates outside the requested L∞ view range
  reject; `(1 << slot) & Foliage_SampleFarMapMask(x, -z) @ 0x6066d0` must
  survive. **Corrected 2026-07-10** (the 00TRg overcoverage grill): the buffer
  behind `0x6066d0` (kong-misnamed `Terrain_GetSurfaceTypeAtFixedPoint`) is
  the **FOLIAGEMAP**, not the charmap — `sub_605AD0` loads the foliagemap PCX
  ("PolyTrn Foliagemap" `@ 0x605b35`) and remaps every pixel through
  `sub_5FF4E0`: bit(def) set when the pixel equals ANY of the def's four
  `match` bytes (record +0x108..+0x10B; TRN corpus authors one — "match with
  up to four different colors in the foliagemap"); pixel 0 never matches. The
  FAR accessor addresses the remapped map FLAT (`(x & 1023, -z & 1023)
  >> (10 − log2 W)`, no sector routing); the MODEL accessor `@ 0x606620`
  routes the SAME buffer through SectorGrid + the 512-world quadrants. The
  charmap (walking surfaces) is not a foliage input anywhere.
- **Path/spacing gate**: `sub_606490(x, -z, 0x20000)` rejects a candidate within
  2.0 world units of a blocker unless def attrib bit 0 (`forceon`) is set.

The candidate math is in `placement.cpp`; source-dependent work deliberately
lives in `far_mesh_emitter.cpp`. The host still needs the terrain renderer's
exact visible-cell key list and blocker registry (D-FOLIAGE-7).

### Full-source-mesh emission

For every accepted candidate, the loop walks **all source vertices** and then
replicates **all source indices**. It preserves each source UV and does not
synthesize a four-vertex primitive. In retail render coordinates:

```text
renderX = candidateX + srcX·sin(yaw) + srcZ·cos(yaw)
renderZ = candidateZ + srcX·cos(yaw) − srcZ·sin(yaw)
renderY = terrainHeight(renderX, renderZ) + srcY·0.5
```

XZ source scale is exactly **1.0** and Y scale is **0.5**. Terrain height is
sampled independently at the transformed XZ of every source vertex, so the
whole authored mesh bends onto terrain without the MODEL tier's corner/midpoint
fit. In repository coordinates the 3DI importer has already reflected source X;
the equivalent host transform is `rotY(yaw + π/2)`. `emit_far_mesh` applies that
basis directly and replicates the source UV/index topology.

The final emitted D3DCOLOR is not terrain lighting. Although the routine takes
four terrain-color samples and performs intermediate writes, every branch joins
at `0x6002DB`, after which `0x60030A` unconditionally overwrites the color with:

```text
clamp(trunc(srcY * 128), 0, 255) << 16
```

Only red is populated. `Foliage_WindSwayVS` consumes normalized red as a linear
source-height wind weight. The earlier “four terrain samples → per-vertex
foliage color” and half-plus-bias claims are retracted (D-FOLIAGE-1).

### FAR wind and material passes

`Terrain_CreateFoliageVertexShaders @ 0x5ff630` assembles
`Foliage_WindSwayVS`. With `w = vertexRed` and
`phase = renderX + c24.x`, it wraps phase to `[-π, π]` using c27
`(0.159155, 6.2831898, -3.1415901, 0.25)` and evaluates:

```text
P(x) = 1 - 0.5x² + 0.041666601x⁴ - 0.00138884x⁶
         + 0.0000247609x⁸ - 0.00000025239899x¹⁰
renderZ += w * 0.03 * P(x)
```

`setup_water_vertex_shader_constants @ 0x600450` supplies
`c24.x = GetTickCount()*0.003 + Env_WaveOscRing[0]*1.5258789e-6` and
`c24.y = 1`. Retail displaces render Z only (negative Godot world Z).
`foliage_far.gdshader` ports the polynomial and axis exactly. The host forwards
the current weather oscillator sample through its environment/global/dispatcher
seam and binds the complete witnessed c24.x expression.

FAR uses separate high/low pass objects, not the MODEL pass. T0 is `:fd`, T1
is the terrain lightmap/colormap input, and alpha-test refs are **180** (high,
flags `0x02460000`) and **8** (low, flags `0x02560000`). On the pixel-shader
path `Foliage_LightmapBlendPS @ 0x5ff7a0` computes:

```text
rgb = t0 * (t1 * (t1.a*c1 + c0)) * c6.rgb * 8
alpha = t0.a * c6.a
```

`render_terrain_lightmaps @ 0x609de0` selects high when the visible-sector
metric is below 33.0 and its force-low argument is clear. High is not a
mutually exclusive replacement: after the solid high draw it rebinds low with
the wireframe flag and submits the same geometry again. Otherwise it draws low
only `[orig: @ 0x60a653..0x60a694; Terrain_SetupSectorModelDraw @ 0x6007c0]`.
The current slot-wide host has no per-sector draw boundary, so it uses one
high-ref material; exact pass splitting is tracked by D-FOLIAGE-7.

The vertex red wind weight is not used as lighting; v0 is c6. On the
fixed-function fallback stage 0 is `2*T0*diffuse` for RGB and
`T0.a*diffuse.a` for alpha, followed by stage 1 `2*T1*current`. c6 RGB comes
from the terrain lighting factors. c6 alpha is 1 through apparent distance 20,
then `1 - (distance - 20)/22` (with an alternate-path ×0.1). The shader mechanics
are ported; exact sector-owned T1 and c6 source values remain D-FOLIAGE-7.

### The FAR feed and slot pool (witnessed 2026-07-10)

The FAR tier is a bake-once pipeline driven by terrain traversal, not a
per-frame regeneration and not a per-entity quadrant walk:

- **Collect**: `Terrain_TraverseQuadtreeNode @ 0x608a00` calls
  `Terrain_CollectNearFoliagePatches @ 0x603e60` (ex kong
  `collect_nearby_sector_patches`) for frustum-surviving nodes with LOD >= 3
  inside 42.0 (`flt_7DEA3C`, gate `@ 0x60905c..0x609078`). The collector
  recurses to leaves and admits each leaf whose clamped-AABB 3D distance from
  the camera (Y term `|node+52 - camY|`) is <= **42.0** (`@ 0x603f5c`),
  appending `(key, mesh, 0, sector_ox, sector_oz, dist, node)` to
  `Terrain_NearSectorPatchList @ 0x319B2F0` and the packed key to
  `Foliage_VisibleFarKeyList @ 0x319C0F8` - both capped at **128**
  (`@ 0x603f98 / 0x603ff1`). Key layout per axis half: bits 14..10 = sector
  index (x1024 in the packed value, x512 world), bit 9 unused, bits 8..0 =
  world units within the sector; `0x80000000` marks OOB sectors. The HIGH
  half is world X, the LOW half world Z (the generator passes the
  HIWORD-derived coordinate as the X argument of the spacing/FOLIAGEMAP
  slot-mask queries
  `@ 0x600001..0x600009`).
- **Bake once per new key**: after tile rendering, `PolyTrn_RenderFrame
  @ 0x60f0ef` runs def slots 0..3 through `Foliage_UpdateFarCellSlotsForDef
  @ 0x601d50` -> `Foliage_UpdateFarCellSlots @ 0x601b30` (ex kong
  `foliage_lod_update_texture_slots` - its "textures" are the packed keys).
  Resident keys are only re-stamped (`Foliage_FarSlotFrameCounter
  @ 0x2C266D8`); each NEW key evicts the max-age slot record (7 dwords at
  region+38: key, stamp, active, IB base, idx count, vtx count), locks that
  slot's VB/IB stripe (region[15]/[16] = verts/indices per slot; stripes
  sized in `Foliage_InitModelTileBuffers @ 0x5ffcd0`, which also resets the
  1000 record slots), and bakes it with `generate_foliage_instances_0`.
  FAR cell geometry regenerates only on slot eviction - never per frame.
- **Draw per collected patch**: `render_terrain_lightmaps @ 0x609de0`
  bubble-sorts the near list far-to-near (`@ 0x609fb9..0x60a141`), then per
  entry per def: looks up the tile's detail-lightmap render target
  (`sub_6042A0 @ 0x60a1de`, bound as T1 through a world-planar texture
  transform `@ 0x60a220..0x60a356`), binds the pool ranges/`:fd`/passes via
  `Terrain_SetupSectorModelDraw @ 0x6007c0` (high fill + ref 180 / low +
  optional wireframe fill + ref 8), uploads c6 = (lighting-factor RGB,
  fade) through `setup_water_vertex_shader_constants`'s float4 argument
  (`@ 0x60a4ca..0x60a526`) and PS c0/c1 = `PolyTrn_PSConstC0_SkyColorR` /
  `PolyTrn_PSConstC1_LightColorR` (`@ 0x60a53b..0x60a55a`). The per-entry
  draw state: **fade = 1 through distance 20.0** (`flt_7D8E60`), then
  `1 - (d - 20) * (1/22)` (`flt_7DF1BC`) - reaching 0 exactly at the 42.0
  collect edge - with an alternate-path x0.1 (`flt_7C69F4` `@ 0x60a4a1`);
  **high pass when dist < 33.0** (`flt_7DF1C0` `@ 0x60a171`), and after a
  high draw the same geometry is resubmitted on the low pass with the
  wireframe fill flag (`@ 0x60a655..0x60a694`). The fixed-function fallback
  block (`D3DRS_AMBIENT` + material `(0.8, 1.0, 0.7, fade)` `@ 0x60a412..`)
  is skipped on the shader path.

**Host mapping** (`NovaFoliageDispatcher`): the dispatcher enumerates the 16u
cells whose clamped-box 3D distance from the camera is <= 42.0 (terrain
footprint min/max samples approximate the retained leaf Y bounds; Godot's
per-node frustum culling stands in for the traversal's frustum gate), keeps a
per-slot
bake-once pool capped at the 128 collect bound (the retail resident count -
pool header field [37] - is untraced), and per frame only toggles pooled
cell visibility and refreshes the witnessed per-cell fade/pass instance
parameters. Retail T1 is the **terrain patch-cache RT**, sampled at LOD 0
(the render target carries no mip chain). This is not the `.til` overlay list
or a `.til` tile; optional overlay/decal/scorch draws can nevertheless
contribute to the cache entry's RGB when present. The 2026-07-10 regrill
witnessed its base colormap RGB and saturate(N·L) alpha paths (§The terrain
patch-cache RT below) — there is NO detail-splat term in retail foliage T1.
The host binds the colormap alone, so it matches a base-only cache entry but
omits optional composite RGB. Because real terrain colormaps such as Dvxi5 are
24-bit and therefore sample alpha=1, FAR mesh COLOR.gba now carries the
central-difference heightfield normal under each transformed vertex; the
shader reconstructs `saturate(N·L)` against the active sun/moon direction.
c6.rgb carries the witnessed 128/255 avg-detail constant. Exact per-texel
normal-map sampling and optional composite RGB remain D-FOLIAGE-7.

## The MODEL tier (`Foliage_GenerateModelTileInstances @ 0x600980`)

The def's `graphic` 3DI is a REAL asset in retail — the earlier "billboards
only" picture missed a second, model-geometry tier. The 2026-07-08 port-time
witness leg closed all three pins (fold arithmetic, VS constant packing, `:fd`
bake) and CORRECTED four claims from the first grill (scale, driver, the "36
constants" misread, the gate source). IDB renames this leg:
`Foliage_UploadModelTileVSConstants @ 0x600f00` (ex
"set_water_pixel_shader_constants" — its only xref is the foliage draw),
`Foliage_InitModelTileBuffers @ 0x5ffcd0` (ex "CD3DDevice_InitResources"),
`Foliage_FillModelInstanceBuffersLocked @ 0x5ffc40`,
`Foliage_FillInstancedModelBuffers @ 0x5ffa20`,
`Foliage_SampleFoliageMapMask @ 0x606620`, plus the
`Foliage_DefTable_*` family at `0x3162060` and
`Foliage_ModelTileCachePerDef @ 0x2C266F0`, `Foliage_ModelPrngState
@ 0x2C25E70`, `Foliage_ModelWindPhaseCounter @ 0x3162170`,
`Foliage_UseSWVertexProcessing @ 0x2C25E58`.

### The driver — models cluster around sector entities

`[orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]` (the ONLY caller,
4 call sites = def slots 0..3): for each **visible terrain sector entity**
(the .trn/.bms-placed world models), after the water-side split and occlusion
test, if the entity's **view-space depth ≥ 38.0** (transform via the view
matrix `@ 0x2721A00`):

- `alphaRef = clamp(int(4096.0 / ((dist_to_camera >> 16) + 1)), 8, 128)`
  (`flt_7C6F90 = 4096.0`) — near entities get a TIGHT alpha silhouette
  (ref 128), far ones a fat one (ref 8, compensating mip alpha erosion);
- `Foliage_UpdateModelTiles(slot, entityPos, 0x40000, alphaRef)` for each
  def slot — the generator's per-axis candidate radius is the FIXED
  `0x40000` (±4.0 u) around the ENTITY, not a camera radius.

There is NO camera-carpet model dispatch: foliage models exist only as
clusters around sector entities. (The first grill misread the 8..128 value
as a "distance-derived view radius" — it is the alpha-test ref, and the
±4 u radius is constant.)

### Def-asset load and the `:fd` bake

`[orig: Foliage_LoadDefAssets @ 0x601260]` (ex kong "Terrain_InitSectors"),
from `Terrain_Init @ 0x60fd16` via the module def copy (`sub_5FF4C0
@ 0x5ff4c0` → `0x2C25E78`, 4 × 536-byte records; attrib flags at record
byte +532, bit 0 = `forceon`): per def, `sub_5B6160(graphic)` loads the 3DI
under load-context `0x30000F`. The def table (`Foliage_DefTable_*`,
17 dwords per def at `0x3162060`): [0] model handle, [1] vertex array
(40-byte stride), [2] vertex count, [4] submesh[0] index count (u16 read),
[5] submesh[0] index data, [6] bound radius, [7] footprint, [8..10] bound
center (x, 0, z), [11] stampdown texture (record +0x10C, channel
`0x100001`), [13]/[14] per-def GfxResource objects (image objects over the
scratch buffers `unk_2560000`/`unk_2460000` — NOT shader blobs; the first
grill's "blob pair" theory is corrected below), [15] the `:fd` texture,
[16] the draw's pass object.

- **Bounds**: XZ min/max over the raw 3DI vertices → center `(cx, 0, cz)`;
  radius = **max(half-extent X, half-extent Z)** (Chebyshev, not Euclidean);
  **footprint = 0.75 × radius** (`Foliage_DefTable_Footprint @ 0x316207C`).
- **The `:fd` bake** (shared by BOTH tiers — the model draw binds it too):
  texture name = the model submesh[0]'s own texture name (+32 in the
  submesh record), pixels loaded TGA-first, `.dds` fallback
  (`CTerrainTileData_LoadTGAFromArchive @ 0x56e570`,
  `decompress_dds_from_pff32 @ 0x56e450`). Alpha channel smoothed by a
  3×3 kernel — center 4, N/S/E/W edges 1, corners 2, sum/16, power-of-two
  wraparound at the borders. RGB then FLATTENED to exactly `0x808080`
  (`out = 0x808080 | (px & 0xFF808080)` — per channel `(c & 0x80) | 0x80 =
  0x80` always; alpha byte preserved = the smoothed value). Registered as
  `"%s:fd"` via `Texture_LoadByNameWithChannel` (channel `0x100000`) and
  filled via `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`. The
  bake is therefore an alpha-silhouette asset. FAR supplies visible RGB through
  its terrain-light combine; MODEL selects black diffuse RGB and uses only the
  baked alpha before fog.
- **Static instancing buffers**: `[orig: Foliage_InitModelTileBuffers
  @ 0x5ffcd0]` sizes the per-def draw pair for exactly **21 instances**
  (VB = 756·vtx_count bytes = 21 × 36-byte stride; IB = 42·idx_count =
  21 × u16), then `Foliage_FillModelInstanceBuffersLocked @ 0x5ffc40` →
  `Foliage_FillInstancedModelBuffers @ 0x5ffa20` fills 21 copies of the
  model ONCE (static):
  - `pos.x = (x − cx) / (2·radius) + 0.5`, `pos.z = (z − cz) /
    (2·radius) + 0.5` — XZ **normalized to [0,1] over the bound square**;
  - `pos.y = y × 0.5` — **the model height is HALVED in the VB**;
  - diffuse dword = `i × 0x01010101` (instance index in every channel);
  - uv = source floats [6],[7]; the FVF normal slot carries filler
    (src[5], src[4], src[5]) the VS never reads;
  - IB = model indices + `i × vtx_count`.

  **Scale correction (the too-tall-trees root cause)**: the first grill's
  "no scale field → native 3DI units" was wrong. No per-def scale FIELD
  exists, but the pipeline bakes constants: with the VS bilinearly mapping
  normalized XZ onto footprint corners at ±0.75·radius, the effective
  render scale is **0.75 on XZ and 0.5 on height**. In host coordinates the
  importer has already reflected source X, so the equivalent upright basis is
  `rotY(yaw + π/2)`; this is the same retail-to-Godot axis correction used by
  the FAR emitter, with MODEL's different horizontal scale.

### The tile walk

`[orig: Foliage_UpdateModelTiles @ 0x601f50]`: 4 quadrant tiles = the 16-u
cells (snap mask `0xFFF00000`) overlapping entity ±0x80000 (±8 u); tile key
= `(snapX & 0x7FFF0000) | (((snapZ + 0x100000) >> 16) & 0x7FFF)` — the
+16 u on the Z half compensates the negated local axis (below). Per-def
1000-entry cache (342-dword entries at `0x2C266F0 + 342935·def` dwords;
entry: [0] key, [1] last-touch frame stamp, [3] count×vtx, [4] count×idx,
[5..340] 21 × 16 constant floats, [341] instance count). On hit: touch;
regenerate only on the 8-frame stagger `(frame + 2·def) & 7 == 0`. On miss:
evict the max-age entry and generate. Draw only when count > 0.

### Instance generation — the 8-sample ground fit

`[orig: Foliage_GenerateModelTileInstances @ 0x600980]`, per tile:

- **PRNG**: `state = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F)`;
  step `state = ROL32(state + ROL32(state, 11), 4) ^ 1`, value =
  `(u16)state` — three draws per candidate (jitter A, jitter B, yaw).
- **Grid**: 36 candidates; local A = `1 + (i % 6)·2.6 + draw·1.8/65536`,
  local B same with `i / 6` (constants 2.6 `@ 0x7D8E50`, 1.8/65536
  `@ 0x7DE9CC`, margin 1.0) — the tile spans 16 u. Yaw = `draw · 2π/65536`
  (`@ 0x7CD4DC`).
- **Axis form**: candidate world X = `(keyHigh + localA) · 65536`; world Z
  = `(keyLow − localB) · 65536` — the LOCAL B axis runs NEGATIVE world Z
  (hence the walk's +16 u key bias; the tile still covers
  `[snap, snap+16]²`).
- **Gates**, in order: per-axis range `|world − entity| ≤ 0x40000`; path
  spacing `sub_606490(x, −z, 0x20000)` skipped on attrib bit 0 `forceon`
  (`byte_2C2608C`, record +532); **foliage-map mask** `(1 << def) &
  Foliage_SampleFoliageMapMask(x, z) @ 0x606620` — the model tier routes the
  match-remapped FOLIAGEMAP through SectorGrid + the 512-world quadrants
  (resolution-shift downsample). The FAR gate `@ 0x6066d0` reads the SAME
  remapped buffer flat (corrected 2026-07-10 — its former "charmap
  surface-type" reading is retracted; see the FAR gate above).
- **Corners**: k = 0..3 at `(A, B) = (k&1 ? +F : −F, k&2 ? +F : −F)`,
  F = footprint; corner local = `(LA + A·cos − B·sin, LB + A·sin +
  B·cos)`; heights `h[k] = Terrain_SampleHeightBilinear @ 0x6067b0`
  (÷65536) at the corner world positions.
- **Edge midpoints and the fold**: 4 more height samples at the edge
  midpoints `mid(c0,c2) = −A edge`, `mid(c1,c3) = +A`, `mid(c0,c1) = −B`,
  `mid(c2,c3) = +B`; per edge the SAG `D_edge = h_mid − (h_cornerA +
  h_cornerB)/2`; folded to `E_A = (D_−A + D_+A)/2`, `T_A = D_+A − E_A`,
  `E_B = (D_−B + D_+B)/2`, `T_B = D_+B − E_B`.
- **The 64-byte constant block** (16 floats per instance, cache entry
  dwords 5+16i): `[0..3]` corner B-coords (tile-local, render-Z basis),
  `[4..7]` corner heights, `[8..11]` corner A-coords, `[12..15]`
  `(E_A, T_A, E_B, T_B)`. Cap: 21 accepted; entry [3]/[4] = count × the
  def's vertex/index counts — **the full 3DI geometry stamps per
  instance**.

### The draw and the vertex shader

`[orig: Foliage_DrawModelTileSlot @ 0x601d90]` (D3D9 device):
`SetSoftwareVertexProcessing(Foliage_UseSWVertexProcessing)` bracket,
`Foliage_UploadModelTileVSConstants @ 0x600f00`, bind the def's `:fd`
texture + pass object (`GfxShader_ApplyPassChecked` — foliage alpha-tests,
pass bit 0x40000; see
[render/render-material-re.md](../render/render-material-re.md)),
`CGfxDevice_SetAlphaTestRef(alphaRef)` (the driver's distance value),
`SetVertexShader(Foliage_GridPlacementVS)`, `SetFVF(0x152)`
(XYZ|NORMAL|DIFFUSE|TEX1 = 36-byte stride — **the first grill's "36 VS
float4 constants per tile" was a misread of `SetStreamSource(..., 36)`**),
`DrawIndexedPrimitive(TRILIST, verts = count·vtx, prims = count·idx/3)`.

`[orig: Foliage_UploadModelTileVSConstants @ 0x600f00]` uploads: c0-c3 =
transposed WVP; c4 = `(1023, 2, 1, 0)`; c5 = fog params; **c6 = oD0 vertex
color = `(0, 0, 0, 1)` at this call site**; c7/c8 (planar colormap rows)
SKIPPED — stale from the quad path; c9 = `(sin(counter·0.001)·0.08, 1, 0,
0)` — the model-tier wind term, counter incremented once per tile draw
(`Foliage_ModelWindPhaseCounter`); then per instance i:
`c[12+4i .. 15+4i]` = the constant block with row 0 + `−(snapZ+16)` (the
render-Z base) and row 2 + `snapX` (the world-X base), heights and fold
raw.

`Foliage_GridPlacementVS` is created in `[orig:
Terrain_CreateFoliageVertexShaders @ 0x5ff630]` by `D3DXAssembleShader`
from an INLINE vs_1_1 assembly text (the second string; the first is the
FAR tier's `Foliage_WindSwayVS`) — the "decode the blobs" pin dissolved:
`unk_2460000`/`unk_2560000` are texture-image scratch for `sub_679630`
objects, and the true shader source is plaintext in the binary. The
witnessed placement math, per vertex:

- inputs: `v0` = (x_n, y_half, z_n) from the static VB, `v5.x` = instance
  index/255, `v7` = uv;
- `a0 = v5.x × 1023` (≈ 4i, exact for i ≤ 21) → constants at `c[a0+12]`;
- bilinear weights `w = ((1−x_n)(1−z_n), x_n(1−z_n), (1−x_n)z_n,
  x_n·z_n)` over the 4 corner rows: renderZ = `w·c[a0+12]`, ground height
  = `w·c[a0+13]`, worldX = `w·c[a0+14]` — **the yaw rotation lives
  entirely in the corner geometry**;
- sag: `u = 2x_n − 1`, `v = 2z_n − 1`; height += `(1−v²)(E_A + u·T_A) +
  (1−u²)(E_B + v·T_B)` via `c[a0+15]` — a biquadratic 8-sample fit
  (exact at the 4 corners AND reproduces each D_edge exactly at the 4 edge
  midpoints);
- vertex rise: `pos += y_half × c9` → height += `y × 0.5` and renderZ
  (−worldZ) += `y_half × sin·0.08` — height-weighted wind on the world-Z
  axis only;
- `oPos = WVP·pos`; `oD0 = c6`; `oT0 = v7`; `oT1 = (dp4 c7, dp4 c8)` =
  the planar colormap projection; fog from clip-Z vs c5.

### The MODEL material pass (resolved 2026-07-09)

The open color-chain theory is closed. The def pass object is table field [16],
created with one texture stage and flags `0x00440000`. The draw binds `:fd` as
T0. Stage 0 selects diffuse argument 2 for RGB and multiplies `T0.a` by
diffuse alpha. `Foliage_GridPlacementVS` emits c6 = `(0,0,0,1)`, so MODEL's
unfogged RGB is deliberately black while `:fd` supplies the alpha silhouette;

### The FAR draw constants — 2026-07-10 trust-but-verify grill

Witnessed at the instruction level (session addenda; IDB comments landed):

- **c0/c1 identity**: `init_terrain_lighting_color_ramps @ 0x604ee0` writes
  `PolyTrn_PSConstC0_SkyColorR ← Env_SkyBlock` and `PolyTrn_PSConstC1_LightColorR
  ← Env_LightBlock` — the kong PARAMETER names are inverted (the caller
  `Render_TerrainScene @ 0x610e8b..0x610ea1` pushes LightBlock as arg1, SkyBlock
  as arg2); the C0/C1 global names are right. The FAR PS fold `t1*(t1.a*c1+c0)`
  is therefore `patchRT.rgb * (patchRT.a * LIGHT + SKY)`. The host now maps
  that to `terrain_n_dot_l * opennova_sun_light + opennova_sky_ambient`, with
  `terrain_n_dot_l` reconstructed from the underlying heightfield normal;
  sampling a 24-bit colormap's implicit alpha=1 was not matching. The same
  c0/c1 registers are re-uploaded per FAR patch
  `[orig: @ 0x60a53d / @ 0x60a552]`. An NV-ish special path feeds constants
  `0x101010 / 0xFFF0F0F0` `[orig: @ 0x610e53]`.
- **c6.rgb identity**: the per-patch VS color block builds
  `flt_319F9D0[rgb] × flt_319F9E0[rgb]` on the multitexture path
  `[orig: @ 0x609efe..0x609f30]`. `flt_319F9D0` = the average detail color —
  **128/255 = 0.50196 constants on the blendmap tier** (terrain-re.md §Texture
  build) — and `flt_319F9E0[rgb] = (1,1,1)` constants `[orig: writer @ 0x604ffa
  region]`. So PS-path c6.rgb ≈ **0.50196 neutral**: the reimpl's 0.5 was right
  to 0.4%; `foliage_far.gdshader` now carries 128/255 exactly. The
  fixed-function path folds `flt_319FA50[rgb] × flt_319F9D0[rgb] × 2.0`
  `[orig: @ 0x609f32..0x609f70; flt_7C3B90 = 2.0]` with
  `flt_319FA50 = 0.707·LIGHT + SKY` per channel `[orig: @ 0x604ee0]`, and the
  same trio feeds D3D light 4's AMBIENT for the FF fallback
  `[orig: SetLight(4)/LightEnable(4) @ 0x609f94/0x609fa4]`.
- **Fade + pass constants confirmed at the byte level**: knee 20.0
  (`flt_7D8E60`), slope 1/22 (`flt_7DF1BC = 0x3D3A2E8C`), high-pass threshold
  33.0 (`flt_7DF1C0`, gate `[orig: @ 0x60a171]`), fade → c6.a with the
  distance from the patch record `[orig: @ 0x60a45d..0x60a483]`. NEW: the
  SECOND call (arg_8 — the low/wireframe resubmit) forces the LOW pass
  `[orig: @ 0x60a193]` and multiplies the fade by **0.1** (`flt_7C69F4`)
  `[orig: @ 0x60a4a8]` — the resubmit renders at 10% fade. The host models
  fade+ref per cell but does not yet run the second draw (in-row at
  D-FOLIAGE-7).
- **The per-def pass objects**: the HIGH pass applies `def[14]`
  (`dword_3162098`, ref 180 `[orig: @ 0x6008eb/0x6008f5]`) and the LOW pass
  `def[13]` (`dword_3162094`, ref 8 `[orig: @ 0x60090e/0x600931]`) — correcting
  this record's earlier "[13]/[14] are GfxResource image objects" attribution.
- **The draw descriptor walk**: `Terrain_SetupSectorModelDraw @ 0x6007c0`
  searches the per-def FAR slot pool (the 7-dword records at region+38) for the
  requested cell key and fills the draw descriptor (VB base = poolIdx ×
  per-def stride, index/vertex counts from the record, primitive count = idx/3,
  FVF 0x152, `Foliage_WindSwayVS`); fog mode 8 (VS fog) when the wind VS
  exists `[orig: @ 0x6008c1]`. `Terrain_FindSectorPatchRT @ 0x6042a0` (renamed
  this session from `sub_6042A0`) resolves the sector/quadrant terrain
  patch-cache render target with progressive LOD masks — T1 is that RT,
  sampled through the world→patch UV matrix staged at
  `[orig: @ 0x60a220..0x60a356]`.

Renames APPLIED 2026-07-12 (maintainer OK'd with the rebuild slice):
`render_terrain_lightmaps @ 0x609de0 → Foliage_RenderFarPatches` (it renders
the FAR foliage patches over terrain patch-cache RTs; the old name was a kong
misnomer),
`Terrain_SetupSectorModelDraw @ 0x6007c0 → Foliage_SetupFarSlotDraw`;
`idb_save` checkpointed.

IDB changes this session: `sub_6042A0 → Terrain_FindSectorPatchRT`; witness
comments at `0x609efe`, `0x60a45d`, `0x60a4a8`, `0x60a171`, `0x60a53d`,
`0x6007eb`, `0x604ee0`; `idb_save` checkpointed.

### The terrain patch-cache RT, the MODEL fog, and the duplicate draws — 2026-07-10 regrill

The play-test regrill (far foliage flickering/black, near foliage bright)
witnessed the FAR T1 content end to end and closed the MODEL fog and
duplicate-draw questions:

- **What the terrain patch-cache RT contains.** `PolyTrn_RenderTile @ 0x60da70`
  bakes each of 128 cache entries with fullscreen quads (FVF `0x2C4`; the ctx
  float the
  helpers set is the QUAD Z = 0.5, not an alpha `[orig: @ 0x60dcf9]`). The
  base pass `PolyTrn_TileBakeBasePass` (ex `dword_319F950`, mode `0x20650`;
  non-multitex `0x20620`) draws the colormap quadrant with
  `MODULATE2X(TEXTURE, DIFFUSE)` and quad diffuse `0x00808080` — rgb ≈
  colormap × 1.004, and the diffuse ALPHA BYTE IS 0, so the base alpha op
  `MODULATE(TEXTURE, DIFFUSE)` writes **patch-cache RT alpha = 0**
  `[orig: @ 0x60dce5..0x60dd5a; mode decode: color family 0x600
  @ decode_mode_color_stage 0x6814b5, alpha nibble 0x50
  @ decode_mode_alpha_stage 0x680d65]`. Optional overlay/decal/scorch quads
  (including `.til` overlays when present) can composite additional RGB over
  that base. This patch-cache is not the `.til` placement data structure; the
  overlay list is merely one optional input to its composition. After those
  composites, `PolyTrn_TileBakeDot3LightPass` (ex `dword_319F8F8`) draws a final
  quad whose diffuse packs the light direction (`(d+1)·127.5` per channel):
  stage 0 = `DOTPRODUCT3(TEXTURE=TrnNMap quadrant, DIFFUSE)` in BOTH color
  and alpha ops, stage 1 = color `SELECTARG1(TFACTOR)` / alpha
  `SELECTARG2(CURRENT)`, framebuffer blend ONE/ONE additive — and the
  multitexture path sets TFACTOR = 0, so the pass adds **nothing to rgb and
  saturate(N·L) to alpha** `[orig: @ 0x60e38a; TFACTOR @ 0x60e1fc; desc
  layout via RenderState_ApplyToDevice @ 0x681920; blend fields via
  GfxBlend_ApplyToDevice @ 0x6817d0]`. **Final terrain patch-cache RT: base-only
  rgb ≈ the colormap (NO detail splat, NO noise), with optional
  overlay/decal/scorch RGB composition; alpha = saturate(N·L) with the CURRENT
  light direction** — which is why the cache stamps `Env_TodMinutesElapsed`
  (`@ 0x60dbc0`): entries rebake when the TOD moves.
- **The ground consumes the same RT.** The sector mesh draw resolves its t0
  through the SAME tile cache (`CD3DDevice_FindBestTexturePermutation
  @ 0x604392` → dynamic slot `0x3266E8C`, LRU re-stamped), so the terrain PS
  fold `(t0.a·c1 + c0)` consumes **t0.a = patch-cache RT alpha =
  saturate(N·L)** — not
  the raw colormap alpha (which the bake multiplies by 0). Retail
  ground = `(N·L·LIGHT + SKY) × colormap × 2·dp3 × 4·splat`; retail foliage
  = `fd(0.502) × patchRGB × (N·L·LIGHT + SKY) × c6 × 8` — foliage carries NO
  detail/normal modulation and compensates with c6 = the AVERAGE detail
  color, i.e. **foliage = ground-at-average-detail** (0.502 × 0.502 × 8 ≈
  2.016 ≈ 4·avgSplat × 2·avgDp3). The avg-detail constants are 128/255 ONLY
  on the blendmap tier (`PolyTrn_HasBlendmap && tier ≥ 1`); single-detail
  terrains store the REAL pixel average of the detail texture (fallback 128)
  `[orig: PolyTrn_InitTextures @ 0x60ab00]`.
- **Duplicate MODEL draws overwrite, they do not coexist.** Each
  `Foliage_DrawModelTileSlot` call issues its `DrawIndexedPrimitive`
  immediately with z-write on and default `LESSEQUAL`, so when several
  qualifying sector entities submit the same tile in one frame the LATER
  draw's fragments overwrite the earlier ones — the framebuffer shows the
  LAST submission's alpha ref and wind phase `[orig: @ 0x601e33]`. The wind
  counter still advances once per SUBMISSION (`@ 0x600f00` pre-increments
  per call).
- **FAR pass words.** High `0x02460000` / low `0x02560000` both carry
  FOGENABLE (`0x20000`) — the FAR tier fogs — and the LOW word additionally
  carries `0x100000` = **z-write OFF** (the dissolving far band does not
  write depth) `[orig: pass-flag decode @ CGfxShader_ApplyPass
  0x683232/0x68324f]`.

Host fixes shipped with this regrill (PR #219): `foliage_far.gdshader` binds
the colormap as the base-only terrain patch-cache RT stand-in and DROPS the
former colormap ×
detail-splat × 2 recompose (the splat stamping made near tufts up to ~2×
brighter than the ground and its forced-LOD-0 detail sampling aliased at
20–42u — the reported shimmer/black shards); `foliage_model.gdshader` drops
its fog fold (FOGENABLE-off witness above); the dispatcher renders ONE batch
per (slot, tile) per frame carrying the LAST submission's draw state
(D-FOLIAGE-10).

2026-07-12 PR #233 follow-up: that regrill's colormap-alpha stand-in was
invalid for actual 24-bit terrain TGAs (`alpha=1`), which applied full LIGHT
to every FAR fragment and produced the reported washed-out foliage. FAR cells
now pack their sampled heightfield normal into otherwise-free COLOR.gba and
evaluate the current-light N·L fold in the shader. Both FAR and MODEL also
stopped writing Godot `ALPHA`: retail uses the shader alpha only for
D3DCMP_GREATER alpha testing, with alpha blending disabled, so surviving
fragments are opaque cutouts. The host retains per-vertex/interpolated normals
rather than retail's per-texel patch-cache normal map, and optional composite
RGB remains omitted under D-FOLIAGE-7.

IDB changes this session (regrill): `dword_319F950 →
PolyTrn_TileBakeBasePass`, `dword_319F8F8 → PolyTrn_TileBakeDot3LightPass`,
`dword_319F8FC → PolyTrn_TileBakeDot3LightPassAlt`, `dword_31A13D8 →
PolyTrn_HasBlendmap`; witness comments at `0x60dce5`, `0x60dcf9`, `0x60e38a`,
`0x604392`, `0x601e33`, `0x600f92` (c5 = the VS fog vector, ln(64) form),
`0x60ab00`; `idb_save` checkpointed.

MODEL does not run the FAR lightmap blend, and it does not fog: the draw
applies the def pass with mode 0, so the combined word is exactly the pass's
`0x00440000` (ALPHATEST | CULLNONE) — no FOGENABLE (`0x20000`) — and
`CGfxShader_ApplyPass` latches the fog state UNCONDITIONALLY from the
combined word, committing `D3DRS_FOGENABLE = FALSE`
`[orig: Foliage_DrawModelTileSlot @ 0x601e33; CGfxShader_ApplyPass
@ 0x68324f]`. The c5 fog constants uploaded alongside c6 are dead on this
pass (oFog is ignored with fog disabled). Retail far models are therefore
UNFOGGED pure-black alpha silhouettes at every distance; this record's
earlier "distance fog can lift that black toward the fog color" sentence is
RETRACTED (2026-07-10 regrill). `foliage_model.gdshader` expresses this
separate pass, closing D-FOLIAGE-6 `[orig: Foliage_LoadDefAssets @ 0x601260;
Foliage_DrawModelTileSlot @ 0x601d90; Foliage_UploadModelTileVSConstants
@ 0x600f00]`.

The draw contract is per driver invocation: the sector entity supplies the
anchor-derived alpha ref, a shared cached tile is drawn again when reached from
another qualifying entity, and `Foliage_ModelWindPhaseCounter` increments once
per tile draw. The host preserves those duplicate draws instead of deduplicating
`(slot,tile)`, carries alpha per anchor, and advances wind per draw. The remaining
gap is upstream: the host does not yet receive retail's exact visible,
occlusion-tested sector-entity stream (D-FOLIAGE-7).

## Host upstream seams still open

The FAR feed, slot pool, per-patch fade/pass draw state, and terrain
patch-cache RT base/DOT3 paths are witnessed (see The FAR feed and slot pool
and §The terrain patch-cache RT above). The remaining approximations, recorded
together as D-FOLIAGE-7:

- the terrain patch-cache RT RGB composition (retail: base colormap plus any
  optional overlay/decal/scorch contributions; host: colormap only);
- the fold-input sampling granularity (retail: patch-cache RT saturate(N·L)
  from the per-texel generated normal map; host: the same current-light fold
  from central-difference heightfield normals packed per FAR vertex and
  interpolated across the source mesh);
- the exact visible and occlusion-tested sector-entity stream that drives
  MODEL (host: placed world objects with a view-depth range gate);
- the world/path blocker registry consumed by the 0x20000 spacing gate
  (host: always-clear);
- the conditional second low wireframe resubmit after a high-pass draw
  (host: single pass per cell);
- the retail FAR pool residency count (header field [37]; host: the 128
  collect cap).

The host's direct 42u disc enumeration (vs the traversal's frustum-gated
collect) is a coverage-equivalent adapter: retail skips collecting off-frustum
cells, the host bakes them and lets per-node frustum culling drop the draws.

## The 2026-07-12 pre-PR review round

An adversarial multi-agent review of the rebuilt branch (4 lenses, every
finding refutation-verified) before the PR opened. Confirmed findings and the
fixes, all landed in the same round:

- **The editor half of the z-mirror (D-FOLIAGE-9(c))**: the runtime read was
  fixed and measured, but ONED's preview/brush/eyedropper still resolved the
  foliage map at the un-negated (+render) row via the editor-mesh chain —
  the whole editor was internally consistent yet z-mirrored against the
  game's witnessed read (`Foliage_SampleFarMapMask @ 0x6066d0` /
  `Foliage_SampleFoliageMapMask @ 0x606620` both index (−z); heights index
  (+z) — the foliagemap alone carries the PCX row-order compensation). Fix:
  `NovaTerrainData.world_to_source_coords_wrapped` exposes the runtime
  kernel form (& 0xF sector wrap, `coords_runtime_options`), and the three
  editor surfaces address the gate texel at −z through it. The
  editor-options kernel bounds-rejects the negated region instead of
  wrapping — a bare sign flip in the old chain would have blanked the
  preview.
- **Per-anchor stagger regen restored (D-FOLIAGE-11 correction)**: the
  same-frame regen dedup's determinism premise was false (the accept gate is
  anchor-relative ±4u `[orig: Foliage_GenerateModelTileInstances @ 0x600980]`),
  so the cache froze the FIRST touching anchor's subset where retail ends
  the frame on the LAST. Regen now runs per touching anchor
  `[orig: Foliage_UpdateModelTiles @ 0x601f50 regenerates per hit]`; the
  host's repeat-submission upsert re-borrows the regenerated instance view
  (the earlier borrowed pointer dies with the entry's reallocation).
- **Content stamps made dispatcher-monotonic**: per-entry generation counters
  could collide across an evict/re-adopt cycle and freeze a stale MultiMesh
  upload; `ModelCacheEntry.generation` now stamps from a dispatcher-wide
  monotonic counter.
- **The height-sampler "no terrain" contract implemented**: the documented
  `<= -1e6` sentinel was consumed as a real height — FAR bent blades to the
  sentinel (and the ×65536 cast overflowed int32), the MODEL fit poisoned its
  corners. `HEIGHT_INVALID` (placement.h) now flows the contract: the FAR
  emitter drops the instance, the MODEL fit rejects the candidate. A host
  concept — retail's wrapped world always resolves a height.
- **`:fd` texture swaps re-create the shared FAR materials eagerly**: the
  lazy re-adopt and the per-frame wind-phase refresh both key off material
  validity, so deferring recreation to the next new-cell bake left resident
  cells on the stale texture with frozen wind.
- **Comment corrections**: the FAR gate comments described a "raw
  charmap/surface-map byte" — the record's own 2026-07-10 correction stands
  (the FOLIAGEMAP, match-remapped; the charmap is not a foliage input
  anywhere); the deleted jodemo quad dispatcher's residual mentions dropped;
  the ctest group comment no longer advertises deleted tests.
- **Probe corrections**: `runtime_scene_probe` returned a source-space
  position (the game grows the texel at the NEGATED z); the 00TRg capture
  probe shed a leftover red debug-material override and gained a wind-sway
  A/B pair plus a hide-foliage attribution switch (which pinned the 00TRg
  yellow-green sky blob on the env layer, not foliage).
- **Dispositions normalized (ADR 0022)**: the catalog's provisional "HOST
  TRANSLATION" term is not in the normative vocabulary; D-FOLIAGE-10/11/12
  are PERMANENT (host translation) with ADR 0022 register entries, ratified
  with this slice's merge.

## 2026-07-12 full-implementation validation

A post-rebuild contract audit found and fixed implementation defects that were
not represented in the divergence catalog:

- all four repeated `match` bytes now survive TRN/TPJ load-save-load and feed
  both FAR and MODEL; ONED preserves shared-match OR semantics instead of
  canonicalizing definitions onto orphaned pixels;
- FAR now uses the witnessed flat wrapped FOLIAGEMAP address while MODEL keeps
  its distinct sector-routed address; the former comparison probe had compared
  the routed accessor to a wrapper around itself;
- FAR and MODEL explicitly reject alpha equal to the reference, matching
  `D3DCMP_GREATER` rather than accepting equality through Godot scissor state;
- resident FAR cells rebind a recreated material when the colormap source
  changes, and MODEL's stagger clock advances through anchorless frames;
- FAR collection clamps camera Y to a sampled cell-footprint height range;
  editor height edits/undo and in-place definition edits invalidate bake-once
  preview state; the runtime probe follows the current FAR/MODEL node names.

This validation does **not** establish a full retail implementation. The
evidence/host gaps in D-FOLIAGE-7 and the native-key provenance gap in
D-FOLIAGE-8 remain open. They are kept explicit rather than being inferred
closed from green host regressions.

## D-FOLIAGE divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FOLIAGE-1 | A | **FIXED / false premise retracted (2026-07-09)** | The four terrain-color samples and intermediate writes in the FAR generator are dead with respect to emitted color. After the join at `0x6002DB`, `0x60030A` unconditionally stores red-only `clamp(trunc(srcY*128),0,255)<<16`; `far_mesh_emitter.cpp` emits that source-height wind weight. The former per-vertex terrain-color and half-plus-bias claims are withdrawn. |
| D-FOLIAGE-2 | A | **FIXED (2026-07-12 alpha-state correction)** | FAR runs `Foliage_LightmapBlendPS`: `rgb=t0*(t1*(t1.a*c1+c0))*c6.rgb*8`, `a=t0.a*c6.a` `[orig: @ 0x5ff7a0]`. Alpha feeds the enabled D3DCMP_GREATER test; foliage blending is disabled, so surviving fragments are opaque. The port lives only in `foliage_far.gdshader`; MODEL has a different fixed-function pass. Exact sector T1 RGB and high/low resubmission ride D-FOLIAGE-7. |
| D-FOLIAGE-3 | A | **FIXED (2026-07-09)** | `foliage_far.gdshader` ports `Foliage_WindSwayVS`: source-height red weight, c27 phase wrap, the witnessed tenth-order polynomial, amplitude 0.03, and render-Z-only displacement `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; constants @ 0x600450]`. The host binds the exact `GetTickCount()*0.003 + Env_WaveOscRing[0]*1.5258789e-6` phase expression. |
| D-FOLIAGE-4 | B | **FIXED (corrected 2026-07-09)** | MODEL stamps full source geometry with cap 21, XZ 0.75/Y 0.5, yaw-only eight-sample ground fit, anchor-derived alpha, duplicate tile draws per qualifying anchor, and per-tile-draw wind counter `[orig: @ 0x600980, 0x601f50, 0x601d90, 0x600f00]`. The former shared-shader/shared-cap and FAR ground-patch addenda are retracted; FAR instead uses `far_mesh_emitter`. Exact upstream entity visibility rides D-FOLIAGE-7. |
| D-FOLIAGE-5 | B | **FIXED (2026-07-08)** | Both tiers bind the model submesh[0] `:fd` bake: wrapped 3×3 alpha kernel and flattened `0x808080` RGB `[orig: Foliage_LoadDefAssets @ 0x601260; Foliage_DrawModelTileSlot @ 0x601d90]`. |
| D-FOLIAGE-6 | C | **FIXED (2026-07-12 alpha-state correction)** | MODEL's table[16] pass is now witnessed: one T0=`:fd` stage, flags `0x00440000`, RGB selects c6 diffuse `(0,0,0,1)`, alpha multiplies `T0.a*diffuse.a`. The alpha-tested survivors are opaque, unfogged black cutouts; `foliage_model.gdshader` ports that pass without Godot transparency rather than sharing FAR's lightmap combine. |
| D-FOLIAGE-7 | A | WITNESSED-READY-DEFERRED (narrowed 2026-07-12 lighting fix) | The 42.0/128 FAR collection limits, bake-once slot pool mechanics, per-patch fade/high-low draw state, terrain patch-cache base/DOT3 light fold, and opaque alpha-test state are witnessed. Remaining approximations: host direct-disc enumeration plus Godot culling instead of the retail traversal/frustum feed; sampled footprint heights instead of retained leaf Y bounds; colormap-only RGB instead of optional overlay/decal/scorch patch-cache composition; per-vertex/interpolated heightfield normals instead of the patch cache's per-texel normal map; the MODEL visible/occlusion-tested sector-entity stream; blocker registry; second low wireframe/z-write-off resubmit; and retail pool residency count. c6.rgb remains exact on blendmap terrains and approximate on single-detail terrains. |
| D-FOLIAGE-8 | A | OPEN (minted 2026-07-10; flat sampler corrected 2026-07-12) | FAR cell-key provenance: retail keys pack native sector+offset coordinates (always in the wrapped [0,1024) domain) and the per-cell PRNG seeds from that key; the host's FAR cells remain keyed in Godot render space (z = −native, signed near the origin), so per-cell jitter patterns diverge. The FAR data accessor now implements the witnessed flat `(x & 1023, −z & 1023)` FOLIAGEMAP read directly instead of routing through MODEL's sector accessor. Until keys move native, `place_cell`'s pre-negation presents native z at the host callback, so runtime and preview retain one explicit host-key compensation to land on the retail native map row. Fix direction: native-keyed collect + native placement with a render-space conversion at height/emission; then remove that compensation. |
| D-FOLIAGE-9 | B | **FIXED (2026-07-12 — stacked placement and lighting causes)** | The 00TRg no-rasterize report combined z-mirrored foliage-map reads with a capture probe that loaded the mission while its World node stayed hidden. MODEL, editor brush, preview, and eyedropper now share the witnessed negated-z sector route; the probe follows the shell's visible-world lifecycle. FAR's former wrapper around that routed accessor was superseded by the direct flat sampler recorded in D-FOLIAGE-8, with an explicit temporary sign compensation for the still-render-keyed host cells. The remaining washed-out Dvxi5 foliage came from treating a 24-bit colormap's implicit alpha as N·L=1 and enabling Godot transparency on retail alpha-test-only passes; both are corrected above. |
| D-FOLIAGE-10 | C | PERMANENT (host translation, minted 2026-07-10; ADR 0022 register) | Duplicate MODEL tile submissions: retail immediate-mode draws the shared (slot, tile) once per qualifying sector entity and each later draw OVERWRITES the earlier (z-write on, LESSEQUAL `[orig: Foliage_DrawModelTileSlot @ 0x601e33]`) — the framebuffer keeps the LAST submission's alpha ref/wind phase. Two coexisting retained copies z-fight instead (the reported far-foliage flicker), so the host renders ONE `MultiMeshInstance3D` per (slot, tile) per frame carrying the LAST walk-order submission's draw state, with a `submissions` counter preserving the retail draw count and the per-SUBMISSION wind-counter advance. Residual: retail's earlier-draw edge texels can survive where the later draw's alpha test discards (ref differences between anchors) — sub-texel at ≥ 38u and unreproducible without immediate-mode compositing. Pinned by foliage_model_draw_state_test.gd. |
| D-FOLIAGE-11 | C | PERMANENT (host translation, minted 2026-07-12; corrected in the pre-PR round; ADR 0022 register) | Model-tier cache mechanics, host-optimized at jungle-map density with identical hit/evict/stamp semantics: (1) a key→index hash beside the 1000-entry cache (retail linear-scans its stripe per tile `[orig: Foliage_UpdateModelTiles @ 0x601f50]` — ~2.3M compares/frame on REVVY ASB_G11A); (2) walk() emits BORROWED views of cache entries instead of copying instance lists per frame (retail draws straight from the entry; the LRU never evicts a this-frame-touched entry, and a miss with no safe victim skips adoption — retail would recycle, but its draws are immediate so nothing aliases; the entry content stamp is a dispatcher-monotonic counter so an evict/re-adopt cycle can never alias a host upload skip). A third mechanism minted here — same-frame stagger-regen dedup — was REMOVED in the pre-PR review round: its premise (generate deterministic per (slot, key)) is false, since the accept gate is anchor-relative (|world − anchor| ≤ 0x40000) and anchors sharing a tile produce different subsets; the dedup froze the FIRST touching anchor's subset while retail's per-hit regen leaves the LAST touching anchor's as the frame's end state (the same last-wins the retained draw keeps per D-FOLIAGE-10). Regen now runs per touching anchor, exactly as retail. foliage_us on ASB_G11A: 15849 (pre-rebuild) → 10993 (correct density, pre-optimization) → 1370 (with the since-removed dedup) → 1472 re-measured with per-anchor regen (Phase C gate < 3000 holds). |
| D-FOLIAGE-12 | C | PERMANENT (host translation, minted 2026-07-12; ADR 0022 register) | FAR new-key bake budget: the engine's per-call new-texture list is a 64-entry stack array with no growth or overflow guard (`_WORD *[65]`, keys at [1..64] `[orig: Foliage_UpdateFarCellSlots @ 0x601b30]`) — churn is expected to stay under 64/frame. The host caps adoptions at 64/slot/frame explicitly. With the witnessed 42u collect disc (~49 cells/slot) per-frame demand tops out below the cap — the budget is the faithful guard on the retail array bound, not a steady-state path. |

Candidate placement arithmetic carries no divergence **for a supplied retail
key**: seed function, PRNG steps, match-remapped FOLIAGEMAP gate, the FAR
36-candidate/36-accepted ceiling, MODEL's separate 21-accepted ceiling, and
spacing arithmetic are witnessed. D-FOLIAGE-8 tracks the still-divergent host
key supplied to FAR; D-FOLIAGE-7 tracks the exact blocker/entity/draw inputs.

## Cross-references

- Reimpl: `libs/foliage` (`placement.cpp` plus `far_mesh_emitter.cpp` for
  FAR; `model_placement.cpp`/`model_dispatcher.cpp` for MODEL; `fd_bake.cpp`
  for the shared asset; the jodemo-era `dispatcher.cpp` was deleted with the
  2026-07-10 slot-pool witness),
  `godot/engine/terrain/nova_foliage_dispatcher.cpp` (the two-tier host),
  `godot/shaders/foliage_far.gdshader` (FAR wind/lightmap pass),
  `godot/shaders/foliage_model.gdshader` (MODEL fit/wind/black-alpha pass),
  `godot/engine/terrain/veg_assets.gd` (`resolve_slot_fd_textures`).
- FAR's T1/c6 terrain-lighting feed is cross-recorded in
  [terrain/terrain-re.md](../terrain/terrain-re.md); it is not vertex COLOR.
- Tiles (`libs/til`, PAR-R3) is jodemo-cited in code but retail-auditable
  (`PolyTrn_RenderTile @ 0x60df0d`, `serialize_terrain_tiles @ 0x6080F0`), just
  multi-part; terrain (PAR-R1) is the larger renderer/mesh pipeline. See the
  divergence-ledger.md UNAUDITED table for the per-audit binary scoping.
