# Foliage placement — reverse-engineering record

Structure-mapping record for the original engine's **procedural foliage
placement** (the deterministic per-cell grass/bush instancing) and its color
sampling. The reimplementation surface is `libs/foliage` (`placement.cpp` /
`dispatcher.cpp`) and the Godot host `NovaFoliageDispatcher`
(`godot/engine/terrain`). Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-FOLIAGE-…` divergence catalog. Produced by a
read-only IDA audit (PAR-R2, 2026-07-05); no IDB renames were made. It converts
the **Foliage** system from `UNAUDITED` to tracked (divergence-ledger.md).

Note on binaries: `libs/foliage/placement.cpp` was originally ported from
`jodemo.exe` (it cites `sub_5C0240`/`sub_5C6450`/`sub_5C65E0`); this audit
**re-confirms the placement byte-for-byte against the RETAIL function**
`generate_foliage_instances_0 @ 0x600197` — identical seed, PRNG, and gates —
so the port is faithful to the shipped product, not just the demo.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Per-cell placement (seed + PRNG + gates) | **MATCHING** | `libs/foliage/placement.cpp` = retail `generate_foliage_instances_0 @ 0x600197` field-for-field (below) |
| Foliage instance color | **DIVERGENT (approximation, narrowed 2026-07-07)** | D-FOLIAGE-1 — the half-plus-bias emitter form is applied; the per-vertex gradient is the residual |
| Fragment combine (the blend PS) | **MATCHING (ported 2026-07-07)** | D-FOLIAGE-2 FIXED — `foliage.gdshader` runs the witnessed `t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8` chain (t1 = planar-projected colormap; c0/c1 = the SKY/LIGHT blocks) `[orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0]`; witness text in [terrain/terrain-re.md](../terrain/terrain-re.md) §Foliage / sector models |
| Wind-sway vertex path | **DIVERGENT (stand-in)** | D-FOLIAGE-3 — axis/weight/waveform diverge from `Foliage_WindSwayVS`; the sway state globals are live |
| The MODEL tier (sector-entity 3DI stamping) | **MATCHING (ported 2026-07-08)** | D-FOLIAGE-4 FIXED — `libs/foliage` `model_placement`/`model_dispatcher` + the two-tier host stamp the witnessed clusters (±4u around anchors, ≤21/tile, 0.75-XZ/0.5-height, upright, the 8-sample biquadratic fit in `foliage_model.gdshader`); the model-pass COLOR chain is the D-FOLIAGE-6 residual. §The model tier |
| The `:fd` foliage texture (both tiers) | **MATCHING (ported 2026-07-08)** | D-FOLIAGE-5 FIXED — `bake_fd_rgba` (exact kernel + wrap + `0x808080` fold) via `VegAssets.resolve_slot_fd_textures`; BOTH tiers bind it `[orig: Foliage_LoadDefAssets @ 0x601260]` |
| The model-pass color chain | **DIVERGENT (needs-RE)** | D-FOLIAGE-6 — the retail model draw's pass object + oD0 = black is unwitnessed; the host runs the quad combine family with the in-shader emitter-form v0. §The draw and the vertex shader |

## The witnessed placement (`generate_foliage_instances_0 @ 0x600197`)

- **Cell key**: `key & 0x3FF` (x), `HIWORD(key) & 0x3FF` (z) — 10-bit tile
  coords; the sign bit (`0x80000000`) → empty cell (0 instances).
- **PRNG seed**: `state = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F)`
  (`-1520754963` = **0xA55B1EED**; our `PRNG_SEED_CONST`, `int_convert`-verified).
- **PRNG step**: `state = ROL32(state + ROL32(state, 11), 4)`, value `= (u16)state ^ 1`
  (our `placement.cpp` `rol32(state + rol32(state, 11), 4) ^ 1`).
- **Candidates per cell**: **36** (`random_angle < 36`; our `FOLIAGE_CANDIDATES_PER_CELL = 36`,
  a 6×6 grid).
- **Surface gate**: an instance is placed only where `(1 << slot) &
  Terrain_GetSurfaceTypeAtFixedPoint(x, -z) @ 0x6066d0` is set (our port's
  `(1 << slot) & sub_5C65E0(...)` surface-mask gate).
- **Proximity/spacing**: a `0x20000` (2.0 world units, 16.16) path/spacing
  reject via `sub_606490` (our `samplers.path_blocked(x, -z, 0x20000)`), skipped
  when the foliage-type flag `byte_2C2608C & 1` is set.
- **Quad geometry**: each accepted instance emits a quad sampled at its 4 corners
  (`sub_606000` at `±0x10000`) for tangent/normal; our port mirrors the corner
  geometry.
- **Color**: `sample_terrain_colormap_tinted @ 0x606030` at the instance ±0x8000 on both
  axes (4 samples), 2×2 SWAR average of RGB + alpha (the tint ported for env #19,
  D-ENV #19); the emitter's per-vertex color is `0xFF000000 | (0x404040 +
  (avg>>1))` under a 2× draw — the half-plus-bias form is applied by the host
  dispatcher since 2026-07-07 (alpha forced opaque; per-channel `0x40 + avg/2`
  cannot carry).

## The witnessed fragment combine (ported 2026-07-07)

The blend PS `Foliage_LightmapBlendPS @ 0x5ff7a0` (witness text in
[terrain/terrain-re.md](../terrain/terrain-re.md) §Foliage / sector models):
`rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8`, `a = t0.a × v0.a` — t1 is the
terrain colormap sampled by a planar world-position projection (VS oT1 =
dp4(world, c7/c8); host: uv = `(x, −z) / texsize`, wrap — identical to the CPU
sampler `NovaTerrainData::get_colormap_color_world`), c0 = the SKY block and
c1 = the LIGHT block — the same post-modulator constants the terrain surface
serves (hosted as the `opennova_sky_ambient` / `opennova_sun_light` globals).
`foliage.gdshader` previously ran an unwitnessed ratio stand-in
(`(sun/(ground·0.707+sun))×255/128`, no colormap sample, no SKY term) — the
gobj-era family the gamma-faithful pipeline (D-RMAT-7) exposed; D-FOLIAGE-2.

## The model tier (witnessed 2026-07-08; port-time pins closed same day; PORTED same day — the model-tier port slice)

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
  foliage look's color therefore comes from the lighting/colormap chain,
  never from the diffuse RGB.
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
  render scale is **0.75 on XZ and 0.5 on height**.

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
  Foliage_SampleFoliageMapMask(x, z) @ 0x606620` — the model tier gates on
  the FOLIAGEMAP byte (512-quadrant tile layout, resolution-shift
  downsample), NOT the quad tier's charmap surface-type fn `@ 0x6066d0`.
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
quad tier's `Foliage_WindSwayVS`) — the "decode the blobs" pin dissolved:
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

**Open residual (model-pass color chain — D-FOLIAGE-6)**: the draw sets NO
pixel shader of its own; pass state comes from the def's pass object
(`table[16]`, created over a static buffer arg the decompiler folds as
`dword_440000`) and oD0 is the constant BLACK `(0,0,0,1)` — under the quad
tier's blend-PS form `t0 × (…) × v0 × 8` that would render black, so the
model pass's combine must differ (fixed-function TSS or another PS). The
stage/combine content of that pass object is NOT yet witnessed; the host
port renders models through the same witnessed foliage combine family
(`foliage_model.gdshader`) with v0 DERIVED IN-SHADER as the quad-emitter
half-plus-bias form of the tinted colormap sample (`0x40/255 + tinted_cm/2`)
— minted as D-FOLIAGE-6 (needs-RE) at the port slice.

The quad tier (`generate_foliage_instances_0 @ 0x5ffdd0` →
`foliage_lod_update_texture_slots @ 0x601b30` VB slots) remains the
byte-exact-ported placement; its quads texture with the same `:fd` bake.

## D-FOLIAGE divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FOLIAGE-1 | A | OPEN (approximation — narrowed 2026-07-07) | Foliage instance color: the host bakes ONE color per `MultiMesh` instance where the engine emits a per-VERTEX quad color (four corner samples) and reads an alpha-premultiplied colormap. The half-plus-bias emitter form (`0xFF000000 | (0x404040 + (avg>>1))`) IS applied since the combine slice; the per-vertex gradient + the premultiplied read are the residual. Rides the foliage render-emitter parity. |
| D-FOLIAGE-2 | A | **FIXED (2026-07-07, REN-6 rider)** | The fragment combine was an unwitnessed ratio stand-in (`(sun/(ground·0.707+sun))×255/128` — no terrain-colormap sample, no SKY term; the shader header already carried the correct witness) vs the witnessed `rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8, a = t0.a × v0.a` `[orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants terrain_setup_lighting_and_shader @ 0x604420]`. Ported 1:1: planar uv `(x,−z)/texsize` wrap ≡ the CPU sampler; the dispatcher binds the colormap from the CPU-color source chain (runtime terrain → editor colormap source) with a neutral no-terrain fallback (retail never draws foliage without a colormap). |
| D-FOLIAGE-3 | A | OPEN (stand-in — minted 2026-07-07) | Wind sway: the host displaces X weighted by height (`VERTEX.y/8`, `sin(phase + 0.11x + 0.07z)`) where the witnessed VS displaces Z weighted by vertex RED via a polynomial sine of `world.x·c24.y + time` `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; Foliage_WindSwayVS @ 0x2c25e5c]`. The sway amount/phase state is live (NovaWeather globals); the axis/weight/waveform ride the foliage render-emitter parity. |
| D-FOLIAGE-4 | B | **FIXED (2026-07-08 — the foliage model-tier port slice)** | The witnessed MODEL tier was unhosted: retail stamps the def graphic's FULL 3DI geometry in clusters around SECTOR ENTITIES (view depth ≥ 38, ±4u candidate radius, ±8u quadrant 16u tiles, ≤21 instances/tile, foliagemap-byte gate) — upright yaw-only, **effective scale 0.75 XZ / 0.5 height** (VB normalization × the 0.75R footprint corners), anchored by the 8-sample biquadratic ground fit (4 rotated corners + 4 edge midpoints → the `(E_A, T_A, E_B, T_B)` sag fold evaluated in `Foliage_GridPlacementVS`), alpha-ref `clamp(4096/(dist+1), 8, 128)`, height-weighted world-Z wind `sin(ctr·0.001)·0.08` `[orig: Foliage_GenerateModelTileInstances @ 0x600980; Foliage_UpdateModelTiles @ 0x601f50; Foliage_DrawModelTileSlot @ 0x601d90; Foliage_UploadModelTileVSConstants @ 0x600f00; Foliage_FillInstancedModelBuffers @ 0x5ffa20; Terrain_CreateFoliageVertexShaders @ 0x5ff630; Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]` — while the host rendered the def's 3DI mesh at EVERY quad placement, slope-tilted, single-point anchored, native scale. Ported: `libs/foliage` `model_placement.{h,cpp}` (generator, pinned by float32-emulated hand vectors) + `model_dispatcher.{h,cpp}` (walk/1000-cache/stagger) + the two-tier `NovaFoliageDispatcher` (`foliage_model.gdshader` runs the fit/wind/alpha curve; the far tier now draws upright `:fd` quads and the slope-tilt path is DELETED). Host mappings (commented in code): anchor source = placed world objects (the visible-sector-entity equivalent) with a view-depth range gate standing in for the sector render's visibility cull; far-quad SIZE from the model bounds pending the quad-emitter grill; the in-shader alpha-ref argument = instance center distance (retail: the anchor distance, one ref per tile draw); wind phase advanced per frame, not per tile draw; shared tiles emitted once per frame where retail re-draws per entity. Residual: the model-pass COLOR chain → D-FOLIAGE-6. |
| D-FOLIAGE-5 | B | **FIXED (2026-07-08 — the foliage model-tier port slice)** | The `:fd` texture: retail bakes it from the MODEL submesh[0]'s OWN texture (TGA-first, `.dds` fallback) — alpha smoothed by the 3×3 kernel (center 4, edges 1, corners 2, /16, wrap), RGB flattened to exactly `0x808080` — and binds it for BOTH tiers (the far quads AND the near model draw) `[orig: Foliage_LoadDefAssets @ 0x601260 tail; Foliage_DrawModelTileSlot @ 0x601d90]`. The host previously bound the raw diffuse everywhere. Ported: `libs/foliage/fd_bake.{h,cpp}` (exact kernel + pow2 wrap + the `0x808080` fold, hand-vector ctest) exposed as `NovaFoliageDispatcher.bake_fd_image`; `VegAssets.resolve_slot_fd_textures` bakes per def and BOTH tiers' materials bind it (non-pow2 diffuses fall back to the raw texture with a one-line warning — the retail wrap masks assume pow2). |
| D-FOLIAGE-6 | C | OPEN (minted 2026-07-08 — the model-tier port slice; NEEDS-RE facet) | The retail model-pass COLOR chain is unwitnessed: the draw sets no PS of its own — pass state comes from the def's pass object (`Foliage_DefTable` [16], created over a static buffer arg folded as `dword_440000`) and oD0 = the constant BLACK `(0,0,0,1)` `[orig: Foliage_UploadModelTileVSConstants @ 0x600f00 c6; Foliage_DrawModelTileSlot @ 0x601d90]` — under the quad blend-PS form that would render black, so the model combine must differ (FF TSS or another PS). The host renders models through the witnessed quad combine family (`foliage_model.gdshader`) with v0 DERIVED IN-SHADER as the quad-emitter half-plus-bias form of the tinted colormap sample (`0x40/255 + tinted_cm/2`). Route: grill the pass object's stage/combine content. |

Placement itself carries **no divergence** — the seed, PRNG, candidate count,
surface gate, and proximity spacing are byte-exact against retail.

## Cross-references

- Reimpl: `libs/foliage` (`placement.cpp`/`dispatcher.cpp` — the quad tier;
  `model_placement.cpp`/`model_dispatcher.cpp`/`fd_bake.cpp` — the model tier
  and the `:fd` bake), `godot/engine/terrain/nova_foliage_dispatcher.cpp`
  (the two-tier `MultiMesh` host), `godot/shaders/foliage_model.gdshader`
  (the grid-placement fit/wind/alpha-curve port),
  `godot/engine/terrain/veg_assets.gd` (`resolve_slot_fd_textures`).
- The terrain tint the color path consumes is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`env::foliage_lightmap_tint`).
- Tiles (`libs/til`, PAR-R3) is jodemo-cited in code but retail-auditable
  (`PolyTrn_RenderTile @ 0x60df0d`, `serialize_terrain_tiles @ 0x6080F0`), just
  multi-part; terrain (PAR-R1) is the larger renderer/mesh pipeline. See the
  divergence-ledger.md UNAUDITED table for the per-audit binary scoping.
