# Foliage — fresh reverse-engineering record

**Status:** re-grilled and reimplemented 2026-07-13, with the MODEL blend/depth
state corrected 2026-07-14 after a renderer capture exposed the missing
combiner. The same-day follow-up also ported the mission-tile exclusion, near
secondary LOW submission, and model-own custom `:fd` mip chain. The previous
reimpl runtime was deleted rather than repaired because it
inverted the two retail tiers and encoded several disproven geometry and color
assumptions.

**Witness:** retail `Jointops.exe` (IDB `Jointops.exe.kong.i64`).

**Scope:** runtime collection, deterministic placement, geometry expansion,
`:fd` preprocessing, render-state/shader behavior, and the Godot adapter. The
foliage definition and map formats remain in place; this closure also aligns
world-space paint and eyedropper tools with the DETAIL consumer's flat map.

## Verdict

| Surface | Retail witness | Reimpl result | Verdict |
|---|---|---|---|
| Definition/map authoring | `.trn` foliage defs, charmap, foliagemap | formats retained; foliage paint/eyedrop share DETAIL's flat wrapped coordinate policy | matching format; DETAIL editing is WYSIWYG |
| Detail-cell collection | frustum-surviving traversal nodes (level ≥ 3) hand subtrees to `Terrain_CollectNearFoliagePatches @ 0x603e60`, cap 128 | the same handoff from the ported traversal into the 16-unit mip-bound collector | matching (seating corrected 2026-07-16, D-FOLIAGE-13) |
| Detail placement | `generate_foliage_instances_0 @ 0x5ffdd0` | fresh `foliage::Runtime` literal vectors | matching |
| Detail geometry | every surface of every LOD0 submesh expanded and terrain-bent | fresh CPU-expanded aggregate ArrayMesh batches | matching |
| Detail pass split | high ref 180 / low ref 8 at distance 33 | separate high/low shaders and batches | matching |
| Distant MODEL placement | `Foliage_GenerateModelTileInstances @ 0x600980` | fresh runtime, four cells, cap 21/cell | matching |
| Distant ground fit | four corners + four edge midpoints | portable fold vectors + CPU evaluation | matching |
| Distant MODEL blend/depth | `Foliage_LoadDefAssets @ 0x6015b7`, `Foliage_DrawModelTileSlot @ 0x601d90` | additive black, alpha-tested, depth-writing mask | matching effect/state; D-FOLIAGE-10 records bounded reimpl order/reflection limits |
| `:fd` bake/sampling | `Foliage_LoadDefAssets @ 0x601260`, `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`, device sampler init | exact dual-source chain, sampled anisotropically with the 4x4 terminal-mip gradient clamp (the round-4 filtering adjudication: the reference device renders anisotropic despite the cfg-0 MIP POINT register) | matching; D-FOLIAGE-5 fixed |
| Detail lightmap input | composed per-tile render target | exact bare tile + hosted static `.til`; retail page/cache and ordered model/depth contributions absent | partial; D-FOLIAGE-7 |
| Mission-tile exclusion | `Foliage_PathBlockedByPlacedTile @ 0x606490` | shared parsed `<mission>.til`, exact inclusive 16x16 AABB scan | matching; D-FOLIAGE-8 fixed |
| MODEL-anchor selection | crouched/prone infantry on terrain (`MoveOrder & 0x300`, empty `groundEntity`) among visible sector entities | sim stance query + camera frustum | matching gate (D-FOLIAGE-11 FIXED 2026-07-16); occlusion membership partial, D-FOLIAGE-9 |

## The correction: two overlapping tiers

The old record and port named the tiers backwards. Fresh decompilation from the
actual function starts establishes this schedule:

1. **Detail tier, camera distance ≤ 42:**
   `Terrain_CollectNearFoliagePatches @ 0x603e60` supplies 16-unit terrain
   leaves to `generate_foliage_instances_0 @ 0x5ffdd0`. This generator expands
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

The former `generate_foliage_instances_0 @ 0x600197` anchor was also wrong:
`0x600197` is an internal colormap-sampling instruction inside the function,
not its entry point. The correct start is `0x5ffdd0`.

## Shared deterministic candidate stream

Both generators derive candidates from the packed 16-unit cell key. Direct
instruction tracing resolves the key orientation:

- high 15 bits: world X at the cell's left edge;
- low 15 bits: world Z at the cell's positive-Z edge;
- bit 31: invalid sentinel;
- the candidate's local B coordinate subtracts from that Z edge.

Each cell evaluates 36 candidates as a 6×6 grid:

- base offset `1.0`;
- grid step `2.6`;
- independent X/Z jitter `1.8 × draw / 65536`;
- yaw `draw × 2π / 65536`.

The fresh portable vectors pin the first candidates for key `0x00100030`,
including `(18.632156, 46.301346)` and yaw `4.831273`. They also pin signed key
decoding, gate selection, quadrant keys, caps, and stable output order.

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
`Terrain_LoadFoliageFile @ 0x60a740` loads the same `<mission>.til` array
used by `Terrain_GetSurfaceTypeAtPosition @ 0x606510` and streamed by
`serialize_terrain_tiles @ 0x6080f0`. The reimpl parses it once before terrain
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
frustum-surviving emitted node of LOD level ≥ 3 whose LOD distance minus its
extent can still reach 42, `Terrain_TraverseQuadtreeNode` tail-hands that
node's subtree to the collector [`orig: gate + call @ 0x60905c..0x60907c;
foliage-enabled flag 0x319FB34`]. Cells behind the camera therefore never
enter the visible-key list (`Foliage_VisibleFarKeyList`, cap 128), which is
what keeps the far-slot pool's working set below its 16-bit-index capacity
(D-FOLIAGE-13). `Terrain_CollectNearFoliagePatches @ 0x603e60` then
recursively reaches 16-unit leaves over the height mipchain, rejects a node
beyond distance 42, and appends at most 128 keys. Its distance combines:

- X and Z distance to the node AABB, clamped to zero while the camera lies
  inside that interval;
- Y distance to the node height-bounds center.

The reimpl hands off per frustum-surviving emitted patch (64-unit leaves near
the camera) into the same 512→16 height-mipchain descent, starting at the
handoff node's rect; sector IDs select the same four 512-unit atlas quadrants
as terrain rendering. The 2026-07-16 grill retracted the earlier radial
whole-disc walk: it over-collected ~34 cells at open-ground poses, exceeding
the pool capacity and thrashing the witnessed LRU into a two-frame cell
blink retail does not show (its frustum wedge stays under capacity).

### Gate and expansion

For every detail candidate:

- the **foliagemap** palette index selects matching definition slots;
- the surface/charmap is not consulted;
- the full source model is yawed and translated without subtracting its bounds
  center;
- every output vertex gets
  `world_y = terrain_height(world_x, world_z) + source_y × 0.5`;
- the source height becomes the clamped bend byte used by the wind shader.
The scale audit rules out a hidden foliage-width correction. Raw source X/Z
feed the yaw/translation path directly at `generate_foliage_instances_0 @
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
`0x60a659..0x60a694`). The `flt_7C69F4 = 0.1` fade multiply at
`0x60a497..0x60a4ae` is gated on the function's third argument, which the
scene renderer pushes as **reflectionEnabled**
(`Terrain_RenderSceneWithReflection @ 0x5c95c1/0x5c9661`); the same flag
forces every patch to LOW (`0x60a193..0x60a19c`). The 0.1 scale is therefore
the water-REFLECTION scene's dimmed LOW-only foliage, and never applies to
the main scene. The main scene instead invokes the renderer twice per frame
split by water side (`formatType` at `0x5c95c3/0x5c9663` versus the
camera-below-water flag; patch min-height vs `Env_WaterHeightFixed` at
`0x60a1a2..0x60a1c6`): far-side-of-water foliage before the water surface,
camera-side foliage after it.

Both detail passes alpha-blend: the technique block enables
`ALPHABLENDENABLE` with `SRCBLEND=SRCALPHA` and `DESTBLEND=INVSRCALPHA`
(`Foliage_LoadDefAssets @ 0x60141f..0x601427`), and the PS emits
`r0.a = t0.a × v0.a` — the tested alpha is also the blended weight. The
distance fade is a continuous transparency ramp, not just an alpha-test
threshold shift; rendering these passes opaque turns the low-alpha `:fd`
fringe into hard sheets and makes cells pop as the fade crosses per-texel
thresholds.

The setup flag is also a depth-comparison toggle, not a wireframe or fill-mode
toggle. `Foliage_SetupFarSlotDraw @ 0x6007c0` selects value 2 for the
secondary call at `0x6008fc..0x600912`; wrapper `0x67cac0..0x67caea` applies
that value to render state `0x17` (`D3DRS_ZFUNC`), so value 2 is
`D3DCMP_LESS` and the normal value 4 is `D3DCMP_LESSEQUAL`. Both passes are
double-sided and use terrain fog. HIGH and LOW reuse one cache revision but
receive distinct submission IDs. Consequently detail residents/hits remain
cell-based, while submission, intent, batch, and rendered-instance counters
count both near draws.
The reimpl ports the ordered submissions, references, shared fade, blending, and
write policy. The strict-`LESS` secondary is emulated exactly for
same-geometry resubmission: because HIGH wrote depth precisely where its
GREATER test passed, the secondary discards texels whose
`:fd alpha × fade` exceeds the HIGH reference (`u_high_pass_cutoff`),
landing only where HIGH left no depth. The reflection-scene LOW-only
`fade × 0.1` variant is not ported (the reimpl does not yet render foliage
into water reflections); that gap is noted under D-FOLIAGE-10.

### Detail shader

`Terrain_CreateFoliageVertexShaders @ 0x5ff630` creates the wind shader. It
displaces render Z by a sine of world X plus the frame phase, weighted by the
source-height bend and scaled by `0.03`.

`Foliage_CreateLightmapBlendPS @ 0x5ff7a0` establishes the fragment combine:

`t0 × (t1 × (t1.a × c1 + c0)) × v0 × 8`

where:

- `t0` is the model's baked `:fd` texture;
- `t1` is the composed terrain tile-cache render target;
- `c0/c1` are sky and sun lighting registers;
- output alpha is `t0.a × v0.a` for the alpha test.

The reimpl implements the exact arithmetic and render states. For bare terrain it
reconstructs `t1` from raw colormap RGB plus the byte-quantized
heightfield-normal/light DOT3 alpha. `PolyTrn_RenderTile @ 0x60da70` clears
authored colormap A in its `0x00808080` base draw, so no colormap sun mask
survives into this input; `Terrain_GenerateNormalMap @ 0x603210` and
`PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e` supply the alpha
instead. The shared 1024 normal atlas is clamped to the selected quadrant's
texel-center bounds, matching four retail 512 CLAMP samplers. The detail
emitter color is not a second dynamic time-of-day input.
`Foliage_RenderFarPatches @ 0x609efc..0x609f2a` computes c6.rgb as the
componentwise product
`0x319F9D0/D4/D8 × 0x319F9E0/E4/E8`. The complete xref set for the second
vector contains only the render reads plus the writes of literal `1.0` at
`init_terrain_lighting_color_ramps @ 0x604ffa/0x605004/0x605010`; there is no
per-frame color writer. Therefore the active multitexture path's c6.rgb is
exactly the first vector.

`PolyTrn_InitTextures @ 0x60b03c..0x60b065` writes that first vector to
`(128/255,128/255,128/255)` when `PolyTrn_HasBlendmap` is true and
`PolyTrn_ShaderTier >= 1`. The constant at `0x7DF2E0` has bytes
`81 80 00 3F`, the exact float encoding of `128/255`. Only the fallback at
`0x60b06a..0x60b13e` writes the source terrain texture's average RGB divided
by 255. The reimpl's fixed `0.5019608` uniform is therefore exact for the top
splat path, not a visual approximation; the fallback remains a distinct
non-splat path. c6.a is still the distance/pass fade described above, and
MODEL masks do not use this emitter factor.

For the analytic t1 reconstruction, EnvFile preserves the direct
`Environment_GetLightDirectionFloat @ 0x57d870` tuple `g=(g0,g1,g2)`, not a
Godot/world XYZ vector. PolyTrn's D3DCOLOR pack (`0x60e201..0x60e331`) writes
GPU diffuse RGB `(g2,g0,g1)`, so the reimpl now packs `(z,x,y)` against the
normal-map RGB `(grid X slope, grid Y slope, up)`; `FORMAT_RGBA8` preserves
those channels. The old `(x,z,y)` mapping swapped the horizontal DOT3 axes.
The 08:00 oracle is light bytes `(231,83,187)`, with slope alphas
`0.8987774/0.0794002`. Retail foliage itself does not compute this DOT3; its
blend PS consumes the cached `t1.a`. The hosted static `.til` RGB/tint is also
composed before lighting. General retail tile-cache projection and ordered
tile-model/depth-alpha contributions remain open under D-FOLIAGE-7/
D-TERRAIN-7.

The tile projection is explicitly **pre-wind**. In the
`Foliage_WindSwayVS` literal (`0x7de648`, copied at `0x5ff691`, assembled
at `0x5ff6df`), `mad r1.z` applies wind at `0x7de7f1` and
`m4x4 oPos,r1,c0` consumes it at `0x7de821`; independently,
`m4x3 r10,v0,c12` at `0x7de835` and the c7/c8 `oT1` projections at
`0x7de881/0x7de897` still consume the original vertex. The reimpl therefore
derives overlay UV before displacing render Z. Its
`fract((world.x,-world.z)/1024)` exactly inverts the hosted 1024 bake and
matches retail `.til` X/negated-Z axes (`0x60de23/0x60de28`), patch-local
positioning (`0x60de7c..0x60debb`), and draw (`0x60df1b`).

That closes the supported static `.til` component, not the general retail
page/cache projection. The RT resolve at `0x60a1de`, c7/c8 construction at
`0x60a220..0x60a34f`, uploads at `0x6006f0/0x600704`, and ordered
tile-model/depth-alpha contributions remain part of the bounded D-FOLIAGE-7 /
D-TERRAIN-7 producer gap.

## Distant MODEL/depth-mask tier

### Driver and cells

The driver is the visible sector-entity walk in
`Terrain_RenderSectorEntitiesBySide @ 0x5c7d50`, and the tier is the
**hide-in-grass mechanic**: the walk calls the foliage update only for
entities whose `MoveOrder` dword carries a stance bit — `0x100` prone /
`0x200` crouch — and whose `groundEntity` is empty (standing on terrain, not
on a vehicle deck or floor) [`orig: flags test @ 0x5c7dc2/0x5c7ded
(MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7`]. Placed
objects never write `MoveOrder`, so retail never generates model foliage
around crates, fences, or buildings; only infantry ever carry the bits
(local packer `Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd` from the
stance latches, remote apply `NapiNPServerMsg_HandleStanceChange @
0x501c60`, vehicle attach clears `@ 0x435c54/0x43561e`). A passing entity
also splits on the water side of the pass and enters the tier at view depth
38 (`Foliage_UpdateModelTiles`'s own `>= 38.0` view-Z gate). The per-entity
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
(`NovaSimulation::get_foliage_mask_anchor_positions`, from the replicated
`net_stance_bits` + `ground_target`), corrected 2026-07-16 — the earlier
placed-object anchor feed (D-FOLIAGE-11, FIXED) generated masks retail
never renders and collapsed frame rate on object-dense vistas (03TR
airfield: 1143 frustum-passing anchors thrashed the 1000-entry model
caches at ~7k regenerations per frame, 212 ms of a 354 ms frame; with the
witnessed gate the tier idles in object-only scenes). The reimpl
frustum-culls the surviving anchors, but retail's broader
sector-visibility/occlusion membership is still not reproduced; see
D-FOLIAGE-9.

### Normalization and eight-sample ground fit

`Foliage_FillInstancedModelBuffers @ 0x5ffa20` and
`Foliage_UploadModelTileVSConstants @ 0x600f00` show the model normalization:

- footprint radius is `0.75 × max(halfExtentX, halfExtentZ)`;
- model height is scaled by `0.5`;
- yaw is applied once by the generated corner positions.

The generator samples four rotated footprint corners and the four edge
midpoints. Those eight heights form `(E_A, T_A, E_B, T_B)`, the biquadratic
correction consumed by `Foliage_GridPlacementVS`. The fresh runtime returns the
four corners and fold explicitly; the Godot adapter evaluates the same surface
while expanding the normalized source mesh.

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

Retail inserts the draw after its initial sector flush and before later entity
and foliage consumers. Stock Godot's transparent pass cannot reproduce every
one of those arbitrary opaque insertion points; the remaining ordering limit is
recorded as D-FOLIAGE-10 rather than hidden inside a state-parity claim.

The grid-placement wind term displaces render Z by the already-halved source
height times `sin(counter × 0.001) × 0.08`; in this tier it moves depth coverage,
not black scene color.

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
`generate_foliage_instances_0` then unconditionally overwrites that packed
color at `0x6002db..0x60030a` with the source-height bend carrier.
`Foliage_WindSwayVS` (`0x7DE648`) consumes v5.x only for bend and emits
`oD0=c6`. Authored leaf color therefore survives through sampled t0, not v0
or `color_upper`.

The reimpl now builds the complete chain in portable
`build_fd_rgba_mip_chain`, passes the packed levels directly to Godot, and no
longer asks Godot to regenerate generic mips. Native and GUT literal vectors
pin authored mip-0 RGB, the wrapped alpha, the exact 4×4 retail blend, and the
reimpl-required 2×2/1×1 terminal continuation. This fixes D-FOLIAGE-5 and the
former flat ground-olive result. `VegAssets`' primary-submesh
lookup remains only the binding locator: surviving geometry is still the
aggregate of every surface of every `build_lod_submeshes(0)` result.
The retail comparison configuration has `texfilter_level=0`. The parser at
`0x551217..0x551239` maps that value to HLSL filter mode 0
(`0x58640e..0x586427`), which selects no trilinear or anisotropic macro
(`0x5ae6d1..0x5ae6f5`). Device initialization/reset at
`0x679c28..0x679c9c` and `0x677f9e..0x677ff9` then sets MAG/MIN LINEAR,
MIP POINT, MAXMIPLEVEL 0, and a zero mip bias — the literal cfg-0 register
state. The round-4 adjudication supersedes the register reading for the
reimpl: the reference machine demonstrably renders the anisotropic texfilter
mode (`MINFILTER=ANISOTROPIC` + `MAXANISOTROPY` per-stage select
`@ 0x67e38a..0x67e45b`; driver-forced despite `texfilter_level=0`), so the
reimpl samples the `:fd` chain anisotropically through `textureGrad` with the
terminal-mip gradient clamp. It still never admits Godot's synthetic 2x2/1x1
tail past retail's 4x4 terminal level.

## Persistent caches and submission identity

The detail tier uses the persistent slot pool in
`Foliage_UpdateFarCellSlots @ 0x601b30`, with strict signed-age LRU
replacement. In `PolyTrn_RenderFrame @ 0x60f0ea..0x60f10f`, cached geometry
is drawn before the update, so a miss first becomes visible on the next terrain
render; duplicate misses in one update allocate once. Retail's 16-bit index
ceiling yields the hosted per-definition capacity
`min(128, floor(65534 / (36 × sourceVertexCount)))`.

The distant model cache in `Foliage_UpdateModelTiles @ 0x601f50` is a fixed
1000-entry pool per definition. A hit touches its resident entry and refreshes
geometry only when `((sceneCounter + 2 × slot) & 7) == 0` (check at
`0x60209a`); every caller still submits the resident entry.

Retail reaches that cache through one visible sector-entity walk. The reimpl's
stance-gated anchor feed can still revisit the same `(slot, cell key)`
through clustered anchors in one frame. The portable runtime therefore
coalesces only the reimpl-created duplicate refresh work: the first visit on the
exact eight-scene phase regenerates the resident, later visits retain their
separate submissions, and geometry-identical refreshes keep the resident
revision. At the exact `00TRe.bms` spawn's worst measured heading this made
fixed-input MODEL output stable at 1,795 instances / 568 submissions, removed
all phase mesh uploads, and reduced dispatcher p95 from 55-73 ms to 15.8 ms.
It does not cap anchors or claim retail visibility membership; that remains
D-FOLIAGE-9.

The fresh runtime identifies a resident mesh by slot, cell key, and monotonic
revision, and gives every draw a separate submission ID. The adapter builds all
same-frame submissions before applying evictions, then clears draw-pool mesh
references before erasing evicted cache entries. This preserves duplicate
same-frame draws without stale references. A `terrain_changed` signal also
resets the runtime when the same `NovaTerrainData` resource mutates in place.

## Reimpl architecture

The fresh implementation deliberately has three layers:

1. `libs/foliage/runtime.{h,cpp}` — Godot-neutral placement, gates, fades,
   alpha refs, corners/fold, and `:fd` preprocessing.
2. `NovaTerrain` — exact runtime 16-unit detail-cell collection from terrain
   height bounds.
3. `NovaFoliageDispatcher` — definition/mesh adaptation, live samplers, model
   expansion, persistent slot/key/revision mesh caches, and dynamic
   per-submission draw pools for detail high, detail low, and silhouette.

The discarded placement, dispatcher, model-dispatcher, and fd-bake clusters
were deleted. The live foliage-map resource now single-sources DETAIL sampling,
world-to-map coordinates, and effective resolution for runtime and authoring.

Runtime sampling comes directly from `NovaTerrainData`:

- detail gate: flat-wrap `get_detail_foliage_index_fixed` on the candidate's
  original Q16 coordinates;
- MODEL gate: sector-routed `get_foliage_index_world`;
- ground: bilinear terrain height;
- terrain projection: the shared world-to-source transform.
- exclusion: the active mission `NovaTerrainTileInfo` inclusive AABB scan.

The editor uses the same adapter with separate live foliage-map Callables:
DETAIL reads the flat wrapped map resource, while MODEL keeps the routed
terrain projection. Foliage paint and eyedropper target that same flat DETAIL
pixel, including the loader's floor-log2 resolution for brush scale and a
wrapped footprint at the 1024-world seam. Its detail cells are a deterministic
preview set; only
the runtime native collector is claimed as retail-exact. Runtime dispatchers
also inherit the parent `NovaTerrain` heightfield-normal atlas; the standalone
editor foliage preview has no such parent and currently falls back to mesh
normals for bare-tile alpha. That preview-only input is included in
D-FOLIAGE-7 rather than claimed as tile-light parity.

## Verification

- `foliage_map_test`: negative/fractional Q16 wrapping, integer boundaries,
  non-power-of-two widths, exponent-10 actual-width stride, and shared
  runtime/authoring coordinates, including wrapped brush seams that do not
  touch unused non-power-of-two stride cells.
- `foliage_runtime_vectors`: literal detail PRNG positions/yaws, separate Q16
  flat-detail and routed-MODEL gate callbacks with disjoint routing, path/force-on
  behavior, 20–42 fade shared by both near submissions, pass switch, the
  strict-LESS secondary marker, four silhouette cells, 21 cap,
  eight-sample fold, distance alpha ref, and `:fd`.
- `til_foliage_blocker`: inclusive min/max boundaries, stored-negated Z,
  unsnapped entry positions, and empty-array behavior for the shared mission
  tile scan.
- `terrain_foliage_detail_collector`: mip-bound 16-unit keys, distance boundary,
  quadrant mapping, signed packing, traversal order, and global cap.
- `foliage_runtime_adapter_test.gd`: fresh public adapter contract, disjoint
  DETAIL/MODEL callback routing, tier map selection, full multi-surface LOD0
  aggregation, view-depth gate, no
  manufactured anchors, cache identity/eviction ordering, reset behavior, and
  the additive/depth shader-state contract.
- Adapter regressions pin ordered near HIGH/LOW draw nodes and their depth
  states, exact `00TRa` entry #25's stored-negated-Z decode directly onto the
  positive terrain/Godot Z plane, the blocked armory-truck center and uncovered
  c3/c4/c5 controls, selective rather than blanket blocking, and active-blocker
  frame diagnostics.
- `game_world_test.gd`: parses the co-named mission payload before terrain
  build, shares one parsed resource with terrain/foliage, clears it on unload,
  and prevents stale blockers on a subsequent no-TIL load.

- `foliage_shader_contract_test.gd`: strict `D3DCMP_GREATER` boundary for the
  shared detail include and the MODEL shader, plus the anisotropic `:fd`
  sampler hints, the textureGrad call sites, and the 4x4 terminal clamp.
- `foliage_black_flicker_regression_probe.gd`: real rasterized destination-color
  preservation, strict alpha acceptance/rejection, retained late-consumer
  depth, screenshot-shaped exact-black-component detection, and fixed-input
  frame stability for both tiers.
- `foliage_spawn_capture_probe.gd` with `NOVA_FOLIAGE_FLICKER_PROBE=1`: the real
  `00TRe.bms` player spawn, fixed-input HIGH/automatic coverage with deliberate
  dropout and fade-response controls. LOW-far is explicitly reported as
  skipped when it has no pixels in the exact spawn view.
- `runtime_scene_probe.gd` and the visual probes: detail camera selection uses
  the flat gate oracle; the runtime scene consumes NovaTerrain's typed native
  detail-cell vector, while its minimal fixture intentionally contains no
  `.3di` models.

## D-FOLIAGE divergence catalog

| ID | Fresh disposition |
|---|---|
| D-FOLIAGE-1 | **RETRACTED/FIXED, clarified 2026-07-14.** The prior per-corner colored-emitter premise belonged to the inverted port. Detail vertex diffuse is the recovered bend carrier; the distant MODEL pass uploads black only as the zero-contribution source of an additive depth mask. |
| D-FOLIAGE-2 | **FIXED.** Exact detail lightmap-blend arithmetic is hosted; the missing composed `t1` producer is separated as D-FOLIAGE-7. |
| D-FOLIAGE-3 | **FIXED 2026-07-13.** Both recovered Z-wind terms are hosted in their correct tier-specific shaders. |
| D-FOLIAGE-4 | **SUPERSEDED/FIXED 2026-07-13.** The 2026-07-08 two-tier port was itself inverted and has been deleted. Fresh detail expansion and MODEL ground-fit paths replace it. |
| D-FOLIAGE-5 | **FIXED 2026-07-14.** The portable custom chain keeps authored RGB at mip 0, blends recursively downsampled later retail mips toward `0x808080` with `w=min(256,floor(320*i/N))`, preserves base-chain alpha, supplies Godot's terminal levels without generic mip regeneration, and samples anisotropically with the gradient clamp that keeps selection inside retail's 4x4 terminal level (round-4 filtering adjudication). |
| D-FOLIAGE-6 | **FIXED 2026-07-14.** The former opaque-black conclusion missed the downstream ONE/ONE blend state. The reimpl now preserves destination color while retaining the recovered strict-alpha-tested MODEL depth write. |
| D-FOLIAGE-7 | **OPEN, bounded.** Runtime detail reconstructs exact bare-tile RGB/heightfield-DOT3 alpha and composes hosted static `.til` RGB/tint at the exact pre-wind coordinate. Retail's general page/cache c7/c8 projection and ordered tile-model/depth-alpha RT contributions remain absent; standalone editor preview also lacks the parent normal atlas and uses its mesh-normal fallback. |
| D-FOLIAGE-8 | **FIXED 2026-07-14.** Direct retail inspection resolved the supposed path/spacing substrate as the shared mission `.til` array. `til_blocks_foliage` ports the exact linear inclusive 16x16 AABB scan, GameWorld parses `<mission>.til` before terrain build, and terrain/foliage/network state share that resource/payload; `FORCE_ON` continues to bypass the sampler in the portable runtime. [orig: `Foliage_PathBlockedByPlacedTile @ 0x606490`; `Terrain_LoadFoliageFile @ 0x60a740`; `Terrain_GetSurfaceTypeAtPosition @ 0x606510`] |
| D-FOLIAGE-9 | **OPEN, reimpl mapping (narrowed 2026-07-16).** The anchor CLASS is now the witnessed stance gate (D-FOLIAGE-11); what remains approximate is visibility membership — camera frustum stands in for retail's visible-sector walk + `test_sector_entity_occlusion @ 0x5c4610`. No terrain-center fallback remains. Same-frame refreshes of an overlapping reimpl `(slot, cell key)` are coalesced without removing its distinct draw submissions, preventing reimpl-only regeneration/upload storms while this membership gap remains open. |
| D-FOLIAGE-11 | **FIXED 2026-07-16.** The reimpl fed every placed mission object as a MODEL-tier anchor; retail's sector walk generates the tier only for entities with `MoveOrder` stance bits (`0x100` prone / `0x200` crouch) and an empty `groundEntity` — the hide-in-grass masks around infantry [`orig: @ 0x5c7dc2/0x5c7ded/0x5c7dd5`]. Anchors now come from the sim's stance query; the ONED preview feeds none (no infantry exists there), and its placed-object `anchor_provider` plumbing was removed. Placed-object anchoring both drew non-retail grass masks around every object and, on object-dense vistas, thrashed the per-definition model caches into a 3 FPS frame. |
| D-FOLIAGE-13 | **FIXED 2026-07-16.** The reimpl collected detail cells with a standalone RADIAL walk (the full 42-unit disc, ~34 cells on open ground); retail's collector is invoked only from frustum-surviving traversal nodes of level ≥ 3 [`orig: @ 0x60905c..0x60907c`], so its working set is the frustum wedge. Over-collection pushed the far-slot pool past its witnessed 16-bit-index capacity (`min(128, 65534/(36×V))` ≈ 32 for a ~54-vertex def) and the witnessed strict-first-max LRU (per-update stamps, all-ties) then hammered one slot per frame — a user-visible two-frame grass blink at working-set-over-capacity poses that retail never exhibits. Collection is now seated in the traversal handoff; at the reported 00TRa pose: 34→24 cells, 2 misses+evictions/frame→0, blink gone. |
| D-FOLIAGE-12 | **FIXED 2026-07-17.** The portable runtime now carries separate Q16 gate callbacks: detail consumes the flat 1024-wrap lookup while MODEL retains the sector-grid-routed lookup. `foliage_sample_detail_flat_wrap` ports the loader's actual-width stride, width-derived `floor(log2(width))`, low-10-bit coordinate wrap, and reimpl-plane Z convention without a float round-trip [`orig: Foliage_LoadFoliageMapPCX @ 0x605ad0`; `Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0`; `Foliage_SampleFoliageMapMask @ 0x606620`]. ONED preview, foliage paint/eyedropper, and capture probes share the flat DETAIL coordinate API. The Dvxi5 GameWorld fixture pins disjoint flat-positive/routed-negative and routed-positive/flat-negative authored-map witnesses, including the world-to-pixel mapping, then requires production detail output at the flat-positive point. |
| D-FOLIAGE-10 | **OPEN, bounded reimpl order.** Retail inserts each immediate MODEL depth-mask draw between its initial sector flush and later entity/foliage consumers; the reimpl's transparent-pass depth sorting reproduces the mask-occludes-farther-detail effect but cannot cull already-drawn farther tufts under a nearer mask, and cannot reproduce every arbitrary insertion point. The near secondary LOW's strict `LESS` is now emulated exactly via the high-pass cutoff discard (the 2026-07-15 grill retired the state half of this entry). The water-REFLECTION scene's LOW-only `fade × 0.1` foliage pass (arg_8 = reflectionEnabled @ `0x5c95c1/0x5c9661`) is not ported while reflections carry no foliage. |

## Cross-references

- [Terrain RE record](../terrain/terrain-re.md) — terrain shader inputs, mip
  bounds, tile composition, and render ordering.
- [Tile overlay RE record](../tiles/til-re.md) - the shared mission array,
  world-coordinate decode, and inclusive foliage blocker.
- [Render lighting record](../render/render-lighting-re.md) — shared sky/sun
  constants and terrain/foliage lighting blocks.
- [Correspondence matrix](../correspondence.md) — function-to-reimpl mapping.
- [Divergence ledger](../divergence-ledger.md) — canonical dispositions.
