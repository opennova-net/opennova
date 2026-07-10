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
| FAR candidate placement | **MATCHING** | `placement.cpp` preserves the 36-candidate ROL-hash sequence and gates from `generate_foliage_instances_0 @ 0x5ffdd0`; accepted count is 0..36, not capped at 21; no view cull (the retail generator has no center/radius arguments) |
| FAR feed + slot pool | **MATCHING (witnessed 2026-07-10)** | Traversal collects leaf cells within **42.0** of the camera (<=128) `[orig: Terrain_TraverseQuadtreeNode @ 0x60905c -> Terrain_CollectNearFoliagePatches @ 0x603e60]`; per-def slot pools bake each new key ONCE and LRU-evict by frame stamp `[orig: Foliage_UpdateFarCellSlots @ 0x601b30]`; the host `NovaFoliageDispatcher` runs the same bake-once pool over the 42u disc |
| FAR geometry and vertex payload | **MATCHING (corrected 2026-07-09)** | `far_mesh_emitter.cpp` copies the complete source mesh per accepted candidate, transforms XZ at 1.0 scale, halves source Y, samples terrain below every transformed source vertex, preserves UV/index topology, and emits the witnessed red wind weight `[orig: @ 0x6002DB..0x60030A]` |
| FAR wind and fragment pass | **MATCHING hosted mechanics** | `foliage_far.gdshader` ports the `Foliage_WindSwayVS` polynomial/axis and `Foliage_LightmapBlendPS`, the witnessed c24.x phase expression, and the per-cell draw state (fade knee 20.0, slope 1/22, high pass under 33.0 with refs 180/8); D-FOLIAGE-7 keeps the exact T1 render-target content and c6.rgb float chain |
| MODEL tier geometry/placement | **MATCHING mechanics; exact terrain driver pending** | Full source mesh, cap 21, 0.75 XZ / 0.5 Y, eight-sample biquadratic fit, per-anchor alpha, and per-tile draw/wind cadence are hosted; D-FOLIAGE-7 tracks the exact visible/occlusion-tested sector-entity feed |
| The `:fd` foliage texture (both tiers) | **MATCHING (ported 2026-07-08)** | D-FOLIAGE-5 FIXED — `bake_fd_rgba` (exact kernel + wrap + `0x808080` fold) via `VegAssets.resolve_slot_fd_textures`; BOTH tiers bind it `[orig: Foliage_LoadDefAssets @ 0x601260]` |
| MODEL material pass | **MATCHING (resolved 2026-07-09)** | Pass table[16] selects the black c6 diffuse for RGB and multiplies `:fd.a × diffuse.a`; `foliage_model.gdshader` is deliberately separate from FAR `[orig: Foliage_DrawModelTileSlot @ 0x601d90]` |

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
  HIWORD-derived coordinate as the X argument of the spacing/surface queries
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
height under the cell center as the leaf Y metric; Godot's per-node frustum
culling stands in for the traversal's frustum gate), keeps a per-slot
bake-once pool capped at the 128 collect bound (the retail resident count -
pool header field [37] - is untraced), and per frame only toggles pooled
cell visibility and refreshes the witnessed per-cell fade/pass instance
parameters. The T1 stand-in recomposes the tile render target's content
(colormap x blend-weighted detail splat, sampled at LOD 0 because the retail
render target carries no mip chain) from the same `.trn` sources the terrain
shader consumes; c6.rgb holds the 0.5 neutral that reproduces the
fixed-function `2*T0*diffuse` level over the untinted terrain surface
(env #19: the terrain_rgb texture-bake consumer is retail dead code).

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
distance fog can lift that black toward the fog color. MODEL does not run the
FAR lightmap blend. `foliage_model.gdshader` now expresses this separate pass,
closing D-FOLIAGE-6 `[orig: Foliage_LoadDefAssets @ 0x601260;
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

The FAR feed, slot pool, and per-patch fade/pass draw state are now witnessed
AND hosted (see The FAR feed and slot pool above). The remaining
approximations, recorded together as D-FOLIAGE-7:

- the exact per-tile T1 render-target content (the host recomposes
  colormap x detail splat from the `.trn` sources at LOD 0; retail renders
  terrain tiles into the texture it binds);
- the exact per-frame c6.rgb lighting-factor floats (host: the 0.5
  fixed-function-parity neutral);
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

## D-FOLIAGE divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FOLIAGE-1 | A | **FIXED / false premise retracted (2026-07-09)** | The four terrain-color samples and intermediate writes in the FAR generator are dead with respect to emitted color. After the join at `0x6002DB`, `0x60030A` unconditionally stores red-only `clamp(trunc(srcY*128),0,255)<<16`; `far_mesh_emitter.cpp` emits that source-height wind weight. The former per-vertex terrain-color and half-plus-bias claims are withdrawn. |
| D-FOLIAGE-2 | A | **FIXED (2026-07-09 scope correction)** | FAR runs `Foliage_LightmapBlendPS`: `rgb=t0*(t1*(t1.a*c1+c0))*c6.rgb*8`, `a=t0.a*c6.a` `[orig: @ 0x5ff7a0]`. The port now lives only in `foliage_far.gdshader`; MODEL has a different fixed-function pass. Exact sector T1/c6 values and high/low pass splitting ride D-FOLIAGE-7. |
| D-FOLIAGE-3 | A | **FIXED (2026-07-09)** | `foliage_far.gdshader` ports `Foliage_WindSwayVS`: source-height red weight, c27 phase wrap, the witnessed tenth-order polynomial, amplitude 0.03, and render-Z-only displacement `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; constants @ 0x600450]`. The host binds the exact `GetTickCount()*0.003 + Env_WaveOscRing[0]*1.5258789e-6` phase expression. |
| D-FOLIAGE-4 | B | **FIXED (corrected 2026-07-09)** | MODEL stamps full source geometry with cap 21, XZ 0.75/Y 0.5, yaw-only eight-sample ground fit, anchor-derived alpha, duplicate tile draws per qualifying anchor, and per-tile-draw wind counter `[orig: @ 0x600980, 0x601f50, 0x601d90, 0x600f00]`. The former shared-shader/shared-cap and FAR ground-patch addenda are retracted; FAR instead uses `far_mesh_emitter`. Exact upstream entity visibility rides D-FOLIAGE-7. |
| D-FOLIAGE-5 | B | **FIXED (2026-07-08)** | Both tiers bind the model submesh[0] `:fd` bake: wrapped 3×3 alpha kernel and flattened `0x808080` RGB `[orig: Foliage_LoadDefAssets @ 0x601260; Foliage_DrawModelTileSlot @ 0x601d90]`. |
| D-FOLIAGE-6 | C | **FIXED (2026-07-09)** | MODEL's table[16] pass is now witnessed: one T0=`:fd` stage, flags `0x00440000`, RGB selects c6 diffuse `(0,0,0,1)`, alpha multiplies `T0.a*diffuse.a`. The resulting unfogged RGB is black; `foliage_model.gdshader` ports that pass rather than sharing FAR's lightmap combine. |
| D-FOLIAGE-7 | A | WITNESSED-READY-DEFERRED (narrowed 2026-07-10) | The FAR feed (42.0 traversal collect, 128 caps), bake-once slot pool, and per-patch fade/high-low draw state are witnessed AND hosted. Remaining approximations: exact per-tile T1 render-target content (host recomposes colormap x detail splat at LOD 0), exact c6.rgb lighting-factor floats (host: 0.5 FF-parity neutral), the MODEL sector-entity visibility stream (the host now frustum-gates anchors — the occlusion half stays unhosted), the blocker registry, the second low wireframe resubmit, and the retail pool residency count (field [37]). |
| D-FOLIAGE-8 | A | OPEN (minted 2026-07-10) | FAR cell-key provenance: retail keys pack native sector+offset coordinates (always in the wrapped [0,1024) domain) and the per-cell PRNG seeds from that key; the host's FAR cells are keyed in Godot render space (z = −native, signed near the origin), so per-cell jitter patterns diverge from retail and the mask sampler needs a compensating sign at the slot_mask boundary. Fix direction: native-keyed collect + native placement with a render-space conversion at emit (touches the collect, both height lambdas, and the emitted-vertex Z). |
| D-FOLIAGE-9 | B | OPEN (minted 2026-07-10) | With the corrected foliagemap gate, painted 00TRg cells bake instances (nodes, meshes, materials all present and in-frustum) that do not rasterize; on Dvxi5 the same path draws. Also the Dvxi5 far tufts render white-silver — the T1 recompose (u_terrain_light/detail chain) reads wrong or missing sources on flat-extract loads. Both need a focused render-side session (probes committed: foliage_00trg_capture_probe, foliage_sampler_compare_probe). |

Candidate placement math carries **no divergence**: seed, PRNG, raw surface
mask, the FAR 36-candidate/36-accepted ceiling, MODEL's separate 21-accepted
ceiling, gates, and spacing arithmetic are witnessed. The former L-infinity
view cull in `place_cell` was jodemo-only and is deleted - the retail
generator bakes the whole cell. Supplying the exact blocker/entity/draw
inputs is D-FOLIAGE-7.

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
