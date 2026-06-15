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

> **Status: engine-research landed; reimpl port in flight.** The original system
> is witnessed end to end (load path + lighting math). The current
> `libs/renderer` builder is a pre-witness approximation with invented lighting
> and is **DIVERGENT** on the axes catalogued in D-RENDER below. The faithful
> port (mirror the `.fx` macro structure into Godot shaders) is the follow-up.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Effect load + registry (`HLSLEffect_*`) | **WITNESSED** (engine-research) | 8 cited functions; `D3DXCreateEffect @ 0x690293` (d3dx9 statically linked); 1004-byte effect entries at `byte_27E56A0`; ~87 named parameters resolved by name |
| Shader authoring model (macro über-shader) | **WITNESSED** | `_FFP.fx` macro permutations = the 24 `FF_*` tags; per-technique files = the `VS_*`/`FFP_*` tags; `#include` + `CIncludeManager`; the `#UV` variant = `TEX_UVXFORM` / `EffectAlt_UV` |
| Lighting / fog / attenuation / specular / UV math | **WITNESSED** | summarized from `_BaseInc.fx`, `_psDiff.fx`, `_psPhong.fx`, `_vsFlat.fx`, `_vsDiffT.fx`, `_vsPhongT.fx` (see witness map) |
| `libs/renderer` GLSL builder (`object_shader_template.cpp`) | **DIVERGENT** (invented, pre-witness) | lighting is hand-authored, not ported; D-RENDER-1..11 |
| Runtime per-frame parameter feed | **UNWITNESSED** | how `DirLightColor`/`HemiSkyColor`/`Mat*`/fog reach the effect each frame, and how a `.3di` material's tag selects the effect + binds textures — open follow-up |
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
  registry name gets `", UVGen"` appended. That `#UV` suffix is our `…#UV` tag.
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
  with `BLEND_NONE|ALPHA|ADD|MULT` × `TEX_SINGLE|TEX_MULTIPLE` × `SELFLUM` ×
  `TEX_UVXFORM`. `FF_MT_OP` = `TEX_MULTIPLE` + `BLEND_NONE`; `FF_ST_AB_LUM#UV` =
  `TEX_SINGLE` + `BLEND_ALPHA` + `SELFLUM` + `TEX_UVXFORM`. The file's own
  `EffectTag` is `"FFP_BORING"`; the per-macro `FF_*` registry names are
  generated by `HLSLEffect_InitFixedFunctionShaders @ 0x5af790` (body
  unwitnessed — see open questions). `[orig: FFP_BORING in _FFP.fx]`
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

## Divergence catalog — D-RENDER

Our current `libs/renderer` builder vs the original `.fx`. These are the
fidelity gaps the port closes; IDs are stable.

| ID | Ours (`object_shader_template.cpp`) | Original (`.fx`) | Why / consequence |
| --- | --- | --- | --- |
| D-RENDER-1 | no overall doubling (ad-hoc `*1.5`/`*1.6`) | `Modulate2x` / `mul_x2` everywhere | everything renders dim and flat; the single biggest visual gap |
| D-RENDER-2 | `mix(fill_color, ambient_color, n.y·0.5+0.5)` | bumped: `Ambient+(HemiSky−Ambient)·(N·up)`; gouraud: `lerp(HemiGround,HemiSky,up)` | wrong ambient tint; uses invented `fill`/`ambient` uniforms instead of `HemiGround`/`HemiSky`/`Ambient` |
| D-RENDER-3 | world-distance linear / smoothstep fog | planar view-Z `1−(viewZ−FogStart)·FogRangeRecip` | wrong falloff shape and onset; mismatches terrain/sky fog which already follow the planar model |
| D-RENDER-4 | linear NEAR/FAR point falloff | D3D `1/(c+l·d+q·d²)` | wrong intensity curve and range |
| D-RENDER-5 | `pow(N·H,16)·0.8` added | `(N·H)^≈8–16`, gated by diffuse-alpha gloss | no per-pixel gloss control; wrong magnitude |
| D-RENDER-6 | `max(dot,0)` hard terminator | `clamp(N·L·10+0.5,0,1)` soft wrap | harsher terminator, black back faces |
| D-RENDER-7 | Dot3 family treated as flat diffuse (normal map only if `OSCAP_NORMAL_MAP`) | Dot3 **is** the bump (per-texel `DotProduct3` / `dp3`) | bump lighting dropped for the Dot3 families |
| D-RENDER-8 | scale + rotate + offset UV | `3×2` matrix `MatTexCoord1` | sheared/animated UVs wrong |
| D-RENDER-9 | single fragment, 1 dir + 1 local light | multi-pass additive; **or** the engine's `vsFlatBaseHemi` ≤4-light array loop | collapsing to one pass is acceptable **iff** it follows the array path (≤4 lights, D3D atten, additive-equivalent); today's single light + linear atten does not |
| D-RENDER-10 | fresnel-mix `env` approximation for Glass/Env | cube-env reflection (`CAMERASPACEREFLECTIONVECTOR` / `reflect`), `MaterialEmissive=ReflectColor`, additive no-z-write | wrong reflection model; needs an environment cube source |
| D-RENDER-11 | `u_emissive` bypass to base color | `MaterialEmissive = SelfLumColor·ColorSrcGlobalGain`, diffuse 0 | ignores `SelfLumColor` and the global gain |

## Open questions / follow-ups

- **Runtime parameter feed (highest value next):** where the per-frame values of
  `DirLightColor`, `HemiGroundColor`/`HemiSkyColor`, `AmbientColor`, fog, and the
  `Mat*` transforms come from, and how a `.3di` material's shader tag selects the
  registered effect and binds its textures to `TexDiffuse1`/`TexNormal1`/… Some
  global light scalars are already witnessed in
  [env/env-tod-re.md](../env/env-tod-re.md) (`Render_LightScaleRGB @ 0x8409f4`,
  `EffectWorld_AmbientScaleRGB @ 0x840b24`); the per-object feed is not.
- **`HLSLEffect_InitFixedFunctionShaders @ 0x5af790`:** confirm it is what mints
  the 24 `FF_*` registry names from `_FFP.fx` macro permutations, and recover the
  tag-naming rule.
- **Non-`NORMAL` techniques:** the runtime selector that chooses CLIP (water
  reflection / portal clip), PROJSHAD, DEPTHMASK, GLOW, MATCHTERRAIN per draw.
- **Skinned + Env/Glass full math:** the `Sk*` palette path and the
  `_vsBmEnv.fx`/`_psBmEnv.fx` bump-environment pass are witnessed structurally
  but not yet reduced to port-ready equations.

## Proposed IDB changes (not yet applied — shared state)

Per the worktree-only / shared-IDB rule, these are proposals, not applied
renames:

- `sub_5B0080 @ 0x5b0080` → `HLSLEffect_LoadAllEffects` (the init/reset reload
  entry).
- `sub_5ADFA0 @ 0x5adfa0` → `HLSLEffect_PostLoadFixup` (its tail call; body
  unconfirmed).
- Comments worth adding: the `byte_27E56A0` stride/layout (1004 B; name+0,
  caps+160, sort+164, `ID3DXEffect*`+172, handles+656), and the `, UVGen`
  name-suffix rule at `0x5afe66`.
