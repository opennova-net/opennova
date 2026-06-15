# Object renderer (HLSL `.fx` effect system) — reverse-engineering record

Engine-research record for the original engine's **object material/shader
system** — the renderer that draws `.3di`/GP models. Implementing code today is
`libs/renderer` (the GLSL string builder) + `godot/engine/object`
(`NovaObjectShaderCache`, `nova_object_model.gd`); the descriptor table is
`libs/oed/include/oed/material_descriptor.h`. Binary: retail **Jointops.exe**
(IDB `Jointops.exe.kong.i64`); all addresses are that binary's.

A second, equally authoritative evidence source is used here: the original
engine compiles its object shaders from **HLSL `.fx` effect files shipped in the
game's PFFs** (SCR-encrypted on disk — `'SCR',0x01`, key `0xA55B1EED`, via
`ScriptFile_LoadAndDecrypt @ 0x5AE060`; see
[audio/mus-sbf-re.md](../audio/mus-sbf-re.md) D-SCR-2). Decrypted, they are
plain HLSL. They are NovaLogic's copyrighted source: this record **cites and
summarizes** their behavior (`[orig: <name> in <file>.fx]`) and never vendors
them, exactly as binary findings are summarized, not pasted. The decrypted set
was read from a local extract during the 2026-06-14 session.

> **Status: shader math ported; runtime feed witnessed; alignment gaps tracked.**
> The original is witnessed end to end — load path, lighting math, the FF_* mint
> rule, and (grill 2026-06-15) the runtime parameter feed that drives the effects
> per frame. The shader-math port landed (PR #158): the Godot shaders mirror the
> `.fx` macro structure with the witnessed lighting. The remaining divergences are
> now mostly in the **runtime feed**, not the shader math — chiefly the missing
> iris exposure (D-RENDER-12), the wrong point-light source (D-RENDER-9), the
> bumped-hemisphere ambient block (D-RENDER-14), the `MatTexCoord1` UV matrix
> (D-RENDER-8), and case-sensitivity (D-RENDER-13) — plus the open questions below.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Effect load + registry (`HLSLEffect_*`) | **WITNESSED** (engine-research) | 8 cited functions; `D3DXCreateEffect @ 0x690293` (d3dx9 statically linked); 1004-byte effect entries at `byte_27E56A0`; ~87 named parameters resolved by name |
| Shader authoring model (macro über-shader) | **WITNESSED** | `_FFP.fx` macro permutations = the 24 `FF_*` tags; per-technique files = the `VS_*`/`FFP_*` tags; `#include` + `CIncludeManager`; the `#UV` variant = `TEX_UVXFORM` / `EffectAlt_UV` |
| Lighting / fog / attenuation / specular / UV math | **WITNESSED** | summarized from `_BaseInc.fx`, `_psDiff.fx`, `_psPhong.fx`, `_vsFlat.fx`, `_vsDiffT.fx`, `_vsPhongT.fx` (see witness map) |
| `libs/renderer` GLSL builder (`object_shader_template.cpp`) | **DIVERGENT** (invented, pre-witness) | lighting is hand-authored, not ported; D-RENDER-1..11 |
| Runtime per-frame parameter feed | **WITNESSED (mostly)** (grill 2026-06-15) | draw loop `CRenderBatchQueue_FlushBatches @ 0x5d9f50` → per-entity `setup_entity_lighting_and_shader_constants @ 0x5d98a0` (`DirLightVector` h62, `.w`=0.8) + per-pass `apply_shader_parameters @ 0x58db80` (textures, UV 3×2 `MatTexCoord1` h52, `FogStart`/`FogRangeRecip` h54/55, `CameraPos` h61, iris `ColorSrcGlobalGain` h68 = `Render_LightScaleRGB`) + per-light `sub_5A9180 @ 0x5a9180`. Lighting colours = env `Light`/`Sky`/`Ground` blocks ÷255 (terrain twin `CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090`). Open: the exact object `DirLightColor`/`Hemi*`/`AmbientColor` `SetVector`/pool site (h63–66 conflict) |
| Geometry batching + buffer pooling | **WITNESSED** (grill 2026-06-15) | shared pool sub-allocation `allocate_lod_gpu_buffers @ 0x5b2610` / `shadow_renderer_allocate_pool_slot @ 0x585b90` (4 pools); offset draws + change-gated binds in `CRenderBatchQueue_FlushBatches @ 0x5d9f50`; presort + 15-bit sort key in `collect_render_objects_for_batch @ 0x5d8f20`. Reimpl = one mesh/draw per primitive (D-RENDER-15) |
| Non-`NORMAL` techniques (CLIP / PROJSHAD / DEPTHMASK / GLOW / MATCHTERRAIN) | **WITNESSED (structure), unported** | technique-type selector enumerated in `_BaseInc.fx`; per-pass render-state recovered; the runtime selector is unwitnessed |

## Effect load + registry — witness map

The renderer is a runtime HLSL effect system, not a fixed-function-in-code
pipeline. Effects are compiled at device init/reset and registered by name; a
`.3di` material's shader tag (e.g. `FF_MT_OP`, `VS_PHONGT`) is the registry key.

- **Reload entry** — `sub_5B0080 @ 0x5b0080` (proposed name
  `HLSLEffect_LoadAllEffects`): releases existing effects
  (`HLSLEffect_ReleaseAllDeviceObjects @ 0x5add80`), creates the effect pool
  (`EffectPool_Create @ 0x69013f`), queries device caps, runs
  `HLSLEffect_InitFixedFunctionShaders @ 0x5af790`, then loads effects from
  **either** an override directory of loose `.fx` **or** the default path plus
  up to 15 PFF archives (`HLSLEffect_LoadAllFromPFFArchive @ 0x5afed0`). The
  override path is the modder/editor hook.
- **Directory scan** — `HLSLEffect_LoadFromDirectory @ 0x5afd40`: globs
  `"%s\*.fx"` (`@ 0x7da6e0`), **skips `_`-prefixed files** (those are `#include`
  fragments, not standalone effects), and loads each twice — a base load and,
  when the effect sets `EffectAlt_UV`, a second `TEX_UVXFORM` load whose
  registry name gets the literal `"#UV"` token appended — **not** `", UVGen"`
  (an earlier draft error; the witnessed append is the 4-byte `"#UV"` at
  `0x565523`, `[orig: HLSLEffect_InitFixedFunctionShaders @ 0x5afc34]`; the only
  `"…UVGen…"`-like text is the human display name, never the registry key). That
  `#UV` suffix is our `…#UV` tag.
- **Compile + reflect** — `HLSLEffect_LoadFromFile @ 0x5ae690`: decrypts the
  file, then `D3DXCreateEffect @ 0x690293` with preprocessor macros
  `TRILINEAR`/`ANISO` (filtering), `FFPTRANSPOSE`, and (for the UV pass)
  `TEX_UVXFORM`. It reads the `EffectInfo` technique's annotations
  (`EffectTag`, `EffectName`, `EffectSpecial`, `EffectAlt_UV`, `MinToolVersion`),
  dedupes by name (`HLSLEffect_FindByName @ 0x5ade70`), copies pass metadata for
  up to 6 pass-type slots (`HLSLEffect_ParsePassData @ 0x5ae120`), then resolves
  ~87 parameter handles **by name** and derives per-effect capability flags by
  testing which parameters each technique actually references
  (`D3DXGetShaderInputSemantics @ 0x68fbf6` detects tangent input).
- **Effect entry** — `byte_27E56A0`, stride **1004 bytes**, count
  `dword_28E06A8`. Name at +0, display name at +32, the `ID3DXEffect*` at +172,
  capability flags at +160, sort key at +164; the resolved parameter handles
  occupy +656.. (87 dwords).

The ~87 parameter names (all `[orig: … in _BaseInc.fx]`, resolved at
`0x5aec09..0x5af6ab`) define the engine's shading interface. The lighting-
relevant ones: `DirLightVector`, `DirLightColor`, `HemiGroundColor`,
`HemiSkyColor`, `AmbientColor`, `PointLightCoord/Color/Atten` (+ `…Array` and
`CurNumPointLights`), `SpotLight*`, `FogStart`, `FogRangeRecip`, `CameraPos`,
`SelfLumColor`, `AlphaGenValue`, `AlphaTestFlag`, `ReflectColor`,
`ReflectBumpDepth`, the `Mat*` transforms, and `SkinWorldMatrixArray` /
`SkinModelLightArray`.

## Shader authoring model — the macro über-shader

The original is **not** 45 independent shaders, and **not** an abstract feature
table. It is a small set of `.fx` files sharing `_BaseInc.fx`, multiplied out by
preprocessor macros. This is the structure the Godot port mirrors (decided
2026-06-14: shared `_baseinc.gdshaderinc` + ~6 family shaders with `#define`
toggles).

- **Fixed-function family (24 `FF_*` tags) = one file, `_FFP.fx`**, compiled
  with `BLEND_NONE|BLEND_ALPHA|BLEND_ADD` (three modes — **there is no `MULT`
  permutation**) × `TEX_SINGLE|TEX_MULTIPLE` × `SELFLUM` × `TEX_UVXFORM`.
  `FF_MT_OP` = `TEX_MULTIPLE` + `BLEND_NONE`; `FF_ST_AB_LUM#UV` = `TEX_SINGLE` +
  `BLEND_ALPHA` + `SELFLUM` + `TEX_UVXFORM`. The file's own `EffectTag` is
  `"FFP_BORING"` (display name `"Boring - …"`). The per-macro `FF_*` registry
  names are minted by `HLSLEffect_InitFixedFunctionShaders @ 0x5af790`
  (**witnessed 2026-06-15**): a `tex{ST,MT} × selflum{off,LUM} × blend{OP,AB,AD}`
  loop, each with an optional `#UV` (`TEX_UVXFORM`) variant gated on the base
  effect's `EffectAlt_UV` bit (entry `+169 &1`) = 24 rows. Name =
  `sprintf("FF%s%s%s", texTag, blendTag, selflumTag)` `[orig: @ 0x5afad6,
  `"FF%s%s%s"` @ 0x7da668]` with `texTag∈{_ST,_MT}`, `blendTag∈{_OP,_AB,_AD}`,
  `selflumTag∈{"",_LUM}`, then `"#UV"` appended for the UV variant `[orig:
  @ 0x5afc34]`; it is `strcpy`'d over the row's `name@+0` (replacing
  `FFP_BORING`) and `caps@+160` set to `combined_flags = selflum|tex|blend`
  (`tex {ST:4, MT:0xC}`, `blend {OP:0, AB:0x1002, AD:0x1000}`, `selflum {off:0,
  on:0x10000001}`, `|0x10000` for `#UV`). `[orig: FFP_BORING in _FFP.fx;
  HLSLEffect_InitFixedFunctionShaders @ 0x5af790]`
- **Shader families = per-technique files**, each `#include`-ing `_BaseInc.fx`
  plus `_vs*.fx`/`_ps*.fx` fragments: `VS_PHONGT` (`PhongT.fx`), `VS_DOT3DIFF`
  (`Dot3DiffT.fx`), `VS_ENVPHONGT` (`EnvPhongT.fx`), `FFP_GLASS` (`Glass.fx`),
  `VS_FLAG` (`Flag.fx`), the `Sk*` skinned set, the mirror/bump set.
- Each effect carries **multiple techniques selected by `ttype`** —
  `TECHNIQUE_NORMAL`(0), `PROJSHAD`(1), `DEPTHMASK`(2), `CLIP`(3), `GLOW`(4),
  `MATCHTERRAIN`(5) — and within a technique multiple **passes** filtered by
  `passrules` (per point light, per spot light, with/without spotlights). The
  port targets `NORMAL` first. `[orig: technique-type + pass-rule defines in _BaseInc.fx]`

## Lighting model — the witnessed math

All equations below are summarized from the shipped `.fx` (cited per line); they
are the behavior the port reproduces. Two pervasive conventions first:

- **2× overbright.** Every surface combine ends in a doubling — `Modulate2x` in
  fixed-function texture stages, `mul_x2`/`mad_x2` in the pixel shaders. Final
  color is `texture × light × 2`. This is the dominant look of the era; omitting
  it reads as flat and dim. `[orig: ps11Diffuse in _psDiff.fx; Modulate2x throughout _FFP.fx]`
- **`_bx2` normal decode.** Normal-map and light vectors are signed-expanded
  `v·2−1` before the `dp3`. Tangent-space light/half vectors are cube-map
  normalized (`TexCubeNormalize`) per pixel.

**Canonical bumped-diffuse combine** — `[orig: ps11Diffuse in _psDiff.fx]`, fed
by `[orig: vsTanDot3DirPS in _vsDiffT.fx]`:

```
N      = decode(normalMap)                       // tangent space, per pixel
dir    = saturate(N · L_tan) · (DirLightColor · selfshadow)
hemi   = AmbientColor + (HemiSkyColor − AmbientColor) · (N · up_tan)
out.rgb = 2 · diffuseTex.rgb · (dir + hemi)
out.a   = diffuseTex.a
```

- **Directional self-shadow** — `selfshadow = clamp(N_geom·L · 10 + 0.5, 0, 1)`,
  computed in the vertex shader (a soft wrap that keeps back faces from going
  fully black). `[orig: CalcSelfShadowTerm in _BaseInc.fx]`
- **Hemisphere, pixel (bumped) path** — midpoint `AmbientColor`, up-pole
  `HemiSkyColor`; `HemiGroundColor` is **not** used on this path.
  `[orig: ps11Diffuse constants c1/c2, set in Dot3DiffT.fx P0]`
- **Hemisphere, gouraud (non-bump / FFP) path** —
  `lerp(HemiGroundColor, HemiSkyColor, N_world.y·0.5+0.5)`, evaluated per vertex.
  This path **does** use `HemiGroundColor`. `[orig: HemicolorFromVectorY in _BaseInc.fx]`
- **Point light** — additive, `PointLightColor · saturate(N·L) / atten` with
  D3D attenuation `atten = a.x + a.y·d + a.z·d²` (constant/linear/quadratic).
  `[orig: CalcPointLightAttenuation in _BaseInc.fx; ps11DiffuseSingle in _psDiff.fx]`
- **Single-pass multi-light path** — `vsFlatBaseHemi` accumulates hemisphere +
  directional + a loop over `CurNumPointLights ≤ MAX_POINTLIGHTS (4)` using the
  `PointLight*Array` uniforms, compiled as a 4-entry array indexed by light
  count. This is the engine's own collapse of the per-light additive passes into
  one draw, and is the sanctioned model for the Godot single-fragment port.
  `[orig: vsFlatBaseHemi + vscFlatBaseHemiArray[4] in _vsFlat.fx]`
- **Specular (Phong)** — `(N·H)` raised to ~8–16 by repeated squaring, gated by
  per-pixel gloss in `diffuseTex.a`, added before the ×2; the `…SpecOnly`
  variant documents the `^16` form explicitly.
  `[orig: ps11Phong / ps11PhongDepthSpecOnly in _psPhong.fx]`
- **Fog** — planar in clip space: `fogFactor = 1 − (viewZ − FogStart)·FogRangeRecip`
  (1 = clear), evaluated per vertex via the `WorldViewProj` third row; the
  fixed-function fog stage blends to the fog color. `FOGMODE_*` per pass selects
  normal/set/add/white blending. `[orig: CalcFogSegmented + FOGMODE_* in _BaseInc.fx]`
- **UV animation (`#UV`)** — a full `3×2` texture matrix `MatTexCoord1`
  (`uv' = (uv,1) · MatTexCoord1`), not scale/rotate/offset.
  `[orig: CalcAnimatedUV in _BaseInc.fx]`
- **Self-illumination (`_LUM`)** — `MaterialEmissive = SelfLumColor · ColorSrcGlobalGain`,
  `MaterialDiffuse = 0`; the surface is fully unlit-bright.
  `[orig: SELFLUM branch in _FFP.fx]`

### Per-family summary

| Tag(s) | File | Shading |
| --- | --- | --- |
| `FF_*` (24) | `_FFP.fx` | FFP T&L, `Modulate2x(tex, vertex-light)`, `MaterialEmissive=AmbientColor`; `SELFLUM`→`SelfLumColor·gain` |
| `VS_DOT3DIFF[2]` | `Dot3DiffT.fx` | per-pixel bump diffuse + hemisphere, no specular (PS hw); FFP `DotProduct3` + `TextureFactor=DirLightColor` fallback |
| `VS_PHONGT` / `VS_PHONGO` | `PhongT.fx`/`PhongO.fx` | bump diffuse + Phong specular (gloss in diffuse-alpha) |
| `VS_ENVPHONGT` | `EnvPhongT.fx` | Phong + extra pass: cube-env reflection (`reflect(eye,N)`, tinted by `ReflectColor`) |
| `FFP_GLASS` | `Glass.fx` | untextured cube-env reflection via `CAMERASPACEREFLECTIONVECTOR` + `MaterialEmissive=ReflectColor·gain`, additive, no z-write |
| `VS_FLAG` | `Flag.fx` | vertex cloth wave (consts move=0.15/speed=8/tight=3/flap=0.8), recomputed normal, hemi+dir, `Modulate2x` |
| `VS_SK*` | `SkBasic.fx`,`SkB*.fx` | 4-bone palette skin (`SkinWorldMatrixArray`, `D3DCOLORtoUBYTE4` indices) over the above |

## Runtime parameter feed — witness map (grill 2026-06-15)

How the per-frame / per-object values actually reach an object effect — the path
the verdict row above called UNWITNESSED. The object draw loop is
`CRenderBatchQueue_FlushBatches @ 0x5d9f50`: per batch it takes the strip's
`ID3DXEffect*` (`*(*meshData+592)`), calls the per-entity then per-pass binders,
then loops the gathered lights and draws (`DrawIndexedPrimitive`, vtable+328).

- **Effect selection (per material, load time).** A `.3di` material resolves its
  effect by **case-insensitive exact full-name** match —
  `HLSLEffect_FindByName @ 0x5ade70` does `stricmp(material.name, entry.name)`
  over the 1004-byte `byte_27E56A0` table. `Material_BindEffectAndCopyPassData`
  (`sub_5B1870`) stores `&byte_27E56A0[1004·idx]` at `material+596` and copies the
  six per-`ttype` pass-data blocks (parsed by `HLSLEffect_ParsePassData @ 0x5ae120`:
  `usevs/useps/useffplights/TANGENT`, per-pass `zmode/amode/passrules/fogmode`)
  into `material+600..+1000`.
- **Per-entity constants** — `setup_entity_lighting_and_shader_constants @ 0x5d98a0`
  sets `DirLightVector` (handle 62) = `-entity.lightDir`, **`.w = 0.8`** (a wrap
  bias carried in `w`, not padding); skin/`Mat` matrices (handles 50/51);
  `FloatTicks` (handle 85 — the animation clock, i.e. what drives `VS_FLAG` wind,
  *not* `AlphaGenValue` which is handle 59); `AlphaTestFlag` (handle 86 =
  `material.flags&1`, via `SetBool`); bbox scale/extents; and a per-entity D3D
  point light. `[orig: @ 0x5d9967]`
- **Per-pass material params** — `apply_shader_parameters @ 0x58db80` (arg1 is the
  `ID3DXEffect*`; arg3 `+596`→the effect entry, `+656`→the handle table) binds the
  textures (with anim-frame select), the `RgbGen` colours, the **3×2
  `MatTexCoord1` UV matrix** (handle 52, `SetMatrix`), `FogStart`/`FogRangeRecip`
  (handles 54/55, **`SetFloat`** — scalars, not vectors), `CameraPos` (handle 61),
  the shadow-cascade textures, and the iris `ColorSrcGlobalGain` (handle 68) =
  `&Render_LightScaleRGB`. `[orig: @ 0x58e06a (iris); @ 0x58e21b (fog)]`
- **Fog values are view-Z remapped, not raw world.** `FogStart =
  worldFogStart·flt_27219A8 + flt_27219B8`; `FogRangeRecip = 1/(flt_27219B8 +
  flt_27219A8·worldFogEnd − FogStart)` (same formula in
  `CEffectWorld_SetupRenderState @ 0x5e039c`). `worldFogStart` is the fixed **0.5**
  from `Environment_ApplyFogAndAmbient @ 0x57e4db` (`Render_SetFogState(0.5,
  end/65536, type, overcast/65536)`; mode-2/3 starts are `end·0.5`/`end·0.25`).
  `flt_27219A8/B8` (projection view-Z scale/offset) read 0 in the static image
  with no witnessed writer — whether they are identity (so view-Z ≡ world) is the
  open question that sets this divergence's weight. `[orig: Render_SetFogState
  @ 0x58a950]`
- **Lighting colours come from the env colour blocks ÷255 — but the object feed
  is via an `EffectPool` shared param, not yet located.** The only fully-witnessed
  builder is the **terrain** twin `CTerrainRenderer_BuildLightingShaderConstants
  @ 0x5c8090`, whose constant array is `c0 = Env_LightBlock/255` (the **sun/moon
  directional colour**), `c1 = −normalize(Environment_GetLightDirectionFloat)` (dir
  vector), `c2 = Env_SkyBlock/255`, `c3 = Env_GroundBlock/255`, with indoor
  variants from `Env_CeilingBlock`/`Env_FloorBlock`. That is the *terrain* shader's
  register layout, **not** the object effect's named params — and the object
  effects' per-frame lighting-colour `SetVector` was not found in any witnessed
  per-pass/per-entity function. An xref sweep (2026-06-15) of `Env_SkyBlock`/
  `Env_GroundBlock` shows **no object-effect reader** (only the TOD writers, this
  terrain builder, the sky/celestial FFP, and debug), so the object
  `DirLightColor`/`HemiSky`/`HemiGround`/`AmbientColor` (handles 63-66) are almost
  certainly set through an `EffectPool` shared parameter (`EffectPool_Create
  @ 0x69013f`), source unwitnessed. **Correction to the first grill pass:** the
  terrain `c0` = `Env_LightBlock` is the **directional colour** (`DirLightColor`),
  *not* a separate `AmbientColor`; the object effect carries a **fourth** colour
  param (`AmbientColor`, handle 66) with no counterpart in the terrain layout, and
  its source is open. `[orig: CTerrainRenderer_BuildLightingShaderConstants
  @ 0x5c80d5..0x5c818b]`
- **Iris auto-exposure is folded in twice.** Per tick every env block's render
  colour `[0]` is pre-multiplied by `Env_ModulatorBlock`; additionally the effect
  receives `ColorSrcGlobalGain = Render_LightScaleRGB = modulator/64`
  (`Render_UnpackModulatorToLightScale @ 0x58db30`, refreshed in
  `Environment_ApplyFogAndAmbient @ 0x57e458`). The reimpl has neither (env-tod #17).
- **Point lights = effect-world glow-light instances, ≤3 per batch.** The batch
  builder `collect_render_objects_for_batch @ 0x5d8f20` gathers up to **3** active
  glow-light effect-instance handles into the batch entry (`[5..7]`, count `[8]`);
  `FlushBatches` feeds each through `sub_5A9180 @ 0x5a9180` into `PointLight*Array`
  + `CurNumPointLights` (`SetVectorArray`, vtable+144). `sub_5A9180` reads a
  176-byte effect-instance slot (`unk_2732E28`): position fixed-point, Y-negated;
  colour = `slot.rgb · EffectWorld_AmbientScaleRGB(iris) · slot.intensity`,
  optionally `· RgbGen`; **attenuation `(c,l,q) = (1, 0, 15/range²)`** — a pure
  quadratic `1/(1 + 15·(d/range)²)`, `range = slot.range·0.000019073`. So the
  lights illuminating an object are nearby **dynamic light effects** from the
  effect world, **not** the model's own `.3di` light records. `[orig: sub_5A9180
  @ 0x5a91e7 (colour), @ 0x5a926f (atten)]`

The effect carries **both** a single-light handle set (`PointLightCoord`/`Color`/
`Atten`/`RangeRecip`, indices 233-236) **and** an array set (`PointLight*Array` +
`CurNumPointLights`, 237-240), plus spot (`SpotLightProjMatrix` 243) and shadow
(`ShadowProjCenter`/`RangeRecip` 241/242, `TexProjMatrix1` 244) handles. `[orig:
HLSLEffect_LoadFromFile @ 0x5af4b7..0x5af598]`

## Geometry batching & buffer pooling — witness map (grill 2026-06-15)

Model geometry is **not** held in per-instance vertex buffers. Every LOD model
sub-allocates a byte range inside a small set of **shared GPU buffer pools**, and
the draw loop draws each strip as an offset sub-range with change-gated state — the
3DI presort makes the change-gating actually elide binds. This is the engine's
draw-call / state-change reduction; the reimpl reproduces none of it (D-RENDER-15).

- **Buffer build** — `allocate_lod_gpu_buffers @ 0x5b2610`. Per LOD model:
  `sub_5B1480(lod, &stride, &vbDecl)` derives the **vertex stride + declaration**
  (skinned verts carry bone indices/weights → larger stride, distinct decl);
  `shadow_renderer_allocate_pool_slot(g_GpuBufferPools, type, size, stride)
  @ 0x585b90` sub-allocates a **stride-aligned byte range** in one of **4 shared
  pools** (`g_GpuBufferPools @ dword_26DCE50`: type 1 = static VB, 2 = dynamic VB,
  4 = static IB, 3 = dynamic IB), returning `{offset@[0], end@[1]}`; `sub_5B07A0
  @ 0x5b07a0` (VB) / `sub_5B07F0` (IB) `memcpy` the model data into the locked
  range. `BaseVertexIndex = offset/stride`. The model's render slot records
  `{VBhandle, baseVertex, stride, vbDecl, IBhandle, baseIndex, indexCount,
  isStatic}` (the `bufferInfo` the draw loop reads). `render_mode` 1 = static, 2 =
  dynamic is the hard split; skinned vs rigid is **not** a separate buffer — it is
  the per-LOD stride/declaration, bound per draw.
- **Draw + state-change minimization** — `CRenderBatchQueue_FlushBatches
  @ 0x5d9f50` iterates 68-byte entries; per entry it reads `bufferInfo` (entry +12)
  and binds `SetStreamSource` (device vtable+400), `SetIndices` (+416),
  `SetVertexDeclaration` (+348) **only when each differs from the previous batch**
  (tracked in `v158/v160/v165/v166`), and skips `Begin`/`BeginPass`/`SetTechnique`
  when the effect equals the previous batch. `DrawIndexedPrimitive` (+328) then
  draws a sub-range of the bound pooled buffer. Technique chosen by `ttype`
  (`(flags>>4)&7` → `materialDef + {600,680,760,840,920,1000}`).
- **The presort feeds the gating** — the effect table is priority-sorted
  (`sub_5ADFA0 @ 0x5adfa0`); `collect_render_objects_for_batch @ 0x5d8f20` builds a
  15-bit sort key (effect-index + LOD + time) per batch and runs **two passes per
  part, opaque then alpha** (`robjDesc[0,1]`/`[2,3]`) into separate opaque /
  transparent / shadow queues, so same-buffer / same-effect batches end up
  consecutive and the binds are elided.
- **Skinning** — per strip: `meshData[47]` = bone count gates a per-draw bone-
  palette upload (`SkinWorldMatrixArray`, effect vtable+144) indexed by the strip's
  `bone_table` (`+24`); frame matrices live in a shared `CDynList64` (`batchCtx+12`).

## Divergence catalog — D-RENDER

Our current `libs/renderer` builder vs the original `.fx`. These are the
fidelity gaps the port closes; IDs are stable.

| ID | Ours (`object_shader_template.cpp`) | Original (`.fx`) | Why / consequence |
| --- | --- | --- | --- |
| D-RENDER-1 | no overall doubling (ad-hoc `*1.5`/`*1.6`) | `Modulate2x` / `mul_x2` everywhere | everything renders dim and flat; the single biggest visual gap |
| D-RENDER-2 | `mix(fill_color, ambient_color, n.y·0.5+0.5)` | bumped: `Ambient+(HemiSky−Ambient)·(N·up)`; gouraud: `lerp(HemiGround,HemiSky,up)` | wrong ambient tint; uses invented `fill`/`ambient` uniforms instead of `HemiGround`/`HemiSky`/`Ambient` |
| D-RENDER-3 | world-distance linear / smoothstep fog | planar view-Z `1−(viewZ−FogStart)·FogRangeRecip` | wrong falloff shape and onset; mismatches terrain/sky fog which already follow the planar model |
| D-RENDER-4 | linear NEAR/FAR point falloff | D3D `1/(c+l·d+q·d²)`, witnessed `(c,l,q) = (1, 0, 15/range²)` → `1/(1 + 15·(d/range)²)` `[orig: sub_5A9180 @ 0x5a926f]` | wrong intensity curve and range; the engine triple is pure-quadratic, not the reimpl's `lin`/`lin²` from a NEAR/FAR pair |
| D-RENDER-5 | `pow(N·H,16)·0.8` added | `(N·H)^≈8–16`, gated by diffuse-alpha gloss | no per-pixel gloss control; wrong magnitude |
| D-RENDER-6 | `max(dot,0)` hard terminator | `clamp(N·L·10+0.5,0,1)` soft wrap | harsher terminator, black back faces |
| D-RENDER-7 | Dot3 family treated as flat diffuse (normal map only if `OSCAP_NORMAL_MAP`) | Dot3 **is** the bump (per-texel `DotProduct3` / `dp3`) | bump lighting dropped for the Dot3 families |
| D-RENDER-8 | scale + rotate + offset UV | `3×2` matrix `MatTexCoord1` | sheared/animated UVs wrong |
| D-RENDER-9 | single fragment, 1 light **sourced from the model's own `.3di` light records, applied only to itself** | a `PointLight*Array` of **≤3-per-batch effect-world glow-light instances** (`unk_2732E28` slots) gathered near the object (`collect_render_objects_for_batch @ 0x5d8f20`) + `CurNumPointLights`, fed via `SetVectorArray` from `sub_5A9180` `[orig: @ 0x5a9180]` | wrong **source** (the engine illuminates objects with nearby dynamic light *effects*, not their carried lights) and wrong count; collapsing to one pass is acceptable only if it follows the ≤N array path from the right source |
| D-RENDER-10 | fresnel-mix `env` approximation for Glass/Env | cube-env reflection (`CAMERASPACEREFLECTIONVECTOR` / `reflect`), `MaterialEmissive=ReflectColor`, additive no-z-write | wrong reflection model; needs an environment cube source |
| D-RENDER-11 | `u_emissive` bypass to base color | `MaterialEmissive = SelfLumColor·ColorSrcGlobalGain`, diffuse 0 | ignores `SelfLumColor` and the global gain |
| D-RENDER-12 | no exposure/iris term anywhere | global iris auto-exposure folded in **twice**: every env colour block is pre-multiplied by `Env_ModulatorBlock` per tick, AND the effect gets `ColorSrcGlobalGain` (handle 68) = `Render_LightScaleRGB` = modulator/64 `[orig: apply_shader_parameters @ 0x58e06a; Render_UnpackModulatorToLightScale @ 0x58db30]` | un-exposed output (too bright in sun, too dim in shade/indoors); the env-tod #17 modulator chain is the prerequisite. Bridge to env-tod-re.md (`Render_LightScaleRGB @ 0x8409f4`) |
| D-RENDER-13 | tag→shader match is case-**sensitive** `==` (`material_descriptor.h:115`) | `HLSLEffect_FindByName @ 0x5ade70` uses `stricmp` (case-**insensitive**) | a case-mismatched `.3di` tag silently falls back to `FF_ST_OP`/unknown instead of resolving its real effect |
| D-RENDER-14 | bumped hemisphere uses `lerp(HemiGround,HemiSky,up)` for **all** families (ground as the floor) | the **bumped** (pixel-shader) path is `AmbientColor + (HemiSky − AmbientColor)·(N·up)` — floor is a *separate* `AmbientColor` param, `HemiGround` unused; only the gouraud/FFP path is `lerp(HemiGround,HemiSky,up)` `[orig: ps11Diffuse c1/c2 in Dot3DiffT.fx]` | reimpl uses the wrong formula + pole for bumped families. **Fix BLOCKED**: `AmbientColor`'s runtime source is unwitnessed — no object-effect function reads the env Sky/Ground/Light blocks (xref-confirmed 2026-06-15), so it is fed via an `EffectPool` shared param (open). The terrain twin's `c0` = `Env_LightBlock` is `DirLightColor`, not `AmbientColor`; do not guess the source |
| D-RENDER-15 | one `ArrayMesh` + one `MeshInstance3D` **per primitive (strip)** for animated models; no merging, no shared buffer, no sort/group (`build_lod_submeshes`) | strips of a part are offset sub-ranges of a **shared pooled buffer**, drawn with change-gated binds and presort grouping (see §Geometry batching) `[orig: allocate_lod_gpu_buffers @ 0x5b2610; FlushBatches @ 0x5d9f50]` | a multi-material character = ~10–15 draws × N entities instead of merged runs. Godot-side fix: merge primitives sharing `(part, shader-key, skinned, opaque/alpha)` into one surface; static props already batch via `MultiMesh` |

## Open questions / follow-ups

- **Object lighting-colour `SetVector`/pool site (the handle 63–66 conflict).**
  `HLSLEffect_LoadFromFile` names handle indices 63–66 `DirLightColor`/
  `HemiGroundColor`/`HemiSkyColor`/`AmbientColor`, yet `setup_entity_lighting`
  reads bbox data at those same indices off the same `+656` table base — a hard
  conflict (verified both ways). The per-frame object `SetVector` of the lighting
  colours was not found in `apply_shader_parameters` / `setup_entity` /
  `CEffectWorld_SetupRenderState` / `FlushBatches`; leading hypothesis is an
  `EffectPool` shared parameter (`EffectPool_Create @ 0x69013f`) set once per
  frame. Resolve before treating either the bbox-handle or the colour-handle
  assignment as final.
- **The `.3di`-light → effect-instance bridge.** Confirmed: objects are lit by
  effect-world glow-light *instances* (`unk_2732E28`), not their own carried
  records. Open: whether a model's authored `.3di` light record *spawns* such a
  glow instance at runtime (the likely link between the two), which would tell us
  whether the reimpl should keep reading `.3di` lights but re-route them through a
  world gather, or drop them entirely.
- **`flt_27219A8` / `flt_27219B8` writers** (the projection view-Z fog scale/
  offset). They read 0 in the static image; finding the runtime writer decides
  whether the effect fog is a real view-Z remap (D-RENDER-3 weight) or identity.
- **Per-draw texture-handle mapping.** `apply_shader_parameters` binds textures by
  the `+656` handles, but which slot → `TexDiffuse1`/`TexDetail`/`TexNormal1` was
  not pinned per-handle; confirm before calling the slot mapping MATCHING.
- **Non-`NORMAL` techniques:** the runtime selector that chooses CLIP (water
  reflection / portal clip), PROJSHAD, DEPTHMASK, GLOW, MATCHTERRAIN per draw —
  i.e. which of the six pass-data blocks at `material+600..+1000` a given draw
  situation uses.
- **Skinned + Env/Glass full math:** the `Sk*` palette path and the
  `_vsBmEnv.fx`/`_psBmEnv.fx` bump-environment pass are witnessed structurally
  but not yet reduced to port-ready equations.

## Proposed IDB changes (not yet applied — shared state)

Per the worktree-only / shared-IDB rule, these are proposals, not applied
renames:

- `sub_5B0080 @ 0x5b0080` → `HLSLEffect_LoadAllEffects` (the init/reset reload
  entry).
- `sub_5ADFA0 @ 0x5adfa0` → `HLSLEffect_SortEffectTableByPriority` (bubble-sort of
  the 1004-byte table descending by `entry+164`; confirmed this grill).
- `sub_58DB80 @ 0x58db80` (`apply_shader_parameters`) →
  `HLSLEffect_BindPerPassMaterialParams` (arg1 is the `ID3DXEffect*`, not a
  device; arg3 `+596`→entry, `+656`→handle table).
- `sub_5D98A0 @ 0x5d98a0` (`setup_entity_lighting_and_shader_constants`) →
  `HLSLEffect_BindPerEntityLightingConstants`.
- `sub_5A9180 @ 0x5a9180` → `HLSLEffect_ExtractGlowLightParams` (reads a 176-byte
  effect-instance slot → `PointLight` coord/colour/atten; atten `(1,0,15/range²)`,
  colour iris-scaled).
- `sub_5B1870 @ 0x5b1870` → `Material_BindEffectAndCopyPassData`.
- `sub_5D8F20 @ 0x5d8f20` → `Render_BuildEffectBatchEntries_Robj` (gathers ≤3 glow
  lights + ≤4 texture handles into the 68-byte batch entry).
- `sub_5B07A0 @ 0x5b07a0` → `Model_UploadVertexRangeToPool`; `sub_5B07F0 @ 0x5b07f0`
  → `Model_UploadIndexRangeToPool` (lock a pool sub-range, `memcpy` model VB/IB,
  unlock).
- `dword_26DCE50` → `g_GpuBufferPools` (the 4-pool static/dynamic VB+IB allocator
  arena read by `allocate_lod_gpu_buffers @ 0x5b2610` /
  `shadow_renderer_allocate_pool_slot @ 0x585b90`).
- `sub_57E440 @ 0x57e440` → `Environment_ApplyFogAndAmbient` (already this name in
  docs/env; the IDB still shows `sub_57E440`).
- `Render_UnpackModulatorToLightScale @ 0x58db30`: the decompiler auto-comment
  calls it `Render_UnpackFogColor` — **wrong**; it unpacks the iris modulator into
  `Render_LightScaleRGB` (÷64), not fog colour. Fix the comment.
- Data fix-ups: `off_7DA564` → `aUvSuffix` (`"#UV"`); `font_name @ 0x7c08c6` →
  `g_macroValueOne` (the `D3DXMACRO` definition `"1"`, not a font); `sub_544D5F @
  0x544d5f` is mistyped as a function — it is the read-only string `"_MT"`
  (`aFfTexMultiTag`).
- Comments worth adding: the `byte_27E56A0` stride/layout (1004 B; name+0,
  caps+160 = `combined_flags`, sort+164, `ID3DXEffect*`+172, handles+656; the
  point/spot handles at indices 233–244), the `"#UV"` registry-name suffix (FF
  mint @ `0x5afc34`), and `byte_27E5748` = `byte_27E56A0 + 0xA8` (the per-effect
  caps field, **not** a separate table).
