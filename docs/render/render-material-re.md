# Object materials / render state — reverse-engineering record

The runtime path from a `.3di` material (shader tag string + per-material flag
byte) to device render state, witnessed in retail `Jointops.exe`
(imagebase `0x400000`, IDB `Jointops.exe.kong.i64`). Implementing code:
`libs/oed/include/oed/{types.h,material_descriptor.h}` (the tag registry),
`libs/renderer` (`material_classify`, `object_shader_template`),
`godot/engine/object/{nova_object_shader_cache,nova_object_data}.cpp`,
`godot/engine/object/nova_object_model.gd`. Landed by maturity REN-2
([maturity-program.md](../maturity-program.md); standing rules
[ADR 0023](../adr/0023-render-visual-parity.md)). The T1 parity instrument
(`tests/renderer/state_vectors_test.cpp` golden) pins every classification in
this record.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Shader-tag registry (46 tags: 24 FF built-ins + shipped file effects + #UV twins) | MATCHING | table arithmetic + flag authorship witnessed at `@ 0x5af790`/`@ 0x5ae690`; `renderer_material_classify` + `renderer_state_vectors` ctests |
| Tag resolution + unknown fallback | MATCHING | case-insensitive lookup `[orig: HLSLEffect_FindByName @ 0x5ade70]`; unknown → effect 0 = FF_ST_OP `[orig: convert_material_definition @ 0x5b0670]`; our unknown-family path composes the same single-texture lit-opaque look |
| Material flag byte → device state (alpha test / invert / two-sided) | MATCHING (after D-RMAT-1/-3 fixes) | `[orig: CRenderBatchQueue_FlushBatches @ 0x5da3a9..0x5da401; CGfxDevice_SetAlphaTestRef @ 0x6770a0]`; `renderer_state_vectors` golden |
| Blend classification per tag (Opaque/AlphaBlend/Additive) | MATCHING | `RSAlphaMode` pass states per shipped `.fx` (`_FFP.fx` `BLEND_NONE/ALPHA/ADD` = FALSE,ONE,ZERO / SRCALPHA,INVSRCALPHA / ONE,ONE; Glass/SkGlass/Tracer = ONE,ONE only); Multiplicative (DESTCOLOR,SRCCOLOR) appears only in non-NORMAL techniques and no registry row claims it |
| Depth policy for blended materials | MATCHING | blended FF variants force ZMODE_NOWRITE across technique slots `[orig: @ 0x5afc92..0x5afcaa]`; applied as `D3DRS_ZWRITEENABLE=0` `[orig: @ 0x5da320]` — mirrored by the composer's `depth_draw_never` for non-opaque, non-alpha-test |
| Per-effect capability/sort flag words (file effects) | unknown | derivation witnessed (`@ 0x5ae690` probe, `@ 0x5af790` authored) but per-file equality vs the OED dump needs a runtime registry dump (D-RMAT-4) |
| Composed lighting math | divergent (tracked) | the uniform surface is witnessed (HemiSky/HemiGround/DirLight/Ambient/ColorSrcGlobalGain `[orig: @ 0x5af453..0x5af49e]`); the composer's gains are prototype values — burns down at REN-5 (D-RMAT-5) |
| Technique-class pass system (6 classes) | witnessed / host-deferred | class selection + slots + fallbacks witnessed; NORMAL-class state ported; CLIP/PROJSHAD/DEPTHMASK/GLOW/MATCHTERRAIN classes ride REN-3/REN-4 (D-RMAT-6) |

## Witness map

**Boot and registry.** `HLSLEffect_InitAndLoadAll @ 0x5b0080` (renamed from
`sub_5B0080`): queries device caps, sets `HLSLEffect_PassClassGates @ 0x27e569c`
(low byte forced 1 = vertex shaders assumed; high byte = adapter caps bit for
pixel-shader techniques), registers the fixed-function set, then loads `.fx`
from an override directory (`FindFirstFileA` — loose files only,
`%s\*.fx` @ 0x7da6e0, `_`-prefixed skipped) or from every mounted PFF
(`HLSLEffect_LoadAllFromPFFArchive @ 0x5afed0`, walking the 36-byte entry
directory; archives via `FS_GetSecondaryArchiveByIndex @ 0x75ad60`, renamed
from `AudioChannel_GetVolumeByIndex` — it indexes `g_FS_SecondaryArchives`).
Registry: 1004-byte records in `HLSLEffect_Registry @ 0x27e56a0`, count
`HLSLEffect_RegistryCount @ 0x28e06a8`.

**Fixed-function built-ins.** `HLSLEffect_InitFixedFunctionShaders @ 0x5af790`
compiles `_FFP.fx` 24 times: `{_ST,_MT} × {"",_LUM} × {_OP,_AB,_AD} × {base,#UV}`
with defines `TEX_SINGLE/TEX_MULTIPLE`, `SELFLUM`, `BLEND_NONE/ALPHA/ADD`,
`TEX_UVXFORM`; tags sprintf'd as `FF%s%s%s`. Flag words AUTHORED:
`_ST` 0x4, `_MT` 0xC, `_AB` 0x1002, `_AD` 0x1000, `_LUM` |0x10000001,
`#UV` |0x10000. LUM effects copy their NORMAL pass block into the GLOW slot
(`@ 0x5afc7f`); blended variants force ZMODE_NOWRITE on every technique slot's
first pass (`@ 0x5afc92..0x5afcaa`).

**File effects.** `HLSLEffect_LoadFromFile @ 0x5ae690`: VFS read + SCR layer
(`ScriptFile_LoadAndDecrypt @ 0x5ae060`, key 0xA55B1EED — see
[scr](../../libs/scr/include/scr/scr.h)); `D3DXCreateEffect` with
`TRILINEAR/ANISO` (from `HLSLEffect_TextureFilterMode @ 0x27e5698`) +
`FFPTRANSPOSE` (+ `TEX_UVXFORM` for the UVGen twin); reads the `EffectInfo`
annotations (`EffectTag`, `EffectName`, `EffectSpecial`, `EffectAlt_UV`,
`MinToolVersion` — default 256, >256 rejects); duplicate names reject; the
UVGen twin registers under `tag + "#UV"` (`@ 0x5aea03`, display name
`", UVGen"`). Capability flags PROBED per technique via
`IsParameterUsed`: TexDiffuse1 0x4, TexDiffuse2 0x8, TexNormal1 0x10,
TexNormal2 0x20, TexHorizon 0x40, TexOcclusion 0x80, TexSpecularCtrl 0x100,
DisplaceAmount 0x200, `blending` annotation 0x1000, ReflectColor 0x2000,
SkinWorldMatrixArray 0x4000, TANGENT input semantic 0x8000
(`D3DXGetShaderInputSemantics`, usage 6), EffectAlt_UV|uvXform 0x10000,
TexCubeRotSpecular 0x10000000. A sort word (`entry+164`) adds
blending+1/diffuse2+4/reflect+8/normal1+16/normal2+32/tangent+64/skin+128/
no-displacement+0x8000/EffectSpecial+0x10000. 87 named parameter handles
resolve (`entry+656`), unused ones zeroed per technique.

**Pass metadata.** `HLSLEffect_ParsePassData @ 0x5ae120` fills 0x50-byte
blocks: `+8` = `ttype` (TECHNIQUE_NORMAL 0 / PROJSHAD 1 / DEPTHMASK 2 / CLIP 3
/ GLOW 4 / MATCHTERRAIN 5 — `_BaseInc.fx` defines), `+12` = tech flags
(bit0 `usevs`, bit1 `useps`, bit2 tangent-from-bytecode, bit3 `useffplights`),
`+16`+8i = per-pass flags|fogmode: passrules<<1 (0x3E — the per-light pass
rules), zmode<<6 (0xC0), amode<<8 (0x300). Blocks land in the registry at
+176/+256/+336/+416/+496/+576 (NORMAL/DEPTHMASK/PROJSHAD/CLIP/GLOW/
MATCHTERRAIN); a missing CLIP block falls back to NORMAL's (`@ 0x5aebc8`).

**3DI material → effect.** `convert_material_definition @ 0x5b03c0` copies
the runtime material record, remaps the channel blend/alpha enum bytes,
resolves the tag (`HLSLEffect_FindByName @ 0x5ade70`, `stricmp`), clamps
missing tags to index 0 (= FF_ST_OP, first registered), copies the flag byte
bits 0/1/2 (alpha-test/invert/two-sided) and the alpha ref byte;
`resolve_effect_subobjects_and_shader @ 0x5b1870` caches the registry entry
pointer + the six pass blocks into the material def (+600..+1000).

**Draw-time state.** `CRenderBatchQueue_FlushBatches @ 0x5d9f50`
(materials-side; the sort/flush semantics ride REN-3): pass class from batch
flag bits 4-6 selects the block; `usevs` techniques switch to the alternate
vertex-stream binding (+28); `D3DRS_CULLMODE(22)` = NONE when flag byte bit
0x4, else CCW (CW when the mirror flag `renderer+840` is set — reflection
passes flip winding); `D3DRS_ZWRITEENABLE(14)` = !(pass zmode NOWRITE);
`D3DRS_ZFUNC(23)` = ALWAYS(8) under zmode NOREAD else LESSEQUAL(4);
`D3DRS_ALPHATESTENABLE(15)`: amode CLIP → on with fixed ref 128, amode
DEPTHTEST → on with ref 0, else the material flag bit 0x1 with the material
ref byte, negated by bit 0x2. `CGfxDevice_SetAlphaTestRef @ 0x6770a0`
(renamed from `CGfxDevice_SetBrightness`): ref ≥ 0 → `ALPHAFUNC =
D3DCMP_GREATER(5)`; ref < 0 → `ALPHAFUNC = D3DCMP_LESSEQUAL(4)`, ref = |ref|.
Blend states live in the `.fx` pass blocks (`RSAlphaMode` macro =
AlphaBlendEnable/SrcBlend/DestBlend, D3DX-applied);
`CD3DDevice_SetFogAndBlendMode @ 0x677740` is fog-side only: fogmode bits 0-1
pick the fog COLOR (scene / gray 0x7F7F7F7F / black / white — black fades
additive surfaces out, white multiplicative, gray-0.5 mod2x), bits 2-3 pick
primary/secondary/off fog parameter sets. `apply_shader_parameters @ 0x58db80`
(sole caller: FlushBatches) binds constants: flipbook frame
(`GetTickCount()/rate % count` or the controlled-anim table `@ 0x83fce8`),
RgbGen/AlphaGen evaluated channels (`AlphaGenValue`), UV-scroll matrix
(`compute_uv_transform_matrix @ 0x5b1990`, time-driven), world/view matrix
family, `FogStart`/`FogRangeRecip`, `MatRotSpecular` from the live light
direction, and `ColorSrcGlobalGain` ← `Render_LightScaleR @ 0x8409f4` (the
env #17 modulator triple's shader-path consumer — REN-5).

**Ground truth corpus.** The shipped shader set: 44 `.fx` in JO:CA
`localres.pff` (SCR\x01-wrapped, key 0xA55B1EED), 20 `_`-prefixed includes +
24 effect files → 19 tags (+3 `EffectAlt_UV` twins: VS_DOT3DIFF, VS_PHONGT,
VS_SKBASIC). 24 FF + 18 table file-tags + 3 twins = OED's 45; the runtime
registry additionally carries VS_TRACER (Tracer.fx: `EffectSpecial=true`,
TECHNIQUE_NORMAL, `usevs`/no ps, TexDiffuse1, `RSAlphaMode(TRUE, ONE, ONE)`,
ZMODE_NOWRITE, unlit). Never committed — retail data; re-derive via
`libs/scr` + `libs/pff` from a retail install.

## Divergence catalog

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RMAT-1 | Alpha test kept `a >= t` and INVERTED THE VALUE (`1-a >= t`); threshold fudged `maxf(0.001, byte/255)` | keep `a > ref`; invert flips the COMPARE to `a <= ref` (`[orig: @ 0x5da3a9..0x5da401; @ 0x6770a0]`) | FIXED (this slice: composer + model threshold; T1 re-dump cited) |
| D-RMAT-2 | VS_TRACER unknown (absent from the OED-derived table); soft-edge `vsTracer` vertex displacement unported | runtime registry carries VS_TRACER (Tracer.fx, unlit additive diffuse) | row FIXED; the soft-edge look WITNESSED-READY-DEFERRED (REN-4) |
| D-RMAT-3 | Tag lookup case-SENSITIVE | `stricmp` (`[orig: HLSLEffect_FindByName @ 0x5ade70]`) | FIXED (case-insensitive exact-tag) |
| D-RMAT-4 | File-effect capability/sort words carried verbatim from the OED dump | derived at load by the technique-usage probe (`@ 0x5ae690`) | NEEDS-RE — dump the live registry (or replicate the D3DX usage probe) and diff per tag; note the 0x10000000 dialect (LUM on FF rows vs TexCubeRotSpecular on probed rows) |
| D-RMAT-5 | Composer lighting gains are prototype values (hemi fill + ×1.5/×1.6/spec 0.8) | witnessed uniform surface `HemiGroundColor/HemiSkyColor/DirLightVector/DirLightColor/AmbientColor/ColorSrcGlobalGain` with engine-fed values | OPEN — burns down at REN-5 (the lighting-chain grill feeds the real values) |
| D-RMAT-6 | Single-pass host materials; no CLIP/PROJSHAD/DEPTHMASK/GLOW/MATCHTERRAIN technique classes | six pass classes selected per batch entry (`@ 0x5d9ff3`), CLIP falls back to NORMAL, LUM populates GLOW | WITNESSED-READY-DEFERRED — the pass structure rides REN-3; GLOW/MATCHTERRAIN ride REN-4 |

## IDB changes made during the session

| Address | Old | New | Basis |
|---|---|---|---|
| 0x27e56a0 | byte_27E56A0 | HLSLEffect_Registry | 1004-byte records, memset 0x3EC, indexed ×1004 |
| 0x28e06a8 | dword_28E06A8 | HLSLEffect_RegistryCount | post-increment allocator |
| 0x28e06a0 | dword_28E06A0 | HLSLEffect_hTestRelayParam | TestRelay handle cache |
| 0x27e569c | word_27E569C | HLSLEffect_PassClassGates | gates passData[12] bits 0/1; written @ 0x5b00e5 |
| 0x27e5698 | dword_27E5698 | HLSLEffect_TextureFilterMode | selects TRILINEAR/ANISO macro |
| 0x5b0080 | sub_5B0080 | HLSLEffect_InitAndLoadAll | caps + FFP init + dir/PFF loads |
| 0x75ad60 | AudioChannel_GetVolumeByIndex | FS_GetSecondaryArchiveByIndex | indexes g_FS_SecondaryArchives; consumed as archive handle |
| 0x6770a0 | CGfxDevice_SetBrightness | CGfxDevice_SetAlphaTestRef | sets ALPHAFUNC/ALPHAREF; sign encodes invert |

## Open questions

- The per-material channel enum remaps in `convert_material_definition`
  (`src[16]`: 1,2,3..7 → 1,2,8..12; `src[17]` identity over {0..18}) against
  `libs/renderer/material_eval` — verify at the REN-4 channel/eval grill.
- The UV-scroll animation path (`compute_uv_transform_matrix`, time-driven
  `MatTexCoord1`) vs our static `u_uv_*` uniforms — which 3DI channel fields
  drive it, and does PANM cover it host-side?
- `AlphaTestFlag` (effect param 250) — pushed where? (Shader-path alpha test;
  the FFP path uses the render states above.) Check at REN-4.
- The secondary fog parameter set (`@ 0x3262230..44`, fogmode bits 2-3 = 4) —
  underwater? Witness at REN-4's fog table decode.
