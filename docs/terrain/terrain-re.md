# Terrain — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **terrain** pipeline — the
heightmap/mesh build, the quadtree LOD, CDEP, lighting/modulation, mesh
simplification, byte packing, and (REN-4) the runtime surface-shading resource
set. The reimplementation surface is `libs/terrain`
(+ `libs/terrain_query`, the world→height seam, ADR 0020) and the Godot terrain
layer. Binaries: **both** `jodemo.exe` (the accessible LOD/quadtree/mip renderer)
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
The bounded runtime gaps are the tile-composition render-target/update details
(including the exact static projected-shadow cache mechanics) and the separate
underwater water-noise modulation, plus CDEP/traversal documentation depth.
Like [mission/mis-format-re.md](../mission/mis-format-re.md), this remains a
partial; it converts terrain from `UNAUDITED` to *tracked (partial)*.

## Module map (`libs/terrain`) and witness basis

| Module | Role | Witness |
|---|---|---|
| `builder` | heightmap → terrain mesh (the build pipeline entry) | the TrnGen byte-identical data path (canonical reference) |
| `quadtree` / `build_quadtree` / `lod` | quadtree LOD traversal, frustum culling, height mipchain, final mesh-family selection | **jodemo** `Terrain_TraverseQuadTreeNode @ 0x5C89C0`, `Terrain_CollectVisibleSectors @ 0x5C9120`, `Terrain_BuildHeightMipChain @ 0x5C5310`; **retail** `render_terrain_sector_batch @ 0x6096f0` (eight families, `clamp(lod_sub, 0, 15) / 2`) |
| `cdep_constraint` | quantized [min,max] of the 256 pixels of a block (CDEP depth constraint) | documented in-code; full CDEP bitstream grill pending |
| `lighting` | terrain lighting colors + per-position modulation | **retail** `Terrain_SetLightingColors @ 0x5C4B10`, `Terrain_GetModulatedColorAtPos @ 0x5C5FE0`; fog via `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` |
| `texture_preprocess` | byte-faithful detail coefficient map, DBlend normalization, and paired near/far mip chains | **retail** `Texture_GenerateNormalMap @ 0x58c070`, `PolyTrn_InitTextures @ 0x60aaa0`, `GTexture_Downsample2x2_RGBA8 @ 0x687000`, `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270` |
| `mesh_simp` | mesh simplification (edge-collapse) | **BYTE-IDENTICAL — verified**: `dvd4_parity` (canonical `.cpt`) + `parametric_parity` (Sample/Gradient/Checker64/Perlin, 4.6–6.8 MB CPTs each) all produce byte-identical output. The in-code "divergence point / vertex 1223" logging is leftover debug scaffolding from when parity was being achieved, now inert. `parametric_parity` is ctest-`DISABLED` only for CI runtime cost (~5 min), not for any correctness gap |
| `packing` | word→byte packing | **retail** `pack_words_to_bytes @ 0x403CD0` (low byte of each u16, 3 bytes/group) |
| `depthmap` | depth/height map storage | in-code |
| `terrain_query` raycast (ENG-3 B1, ported with #209) | world-space height samplers + the segment raycast the editor/celestial hosts adopt | **retail** §Runtime terrain queries below (`Terrain_SampleHeightBilinear @ 0x6067b0`, `Terrain_RaycastHeightmapLoRes @ 0x60cb80`, `Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`) |

The tile overlay and foliage that render over the terrain surface have their own
now-landed records: [tiles/til-re.md](../tiles/til-re.md) (PAR-R3),
[foliage/foliage-re.md](../foliage/foliage-re.md) (PAR-R2). Env #19's
`terrain_rgb` is confined to the dead far-colormap bake described below; the
live top-tier terrain shader has no terrain-tint multiplier
([env/env-tod-re.md](../env/env-tod-re.md) #19).

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
far slopes — i.e. the ANISOTROPIC mode is active on the reference machine
even though its `game.cfg` reads `texfilter_level = 0` (driver-forced or
auto-detected; point-mip or trilinear sampling of the gray-converging
paired chains measurably washes mid-distance terrain that retail renders
grainy: HF-energy 11.1 retail vs 3.0 reimpl before / 7.6 after). The reimpl
therefore samples the detail layers anisotropically
(`filter_linear_mipmap_anisotropic`, project anisotropy 8×) and scales the
gradients back onto the 4×4 terminal so the synthetic Godot tail is never
selected. Which config bit drives `device+600` at runtime on the reference
machine remains an open follow-up.

Implemented in `libs/terrain/include/terrain/texture_preprocess.h` and
`NovaTerrain::_load_textures`;
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
`TrnConfig`, the TPJ build flow, and `NovaTerrainData` preserve all four pairs;
at runtime `height_field_apply_trn` stamps them onto every `TerrainHeightField`
(the `NovaTerrainData` samplers/raycast and `NovaSimulation`'s grounding field),
and `build_heightfield_normal_map` plus the `nova_terrain.cpp` render mesh and
seam normals tap the same `CoordsTaps` kernel. The former Godot-physics
heightfield consumer was removed (#330; range-find rides the ported raycast).

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
`PolyTrn_PSShadowNormalMap` (the legacy name is misleading: for an underwater
camera, t3 = `Water_NoiseColorTexture`; light scale `4·t3²·t0.a`),
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
When the shadow pass is active, stage 3 swaps to the projected-shadow
texture with an `8/density` transform [`orig: 0x6043f2; 0x6097ca`].
The detail splat is `t1×t2.r + t4×t2.g + t5×t2.b`; the `mul_x2` modulation
is `2×dot(t3.rgb, t2.rgb)` — with the normalized blend this is a
blend-weighted scalar of the second detail — and it exists ONLY in
`PS14SplatNormalMap`, selected when `detailmap2` is present; maps without a
second detail run `PS14Splat`, which has no such stage (factor exactly 1)
[`orig: terrain_setup_lighting_and_shader @ 0x604544/0x6044e8`]. Lighting
starts from `(t0.a×c1 + c0)/2`, then follows the witnessed ×2 colormap,
×2 dot-product, and ×4 splat stages
[`orig: PolyTrn_PS14SplatNormalMap @ 0x7dece0, assembled with the full
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
families 0..7 [`orig: render_terrain_sector_batch @ 0x6096f0`].

**Include correction (2026-07-06, D-TERRAIN-2)**: `terrain_lighting.gdshaderinc`
had stacked TWO ×2 detail-normal factors on the splat (the gobj-era "v23
dual-normal" chimera; under the gamma-faithful pipeline it clipped regions
to white). The recovered top-tier path applies exactly one coefficient factor;
the splat-only variant omits it [`orig: PolyTrn_PS14SplatNormalMap @
0x7dece0`; `PolyTrn_PS14Splat @ 0x7dee18`]. The fresh re-grill establishes the paired
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
`CURRENT × CURRENT.aaa × 2` self-modulation tail and per-slot mip-bias
sampler params, opaque + additive variants; the four texture slot ids 1-4
= the terrain's dynamic texture registry) and the framebuffer-mod2x overlay
(`dword_319f934`, mode 0x628 — DESTCOLOR/SRCCOLOR). Draw-time consumers:
`render_terrain_sector_batch @ 0x6092a0`, `render_terrain_lightmaps
@ 0x609de0` (REN-5), `Terrain_CollectAndRenderTileModels @ 0x60d250`,
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
target `dword_319A2D8` at `0x60D5BE`, rasterizes black PROJSHAD silhouettes,
and restores it at `0x60DA4F`. `PolyTrn_RenderTile` then selects the destination
tile-cache RT at `0x60DCC5`, binds that temporary texture at
`0x60E10A..0x60E112`, and composites it only when the collector returned a
candidate (`0x60E0C6..0x60E19D`). The final cache is sampled as t0 by terrain
and as t1 by `Foliage_LightmapBlendPS`; therefore terrain and foliage receive
the projected/draped shadow, while an Ihq01 courtyard/interior floor mesh does
not. Dynamic person/`DynamicShadow` render slots are a separate system
(`Entity_InitFromModel @ 0x40E1BC..0x40E1F7`).

**Bounded runtime gaps after the 2026-07-13 closure**:

- **Tile-composition RT/update gap** — the reimpl currently CPU-bakes one static
  1024×1024 RGBA overlay through `til_bake_overlay_rgba`; the shader applies
  its RGB over the now-exact bare t0 reconstruction. It rebuilds on load,
  toggle/override, and terrain-change notifications. The base colormap draw,
  TrnNMap generation/split/addressing, coordinate-basis-correct light packing,
  DOT3 alpha, supported overlay order, and the static model-shadow
  eligibility/ROBJ/receiver policy are closed. The reimpl approximates the
  latter with a directional-shadow adapter whose static receiver is terrain
  only; admitted caster state follows individual/batched destruction-to-husk
  swaps and editor transforms. Alpha-tested foliage deliberately has no generic
  attenuation catcher because that would darken whole cards, so its retail
  tile-cache projection remains open along with the exact temporary-shadow-RT
  format/projection/composite strength, tile-cache
  allocation and dirty cadence, general patch/page c7/c8 projection, remaining
  ordered depth-alpha contributions, and final RT mip generation/use remain
  open
  [`orig: Terrain_CollectAndRenderTileModels @ 0x60d250`;
  `PolyTrn_RenderTile @ 0x60da70`].
- **Underwater water-noise modulation gap (separate)** — the reimpl does not
  feed `Water_NoiseColorTexture` into terrain when the camera is below water.
  Retail's misleadingly named `PolyTrn_PSShadowBasic` /
  `PolyTrn_PSShadowNormalMap` apply the t3 water-noise scale
  `4×t3²×t0.a`. This is unrelated to the static tile shadow above and does not
  reopen top-tier above-water base-pass parity; the wider record is
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
`libs/terrain_query` raycast; [ADR 0020](../adr/0020-world-terrain-query-seam.md)
§5 growth). All coordinates 16.16 fixed-point world units; the heightmap V axis
runs opposite world y (samplers negate y internally).

### Shared data substrate (renames applied this session)

| Global | Address | Role |
|---|---|---|
| `Terrain_HeightAtlasPtr` (ex `tileMask`) | `@ 0x31a00cc` | base of the 1024×1024 `u16` raw16 height atlas, row stride 1024; `sample << 8` = 16.16 height (raw16/256 units). 512×512 quadrant windows — the same atlas model as `terrain/coords.h` |
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
  glare-occlusion path `nova_celestial.gd::_glare_ray_clear` stands in for
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
`libs/terrain_query/terrain_raycast.{h,cpp}` (`terrain_raycast_march` /
`terrain_raycast_refined`, 16.16 structural translations with a
point+bilinear sampler seam; ~60 pinned checks in the `terrain_raycast`
ctest incl. the step-math exactness, the crossing-rule asymmetry, the
height-0 floor, and the odd refine guard's zero-step no-op-walk interplay),
bound as `NovaTerrainData.raycast_terrain(from, to)` over BOTH reimpl
substrates (live editable Image preferred, baked CPT otherwise — the
existing slice-A sampler cores reused). Adopters: ONED mission picking
(`terrain_editor.raycast_terrain_at` — the GDScript march/slab/bisection
trio deleted) and the celestial glare ray
(`nova_celestial._glare_ray_clear`, the 32-unit stand-in retired). The
editor-mode guard divergences (OOB no-terrain vs retail clamp-to-edge,
no-data NAN vs retail return-HIT, contiguous-atlas bilinear vs the seam
flags) are **D-TERRAIN-4** (class C, PERMANENT candidate).

Open follow-ups from this pass: the seam-flag WRITER (load-time adjacency
derivation) and the rationale (if any) behind HiRes_0's odd skip-refine guard.
The `@ 0x60c760` internals follow-up closed 2026-07-16 — witnessed and ported
as `terrain_raycast_los_clear` on the occlusion slice (the AI LOS
`los_terrain_blocked` stand-in upgraded to it in the same change).

## D-TERRAIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TERRAIN-1 | C | PERMANENT (candidate) | **Terrain-shader edit/runtime split** (the one deliberate divergence): the editor renders terrain with a live-sculpt shader (height edits without rebake), the runtime with the baked shader — the *surface-shading math is shared via an include* so the two cannot drift in look. Tracked, justified by an editing need the runtime path cannot serve, and sharing the fidelity-bearing core ([oned/editor-runtime-parity.md](../oned/editor-runtime-parity.md) §Terrain shaders). Ratify under ADR 0022 to move from candidate to `PERMANENT`. |
| D-TERRAIN-2 | A | **FIXED (2026-07-06)** | **Doubled detail-normal factor** (the gobj-era chimera): `terrain_lighting.gdshaderinc` stacked TWO ×2 `dp3(normalmap, blendmap)` factors on the 3-way splat; the witnessed top-tier ps.1.4 applies exactly ONE `[orig: PolyTrn_PS14SplatNormalMap source @ 0x7dece0; PolyTrn_PS14Splat @ 0x7dee18; compile_terrain_pixel_shaders @ 0x605260]` (the dual-normal product belongs to the separate non-splat ps.1.1 tier). Post-gamma (D-RMAT-7) the squared factor clipped whole regions to white. See §Include correction above; ledger row carries the full witness. |
| D-TERRAIN-3 | C | **FIXED (REN-7, 2026-07-07)** | **Below-horizon fill**: retail fills the below-rim region with the frame clear alone — the env #21 horizon-blended skyfog `[orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]`; no skirt/ring geometry exists in the frame walk (the sky-pass terrain leg `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670` is the plain fogged sector batch), the seam hidden by fog convergence at the 1024 fog reference (= the dome rim radius). The reimpl's clear consumer was swallowed by a `BG_SKY`(null-sky) Environment rendering BLACK; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` in `game_world.tscn`, GUT-pinned — and `get_frame_clear_color()` corrected to the post-blend DOUBLED skyfog (the modulate2x-path Clear takes it verbatim; the "undoubled" 07-05 reasoning was the non-modulate2x fallback, no reimpl analog). Residual (not a retail-parity surface): the ONED editor preview's far-env adoption rides ONED polish/ENV-1. |
| D-TERRAIN-4 | C | PERMANENT (candidate) | **Raycast editor-mode guards** (ENG-3 B1): beyond-extent = no-terrain/no-hit vs retail's clamp-to-edge `[orig: @ 0x31a0010/0x319fc0c]`; no-data = clear/NAN vs retail's return-HIT `[orig: @ 0x60ccf7]`; contiguous-atlas bilinear vs the per-quadrant seam flags `[orig: @ 0x31a17f0..]`. Same class as the ratified `coords_editor_options` guards (ADR 0020); §Runtime terrain queries carries the retail forms for any future runtime-faithful implementation. |
| D-TERRAIN-5 | A | **FIXED (2026-07-13)** | **Top-tier texture/shader source mismatch**: the reimpl incorrectly used its heightmap normal as the t3 detail coefficient, camera-crossfaded near/far textures, float-normalized DBlend, and multiplied an extra terrain tint. the separate heightfield-normal atlas feeds cached-tile alpha; DBlend, paired mip chains, and literal t0..t5 ps.1.4 math are ported. **Corrected 2026-07-15**: the fix's own first reading (t3 = the generated authored-detail B-channel coefficient) was also wrong — t3 is the authored second detail pair (`polytrn_detailmap2` ⊕ `dist2`) at density2; the generated coefficient belongs to the ps.1.1 tiers at stage 7 [`orig: Texture_GenerateNormalMap @ 0x58c070`; `Terrain_GenerateNormalMap @ 0x603210`; `PolyTrn_InitTextures @ 0x60aaa0`; `GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270`; `PolyTrn_PS14SplatNormalMap @ 0x7dece0`]. |
| D-TERRAIN-6 | A | **FIXED (2026-07-13)** | **LOD/fog/overlay base-pass semantics**: both raw `lod_sub / 2` sites now use the exact clamped eight-family selector; exponential fog uses eye-space depth while linear types use radial distance; tile-overlay RGB is composed before terrain lighting without replacing the cached heightfield/light DOT3 alpha [`orig: render_terrain_sector_batch @ 0x6096f0`; `Render_SetFogState @ 0x58a950`; `PolyTrn_RenderTile @ 0x60da70`]. |
| D-TERRAIN-7 | A | **OPEN (bounded)** | **Tile-composition RT/update parity**: the bare t0 producer, quadrant CLAMP behavior, alpha math, static `.til` composition, and static model-shadow eligibility/ROBJ/receiver policy are closed. The reimpl uses a terrain-receiver-only directional-shadow approximation and preserves caster eligibility through destruction-to-husk swaps and editor transforms. Retail's alpha-aware foliage projection, exact temporary-RT projection/composite, tile-cache allocation/dirty cadence, general patch/page c7/c8 projection, remaining ordered depth-alpha contributions, and final RT mip behavior remain open `[orig: Terrain_CollectAndRenderTileModels @ 0x60D250; PolyTrn_RenderTile @ 0x60DA70]`. |
| D-TERRAIN-8 | A | **OPEN (bounded)** | **Underwater terrain water-noise modulation**: the reimpl does not select the below-water `PolyTrn_PSShadowBasic/NormalMap` variants or sample `Water_NoiseColorTexture` as t3 (`4·t3²·t0.a`). This is unrelated to scene/model shadows `[orig: below-water flag @ 0x60FEE0 → dword_319FB3C @ 0x60915F; stage-3 bind @ 0x6043C2..0x6043F2; shader select @ 0x6044B1]`; see [render/render-lighting-re.md](../render/render-lighting-re.md). |
| D-TERRAIN-9 | B | **OPEN (editor-preview-only)** | **Derived input preprocessing**: runtime binds integer-normalized DBlend and paired base/far C1/C2/C3 mip chains, including an explicit 4x4 terminal-LOD clamp. The live editor preview binds raw DBlend and raw detail textures; its authored-B coefficient fallback is exact, but minified detail/blend can differ from play. |
| D-TERRAIN-10 | A | **FIXED (2026-07-14)** | **Terrain light-vector coordinate basis**: EnvFile preserves the direct retail getter tuple `g`, not Godot/world XYZ. Retail's D3DCOLOR pack writes GPU diffuse RGB `(g2,g0,g1)`, matching normal-map RGB `(grid X slope, grid Y slope, up)`; the old reimpl `(x,z,y)` pack swapped the horizontal DOT3 axes. Terrain and analytic foliage now pack `(z,x,y)`. Flat 06:00/12:00/18:00 checks could not distinguish the swap, so a non-flat 08:00 oracle pins light bytes `(231,83,187)` and slope alphas `0.8987774/0.0794002` [`orig: Environment_GetLightDirectionFloat @ 0x57d870; Terrain_GenerateNormalMap pack @ 0x603470..0x6034eb; PolyTrn light pack @ 0x60e201..0x60e331; PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e`]. |

The fresh pass closes D-TERRAIN-5/6/10 and bounds D-TERRAIN-7/8/9. The terrain
data path remains the byte-identical TrnGen port; the pending grill below is
now documentation depth around CDEP/traversal plus those two runtime gaps.

## Pending (the deep grill, to complete R1)

The **data path is proven byte-identical** — the build → mesh-simplify → CPT
export chain reproduces the canonical output exactly across 5 fixtures (above),
so `mesh_simp` needs no further witness (it was the concern; it is closed). What
remains for a *full* (vs partial) R1 record:

- **CDEP / quadtree witness depth** — finish the on-disk block-encoding record
  and retail traversal/threshold re-confirmation. The final eight-family
  `lod_sub / 2` selector is closed; this item no longer includes shader
  binding or final mesh-family selection. `cdep_read` / `cdep_roundtrip`
  already pin the header and encode/decode round-trip against `Dvxi5.cpt`.
- **Tile-composition mechanics** — close D-TERRAIN-7 by matching retail's
  temporary static-shadow RT projection/composite, render-target lifecycle and
  update cadence, general patch/page c7/c8 projection, remaining ordered
  depth-alpha draws, and the final RT's sampling/edge/mip policy. The bare
  producer's TrnNMap filter/address behavior, hosted static `.til`
  composition, and static-shadow admission/receiver policy are closed.
- **Underwater water-noise modulation** — close D-TERRAIN-8 independently by
  feeding the generated water-noise color texture through the below-water
  terrain variants described in
  [render/render-lighting-re.md](../render/render-lighting-re.md).
- **Editor derived-input parity** — close D-TERRAIN-9 by routing the live
  preview through the runtime integer DBlend normalization and paired custom
  mip-chain builder without replacing its live-sculpt geometry path.

The CDEP/traversal item is documentation depth; D-TERRAIN-7's exact
tile-shadow/cache mechanics and D-TERRAIN-8's underwater modulation are the two
bounded open runtime parity surfaces, while D-TERRAIN-9 is limited to the
editor preview. D-TERRAIN-1 remains the deliberate editor/runtime split.

## Cross-references

- Reimpl: `libs/terrain`, `libs/terrain_query` (ADR 0020, the world→height seam).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env
  far-colormap bake (#19).
