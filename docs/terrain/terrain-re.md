# Terrain — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **terrain** pipeline — the
heightmap/mesh build, the quadtree LOD, CDEP, lighting/modulation, mesh
simplification, byte packing, and (REN-4) the runtime surface-shading resource
set. The reimplementation surface is `libs/terrain`
(+ `libs/terrain_query`, the world→height seam, ADR 0020) and the Godot terrain
host. Binaries: **both** `jodemo.exe` (the accessible LOD/quadtree/mip renderer)
and retail **Jointops.exe** (lighting/modulation/fog/shading). This file is the
committed home for the `D-TERRAIN-…` catalog. Produced 2026-07-05 (PAR-R1);
the runtime shading section landed 2026-07-06 (maturity REN-4).

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
depth/alpha extract pass). The c0/c1 lighting constants are REN-5's
lightmap-chain scope.

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

**Foliage / sector models** (the four terrain-attached model slots):
`Terrain_InitSectors @ 0x601260` loads the models, smooths each texture's
alpha by a 9-tap kernel (2× center+cross, 1× corners, `>> 4`) with
gray-0x808080 mip fill (alpha preserved), and creates TWO materials per
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
`Terrain_SetupSectorModelDraw @ 0x6007c0` (ex-misnomer
`terrain_setup_display_adapter`; per model slot 0-3) picks the LOD entry,
binds the wind VS + FVF 338, fog mode 8 (VS fog) when the wind VS exists,
and **alpha-test ref 180 (high quality) / 8 (low)** — the host
`foliage.gdshader` cutoff 0.33 is a tracked stand-in (its header carries
the witness). The old "0x005BF064 pixel shader" / "sub_5C1790" anchors in
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

## D-TERRAIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TERRAIN-1 | C | PERMANENT (candidate) | **Terrain-shader edit/runtime split** (the one deliberate divergence): the editor renders terrain with a live-sculpt shader (height edits without rebake), the runtime with the baked shader — the *surface-shading math is shared via an include* so the two cannot drift in look. Tracked, justified by an editing need the runtime path cannot serve, and sharing the fidelity-bearing core ([oned/editor-runtime-parity.md](../oned/editor-runtime-parity.md) §Terrain shaders). Ratify under ADR 0022 to move from candidate to `PERMANENT`. |

No other terrain divergence is confirmed yet — the data path is the byte-identical
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
  tables, the foliage-model shader set); the remaining half is the retail
  QUADTREE TRAVERSAL re-confirmation (the reimpl LOD/mip cites **jodemo**)
  plus the draw-time binding walk (`render_terrain_sector_batch @ 0x6092a0`
  internals) and the c0/c1 lighting constants (REN-5's lightmap chain).

Neither is an open *divergence* — they are documentation depth. The one tracked
terrain divergence remains D-TERRAIN-1 (the deliberate shader split).

## Cross-references

- Reimpl: `libs/terrain`, `libs/terrain_query` (ADR 0020, the world→height seam).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env tint (#19).
