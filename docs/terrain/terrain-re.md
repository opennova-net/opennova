# Terrain — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **terrain** pipeline — the
heightmap/mesh build, the quadtree LOD, CDEP, lighting/modulation, mesh
simplification, byte packing, and (REN-4) the runtime surface-shading resource
set. The reimplementation surface is `libs/terrain`
(+ `libs/terrain_query`, the world→height seam, ADR 0020) and the Godot terrain
host. Binaries: **both** `jodemo.exe` (the accessible LOD/quadtree/mip renderer)
and retail **Jointops.exe** (lighting/modulation/fog/shading). This file is the
committed home for the `D-TERRAIN-…` catalog. Produced 2026-07-05 (PAR-R1);
the runtime shading section landed 2026-07-06 (maturity REN-4); the runtime
terrain-query section (height samplers + segment raycast) 2026-07-07
(ENG-3 B0).

**Status: PARTIAL.** Terrain is the largest system and the last of the seven
`UNAUDITED` systems; this record establishes the tracked surface — the module
map, the mixed-binary witness basis, and the one known deliberate divergence —
and honestly scopes the deep byte-parity grill (mesh simplification + the CDEP/LOD
bitstream) as the remaining work, the way [mission/mis-format-re.md](../mission/mis-format-re.md)
is a partial. It converts terrain from `UNAUDITED` to *tracked (partial)*.

## Module map (`libs/terrain`) and witness basis

| Module | Role | Witness |
|---|---|---|
| `builder` | heightmap → terrain mesh (the build pipeline entry) | the TrnGen byte-identical data path (canonical reference) |
| `quadtree` / `build_quadtree` / `lod` | quadtree LOD traversal, frustum culling, height mipchain | **jodemo** `Terrain_TraverseQuadTreeNode @ 0x5C89C0`, `Terrain_CollectVisibleSectors @ 0x5C9120`, `Terrain_BuildHeightMipChain @ 0x5C5310` |
| `cdep_constraint` | quantized [min,max] of the 256 pixels of a block (CDEP depth constraint) | documented in-code; full CDEP bitstream grill pending |
| `lighting` | terrain lighting colors + per-position modulation | **retail** `Terrain_SetLightingColors @ 0x5C4B10`, `Terrain_GetModulatedColorAtPos @ 0x5C5FE0`; fog via `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` |
| `mesh_simp` | mesh simplification (edge-collapse) | **BYTE-IDENTICAL — verified**: `dvd4_parity` (canonical `.cpt`) + `parametric_parity` (Sample/Gradient/Checker64/Perlin, 4.6–6.8 MB CPTs each) all produce byte-identical output. The in-code "divergence point / vertex 1223" logging is leftover debug scaffolding from when parity was being achieved, now inert. `parametric_parity` is ctest-`DISABLED` only for CI runtime cost (~5 min), not for any correctness gap |
| `packing` | word→byte packing | **retail** `pack_words_to_bytes @ 0x403CD0` (low byte of each u16, 3 bytes/group) |
| `depthmap` | depth/height map storage | in-code |
| `terrain_query` raycast (B1 pending) | world-space height samplers + the segment raycast the editor/celestial hosts adopt | **retail** §Runtime terrain queries below (`Terrain_SampleHeightBilinear @ 0x6067b0`, `Terrain_RaycastHeightmapLoRes @ 0x60cb80`, `Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`) |

The tile overlay and foliage that render over the terrain surface have their own
now-landed records: [tiles/til-re.md](../tiles/til-re.md) (PAR-R3),
[foliage/foliage-re.md](../foliage/foliage-re.md) (PAR-R2). The `terrain_rgb`
tint stack the surface modulates through is [env/env-tod-re.md](../env/env-tod-re.md) #19.

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

**Texture build** (`PolyTrn_InitTextures @ 0x60aaa0`): detail textures 1-3
(tier ≥ 2 with names present) or the single detail; the average detail color
→ `flt_319F9D0/D4/D8` (0.50196 = 128/255 constants when the blendmap path is
active); a generated normal map (`Texture_GenerateNormalMap`, scale 1/32);
the blendmap NORMALIZED per texel so R+G+B sums to 255 (zero-sum → pure R)
then quadrant-split into `DBlendmap0..3`; the 1024×1024 colormap
checksummed, quadrant-split into `Colormap0..3` (tier path raw, non-tier
path alpha-PREMULTIPLIED), with `TrnNMap0..3` quadrants from the global
normal-map buffer; the 256×256 far colormap (`"PolyTrn colormap2"
@ 0x319f798`) box-downsampled 4×4 from the premultiplied colormap with the
UNDERWATER TINT baked (texels at/below `Env_WaterHeightFixed >> 15`:
`color/4 + (48,32,32) BGR`) and multiplied by `PolyTrn_TerrainTintFull
@ 0x31a1824` `>> 12` — this loop IS env #19's bake site (its READERS are
zero-xref, confirming the record's dead-code disposition); the `"depthspin"`
256×256 shore texture (4-tap height sums `<< 14` in alpha over white); the
4×4 `"PolyTrnClip"` pattern (gray, alternating alpha rows); the 128×128
`"PolyTrnNoise"` detail noise (PRNG ±4 offsets around 0x80 per channel).

**Terrain pixel shaders** (`compile_terrain_pixel_shaders @ 0x605260`, gate
caps bit 8 = ps1.1; handles at `PolyTrn_PS*`): all share the lighting shape
`r0 = ((t0.a·c1 + c0)/2) ×2 t0 [× detail term]` —
`PolyTrn_PSBasic` (×4 t1 detail), `PolyTrn_PSNormalMap` (×4 `dp3(t1
normalmap, t2 light-direction texture)`), `PolyTrn_PSDualNormalMap` (two
dp3 bump terms ×2/×4), `PolyTrn_PS14Splat` (ps.1.4: three detail textures
sampled at t1 by samplers 1/4/5, blended by the t2 blendmap's RGB — the
3-way splat — then the colormap lighting chain ×4),
`PolyTrn_PS14SplatNormalMap` (splat + dp3), `PolyTrn_PSShadowBasic` /
`PolyTrn_PSShadowNormalMap` (t3 = shadow map: light scale `4·t3²·t0.a`),
`PolyTrn_PSDepthAlpha` (alpha = t0.b via `dp3 c5=(0,0,1)`, rgb = 0 — the
depth/alpha extract pass). The c0/c1 lighting constants are CLOSED (REN-5):
**c0 = the SKY block, c1 = the LIGHT block** (both `[0]` render colors ÷255)
pushed per draw `[orig: terrain_setup_lighting_and_shader @ 0x604420 —
SetPixelShaderConstantF(0, PolyTrn_PSConstC0_Sky @ 0x31a183c) / (1,
PolyTrn_PSConstC1_Light @ 0x31a182c); values init_terrain_lighting_color_ramps
@ 0x604ee0 ← Render_TerrainScene @ 0x610c80 (Env_LightBlock/Env_SkyBlock;
NVG blends the sky arg toward the modulator, the vehicle scope forces
0x101010/0xF0F0F0)]` — so the shared shape is **terrain light =
t0Alpha × light + sky** (the ÷2 and MODULATE2X cancel). **t0-identity
correction (2026-07-10 foliage regrill)**: the mesh draw's t0 is the baked
**terrain patch-cache RT**, not the raw colormap —
`CD3DDevice_FindBestTexturePermutation @ 0x604392` resolves the 128-slot
renderer cache into the t0 dynamic slot — and the base bake zeroes the
colormap alpha (base-pass diffuse `0x00808080`,
alpha byte 0) then adds **saturate(N·L)** into alpha via the additive DOT3
pass (`PolyTrn_TileBakeDot3LightPass`: TrnNMap · packed light-dir diffuse)
`[orig: PolyTrn_RenderTile @ 0x60dce5 / 0x60e38a]`. The earlier "colormap
alpha = the baked sun mask" fold-input reading is RETRACTED — the fold
consumes the patch-cache RT's N·L, rebaked as the TOD moves (the cache stamps
`Env_TodMinutesElapsed @ 0x60dbc0`). Base RGB is the colormap ×1.004 (no
detail/noise); optional overlay/decal/scorch passes can composite RGB. The
host include (`terrain_lighting.gdshaderinc`) folds the colormap alpha as its
base-only stand-in until the heightmap normal-map generator ports (D-TRN
ledger row; the foliage FAR pass shares the same stand-in —
[foliage/foliage-re.md](../foliage/foliage-re.md) §The terrain patch-cache
RT). The
foliage/sector-model blend PS inherits the same
device constants and the same patch-cache RT as its t1. Ported:
`terrain_lighting.gdshaderinc` (the prior
combined/fill pairing was a gobj-era stand-in) +
`renderer::terrain_surface_light` (T1 section 5). Full chain:
[render/render-lighting-re.md](../render/render-lighting-re.md).
**Include correction (2026-07-06, D-TERRAIN-2)**: `terrain_lighting.gdshaderinc`
had stacked TWO ×2 detail-normal factors on the splat (the gobj-era "v23
dual-normal" chimera; under the gamma-faithful pipeline it clipped regions
to white) — rewritten to the witnessed top-tier instruction stream, source
text pinned: `PolyTrn_PS14SplatNormalMap @ 0x7dece0` = `mul/mad ×2 splat`,
`dp3 r3, r3(normalmap), r2(blendmap)`, `mad_d2 (r0.a·c1 + c0)`, `mul_x2 ×
colormap`, `mul_x2 × dp3`, `mul_x4 × splat`; `PolyTrn_PS14Splat @ 0x7dee18`
is the same without the dp3 pair. The witnessed t3 = the heightmap-derived
generated normal map (scale 1/32); the host's near/far .trn detail-normal
crossfade is the texture-source stand-in (note-only residual).

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

**Foliage / sector models** (corrected two-tier witness, 2026-07-09):
`Foliage_LoadDefAssets @ 0x601260` loads each slot's source 3DI and creates the
shared `:fd` alpha texture (wrapped 3×3 smoothing, RGB flattened to 0x808080),
but FAR and MODEL use distinct geometry and material paths.

- **FAR feed** - terrain traversal collects leaf tiles within 42.0 of the
  camera (<=128) into `Terrain_NearSectorPatchList`/`Foliage_VisibleFarKeyList`
  `[orig: Terrain_TraverseQuadtreeNode @ 0x60905c ->
  Terrain_CollectNearFoliagePatches @ 0x603e60]`; per-def slot pools bake each
  new key once and LRU-evict by frame stamp `[orig: Foliage_UpdateFarCellSlots
  @ 0x601b30]`. Draws walk the collected list far-to-near with per-patch
  fade (knee 20.0, slope 1/22) and the high/low pass split at 33.0
  `[orig: render_terrain_lightmaps @ 0x60a171..0x60a53b]`.
- **FAR geometry** — `generate_foliage_instances_0 @ 0x5ffdd0` (args: key, VB, IB, out counts - no view cull) accepts up to
  all 36 candidates and copies the complete source vertex/index arrays per
  candidate; it is not a square/quad/ground patch. XZ scale is 1.0, source Y is
  halved, and terrain is sampled below every transformed source vertex. Source
  UVs and indices are replicated. After dead terrain-color work,
  `@ 0x6002DB..0x60030A` unconditionally stores red-only
  `clamp(trunc(srcY*128),0,255)<<16` as the wind weight. After the importer's X
  reflection, the host basis is `rotY(yaw + π/2)`.
- **FAR render** — `Terrain_CreateFoliageVertexShaders @ 0x5ff630` assembles
  `Foliage_WindSwayVS`: c27 phase wrapping plus the witnessed tenth-order
  cosine polynomial, weighted by vertex red at amplitude 0.03, displacing
  render Z only. `Foliage_LightmapBlendPS @ 0x5ff7a0` computes
  `rgb=t0*(t1*(t1.a*c1+c0))*c6.rgb*8`, `a=t0.a*c6.a`; vertex red is not a
  lighting color. FAR high/low pass flags are `0x02460000`/`0x02560000`, with
  alpha-test refs **180**/**8**. The fixed-function fallback is stage0
  `2*T0*diffuse` plus alpha `T0.a*diffuse.a`, then stage1 `2*T1*current`.
  Retail c24.x adds `Env_WaveOscRing[0]*1.5258789e-6` to the wall-clock phase;
  the host binds that complete witnessed expression through the
  environment/global/dispatcher seam.
- **MODEL** — `Foliage_GenerateModelTileInstances @ 0x600980` stamps the full
  source mesh with its separate cap 21, XZ scale 0.75/Y scale 0.5, and the
  eight-sample biquadratic fit evaluated by `Foliage_GridPlacementVS`. The
  per-anchor alpha ref is `clamp(4096/(distance+1),8,128)` and the wind counter
  advances per tile draw. The one-stage table[16] pass (flags `0x00440000`)
  binds T0=`:fd`, selects c6 diffuse `(0,0,0,1)` for black RGB, and multiplies
  `T0.a*diffuse.a`; MODEL does not run FAR's lightmap blend
  `[orig: Foliage_DrawModelTileSlot @ 0x601d90]`.

The host ports these as separate `foliage_far.gdshader` and
`foliage_model.gdshader` paths, with the FAR feed/pool/draw state hosted as
witnessed. Still approximate under D-FOLIAGE-7: the exact per-tile T1
render-target content, the c6.rgb float chain, the MODEL sector-entity
visibility stream, the blocker registry, and the second low wireframe
resubmit. Full detail is in
[foliage/foliage-re.md](../foliage/foliage-re.md).

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
  implementation with its own inline march (internals **not yet witnessed** —
  follow-up). Callers: `Physics_RaycastTerrainAndSectors @ 0x539910`,
  `Physics_CheckTerrainLineOfSight @ 0x53b080`, `HUD_RenderAllOverlays`,
  `terrain_occlusion_check_three_rays @ 0x610ed0` (the D-RLIT 3-ray
  sun-visibility source).

### B1 port (landed 2026-07-07)

The B1a/B1b slices landed the LoRes march + HiRes_0 refine as
`libs/terrain_query/terrain_raycast.{h,cpp}` (`terrain_raycast_march` /
`terrain_raycast_refined`, 16.16 structural translations with a
point+bilinear sampler seam; ~60 pinned checks in the `terrain_raycast`
ctest incl. the step-math exactness, the crossing-rule asymmetry, the
height-0 floor, and the odd refine guard's zero-step no-op-walk interplay),
bound as `NovaTerrainData.raycast_terrain(from, to)` over BOTH host
substrates (live editable Image preferred, baked CPT otherwise — the
existing slice-A sampler cores reused). Adopters: ONED mission picking
(`terrain_editor.raycast_terrain_at` — the GDScript march/slab/bisection
trio deleted) and the celestial glare ray
(`nova_celestial._glare_ray_clear`, the 32-unit stand-in retired). The
editor-host guard divergences (OOB no-terrain vs retail clamp-to-edge,
no-data NAN vs retail return-HIT, contiguous-atlas bilinear vs the seam
flags) are **D-TERRAIN-4** (class C, PERMANENT candidate).

Open follow-ups from this pass: the seam-flag WRITER (load-time adjacency
derivation), `Terrain_RaycastHeightmapHiRes @ 0x60c760` internals, and the
rationale (if any) behind HiRes_0's odd skip-refine guard.

## D-TERRAIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TERRAIN-1 | C | PERMANENT (candidate) | **Terrain-shader edit/runtime split** (the one deliberate divergence): the editor renders terrain with a live-sculpt shader (height edits without rebake), the runtime with the baked shader — the *surface-shading math is shared via an include* so the two cannot drift in look. Tracked, justified by an editing need the runtime path cannot serve, and sharing the fidelity-bearing core ([oned/editor-runtime-parity.md](../oned/editor-runtime-parity.md) §Terrain shaders). Ratify under ADR 0022 to move from candidate to `PERMANENT`. |
| D-TERRAIN-2 | A | **FIXED (2026-07-06)** | **Doubled detail-normal factor** (the gobj-era chimera): `terrain_lighting.gdshaderinc` stacked TWO ×2 `dp3(normalmap, blendmap)` factors on the 3-way splat; the witnessed top-tier ps.1.4 applies exactly ONE `[orig: PolyTrn_PS14SplatNormalMap source @ 0x7dece0; PolyTrn_PS14Splat @ 0x7dee18; compile_terrain_pixel_shaders @ 0x605260]` (the dual-normal product belongs to the separate non-splat ps.1.1 tier). Post-gamma (D-RMAT-7) the squared factor clipped whole regions to white. See §Include correction above; ledger row carries the full witness. |
| D-TERRAIN-3 | C | **FIXED (REN-7, 2026-07-07)** | **Below-horizon fill**: retail fills the below-rim region with the frame clear alone — the env #21 horizon-blended skyfog `[orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]`; no skirt/ring geometry exists in the frame walk (the sky-pass terrain leg `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670` is the plain fogged sector batch), the seam hidden by fog convergence at the 1024 fog reference (= the dome rim radius). The host's clear consumer was swallowed by a `BG_SKY`(null-sky) Environment rendering BLACK; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` in `game_world.tscn`, GUT-pinned — and `get_frame_clear_color()` corrected to the post-blend DOUBLED skyfog (the modulate2x-path Clear takes it verbatim; the "undoubled" 07-05 reasoning was the non-modulate2x fallback, no host analog). Residual (not a retail-parity surface): the ONED editor preview's far-env adoption rides ONED polish/ENV-1. |

| D-TERRAIN-4 | C | PERMANENT (candidate) | **Raycast editor-host guards** (ENG-3 B1): beyond-extent = no-terrain/no-hit vs retail's clamp-to-edge `[orig: @ 0x31a0010/0x319fc0c]`; no-data = clear/NAN vs retail's return-HIT `[orig: @ 0x60ccf7]`; contiguous-atlas bilinear vs the per-quadrant seam flags `[orig: @ 0x31a17f0..]`. Same class as the ratified `coords_editor_options` guards (ADR 0020); §Runtime terrain queries carries the retail forms for any future runtime-faithful host. |

No other terrain divergence is confirmed — the data path is the byte-identical
TrnGen port. The pending grill (below) may surface facets in mesh_simp / CDEP.

## Pending (the deep grill, to complete R1)

The **data path is proven byte-identical** — the build → mesh-simplify → CPT
export chain reproduces the canonical output exactly across 5 fixtures (above),
so `mesh_simp` needs no further witness (it was the concern; it is closed). What
remains for a *full* (vs partial) R1 record:

- **CDEP / LOD bitstream** — `cdep_constraint` + the quadtree mip chain: witness
  the on-disk CDEP block encoding and the LOD selection thresholds against the
  binary. (`cdep_read`/`cdep_roundtrip` ctests already pin the CDEP header against
  a `Dvxi5.cpt` capture + the encode/decode round-trip — so this is documenting the
  witnessed encoding, not discovering it.)
- **Runtime render pass** — the SURFACE-SHADING half landed at REN-4 (the
  witness map above: tiers, textures, the eight pixel shaders, the stage
  tables, the foliage-model shader set) and the LIGHTING half at REN-5 (the
  c0/c1 constants closed above; the sector-model lightmap-tile pass +
  mission lightmap TGA chain witnessed in
  [render/render-lighting-re.md](../render/render-lighting-re.md) — the
  baked-shadow PS variants + TGA draping stay untracked-hosted, D-RLIT-6);
  the remaining half is the retail QUADTREE TRAVERSAL re-confirmation (the
  reimpl LOD/mip cites **jodemo**) plus the draw-time binding walk
  (`render_terrain_sector_batch @ 0x6092a0` internals).

Neither is an open *divergence* — they are documentation depth. The one tracked
terrain divergence remains D-TERRAIN-1 (the deliberate shader split).

## Cross-references

- Reimpl: `libs/terrain`, `libs/terrain_query` (ADR 0020, the world→height seam).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env tint (#19).
