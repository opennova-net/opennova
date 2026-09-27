# Foliage — fresh reverse-engineering record

**Status:** re-grilled and reimplemented 2026-07-13, with the MODEL blend/depth
state corrected 2026-07-14 after a renderer capture exposed the missing
combiner. The same-day follow-up also ported the mission-tile exclusion, near
secondary LOW submission, and model-own custom `:fd` mip chain. The previous
reimpl runtime was deleted rather than repaired because it
inverted the two retail tiers and encoded several disproven geometry and color
assumptions. The 2026-09-24 rendering parity pass corrected the candidate key
frames, both tiers' wind and the detail fog, the thermal (not reflection) fade
flag, same-frame detail draws, and the detail map gate, and ported the MODEL
depth masks' order, water sides and anchors (see the dated section below).
On 2026-09-26 the detail pass's page addressing was witnessed CLAMP
(D-FOLIAGE-7 closed), the indoor detail-foliage gate was ported, and the
weapon Inset pass got its own detail cells, anchors and mask frame (the
2026-09-26 section below).

**Witness:** retail `Jointops.exe` (IDB `Jointops.exe.kong.i64`).

**Scope:** runtime collection, deterministic placement, geometry expansion,
`:fd` preprocessing, render-state/shader behavior, and the Godot adapter. The
foliage definition and map formats, including the flat-map coordinate helpers,
remain in place; ONED's paint and eyedropper consumers were removed by ADR 0037.

## Verdict

| Surface | Retail witness | Reimpl result | Verdict |
|---|---|---|---|
| Definition/map source data | `.trn` foliage defs (`match` = up to seven byte args per slot, FOUR consumed), charmap, foliagemap | formats (four-code `FoliageDef::match` since 2026-09-14, see Definition match codes) and the flat wrapped map lookup retained; no ONED authoring UI | matching format/coordinate helpers |
| Detail-cell collection | frustum-surviving traversal nodes (level ≥ 3, node distance less 16 within 42) hand subtrees to `Terrain_CollectNearFoliagePatches @ 0x603e60`, independent of the 224 main-draw cap; foliage cap 128 | an independent handoff from the same traversal decisions and node gate into the 16-unit mip-bound collector; each cell carries its Z-min key, atlas minimum and leaf maximum height | matching (seating corrected 2026-07-16, D-FOLIAGE-13; budget independence and node gate corrected 2026-09-13, D-FOLIAGE-14) |
| Detail placement | `Foliage_GenerateInstances_0 @ 0x5ffdd0` | fresh `foliage::Runtime` literal vectors on retail's key frame (Z-min low half, B adds) and yaw step `flt_7CD4DC` | matching; PORTED 2026-09-24 ("Place foliage candidates on retail's key frames") |
| Detail map gate | flat 1024-wrap lookup at the key's atlas position plus the local offsets | gate at `DetailCell::atlas_x/atlas_z` + (A, B) | matching; PORTED 2026-09-24 ("Close the foliage mask residuals: atlas map gate, local anchor, P3 order"); D-FOLIAGE-12's world-position premise refuted |
| Detail geometry | every surface of every LOD0 submesh expanded and terrain-bent | fresh CPU-expanded aggregate ArrayMesh batches | matching |
| Detail pass split | high ref 180 / low ref 8 at distance 33; thermal forces one LOW at fade × 0.1 | separate high/low shaders and batches; `FrameRequest::thermal_view` | matching; thermal PORTED 2026-09-24 ("Draw new detail cells at once and thin them under thermal") |
| Detail water sides | pass 0 / pass 1 split by the leaf's maximum height against the water and the camera side | `kRungFoliageFarSide` / `kRungFoliageCameraSide` | matching; PORTED 2026-09-24 ("Port the foliage depth masks, their water sides and anchors") |
| Detail sway and fog | `Foliage_SetupVertexShaderConstants @ 0x600450`, `g_FoliageWindSwayVS` literal @ 0x7de648 | sector-local sine, `c25.x` scale, 1/655360 ring term; linear eye-depth vertex fog for every fog type | matching; PORTED 2026-09-24 ("Port the foliage vertex-shader sway and fog constants") |
| Distant MODEL placement | `Foliage_GenerateModelTileInstances @ 0x600980` | fresh runtime, four cells in the mission frame, cap 21/cell | matching; key frame PORTED 2026-09-24 |
| Distant ground fit | four corners + four edge midpoints | portable corners/fold packed as `GridPlacementVS` instance blocks, evaluated on the GPU per instance | matching; GPU placement PORTED 2026-09-24 |
| Distant MODEL blend/depth | `Foliage_LoadDefAssets @ 0x6015b7`, `Foliage_DrawModelTileSlot @ 0x601d90` | additive black, alpha-tested, depth-writing mask at the retail wave slots, plus the opaque-pass mask texture the person draws test | matching effect/state and order; D-FOLIAGE-10 FIXED 2026-09-24 |
| `:fd` bake/sampling | `Foliage_LoadDefAssets @ 0x601260`, `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`, device sampler init | exact dual-source chain and anisotropic sampler selection; a conservative longest-gradient guard excludes Godot's synthetic terminal tail but can bias grazing footprints sharper than the device's minor-axis/maximum-anisotropy choice | matching chain/sampler, bounded terminal-LOD approximation; D-FOLIAGE-5 |
| Detail lightmap input | composed per-tile render target found by `Terrain_FindSectorPatchRT @ 0x6042a0`; a patch without one is skipped | detail draws take the page `TerrainTileCompositionCache::lookup` finds (the ported 128-record cache, composed before the draw), sampled clamp-to-edge, and skip a patch with no resident page; no cold fallback under a Terrain | matching; D-FOLIAGE-7 closed 2026-09-26 (both detail pass states sample the page CLAMP) |
| Mission-tile exclusion | `Foliage_PathBlockedByPlacedTile @ 0x606490` | shared parsed `<mission>.til`, exact inclusive 16x16 AABB scan | matching; D-FOLIAGE-8 fixed |
| MODEL-anchor selection | crouched/prone players on terrain (`MoveOrder & 0x300`, empty `groundEntity`) among the collected, render_TOC-surviving BySide entities | the occlusion frame's collected verdicts (collector legs, then the waves' contained render_TOC test) with the stance gate; the local player takes the same verdict | matching; D-FOLIAGE-11 FIXED 2026-07-16, D-FOLIAGE-9 FIXED 2026-09-24 |

## 2026-09-24 rendering parity pass

The rendering parity pass (PR #678) re-read both tiers against the binary and
landed these corrections; the sections below carry the detail.

- **Key frames and yaw** ("Place foliage candidates on retail's key frames"):
  both tiers had placed every candidate as the Z-mirror of retail inside its
  cell. The detail key's low half is the cell's Z-min on the Godot plane and
  local B adds; the MODEL key runs in the mission frame. The yaw step is
  `flt_7CD4DC`, not 2π/65536. See Shared deterministic candidate stream.
- **Wind and fog** ("Port the foliage vertex-shader sway and fog constants"):
  the detail sine takes the sector-local render x and displaces render z by
  `c25.x = 0.03`; the ring term is `/655360`; the MODEL sway adds to render x;
  the detail fog is linear in eye depth for every fog type. See Detail shader
  and MODEL blend/depth draw.
- **Thermal, not reflection** ("Draw new detail cells at once and thin them
  under thermal"): the flag that forces LOW at fade × 0.1 is the thermal view;
  the detail pool updates before the patches draw, so a new cell draws in its
  generation frame. See Fade and pass state and Persistent caches.
- **Masks, water sides, anchors** ("Port the foliage depth masks, their water
  sides and anchors", "Give the foliage MODEL masks their own ladder rungs",
  "Close the foliage mask residuals: atlas map gate, local anchor, P3 order",
  "Keep a person's blended strip off its MATCHTERRAIN path behind a mask"):
  the MODEL tier runs `GridPlacementVS` on the GPU, the masks draw at their
  retail wave slots in both the transparent list and an opaque-pass mask
  texture, the detail passes split by water side, and the anchors ride the
  occlusion frame's collector verdicts (render_TOC included, from "Run
  render_TOC on collected entities outside blink boxes"). D-FOLIAGE-9 and
  D-FOLIAGE-10 close.
- **Map gate** (same residuals commit): the detail gate samples the flat map
  at the key's atlas position, which refutes D-FOLIAGE-12's world-position
  premise.
- **Page input** ("Retire the detail foliage's raw .til overlay fallback",
  "Retire the hosted 1024 .til overlay bake", on top of terrain's "Port
  retail's terrain page record cache and compose pages before they draw" and
  "Encode the terrain atlas and detail layers with retail's D3DX DXT codec"):
  a detail patch with no resident page is skipped, as retail skips it; the raw
  `.til` overlay path (`u_tile_overlay`) and the hosted 1024 bake are gone.
  D-FOLIAGE-7 narrows (closed 2026-09-26, below).
- **AI stance**: retail AI never writes `MoveOrder`'s stance bits, so the
  MATCHTERRAIN pass and the MODEL masks are player-only in retail as in the
  reimpl. See Driver and cells.

### 2026-09-26: the page edge, the indoor gate and the weapon Inset pass

- **The detail page edge is CLAMP** (closes D-FOLIAGE-7). The page reaches
  t1 through indirect slot 2 (`Foliage_RenderDetailPatches @ 0x60A211`), and
  both detail pass states' intrinsic words (LOW `0x2560000`, HIGH
  `0x2460000`; `Foliage_LoadDefAssets @ 0x601519 / 0x601530`) carry the
  stage-1 clamp bit that `CGfxShader_ApplyPass @ 0x68327E..0x683286` applies,
  so a coarser or finer record's page clamps at its edge, as the reimpl's
  clamp-to-edge sampling does. The other-granularity record the lookup can
  return is retail's own `Terrain_FindSectorPatchRT` rule, already ported.
- **The indoor gate** (ported). Retail skips the terrain traversal
  (`Render_ProcessMainSceneFrame @ 0x5CA645..0x5CA654`) and both detail passes
  while the local player's blink flags carry `0x2`
  (`Terrain_RenderWorldScene @ 0x5C93D5..0x5C93E0`, the detail skips
  `@ 0x5C95BD` / `@ 0x5C965D`); the BySide waves, and the MODEL masks drawn
  inside them, take no letter and still run. Ported as
  `ScenePassGates::detail_foliage`: the `FoliageDispatcher` is the terrain's
  sibling, and `set_detail_passes_drawn` hides only the detail draws, so the
  MODEL draws and the mask frame keep running indoors.
- **The weapon Inset pass** (ported). The Inset (`Render_WeaponInsetScene
  @ 0x5C9740`: its traversal `sub_60FF50` called `@ 0x5C9A2F`,
  `Render_TerrainScene` called `@ 0x5C99B7`, `Terrain_RenderWorldScene`
  called `@ 0x5C9DE9`) compiles its own detail cells (from its own terrain
  traversal), anchors (from its own collect) and mask frame on the shared
  compiler, one cache and one model pool, after the main view; both passes
  advance the same counters (`Foliage_UpdateDetailCellSlots`'s
  `g_FoliageFarSlotFrameCounter` `@ 0x601B36`, `Foliage_AdvanceModelSceneCounter`
  from `Render_TerrainScene` `@ 0x610CF7`). The mask frame carries the Inset's
  own eye (`foliage_mask.gdshaderinc` keeps two eye pairs, set per scene
  pass).

### Open after the 2026-09-24 pass

- None.

## The correction: two overlapping tiers

The old record and port named the tiers backwards. Fresh decompilation from the
actual function starts establishes this schedule:

1. **Detail tier, camera distance ≤ 42:**
   `Terrain_CollectNearFoliagePatches @ 0x603e60` supplies 16-unit terrain
   leaves to `Foliage_GenerateInstances_0 @ 0x5ffdd0`. This generator expands
   the complete foliage model once per accepted candidate.
2. **Distant MODEL/depth-mask tier, view depth ≥ 38:**
   `Terrain_RenderSectorEntitiesBySide @ 0x5c7d50` drives
   `Foliage_GenerateModelTileInstances @ 0x600980` around visible sector
   entities. It renders normalized model geometry fitted to the ground. Its
   source RGB is black and unfogged, but ONE/ONE additive blending preserves
   destination color while alpha-tested survivors write depth.

The 38–42 region is intentional overlap: detail geometry fades toward zero
while MODEL masks have already begun. There is no retail transition from a near
ground quad to a far colored model.

The former `Foliage_GenerateInstances_0 @ 0x600197` anchor was also wrong:
`0x600197` is an internal colormap-sampling instruction inside the function,
not its entry point. The correct start is `0x5ffdd0`.

## Shared deterministic candidate stream

Both generators derive candidates from the packed 16-unit cell key. Direct
instruction tracing resolves the key orientation (corrected 2026-09-24; the
earlier reading placed every candidate as the Z-mirror of retail inside its
cell):

- **detail key**: high 15 bits = the cell's X-min, low 15 bits = its Z-min on
  the Godot plane. The collector packs PolyTrn's sector of `-camera_y`
  (`FB20`, the Godot Z axis) plus the node minimum, and local B **adds** to the
  Z-min [`orig: Terrain_CollectNearFoliagePatches @ 0x603f69..0x603f8a`;
  `Foliage_GenerateInstances_0 @ 0x5fff84..0x5fffa2`]. A detail vertex lands
  at render x (Godot Z) = keyLo + B + sx·sin + sz·cos and render z (Godot X) =
  keyHi + A + sx·cos − sz·sin, with the imported x = −sx
  [`orig: Foliage_GenerateInstances_0 @ 0x600112..0x60014d`];
- **MODEL key**: packed in the mission frame (y = −Godot z). The low half keys
  the mission-y cell top and the candidate is (keyHi + A, keyLo − B); corners
  and edge midpoints follow in mission fixed point with `sar 1` midpoints
  [`orig: Foliage_UpdateModelTiles @ 0x601fd0..0x60205b`;
  `Foliage_GenerateModelTileInstances @ 0x600af5..0x600b45,
  @ 0x600bd0..0x600c6a, @ 0x600c70..0x600d32`];
- bit 31: flat-sector flag; retained in collection/cache identity, with empty detail geometry.

Each cell evaluates 36 candidates as a 6×6 grid:

- base offset `1.0`;
- grid step `2.6`;
- independent X/Z jitter `1.8 × draw / 65536`;
- yaw `draw × flt_7CD4DC`, where `flt_7CD4DC = 0x38C90FD0` sits slightly
  below the float nearest 2π/65536 (`0x38C90FDB`)
  [`orig: Foliage_GenerateInstances_0 @ 0x5fff7a;
  Foliage_GenerateModelTileInstances @ 0x600aeb`].

The portable vectors (`foliage_runtime_vectors`, re-derived 2026-09-24 from an
independent emulation of the instruction sequences) pin the first detail
candidate for key `0x00100030` at `(18.63215637, 49.69863892)` with yaw
`4.83126879`. The MODEL golden anchors at Godot `(32, 48)`: keys
`{0x00207FE0, 0x00107FE0, 0x00207FD0, 0x00107FD0}` carry
`{1, 3, 2, 3}` candidates, and the first (cell `0x00207FE0`, candidate 30)
centres at `(34.00364685, 46.20242310)` with yaw `1.53810215` and fold
`(-0.06971931, 0.00000572, -0.03578377, -0.00000191)`. The vectors also pin
signed key decoding, gate selection, caps, and stable output order.

For either tier, retail linearly scans the shared mission tile array unless
foliage attrib bit 0 (`FORCE_ON`) is set. Each 12-byte entry defines an
inclusive world AABB from its decoded minimum through minimum + 16 units.
`Foliage_PathBlockedByPlacedTile @ 0x606490` rejects when the candidate's
axis-aligned radius-2 square overlaps any entry on both axes. Raw
`z_fixed` is stored negated, but `til_world_z_from_fixed` applies that
decode before the blocker sees an entry. The resulting AABB is already on the
terrain/Godot plane, so the dispatcher passes candidate Z unchanged. Mirroring
it again is a double conversion. Exact `00TRa.til` entry #25
(`19070976,-26279936`) decodes to `x[291,307], z[401,417]`. The real-asset
oracle combines entries #25, #27, #747, #753, and #764. At radius 2 it blocks
the authored spawn `(297.805573,+409.123169)`, the armory-truck center
`(311.190552,+394.856628)`, and truck-adjacent candidate c8
`(311.811996,+396.190958)`, while exact candidates c3
`(313.704367,+398.025046)`, c4 `(316.302939,+397.743686)`, and c5
`(318.975696,+397.309534)` remain eligible.
`Terrain_LoadTileInfoFile @ 0x60a740` loads the same `<mission>.til` array
used by `Terrain_GetSurfaceTypeAtPosition @ 0x606510` and streamed by
`Terrain_SerializeTiles @ 0x6080f0`. The reimpl parses it once before terrain
build and shares the resource with terrain, foliage, and listen-server state.

The sibling `shadow` attribute is parsed but dead. `forceon` is compared at
`0x60f591` and ORs bit 0 at `0x60f5a8`; `shadow` is compared at
`0x60f5bd` and ORs bit 1 at `0x60f5d4`. The only generator reads of the
definition attribute byte consume bit 0: detail at `0x5fffb6` and silhouette
at `0x600b5f`. No foliage draw consumes bit 1. The reimpl therefore preserves
the parsed flag but explicitly disables shadow casting on all three batches.

## Detail tier

### Exact 16-unit collection

Collection is seated INSIDE the frustum-culled render traversal: at each
frustum-surviving emitted node of LOD level ≥ 3 whose raw traversal distance
(X/Z clamped to the node box, Y to the node center, which flat sectors zero)
less a fixed 16.0 is within 42, `Terrain_TraverseQuadtreeNode` tail-hands that
node's subtree to the collector [`orig: 16.0 @ 0x608d46; gate + call
@ 0x60905c..0x60907c; foliage-enabled flag 0x319FB34`]. The 16.0 is the raw
literal, not the quality-scaled near zone the LOD decision subtracts: the x87
stack carries `[dist, 16.0]` through the emit block and the gate is
`fsubrp; fcomp 42.0; jp skip`, so a node farther than 58 units is never
handed off however near its own leaves are. Cells behind the camera therefore never
enter the visible-key list (`g_FoliageVisibleDetailKeyList`, cap 128), which is
what keeps the far-slot pool's working set below its 16-bit-index capacity
(D-FOLIAGE-13). `Terrain_CollectNearFoliagePatches @ 0x603e60` then
recursively reaches 16-unit leaves over the height mipchain, rejects a leaf
beyond distance 42, and appends at most 128 keys. Its distance combines:

- X and Z distance to the node AABB, clamped to zero while the camera lies
  inside that interval;
- Y distance to the node height-bounds center.

The reimpl hands off every eligible traversal emission (64-unit leaves near
the camera) into the same 512→16 height-mipchain descent, starting at the
handoff node's rect; sector IDs select the same four 512-unit atlas quadrants
as terrain rendering. This handoff is independent of the main terrain draw
list: reaching its 224-entry cap only skips the main append. The traversal
continues through the foliage-enabled, LOD (≥ 3) and node-distance
(`dist - 16 <= 42`) gates. The original collector then applies separate
128-entry checks to its near records and visible keys. OpenNova retains an
independent per-sector handoff vector, applies the same node gate before the
handoff (corrected 2026-09-13, D-FOLIAGE-14), and keeps the existing 128-cell
output, so the main draw cap cannot starve nearby foliage or widen its
frustum/distance acceptance. The node gate and the collector's leaf test are
not interchangeable: a 64-unit node whose height range is [0,127] under a
camera at Y=0 has node distance 63.5, and a flat sector under a camera above
58 has node distance equal to the camera height (its zeroed center), so
neither reaches the collector even though both contain 16-unit cells within
42 of the camera. Witnesses:
[orig: Terrain_TraverseQuadtreeNode @ 0x608A00, main cap bypass
@ 0x608FBC -> 0x609012, 16.0 @ 0x608D46, foliage gate/call
@ 0x60905C..0x60907C];
[orig: Terrain_CollectNearFoliagePatches @ 0x603E60, near-record cap
@ 0x603F98, visible-key cap @ 0x603FF1].

A 2026-09-13 bounded original-machine probe executed the complete retail
traversal and collector with a qualifying synthetic leaf: all eight
`main={223,224} × near={127,128} × keys={127,128}` starting states ended at
`(224,128,128)`, with one collector call each. With the main list already
full, foliage-disabled, LOD-below-3 and distant-leaf controls retained both
foliage counts at 127 and made no collector call. The existing jo-c
scratch oracle `terrain_traversal_oracle.py` (untracked; never in the repo) executed the verified retail PE
SHA-256 `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`
without replacing engine calls; fresh IDA reads used image base `0x400000`
and `Jointops.exe.kong.i64`, with no IDB writes. These probes establish the
count/control-flow boundary, not full-scene visual equivalence.

Portable `terrain_frame_compiler` fixtures route 223 or 224 earlier authored
draws, and a separate case routes five distant visible sectors before the
nearby sector. The expected nearby keys and order survive both limits while
retaining the forward frustum wedge and 42-unit cell test. Another case keeps
the default empty-sector fallback enabled: its flat draws exhaust all main
slots before nearby authored terrain, while both flagged flat keys and authored
keys enter the independent foliage list in traversal order. This fixture stays
below the foliage cap; flat cells do consume that capacity and can exclude
later authored cells when it fills. The collector regression covers authored
and flat 127→128 transitions and an already-full 128-entry list.

The adversarial follow-up also traced empty-sector keys through their final
consumer. `Terrain_RenderVisibleSectors @ 0x6090C0` sets `0x80000000` at
`0x60924A`, then traverses quadrant 1. The collector retains the raw node `+52`
height center at `0x603F46` and combines the flat flag into the key at
`0x603F7E..0x603F8A`; zero-height terrain drawing does not flatten this distance
input. `Foliage_UpdateDetailCellSlots @ 0x601B30` performs ordinary key lookup,
allocation and LRU touching for flagged keys. The flag test belongs to
`Foliage_GenerateInstances_0 @ 0x5FFDD0`, at `0x5FFE05`: it writes zero index
and vertex counts at `0x5FFE10..0x5FFE16` before placement/map/height sampling.
`Foliage_SetupDetailSlotDraw @ 0x6007C0` subsequently reads those zero counts,
so these are real empty cache residents, not drawable flat grass.

The collector's nonleaf branch descends all four children before any distance
calculation (`0x603E67..0x603E8F`); only the leaf branch reaches the 42-unit
comparison at `0x603F63`. Applying that test to intermediate mip centers can
lose a nearby low leaf under an ancestor spanning low and high terrain. A
bounded retail execution with parent center Y=100 and one child at Y=0 retained
that child at distance zero for both unflagged and flat keys. The portable
collector now tests leaves only. A consistent mixed-height mipchain fixture
sets ancestor bounds to [0,127], one leaf to [0,0], and all other leaves to
[127,127]; the low leaf must survive whole-sector, 64-unit and 32-unit handoffs
for both authored and flat sectors despite the distant parent centers.

A second bounded retail execution passed all nine independent
`near={0,127,128} × keys={0,127,128}` collector cases with the flat flag: each
list advanced by one exactly when below 128 and retained key `0x80100000`.
A capacity-one retail cache seeded with an authored key then consumed
`0x80100030`, called the original generator once, stored zero geometry counts,
and hit/touched that same entry on the second frame without regeneration.
The cache and generator bodies were unchanged; only graphics buffer lock and
unlock services were replaced. Portable regressions carry actual flat terrain
frame keys through `FoliageFrameCompiler`, requiring empty resident hits with
no map/path/height/atlas sampling, mesh builds or draw commands. A separate
runtime regression pins authored→flat→authored eviction; since 2026-09-24 the
regenerated authored geometry draws in its generation frame (see Persistent
caches and submission identity).

The 2026-07-16 grill retracted the earlier radial
whole-disc walk: it over-collected ~34 cells at open-ground poses, exceeding
the pool capacity and thrashing the witnessed LRU into a two-frame cell
blink retail does not show (its frustum wedge stays under capacity).

### Definition match codes

Witnessed 2026-09-14. Each `.trn` foliage definition carries up to SEVEN
`match` args, not one: the config parser's `match` arm stores them as bytes at
slot+0x108.. (`match_idx` 1..7 -> `bytes[536*slot + 6227 + idx]`, slot base
5964, stride 0x218 = 536) `[orig: Terrain_ParseConfigCallback @ 0x60F330]`.
The runtime consumer reads only FOUR of them: `Foliage_RemapPixelToDefMask
@ 0x5FF4E0` turns a foliagemap pixel into a slot mask by setting `1 << slot`
for every slot whose four bytes at +0x108..+0x10B contain the pixel; pixel 0
returns 0 before the slot walk, and a slot whose header byte (+0, the graphic
name's first character) is 0 is skipped. Args five to seven are therefore
stored but never consumed. The reimpl widens `FoliageDef::match` to four codes
(`engine/formats/foliage/foliage.h`, `FOLIAGE_MATCH_CODES`), parses up to seven
and keeps four, writes every authored code on the one `match` line, and hosts
the remap as `foliage_remap_pixel_to_def_mask` (pixel-0 and empty-graphic
gates included); `FoliageDispatcher` evaluates it once per pixel value at
`configure_slots`. The previous single-code port only ever matched the first
authored code; shipped maps author one code per slot (Dvxi5 254/253/252/251),
so no observable divergence was ledgered and no D row is minted. The
2026-09-16 review also pins the parser's byte narrowing: `match -1 256 511
-256` stores `255 0 255 0`, not saturation or the tool API's -1 sentinel.
The authored TRN parse narrows before `foliage_normalize_def`; the latter
retains its tool-input normalization. `[orig: Terrain_ParseConfigCallback
@ 0x60f330]` Ctest `trn_roundtrip` covers the out-of-range tokens.

### Gate and expansion

For every detail candidate:

- the **foliagemap** palette index selects every definition slot whose four
  `match` codes contain it (Definition match codes above); the index is read
  at the key's ATLAS position plus the local offsets (see Runtime sampling
  below and D-FOLIAGE-12), while the path blocker takes the world position;
- the surface/charmap is not consulted;
- the full source model is yawed and translated without subtracting its bounds
  center;
- every output vertex gets
  `world_y = terrain_height(world_x, world_z) + source_y × 0.5`;
- the source height becomes the clamped bend byte used by the wind shader.
The scale audit rules out a hidden foliage-width correction. Raw source X/Z
feed the yaw/translation path directly at `Foliage_GenerateInstances_0 @
0x600112..0x60014d`; only source Y is multiplied by
`flt_7C3B94 = 0.5` at `0x600121..0x60012a`. The bend byte is independently
`clamp(sourceY × 128,0,255)` at `0x6002db..0x60030a`.
`Foliage_LoadDefAssets @ 0x6012ec..0x601320` copies the source vertex/index
pointers and counts directly and derives its X/Z extent from the raw vertices
at `0x60135f..0x6013b3`; the far draw matrix adds translation without another
scale. Retail geometry scale is therefore X/Z `1.0`, Y `0.5`, matching the
reimpl when the imported 3DI positions themselves are faithful.

The recovered `Terrain_GetSurfaceTypeAtFixedPoint` name on this path is a
misnomer: its backing buffer is the foliagemap loaded by `Foliage_LoadFoliageMapPCX`. This is
also required by retail data: Dvxi5's charmap is uniformly index 1, while its
foliage definitions and authored coverage use indices 253 and 254.

This is the decisive reason the prior quad-oriented runtime could not be
salvaged. The fresh adapter emits world-space geometry into two ArrayMesh
batches per definition slot, one for each alpha-test/depth policy.

### Fade and pass state

The per-cell detail fade is:

- `1.0` through distance 20;
- linear from 20 to zero at 42;
- rejected beyond 42.

The distance-33 boundary changes submission count, not just state. Below 33,
retail submits the same resident geometry twice in strict HIGH-then-LOW order.
At and beyond 33 through 42 it submits only LOW:

| Range | Ordered pass/reference | c6.a / fade | Depth write / compare |
|---|---|---:|---|
| `< 33` | HIGH 180/255, then secondary LOW 8/255 | unscaled distance fade on both | enabled / `LESSEQUAL`, then disabled / strict `LESS` |
| `≥ 33` through 42 | primary LOW 8/255 | unscaled distance fade | disabled / `LESSEQUAL` |

The per-patch fade is computed once, uploaded once, and shared by both near
draws — there is no constant re-upload between the primary and secondary
submissions (`0x60a4ca..0x60a55a` upload, secondary setup/draw at
`0x60a659..0x60a694`). The `flt_7C69F4 = 0.1` fade multiply (the `fmul`
@ 0x60a4a8, block `0x60a497..0x60a4ae`) is gated on the function's third
argument, and the same flag forces every patch to one primary LOW with no
secondary (`0x60a193..0x60a19c`). **Corrected 2026-09-24:** that argument is
the THERMAL view flag, not a reflection flag. `Render_ProcessMainSceneFrame`
reads the held weapon's thermal byte (`@ 0x5ca2da..0x5ca2e3`) and passes it
as the scene core's fourth argument (`@ 0x5ca8e3`), which
`Terrain_RenderWorldScene` pushes to both detail passes
(`@ 0x5c95c1/0x5c9661`). Under the thermal view every patch therefore draws
one LOW pass at a tenth of its fade. (The IDB typed the argument
`reflectionEnabled` until 2026-09-25, now `thermalView`; the water mirror
draws no foliage at all, env #30.)

The main scene invokes the renderer twice per frame, split by water side:
`Foliage_RenderDetailPatchesPass(0) @ 0x5c95c5` before the water surface and
`Foliage_RenderDetailPatchesPass(1) @ 0x5c9665` after it (the wrapper
`Foliage_RenderDetailPatchesPass @ 0x60c6d0` brackets `Foliage_RenderDetailPatches`
with fog on/off). The side test compares the collected node's AABB maximum
height (node `+0x28`, `fcom [ecx+28h]` @ 0x60a1a2) with
`g_EnvWaterHeightFixed`: pass `formatType` 0 draws the patches lying wholly
at or below the water while the camera is above it (and the patches reaching
above the water while the camera is below), pass 1 the rest
[`orig: Foliage_RenderDetailPatches @ 0x609df4..0x609e1b, @ 0x60a1a0..0x60a1c6`].
The former reading "patch min-height" was wrong.

Both detail passes alpha-blend: the technique block enables
`ALPHABLENDENABLE` with `SRCBLEND=SRCALPHA` and `DESTBLEND=INVSRCALPHA`
(`Foliage_LoadDefAssets @ 0x60141f..0x601427`), and the PS emits
`r0.a = t0.a × v0.a` — the tested alpha is also the blended weight. The
distance fade is a continuous transparency ramp, not just an alpha-test
threshold shift; rendering these passes opaque turns the low-alpha `:fd`
fringe into hard sheets and makes cells pop as the fade crosses per-texel
thresholds.

The setup flag is also a depth-comparison toggle, not a wireframe or fill-mode
toggle. `Foliage_SetupDetailSlotDraw @ 0x6007c0` selects value 2 for the
secondary call at `0x6008fc..0x600912`; wrapper `0x67cac0..0x67caea` applies
that value to render state `0x17` (`D3DRS_ZFUNC`), so value 2 is
`D3DCMP_LESS` and the normal value 4 is `D3DCMP_LESSEQUAL`. Both passes are
double-sided and fogged through the wind VS's own vertex fog (see Detail
shader). HIGH and LOW reuse one cache revision but
receive distinct submission IDs. Consequently detail residents/hits remain
cell-based, while submission, intent, batch, and rendered-instance counters
count both near draws.
The reimpl ports the ordered submissions, references, shared fade, blending, and
write policy. The strict-`LESS` secondary is emulated exactly for
same-geometry resubmission: because HIGH wrote depth precisely where its
GREATER test passed, the secondary discards texels whose
`:fd alpha × fade` exceeds the HIGH reference (`u_high_pass_cutoff`),
landing only where HIGH left no depth; it also sorts `1e-3` nearer than its
equal-depth HIGH twin so Godot keeps the retail HIGH-then-LOW order
(`kFoliageSecondaryLowSortingOffset`; primary draw @ 0x60a653). Since
2026-09-24 the thermal view is ported: the runtime takes
`FrameRequest::thermal_view` (GameWorld feeds the dispatcher the
environment's world thermal gate each foliage frame) and submits one LOW per
patch at fade × 0.1. The two water-side passes take their own ladder rungs,
`kRungFoliageFarSide` (-6) and `kRungFoliageCameraSide` (-1)
(`engine/runtime/renderer/render_order.h`); the collector hands each leaf's
maximum height through for the side test. The water mirror excludes the
foliage blanket (retail's reflection prerender collects none, env #30), so no
reflection variant exists to port.

### Detail shader

`Terrain_CreateFoliageVertexShaders @ 0x5ff630` creates the wind shader.
**Corrected 2026-09-24:** the sine argument is `v0.x`, the pre-wind vertex's
render x (the Godot Z) RELATIVE to the patch's 512-unit sector origin (the
patch's D3D world translation, `FB20 << 9` stored by the collector
@ 0x603fc1), scaled by `c24.y = 1` and offset by the phase `c24.x`. The
displacement goes to render z (the Godot X): `mad r1.z, sin*bend, c25.x,
v0.z` with `c25.x = 0.03` (`flt_7C9B90`), weighted by the source-height bend
[`orig: g_FoliageWindSwayVS literal @ 0x7de648;
Foliage_SetupVertexShaderConstants @ 0x60074a..0x6007b4`]. The earlier port
swayed Godot Z by a sine of world X. The compiler resolves each detail
command's sector origin (`renderer::foliage_detail_wind_sector_origin_z`)
and the dispatcher stamps it as `u_wind_sector_origin_z`.

The same literal ends `dp4 r1, r1, c2; add r1, r1, -c5.x; mad oFog, -r1.x,
c5.y, c5.z`, with `c5` built from the device fog start/end
(`@ 0x6005ee..0x600652`), and the fog-mode-8 draw turns table and vertex fog
off (`CD3DDevice_SetFogAndBlendMode @ 0x677768..0x67779a`;
`Foliage_SetupDetailSlotDraw` selects mode 8 when the wind VS exists). The
detail fog is therefore linear in eye depth between start and end for EVERY
fog type, clamped per vertex and read from the wind-displaced position.
Ported 2026-09-24 (`foliage_vertex_fog` in `foliage_detail.gdshaderinc`); the
earlier port used exponential fog for type 0 and radial distance otherwise.

`Foliage_CreateLightmapBlendPS @ 0x5ff7a0` establishes the fragment combine:

`t0 × (t1 × (t1.a × c1 + c0)) × v0 × 8`

where:

- `t0` is the model's baked `:fd` texture;
- `t1` is the composed terrain tile-cache render target;
- `c0/c1` are sky and sun lighting registers;
- output alpha is `t0.a × v0.a` for the alpha test.

The reimpl implements the exact arithmetic and render states. Under a
`Terrain`, `t1` is always the composed page the lookup returns; a patch with
no resident page is not drawn, as retail's null lookup result skips the
patch's slot draw [`orig: Foliage_RenderDetailPatches: Terrain_FindSectorPatchRT
call @ 0x60a1de, null skip @ 0x60a1e6..0x60a1e8 to the slot loop's next
iteration @ 0x60a6a2`] (corrected 2026-09-24; the cold fallback and the raw
`.til` overlay path `u_tile_overlay` are retired). Only the terrain-less
editor preview still reconstructs `t1` analytically, from raw colormap RGB
plus the byte-quantized heightfield-normal/light DOT3 alpha.
`PolyTrn_RenderTile @ 0x60da70` clears authored colormap A in its
`0x00808080` base draw, so no colormap sun mask survives into this input;
`Terrain_GenerateNormalMap @ 0x603210` and
`g_PolyTrnTileBakeDot3LightPass @ 0x60e385..0x60e39e` supply the alpha
instead. The preview's 1024 normal atlas is clamped to the selected
quadrant's texel-center bounds, matching four retail 512 CLAMP samplers. The
detail emitter color is not a second dynamic time-of-day input.
`Foliage_RenderDetailPatches @ 0x609efc..0x609f2a` computes c6.rgb as the
componentwise product
`0x319F9D0/D4/D8 × 0x319F9E0/E4/E8`. The complete xref set for the second
vector contains only the render reads plus the writes of literal `1.0` at
`Terrain_InitLightingColorRamps @ 0x604ffa/0x605004/0x605010`; there is no
per-frame color writer. Therefore the active multitexture path's c6.rgb is
exactly the first vector.

`PolyTrn_InitTextures @ 0x60b03c..0x60b065` writes that first vector to
`(128/255,128/255,128/255)` when `g_PolyTrnHasBlendmap` is true and
`g_PolyTrnShaderTier >= 1`. The constant at `0x7DF2E0` has bytes
`81 80 00 3F`, the exact float encoding of `128/255`. Only the fallback at
`0x60b06a..0x60b13e` writes the source terrain texture's average RGB divided
by 255. The reimpl's fixed `0.5019608` uniform is therefore exact for the top
splat path, not a visual approximation; the fallback remains a distinct
non-splat path. c6.a is still the distance/pass fade described above, and
MODEL masks do not use this emitter factor.

The apparent EffectWorld point-light hook is inert on that selected path.
`Foliage_RenderDetailPatches` does call `Light_SelectAndEnableForDraw` once per
patch at `0x60a5dc`, after building the patch AABB, so the legacy device state
really does select and enable up to four D3D lights. But asset load first
creates `g_FoliageWindSwayVS` unconditionally (`Foliage_LoadDefAssets
@ 0x601278 -> Terrain_CreateFoliageVertexShaders @ 0x5ff630`), and
`Foliage_SetupDetailSlotDraw` installs that nonzero handle in every selected draw
descriptor at `0x60087a..0x600883`. The complete `vs_1_1` literal at
`0x7de648` declares only position, color, and texcoord; it has no normal or
light input and emits `mov oD0,c6`. The `ps_1_1` blend then uses that vertex
color only in `mul_x2 r0.rgb,r0,v0`; its lighting remains the cached-tile
`t1.a*c1+c0` fold. Consequently `SetLight`/`LightEnable` has no shader consumer
when the wind VS exists. It can affect only the failed-VS FVF fallback, which
the locked highest-quality retail profile excludes. OpenNova therefore
correctly has no foliage point-light uniforms; adding them would create a
non-retail max-quality response.

For the analytic t1 reconstruction, EnvFile preserves the direct
`Environment_GetLightDirectionFloat @ 0x57d870` tuple `g=(g0,g1,g2)`, not a
Godot/world XYZ vector. PolyTrn's D3DCOLOR pack (`0x60e201..0x60e331`) writes
GPU diffuse RGB `(g2,g0,g1)`, so the reimpl now packs `(z,x,y)` against the
normal-map RGB `(grid X slope, grid Y slope, up)`; `FORMAT_RGBA8` preserves
those channels. The old `(x,z,y)` mapping swapped the horizontal DOT3 axes.
The 08:00 oracle is light bytes `(231,83,187)`, with slope alphas
`0.8987774/0.0794002`. Retail foliage itself does not compute this DOT3; its
blend PS consumes the cached `t1.a`. The `.til` overlay reaches foliage only
through the terrain page composer, which draws the ordered tile quads RGB
only into the page (see the tile record); the page composition itself is
terrain's (D-TERRAIN-7, closed 2026-09-26).

The tile projection is explicitly **pre-wind**. In the
`g_FoliageWindSwayVS` literal (`0x7de648`, copied at `0x5ff691`, assembled
at `0x5ff6df`), `mad r1.z` applies wind at `0x7de7f1` and
`m4x4 oPos,r1,c0` consumes it at `0x7de821`; independently,
`m4x3 r10,v0,c12` at `0x7de835` and the c7/c8 `oT1` projections at
`0x7de881/0x7de897` still consume the original vertex. The reimpl therefore
derives the page UV from the pre-wind position before displacing the vertex
(the former hosted-overlay UV path is retired with the overlay).

The max-quality page/cache projection is now closed as well. The cache record
stores `lod`, packed local X/Z, routed sector X, and routed sector Z
(`PolyTrn_RenderTile @ 0x60db67..0x60dc02`). `Terrain_FindSectorPatchRT @
0x6042a0` does not resolve an exact record: it walks granularity 32 to 512
units and takes the first resident record, in record order, whose masked
coordinate matches the point's within the point's sector, with no LOD
preference and no same-frame restriction, so a coarse match can return a
page that does not contain the point
[`orig: Terrain_FindSectorPatchRT @ 0x6042B0..0x60430B`]. The port is
`TerrainTileCompositionCache::lookup`, queried at the cell centre
(corrected 2026-09-24; the earlier text named the function
`Terrain_FindSectorTileRT` and called the match exact). The live quality
branch is not the inverse-view fallback: `g_FoliageWindSwayVS != 0 @ 0x5ffbb0`
is true after required shader creation, so `Foliage_RenderDetailPatches @
0x60a25d..0x60a297` builds c7/c8 from the packed page origin and
`1/(1024>>lod)`. Its model translation is D3D `(world Z,0,world X)`
(`@ 0x60a35c..0x60a3da`); the literal's `m4x3 r10,v0,c12` followed by c7/c8
therefore reduces exactly to
`((world X-origin X),(world Z-origin Z))/span`. The uploads at
`Foliage_SetupVertexShaderConstants @ 0x6006ab..0x600704` pin the two rows
(c7 upload `0x6006f0`, c8 upload `0x600704`).
`TerrainTilePageProjection` is the single portable reduction used by terrain,
detail foliage, MATCHTERRAIN, and the static-shadow page raster; the old
per-consumer `origin_span` uniforms were removed, with no failed-VS compatibility
projection retained. The page composer's last facet closed with D-TERRAIN-7,
and the detail pass's page-edge addressing, the CLAMP both detail pass states
apply, closed D-FOLIAGE-7 (2026-09-26; see the catalog).

## Distant MODEL/depth-mask tier

### Driver and cells

The driver is the visible sector-entity walk in
`Terrain_RenderSectorEntitiesBySide @ 0x5c7d50`, and the tier is the
**hide-in-grass mechanic**: the walk calls the foliage update only for
entities whose `MoveOrder` dword (entity `+0x12C`) carries a stance bit
(`0x100` prone / `0x200` crouch) and whose `groundEntity` is empty (standing on terrain, not
on a vehicle deck or floor) [`orig: flags test @ 0x5c7dc2/0x5c7ded
(MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7`]. Placed
objects never write `MoveOrder`, so retail never generates model foliage
around crates, fences, or buildings; only infantry ever carry the bits
(local packer `Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd` from the
stance latches, remote apply `NapiNPServerMsg_HandleStanceChange @
0x501c60`, vehicle attach clears `@ 0x435c54/0x43561e`).

Retail AI never sets them either (2026-09-24 byte scan of every `.text`
store to entity `+0x12C`): the complete writer set is
`Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd` (local),
`NapiNPClientMsg_0x00A @ 0x430576..0x43058f` (local latch restore),
`NetPacket_SerializePlayerState @ 0x4c11ec..0x4c1e2c` and
`NapiNPServerMsg_HandleStanceChange @ 0x501cc9..0x501d01` (remote players),
the clears (`Entity_ProcessVehicleAttach @ 0x435c42`,
`Entity_DetachFromVehicle @ 0x43560c`, `PlayerClass_InitEntity @ 0x4b1069`,
`Server_ProcessPlayerDeath @ 0x517880/0x51788c`), the
`Game_ProcessMainFrame` mask `@ 0x52666f`, the save record copy, and the
physics `or 10h` moving-bit writers; `Entity_UpdateInfantryAI` has none. The
MATCHTERRAIN pass and the MODEL masks are therefore player-only in retail,
and the reimpl's AI (which never sets `net_stance_bits`) matches.

A passing entity rides the BySide wave of its water side (entity z − 1.0
against the water, `@ 0x5c7dd2`, side `@ 0x5c7dfd..0x5c7e18`; the far wave
draws first, `Terrain_RenderWorldScene @ 0x5c953e..0x5c955f`) and
enters the tier at view depth 38 (`Foliage_UpdateModelTiles`'s own
`>= 38.0` view-Z gate, `@ 0x601f99..0x601fab`). The per-entity
draw parameter is `clamp(4096 / (distance_units + 1), 8, 128)` — the model
tier's alpha reference, so the mask ring thins with distance but never
disappears [`orig: fdivr 4096.0 @ 0x5c7ea4..0x5c7eb3, clamp @
0x5c7eb8..0x5c7ec9`; hosted as `silhouette_alpha_reference`].
`Foliage_UpdateModelTiles @ 0x601f50` then walks four neighboring 16-unit
cells around the anchor.

Within each cell:

- the **foliagemap** palette index selects definition slots;
- the surface/charmap is not consulted;
- candidates must lie within ±4 units of the anchor on both X and Z;
- at most 21 candidates survive per cell;
- the same path blocker / `FORCE_ON` rule applies.

Reimpl anchors are the sim's crouched/prone infantry standing on terrain
(`Simulation::get_foliage_mask_anchor_positions`, from the replicated
`net_stance_bits` + `ground_target`), corrected 2026-07-16 — the earlier
placed-object anchor feed (D-FOLIAGE-11, FIXED) generated masks retail
never renders and collapsed frame rate on object-dense vistas (03TR
airfield: 1143 frustum-passing anchors thrashed the 1000-entry model
caches at ~7k regenerations per frame, 212 ms of a 354 ms frame; with the
witnessed gate the tier idles in object-only scenes).

**Corrected 2026-09-24 (D-FOLIAGE-9 FIXED):** the camera-frustum stand-in is
gone. The anchors are the occlusion frame's collected organics with stance
bits and no ground entity: placed rows through `entity_render_visible`
(the collector legs, then the waves' contained render_TOC test), runtime
twins through their live verdict, and bare wire rows through the person or
sphere collector legs with the received stance bits and no carrier
(`godot/src/simulation/simulation_occlusion.cpp`). Retail's
`Terrain_TestSectorEntityOcclusion` call inside the BySide wave
(`Terrain_RenderSectorEntitiesBySide @ 0x5c7d96`, skipped for contained
entities `@ 0x5c7d8b..0x5c7da0`) reaches the anchors through those same
verdicts (`OcclusionWorld::render_wave_toc_occluded`). The local player takes
the same collector verdict as any person, because retail's pool walk has no
local-player exception [`orig: Terrain_CollectVisibleEntitiesForTerrain
@ 0x5c8c60`]. The frame compiler applies only the MODEL walk's `>= 38`
view-depth floor and each anchor's water side
(`renderer::foliage_entity_far_side`, camera side
`renderer::foliage_camera_above_water`,
`Terrain_RenderWorldScene @ 0x5c93a1..0x5c93b0`). The foliage frame
leg now runs after the occlusion leg, in the live table and the frozen-pose
replay.

### Normalization and eight-sample ground fit

`Foliage_FillInstancedModelBuffers @ 0x5ffa20` and
`Foliage_UploadModelTileVSConstants @ 0x600f00` show the model normalization:

- footprint radius is `0.75 × max(halfExtentX, halfExtentZ)`;
- model height is scaled by `0.5`;
- yaw is applied once by the generated corner positions.

The generator samples four rotated footprint corners and the four edge
midpoints. Those eight heights form `(E_A, T_A, E_B, T_B)`, the biquadratic
correction consumed by `g_FoliageGridPlacementVS`. The fresh runtime returns the
four corners and fold explicitly. Since 2026-09-24 the GPU runs the
placement: the compiler builds each slot's normalized instanced vertex buffer
(x/z over the bound square mapped to [0,1], y halved)
[`orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20..0x5ffae7`], and each
MODEL submission is one MultiMesh of that mesh whose instances carry the
`c[12+4i]..c[15+4i]` block: render x corners = local B + (−keyLo), the corner
heights, render z corners = local A + keyHi, and the fold
[`orig: Foliage_UploadModelTileVSConstants @ 0x6010e6..0x601209`], at most 21
instances per tile [`orig: Foliage_GenerateModelTileInstances @ 0x600e75`;
`foliage::kModelTileInstanceCap`]. The CPU silhouette expansion and the MODEL
mesh cache are gone.

Portable goldens pin the first silhouette for a known quadratic height field:
candidate identity, four rotated corner positions/heights, all four fold
coefficients, and the fitted center height.

### MODEL blend/depth draw

`Foliage_DrawModelTileSlot @ 0x601d90` binds `:fd`, disables culling and fog,
keeps Z test/write enabled, and selects an alpha reference
`clamp(int(4096 / (distance + 1)), 8, 128)`.

The first 2026-07-13 inspection stopped one state layer too early. The uploaded
diffuse constant is indeed `(0,0,0,1)`, but it is source color, not final scene
color. `Foliage_LoadDefAssets @ 0x6015b7..0x601613` enables blending with
`SRCBLEND=ONE` and `DESTBLEND=ONE`; `RenderState_ApplyToDevice @ 0x681920` and
`GfxBlend_ApplyToDevice @ 0x6817d0` confirm that descriptor mapping. The fixed-
function RGB selects the black diffuse value, so the framebuffer equation is:

```text
0 × ONE + destination × ONE = destination
```

Alpha remains `:fd.a × diffuse.a`, the pass uses `D3DCMP_GREATER`, and accepted
texels still write Z. The retail pass is therefore an unlit, unfogged,
color-invisible depth mask, not an opaque black silhouette. Godot maps the
color/depth effect to `blend_add + depth_draw_always + fog_disabled`, retains
the dynamic manual strict-greater alpha discard, and deliberately omits both
`ALPHA` and `depth_prepass_alpha`. A global alpha prepass would run before
terrain color and can expose clear-color holes instead of preserving the
already-rendered scene.

**Order (ported 2026-09-24, D-FOLIAGE-10 FIXED).** Retail draws each
crouched/prone person's masks immediately inside that person's BySide wave,
after the wave's MATCHTERRAIN subpass and before the wave's queued entities
[`orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7ddf..0x5c7f11 ->
Foliage_UpdateModelTiles @ 0x601f50 -> Foliage_DrawModelTileSlot
@ 0x601d90`]: the far wave inside `BySide(far, 0) @ 0x5c955f`, before the
far-side alpha flush `@ 0x5c9596`; the camera wave inside
`BySide(camera, 0) @ 0x5c9638`, after the water pass `@ 0x5c95dc` and before
`Scar_DrawBatches @ 0x5c9658` [`orig: Terrain_RenderWorldScene`].
Every person of that wave, and everything drawn after it, depth-tests
against them. The reimpl draws the masks in two places:

1. in the transparent list at their retail slots: the far wave's masks lead
   `kRungAlphaFarSide` and the camera wave's lead `kRungScars`, each with the
   sorting offset `-1e6` (`kFoliageMaskFarSideRung` /
   `kFoliageMaskCameraSideRung`, `renderer/foliage_frame.h`), so every later
   transparent and the post-transparent particle pass test against them. The
   ladder's own `kRungFoliageMaskFarSide` (-10) and
   `kRungFoliageMaskCameraSide` (-3) sit immediately before those rungs and
   give the same order; the foliage compiler still uses the leading-offset
   form;
2. in a PRE_OPAQUE RenderingDevice pass (`FoliageMaskCompositorEffect` /
   `FoliageMaskPass`, `godot/src/terrain/foliage_mask_pass.*`) that
   rasterizes the same instances per view into an RG32F target (R = the
   nearest far-wave mask depth, G = the camera-wave one), bound as
   `opennova_foliage_mask_depth`. Person draws, and models drawn in a
   person's slot, carry `u_foliage_mask_side`
   (`ObjectModel::refresh_foliage_mask_frame`) and test it in every pass
   (`godot/shaders/object/foliage_mask.gdshaderinc`): behind a same-wave mask
   the NORMAL draw fails and the MATCHTERRAIN pre-pass colour remains
   (nothing remains without a pre-pass); behind a far-wave mask a camera-wave
   person draws nothing. The person P3 post-multiply layer and the blended
   NORMAL output test the same texture, since retail draws them in the wave's
   flush after the masks; the first-wave P3 of buildings and vehicles stays
   at `kRungObjectPostMultiply`, before both masks, as retail's flush
   `@ 0x5c9506` precedes the person waves. A blended strip behind a mask
   shows nothing of itself: no technique that blends has a MATCHTERRAIN pass
   (`godot/shaders/object/pipeline_manifest.json` gives the skinned-stance
   MATCHTERRAIN contract only to opaque or alpha-tested techniques), so
   retail rejects it too. The eye match keeps the mirror and every other
   camera unmasked.

The grid-placement wind term ADDS the already-halved source height times
`c9.x` to render x (the Godot +Z): `mad r10, v0.y, c9, r10`, with
`c9 = (sin(++counter × 0.001) × 0.08, 1, 0, 0)` re-uploaded per actual model
draw, repeated submissions of one resident entry included
[`orig: Foliage_UploadModelTileVSConstants @ 0x60108e..0x6010cf`;
`renderer::foliage_model_wind_offset`, `u_wind_offset`] (corrected
2026-09-24; the earlier port subtracted it on render Z). In this tier it
moves depth coverage, not black scene color.

The DETAIL tier's phase register is `c24 = (GetTickCount() × 0.003 +
g_EnvWaveOscRing[0] / 655360, 1, 0, -)` with the `0.03` sway scale in `c25.x`
(`Foliage_SetupVertexShaderConstants @ 0x60074a..0x6007b4`: `fild` the ms
word, `fmul` 0.003 (`flt_7DE9D4`), `fild g_EnvWaveOscRing`, `fmul`
`flt_7DE9D0 = 0x35CCCCCD` = 1/655360 (`@ 0x600767`), `faddp`; `c25.x =
flt_7C9B90` uploaded `@ 0x600788..0x6007b4`), so `g_FoliageWindSwayVS`'s
`sin(v0.x + c24.x) × bend × c25.x` sways on a WALL-CLOCK phase (3 rad/s, one
cycle every ~2.1 s) offset by the weather oscillator's ring slot 0
([env-tod-re.md](../env/env-tod-re.md) §weather tick, the ring readers).
**Corrected 2026-09-24:** the ring scale is 1/655360, not 1/65536 (the port's
phase had jumped up to ~1 rad per 256 weather ticks), and `c24.w` carries no
scale. **Corrected 2026-08-30:** the reimpl drove the detail phase
from the terrain-scene counter (`× 0.001` per frame, ~50x slower than retail at
60 fps) with no weather term; `renderer::FoliageFrameCompiler` now takes the
embedder's ms clock and `osc_ring[0]` through `FoliageViewInput` (the
dispatcher's `set_weather`), folding the clock term modulo 2π for float
precision. The MODEL tier keeps its per-draw counter clock.

## The `:fd` asset bake

Both tiers bind the same per-definition `:fd` texture. The load-time operation
is a two-source custom mip-chain construction, not a flat-gray replacement:

1. `Foliage_LoadDefAssets @ 0x60162f..0x601650` locates the model's authored
   diffuse and its `%s:fd` texture object.
2. `0x6017a2..0x60190d` replaces authored alpha with the wrapped power-of-two
   kernel
   `A' = (4C + N+S+E+W + 2(NW+NE+SW+SE)) >> 4`; authored RGB is untouched.
3. `0x601990..0x6019a6` makes a companion whose RGB is exactly `0x808080`
   and whose alpha is the same smoothed `A'`.
4. `0x6019c9..0x6019e4` passes authored-RGB-plus-`A'` as the base and the gray
   companion as the second source to
   `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`.

After any device-limit pre-downsample, let `d=min(width,height)` and increment
`N` while repeatedly shifting `d` right until it is at most 2
(`0x6873a1..0x6873e8`). For power-of-two square inputs,
`N=log2(size)-1`. Retail emits levels `i=0..N-1` at dimensions
`width>>i` by `height>>i`. Before the next level, each source is independently
box-downsampled per channel with floor division:
`(p00+p10+p01+p11)>>2` (`GTexture_Downsample2x2_RGBA8 @ 0x687000`). At level
`i`, the gray weight is `w=min(256, floor(320*i/N))`; each RGB channel is
`floor(((256-w)*authored + w*128)/256)`. Alpha is copied from the downsampled
base unchanged. Thus mip 0 retains authored RGB and smoothed alpha; only
successively smaller mips approach gray. A constant 8×8
`(240,64,16,173)` source produces mip 0 `(240,64,16,173)` and, with
`N=2,w=160`, mip 1 `(170,104,86,173)`.

The definition color modes do not restore this color through vertex diffuse.
The live `color_lower`/`color_upper` fields at `0x2C25F78/0x2C25F7C` are
selected around the source-Y `0.125` boundary at `0x6001fd..0x6002d8`, but
`Foliage_GenerateInstances_0` then unconditionally overwrites that packed
color at `0x6002db..0x60030a` with the source-height bend carrier.
`g_FoliageWindSwayVS` (`0x7DE648`) consumes v5.x only for bend and emits
`oD0=c6`. Authored leaf color therefore survives through sampled t0, not v0
or `color_upper`.

The reimpl now builds the complete chain in portable
`build_fd_rgba_mip_chain`, passes the packed levels directly to Godot, and no
longer asks Godot to regenerate generic mips. Native and GUT literal vectors
pin authored mip-0 RGB, the wrapped alpha, the exact 4×4 retail blend, and the
reimpl-required 2×2/1×1 terminal continuation. This fixes D-FOLIAGE-5's
authored-chain half and the former flat ground-olive result; the terminal-LOD
selection approximation is bounded below. `VegAssets`' primary-submesh
lookup remains only the binding locator: surviving geometry is still the
aggregate of every surface of every `build_lod_submeshes(0)` result.
The maximum-video retail comparison profile has `texfilter_level=3`. The
parser at `0x551217..0x551239` selects the anisotropic branch
(`MINFILTER=ANISOTROPIC`, linear mip filtering, and device-cap maximum
anisotropy at `0x67e38a..0x67e45b`). The older cfg-0/driver-forced account came
from invalidated captures and is no longer admissible evidence. The reimpl
therefore samples the `:fd` chain anisotropically through `textureGrad`. Its
longest-gradient scale is a conservative terminal guard: it never admits
Godot's synthetic 2x2/1x1 tail past retail's 4x4 terminal level, but is not an
exact reconstruction of the device's anisotropic LOD footprint and can bias a
grazing footprint sharper.

## Persistent caches and submission identity

The detail tier uses the persistent slot pool in
`Foliage_UpdateDetailCellSlots @ 0x601b30`, with strict signed-age LRU
replacement; duplicate misses in one update allocate once. **Corrected
2026-09-24:** retail updates the slots BEFORE the patches draw, so a newly
collected cell draws in its generation frame. The terrain frame
(`Render_ProcessMainSceneFrame @ 0x5ca654` -> `PolyTrn_RenderFrame`) ends with
the four per-definition slot updates (`@ 0x60f0ea..0x60f10f`), and only the
later scene core (`Terrain_RenderWorldScene`, called `@ 0x5ca8ec`)
reaches `Foliage_RenderDetailPatches`, whose `Foliage_SetupDetailSlotDraw` finds
this frame's key (`@ 0x600807..0x60081c`). The earlier reading ("cached
geometry is drawn before the update, a miss first shows on the next terrain
render") was wrong; the runtime now updates, then draws. Retail's 16-bit index
ceiling yields the hosted per-definition capacity
`min(128, floor(65534 / (36 × sourceVertexCount)))`.

The distant model cache in `Foliage_UpdateModelTiles @ 0x601f50` is a fixed
1000-entry pool per definition. A hit touches its resident entry and refreshes
geometry only when `((sceneCounter + 2 × slot) & 7) == 0` (check at
`0x60209a`); every caller still submits the resident entry.

Retail reaches that cache through the visible BySide walk, and clustered
anchors revisit the same `(slot, cell key)` there too: every on-phase hit
visit regenerates the resident around its OWN anchor and draws it at once
[`orig: Foliage_UpdateModelTiles @ 0x601f50, hit touch @ 0x60208b, phase test
@ 0x602085..0x60209a, regeneration @ 0x6020aa, draw @ 0x6021a5`]. The
2026-07 reimpl coalesced those refreshes (the first on-phase visit
regenerated, later visits kept their submissions), a measure taken while the
frustum stand-in over-fed anchors (at the `00TRe.bms` spawn it held MODEL
output at 1,795 instances / 568 submissions and cut dispatcher p95 from
55-73 ms to 15.8 ms). **Removed 2026-09-24:** with the anchors on the
collector verdicts and the placement on the GPU (no CPU mesh rebuild per
refresh), the runtime regenerates per visit as retail does.

The fresh runtime identifies a resident mesh by slot, cell key, and monotonic
revision, and gives every draw a separate submission ID. The adapter builds all
same-frame submissions before applying evictions, then clears draw-pool mesh
references before erasing evicted cache entries. This preserves duplicate
same-frame draws without stale references. A `terrain_changed` signal also
resets the runtime when the same `TerrainData` resource mutates in place.

## Reimpl architecture

The fresh implementation deliberately has three layers:

1. `engine/formats/foliage/runtime.{h,cpp}` — Godot-neutral placement, gates, fades,
   alpha refs, corners/fold, and `:fd` preprocessing.
2. `Terrain` — exact runtime 16-unit detail-cell collection from terrain
   height bounds.
3. `FoliageDispatcher`: definition/mesh adaptation, live samplers, the
   persistent detail slot/key/revision mesh cache, one normalized MODEL mesh
   per slot instanced per submission (a MultiMesh of the submission's
   `GridPlacementVS` blocks; no CPU MODEL expansion since 2026-09-24), and
   retained `RenderingServer` scenario-instance RID pools for detail and
   silhouette. It also owns `FoliageMaskPass`, the PRE_OPAQUE RenderingDevice
   mask pass (MODEL blend/depth draw). The typed `FoliageDrawList` is
   diff-applied in submission order; mesh, material, uniform, and visibility
   stamps update only the fields that changed, with no `MeshInstance3D`
   fallback.

`FoliageDispatcher.get_backend_report()` exposes the opaque backend's pool
sizes, ordered portable pass identities, and per-frame server-write counters
without exposing RIDs. Its draw rows carry `far_side`, `render_priority`,
`sorting_offset`, `instance_count` and `wind_offset`, and a `mask` sub-report
describes the mask pass. A stable live frame has zero instance creation,
scenario, configuration, base, material, material-parameter, and visibility
writes. The retail wind clocks still advance, so their instance-uniform writes
are expected rather than structural churn. Raster probes use the public
`apply_probe_draw_control` seam to pin wind and isolate a typed pass; they do
not discover backend state through scene children.

The discarded placement, dispatcher, model-dispatcher, and fd-bake clusters
were deleted. The live foliage-map resource now single-sources DETAIL sampling,
world-to-map coordinates, and effective resolution for runtime and diagnostics.

Runtime sampling comes directly from `TerrainData`:

- detail gate: flat-wrap `get_detail_foliage_index_fixed` at the candidate's
  SOURCE-ATLAS position, the cell's atlas minimum
  (`FoliageDetailPatch` / `DetailCell::atlas_x`, `atlas_z`, carried by the
  collector) plus the local offsets (corrected 2026-09-24, D-FOLIAGE-12); the
  terrain-less editor preview, which has no routing, passes its world
  minimum;
- MODEL gate: sector-routed `get_foliage_index_world`;
- ground: bilinear terrain height;
- terrain projection: the shared world-to-source transform.
- exclusion: the active mission `TerrainTileInfo` inclusive AABB scan.

The runtime adapter keeps the two live foliage-map Callables separate: DETAIL
reads the flat wrapped map resource at atlas coordinates, while MODEL keeps
the routed terrain projection; both land on the same sector-routed pixel for
a routed sector. The world-flat detail seam
(`get_detail_foliage_index_world` / `TerrainFoliageMap.sample_detail_index_world`)
encoded the refuted policy and is deleted. Terrain-less preview dispatchers
still read the analytic heightfield-normal atlas. The former ONED paint, eyedropper, and standalone
preview consumers were removed by ADR 0037 and no longer contribute a parity
claim or divergence.

## Verification

- `foliage_map_test`: negative/fractional Q16 wrapping, integer boundaries,
  non-power-of-two widths, exponent-10 actual-width stride, and shared
  runtime/format coordinates, including wrapped footprints that do not touch
  unused non-power-of-two stride cells.
- `foliage_runtime_vectors`: literal detail and MODEL PRNG positions/yaws on
  the retail key frames (re-derived 2026-09-24 from an independent emulation),
  the detail gate at the atlas position (world `(18.63, 49.70)` samples atlas
  `(530.63, 561.70)`), separate atlas-detail and routed-MODEL gate callbacks
  with disjoint routing, path/force-on behavior, 20–42 fade shared by both
  near submissions, pass switch, the strict-LESS secondary marker, the
  thermal LOW-only × 0.1 pass, same-frame draw across the detail cache cases,
  four silhouette cells, 21 cap, per-visit MODEL regeneration, eight-sample
  fold, distance alpha ref, and `:fd`.
- `renderer_foliage_frame_compile`: per-vertex detail placement, first-compile
  commands, the thermal command, the MODEL slot mesh and instance blocks, the
  water sides, the side and mask rungs and sorting offsets, and the side
  helpers.
- `renderer_foliage_frame_wind`: the 1/655360 ring scale and the sector
  origin of positive and negative keys.
- `renderer_render_order`: the mask rungs' places in the ladder.
- `occlusion` (`test_render_wave_toc_skips_contained_entities`): the waves'
  contained render_TOC test the anchors ride.
- `til_foliage_blocker`: inclusive min/max boundaries, stored-negated Z,
  unsnapped entry positions, and empty-array behavior for the shared mission
  tile scan.
- `terrain_foliage_detail_collector`: mip-bound 16-unit keys, raw-height distance
  boundary for authored and flat sectors, mixed-height ancestors with nearby
  leaves, quadrant mapping, signed/flat packing with Z-min keys, per-quadrant
  and translated atlas minimums, the leaf maximum height, traversal order,
  authored/flat 127→128 capacity, and preservation of a full list.
- `terrain_frame_compiler`: foliage handoff after 223/224 preceding terrain
  draws, saturation by several earlier sectors or default flat fallback,
  unchanged forward-frustum/distance keys and order, flat keys propagated
  into real empty foliage cache entries with no sampling or submissions, and
  the node-distance handoff gate (a [0,127] node under a camera at Y=0, a
  flat sector under a camera at 64 with raw cells at 64, the inclusive
  58-unit boundary).
- `foliage_runtime_adapter_test.gd`: fresh public adapter contract, disjoint
  DETAIL/MODEL callback routing, tier map selection, full multi-surface LOD0
  aggregation, view-depth gate, no
  manufactured anchors, detail-mesh cache identity/eviction ordering (MODEL
  meshes are no longer cached per cell), reset behavior, the additive/depth
  shader-state contract, the MODEL masks leading their side rung, the detail
  passes' side rungs and secondary order, and the thermal single faint LOW.
- `foliage_mask_consumer_test.gd`: person draws take their BySide wave's mask
  side.
- `foliage_game_world_integration_test.gd`: the Dvxi5 witnesses, where the
  sector-routed witness grows detail grass and the flat-only witness grows
  none.
- `simulation_test.gd` / `game_world_test.gd` / `world_frame_order_test.gd`:
  the anchors come from the occlusion frame (the local player on the collector
  verdict, anchored once, not when out of view; standing NPCs never), and the
  foliage leg runs after occlusion.
- Adapter regressions pin the retained RID backend, zero structural server
  writes on a stable live draw list, ordered near HIGH/LOW pass identities and
  their depth states, scenario/reset/visibility lifecycle, exact `00TRa` entry
  #25's stored-negated-Z decode directly onto the
  positive terrain/Godot Z plane, the blocked armory-truck center and uncovered
  c3/c4/c5 controls, selective rather than blanket blocking, and active-blocker
  frame diagnostics.
- `game_world_test.gd`: parses the co-named mission payload before terrain
  build, shares one parsed resource with terrain/foliage, clears it on unload,
  and prevents stale blockers on a subsequent no-TIL load.

- `foliage_tile_cache_runtime_test.gd`: production Terrain-to-dispatcher order,
  shared `Texture2DArray` ownership, valid ready-layer/page bounds, the page
  skip (on a cold terrain frame the submitted detail cells draw nothing until
  their pages compose, then every drawn patch borrows a resident page), and a
  cross-frame regression in which resident fine pages from earlier frames
  still answer the lookup first after a coarser-LOD frame, as retail's
  record-order lookup does (the pre-2026-09-24 regression asserted the
  opposite).
- the `foliage_flicker_regression` probe: real rasterized destination-color
  preservation, strict alpha acceptance/rejection, retained late-consumer
  depth, screenshot-shaped exact-black-component detection, and fixed-input
  frame stability for both tiers. It pins wind through the dispatcher's public
  RID probe-control seam rather than searching for draw children.
- the `foliage_spawn_capture` probe with `flicker: true`: the real
  `00TRe.bms` player spawn, fixed-input HIGH/automatic coverage with deliberate
  dropout and fade-response controls selected by portable pass identity. Its
  material/mesh diagnostics come from `get_backend_report`; LOW-far is
  explicitly reported as skipped when it has no pixels in the exact spawn
  view.
- the runtime scene (the retired `runtime_scene_probe`) and the visual probes: detail camera selection uses
  the flat gate oracle; the runtime scene consumes Terrain's typed native
  detail-cell vector, while its minimal fixture intentionally contains no
  `.3di` models.

## D-FOLIAGE divergence catalog

| ID | Fresh disposition |
|---|---|
| D-FOLIAGE-1 | **RETRACTED/FIXED, clarified 2026-07-14.** The prior per-corner colored-emitter premise belonged to the inverted port. Detail vertex diffuse is the recovered bend carrier; the distant MODEL pass uploads black only as the zero-contribution source of an additive depth mask. |
| D-FOLIAGE-2 | **FIXED.** Exact detail lightmap-blend arithmetic is hosted; the missing composed `t1` producer is separated as D-FOLIAGE-7. |
| D-FOLIAGE-3 | **FIXED 2026-07-13; axes corrected 2026-09-24** ("Port the foliage vertex-shader sway and fog constants"). Both recovered wind terms are hosted in their tier-specific shaders. The detail term displaces render z (Godot X) by a sine of the sector-local render x (Godot Z) scaled by `c25.x`; the MODEL term adds to render x (Godot +Z). See Detail shader and MODEL blend/depth draw. |
| D-FOLIAGE-4 | **SUPERSEDED/FIXED 2026-07-13.** The 2026-07-08 two-tier port was itself inverted and has been deleted. Fresh detail expansion and MODEL ground-fit paths replace it. |
| D-FOLIAGE-5 | **FIXED for the authored chain and sampler selection; bounded terminal-LOD approximation.** The portable custom chain keeps authored RGB at mip 0, blends recursively downsampled later retail mips toward `0x808080` with `w=min(256,floor(320*i/N))`, preserves base-chain alpha, and supplies Godot's required terminal levels without generic mip regeneration. The round-4 filtering adjudication pins anisotropic sampling. The host's longest-gradient guard keeps the synthetic 2x2/1x1 tail out, but can choose a sharper grazing footprint than the device's minor-axis/maximum-anisotropy LOD selection. |
| D-FOLIAGE-6 | **FIXED 2026-07-14.** The former opaque-black conclusion missed the downstream ONE/ONE blend state. The reimpl now preserves destination color while retaining the recovered strict-alpha-tested MODEL depth write. |
| D-FOLIAGE-7 | **FIXED (MATCHING, 2026-09-26: the last facet, the detail pass's page-edge addressing, is the CLAMP the reimpl already sampled; narrowed 2026-09-24 and 2026-08-23).** The foliage half of the page input is ported: detail `t1` is the page `TerrainTileCompositionCache::lookup` returns, retail's `Terrain_FindSectorPatchRT @ 0x6042a0` rule (first resident record in record order at granularity 32..512 in the point's sector, no LOD or same-frame preference, `@ 0x6042B0..0x60430B`), from the ported 128-record cache whose pages compose inside the frame that draws them, with retail's TOD refresh ("Port retail's terrain page record cache and compose pages before they draw"); a patch with no resident page is skipped, as retail's null lookup skips it (`Foliage_RenderDetailPatches @ 0x60a1de`, `@ 0x60a1e6..0x60a1e8` -> `@ 0x60a6a2`; "Retire the detail foliage's raw .til overlay fallback"); the raw `.til` overlay path and the hosted 1024 bake are gone ("Retire the hosted 1024 .til overlay bake"), so the overlay reaches foliage only through the page composer, whose `.til` quads are RGB only over the DXT5 atlas ("Compose terrain pages with the retail D3D9 raster and texture filters", "Encode the terrain atlas and detail layers with retail's D3DX DXT codec"). The c7/c8 reduction, PROJSHAD admission/blending and the temporary-blue composite stay exact as recorded 2026-08-23. The last two facets closed 2026-09-26: (1) where the patch UV leaves the returned page (a coarse or finer record need not contain the patch), the page clamps: it reaches t1 through slot 2 (`@ 0x60A211`), and both detail pass states' intrinsic words (LOW `0x2560000`, HIGH `0x2460000`; `Foliage_LoadDefAssets @ 0x601519 / 0x601530`) carry the stage-1 CLAMP bit (`CGfxShader_ApplyPass @ 0x68327E..0x683286`), the reimpl's clamp-to-edge; (2) the page producer's last facet closed with D-TERRAIN-7. Generic whole-process CTRL/RNG ordering is D-3DI-2. |
| D-FOLIAGE-8 | **FIXED 2026-07-14.** Direct retail inspection resolved the supposed path/spacing substrate as the shared mission `.til` array. `til_blocks_foliage` ports the exact linear inclusive 16x16 AABB scan, GameWorld parses `<mission>.til` before terrain build, and terrain/foliage/network state share that resource/payload; `FORCE_ON` continues to bypass the sampler in the portable runtime. [orig: `Foliage_PathBlockedByPlacedTile @ 0x606490`; `Terrain_LoadTileInfoFile @ 0x60a740`; `Terrain_GetSurfaceTypeAtPosition @ 0x606510`] |
| D-FOLIAGE-9 | **FIXED 2026-09-24** ("Port the foliage depth masks, their water sides and anchors", "Close the foliage mask residuals: atlas map gate, local anchor, P3 order", "Run render_TOC on collected entities outside blink boxes"). Narrowed 2026-07-16 to visibility membership: a camera frustum stood in for retail's visible-sector walk and `Terrain_TestSectorEntityOcclusion @ 0x5c4610`, and same-frame refreshes of one `(slot, cell key)` were coalesced. The anchors are now the occlusion frame's collected organics with stance bits and no ground entity (placed rows via `entity_render_visible`, runtime twins via their live verdict, bare wire rows via the person/sphere collector legs with the received stance bits and no carrier); the waves' contained render_TOC test (`Terrain_RenderSectorEntitiesBySide @ 0x5c7d96`, skipped for contained entities `@ 0x5c7d8b..0x5c7da0`) runs inside those verdicts (`OcclusionWorld::render_wave_toc_occluded`); the local player takes the same collector verdict, since `Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60` has no local-player exception. The compiler keeps only the MODEL walk's `>= 38` floor (`Foliage_UpdateModelTiles @ 0x601f99..0x601fab`), and every on-phase visit regenerates around its own anchor (`@ 0x602085..0x6020aa`), so the coalescing is gone. Since 2026-09-26 the weapon Inset pass (`Render_WeaponInsetScene @ 0x5C9740`: its traversal `sub_60FF50` called `@ 0x5C9A2F`, `Render_TerrainScene` called `@ 0x5C99B7`, `Terrain_RenderWorldScene` called `@ 0x5C9DE9`) compiles its own detail cells (from its own terrain traversal), anchors and mask frame on the shared compiler (one cache, one model pool), after the main view and the Inset collect (the counters `@ 0x601B36`, `@ 0x610CF7`; the 2026-09-26 section). |
| D-FOLIAGE-11 | **FIXED 2026-07-16.** The reimpl fed every placed mission object as a MODEL-tier anchor; retail's sector walk generates the tier only for entities with `MoveOrder` stance bits (`0x100` prone / `0x200` crouch) and an empty `groundEntity`, the hide-in-grass masks around infantry [`orig: @ 0x5c7dc2/0x5c7ded/0x5c7dd5`]. Anchors now come from the sim's stance query (over the occlusion frame's collector verdicts since 2026-09-24, D-FOLIAGE-9). The former ONED preview had no infantry and its placed-object `anchor_provider` plumbing was removed. Placed-object anchoring both drew non-retail grass masks around every object and, on object-dense vistas, thrashed the per-definition model caches into a 3 FPS frame. |
| D-FOLIAGE-13 | **FIXED 2026-07-16.** The reimpl collected detail cells with a standalone RADIAL walk (the full 42-unit disc, ~34 cells on open ground); retail's collector is invoked only from frustum-surviving traversal nodes of level ≥ 3 [`orig: @ 0x60905c..0x60907c`], so its working set is the frustum wedge. Over-collection pushed the far-slot pool past its witnessed 16-bit-index capacity (`min(128, 65534/(36×V))` ≈ 32 for a ~54-vertex def) and the witnessed strict-first-max LRU (per-update stamps, all-ties) then hammered one slot per frame — a user-visible two-frame grass blink at working-set-over-capacity poses that retail never exhibits. Collection is now seated in the traversal handoff; at the reported 00TRa pose: 34→24 cells, 2 misses+evictions/frame→0, blink gone. |
| D-FOLIAGE-14 | **FIXED 2026-09-13 (minted and closed).** Collector capacity and node gating after the D-FOLIAGE-13 seating. The follow-up audit found that collection still depended on the retained 224-entry main draw list, dropping nearby foliage when earlier sectors filled it. Traversal now carries foliage handoffs independently while preserving LOD/frustum admission and the collector's 42-unit/128-cell limits. Full retail leaf execution pins main 223/224 versus independent near/key 127/128 boundaries; portable frame fixtures pin ordered nearby cells after authored or default-flat main saturation, with no extra far/behind-view cells. Adversarial review also corrected sector-zero rejection and the premature high-bit cache gate: flat cells use quadrant 1's raw height center, retain their flagged keys in the independent 128-cell budget, and allocate/touch ordinary empty cache residents whose generator performs no sampling. Nine additional original collector boundary cases and a two-frame original cache/generator sequence pin that behavior; portable terrain-to-foliage frame and capacity-one eviction fixtures cover the full consumer chain. A second collector correction moves distance rejection after unconditional nonleaf descent, preserving near low leaves under mixed-height ancestors; original flagged/unflagged execution and portable whole-sector/64-unit/32-unit fixtures pin that boundary. A third correction restores the traversal's own node gate, which the independent handoff had dropped: a handoff requires the raw node distance less a fixed 16.0 to be within 42, so a far node center (a [0,127] node under a camera at Y=0) or a flat sector under a camera above 58 never reaches the collector; a compiler fixture pins both plus the inclusive 58-unit boundary. See Exact 16-unit collection above. [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, 16.0 @ 0x608D46, cap bypass @ 0x608FBC -> 0x609012, handoff gate @ 0x60906B..0x609078, call @ 0x60907C; Terrain_CollectNearFoliagePatches @ 0x603E60, capacity gates @ 0x603F98 and @ 0x603FF1]. |
| D-FOLIAGE-12 | **FIXED 2026-07-17; premise REFUTED and corrected 2026-09-24** ("Close the foliage mask residuals: atlas map gate, local anchor, P3 order"). The 2026-07-17 entry split the gate into two samplers, detail = the flat 1024-wrap at the WORLD position's low ten bits, MODEL = sector-grid-routed. The flat wrap is real (`Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0` masks `& 0x3FF`; `foliage_sample_detail_flat_wrap` keeps the loader's actual-width stride, width-derived `floor(log2(width))` and the low-10-bit wrap, [`orig: Foliage_LoadFoliageMapPCX @ 0x605ad0`]), but its input is not the world position: the detail key's halves are `(sector << 10)` plus the leaf node's integer position in the one 1024 atlas quadtree (`Terrain_QuadtreeInitRootNode` runs once with total 1024 at origin 0 from `PolyTrn_LoadTerrainConfig @ 0x60e692..0x60e69d`; the node ints are stored `Terrain_QuadtreeNodeInitRecursive @ 0x6082fc..0x608302`; packed at `Terrain_CollectNearFoliagePatches @ 0x603f69..0x603f8a`), and the generator samples at `key & 0x3FF` plus (A, B) (`Foliage_GenerateInstances_0 @ 0x5ffddb..0x5ffdee, @ 0x5fff84..0x5fff9d`, the gate call `@ 0x600065`) while it places vertices from `key & 0x1FF` and rebuilds the blocker's world position as `key & 0x1FF` + bits 10..14 `<< 9` (`@ 0x5fffb8..0x60000e`). The detail sample therefore lands on the sector-routed pixel, the same one `Foliage_SampleFoliageMapMask @ 0x606620` routes the MODEL tier to. The collector now carries each cell's atlas minimum and the gate samples there; the world-flat seam (`get_detail_foliage_index_world`, `sample_detail_index_world`) is deleted. The Dvxi5 GameWorld fixture's witnesses are flipped: the routed witness grows detail grass, the flat-only witness grows none. |
| D-FOLIAGE-10 | **FIXED 2026-09-24** ("Port the foliage depth masks, their water sides and anchors", "Give the foliage MODEL masks their own ladder rungs", "Close the foliage mask residuals: atlas map gate, local anchor, P3 order", "Keep a person's blended strip off its MATCHTERRAIN path behind a mask"). The state half retired 2026-07-15 (the strict `LESS` secondary via the high-pass cutoff discard) and the reflection half 2026-09-01 (the mirror draws no foliage, env #30; the `fade × 0.1` flag it described is in fact the thermal view, ported 2026-09-24). The order half is ported: the masks draw at their retail wave slots in the transparent list (leading `kRungAlphaFarSide` / `kRungScars` at sorting offset `-1e6`) and in a PRE_OPAQUE mask texture that person draws, their P3 layer and their blended NORMAL output test, so a same-wave mask leaves only the MATCHTERRAIN pre-pass and a far-wave mask hides a camera-wave person, as inside retail's BySide waves (`Terrain_RenderSectorEntitiesBySide @ 0x5c7ddf..0x5c7f11`; far wave `@ 0x5c955f`, camera wave `@ 0x5c9638`). First-wave P3 stays before both masks (`@ 0x5c9506`). The last suspected residual, a blended strip losing its MATCHTERRAIN colour behind a mask, is not a divergence: no blending technique has a MATCHTERRAIN pass, so retail's blended strip behind a mask shows nothing of itself either. See MODEL blend/depth draw. |

## Cross-references

- [Terrain RE record](../terrain/terrain-re.md) — terrain shader inputs, mip
  bounds, tile composition, and render ordering.
- [Tile overlay RE record](../tiles/til-re.md) - the shared mission array,
  world-coordinate decode, and inclusive foliage blocker.
- [Render lighting record](../render/render-lighting-re.md) — shared sky/sun
  constants and terrain/foliage lighting blocks.
- [Correspondence matrix](../correspondence.md) — function-to-reimpl mapping.
- [Divergence ledger](../divergence-ledger.md) — canonical dispositions.
