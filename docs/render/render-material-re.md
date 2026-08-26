# Object materials / render state — reverse-engineering record

The runtime path from a `.3di` material (shader tag string + per-material flag
byte) to device render state, witnessed in retail `Jointops.exe`
(imagebase `0x400000`, IDB `Jointops.exe.kong.i64`). Implementing code:
`engine/formats/oed/include/oed/{types.h,material_descriptor.h}` (the tag registry),
`engine/runtime/renderer` (`material_classify`, `material_eval`, `uv_anim`,
`object_shader_template` (typed pipeline descriptor; legacy filename)),
`godot/src/object/{nova_object_shader_cache,nova_object_data_materials,nova_object_data_runtime_eval}.cpp`,
`godot/src/object/nova_object_model.cpp`. Landed by maturity REN-2
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
| Depth policy for blended materials | MATCHING | blended FF variants force ZMODE_NOWRITE across technique slots `[orig: @ 0x5afc92..0x5afcaa]`; applied as `D3DRS_ZWRITEENABLE=0` `[orig: @ 0x5da320]` — mirrored by the typed descriptor and checked-in `depth_draw_never` policies for non-opaque, non-alpha-test techniques |
| Per-effect capability/sort flag words (file effects) | MATCHING (after D-RMAT-4 fixes) | the probe REPLICATED over the shipped localres text (REN-4): booleans are unions over ALL techniques `[orig: technique loop @ 0x5ae690]`; 14/19 tags matched the OED dump, 5 drift rows corrected on the renderer descriptor table (catalog below); `renderer_material_classify` pins the corrected words |
| Object lighting math | MATCHING (after D-RMAT-5 fix, REN-5) | checked-in technique resources implement the witnessed FF model — `tex × min(mix(HemiGround, HemiSky, N.y·0.5+0.5) + DirLightColor·max(0,N·L), 1) × 2`, SELFLUM = `tex × SelfLumColor × min(ColorSrcGlobalGain,1) × 2` — on the witnessed uniform surface (slots pinned: 225 CameraPos, 226 DirLightVector, 227 DirLightColor, 228 HemiGroundColor, 229 HemiSkyColor, 230 AmbientColor `[orig: handle stores @ 0x5af3fe..0x5af485]`); values engine-fed from the env blocks ([render-lighting-re.md](render-lighting-re.md)); `renderer_state_vectors` pins the typed descriptors and the transitive-source golden pins all 132 object shader entry points (128 generated + four postmultiply auxiliaries) |
| Technique-class pass system (6 classes) | MATCHING for the locked highest-quality path | all 57 typed declarations and 138 total pass declarations are decoded and pinned in `retail_effect_inventory.json`; NORMAL maps to 24 runtime techniques, CLIP to the live reflection plane/fallback rules, PROJSHAD to the twelve slot captures plus terrain-page silhouettes, MATCHTERRAIN to the skinned stance/page fold, and GLOW to LUM-copy plus Glass's rotated specular in the isolated Q3 camera. The exact PROJSHAD state audit preserves `_FFP` material blending, forces all 15 shader declarations opaque, and admits no fallback for tracer/flag/glass; the portable terrain raster also honors CCW culling, z writes, and skinned identical-matrix collapse. DEPTHMASK is correctly absent because its sole spot-projector producer is caller-less. `auxiliary_technique_validation.json` pins every disposition and D3D12 raster mode; `FrameFx` reproduces the Q3 target and FrameFX draw sequence directly |
| UV animation (MatTexCoord1) | MATCHING deterministic math, live full-matrix bridge; stochastic lifetime partial | `renderer::uv_anim` structurally ports `[orig: compute_uv_transform_matrix @ 0x5b1990; wave_lookup @ 0x5de6b0]`; `ObjectData` now carries the complete row-vector 2×3 result into two shader `vec3` uniforms, preserving controlled set and shear as well as scroll/scale/rotation. Table/dispatch math is pinned by `renderer_state_vectors` section 4 and `renderer_material_eval`; retail's process-wide CRT RNG lifetime and cross-model submit/flush order remain D-3DI-2 |
| Controlled flipbook | MATCHING for the witnessed fractional and discrete-selector branches | `[orig: apply_shader_parameters @ 0x58db80]` reads a signed CTRL value and keeps 32-bit `IMUL`'s low product before `SAR 16` when the adjacent state is zero; the retail image statically seeds `TEX_TEAM` and `TEX_CAMO1/2/3` state to one, selecting `value % frame_count`; `renderer_material_eval` pins exact generic `0x10000`/negative/wrap cases plus the RevX02 IndoArms selector `0/1/3 -> 0/1/1` |
| RGB/alpha generators and point-light color | MATCHING deterministic/controlled math, live; stochastic lifetime partial | consumer-specific branches are preserved: RGB/light 113/114 `[orig: RgbGen_EvaluateColor @ 0x5b23d0]`, alpha 113 `[orig: AlphaGen_EvaluateValue @ 0x5b2320]`, and waveform fallback otherwise. The signed/wrapping evaluator and live point-light CTRL feed are pinned by `renderer_material_eval`; noise samples retain D-3DI-2's process-wide RNG/order gap |
| Tracer soft edge (VS_TRACER look) | MATCHING (after D-RMAT-2 fix) | `OSCAP_VIEW_FADE` composes `color x \|dot(eye, normal)\|^2` `[orig: vsTracer in Tracer.fx]`; `renderer_material_classify` + the vectors golden pin it |
| Color pipeline (gamma space end to end) | MATCHING (after D-RMAT-7/-8/-9 fixes) | no-sRGB sampler/render-state/effect-state sweeps + identity display ramp (§Color pipeline witness); raw sampling and scene math/blends remain in gamma-domain numeric values until one terminal display decode. The D3D12 calibrate probe proves byte identity 256/256 plus exact SRCALPHA/INVSRCALPHA and ONE/ONE results; the shared checked-in fog math follows the re-witnessed table `[orig: @ 0x58a950 → @ 0x677960]` |

**2026-08-22 highest-quality technique audit.** The former 104-wrapper
prototype matrix is superseded by 128 generated object wrappers and 24 exact
NORMAL-pass techniques. `godot/shaders/object/retail_effect_inventory.json`
pins all 44 decoded effect sources, 59 technique declarations, and 138 pass
declarations; it partitions all 30 NORMAL declarations into 19 selected
highest-quality effects, ten lower-quality fallbacks, and the unregistered
`leaves.FX` effect. `godot/shaders/object/technique_validation.json`
binds each selector to decoded SCR `.fx` symbols and explicitly records RGB,
alpha/material-data, normal-space, light-response, blend/depth, and fog
semantics. Important corrections include: blended cutouts stay no-depth-write;
Flag has no SELFLUM or point-light pass; BumpMirr and BmTxMirr are distinct;
SkBasic does not inherit `_FFP`'s AlphaGen/SELFLUM behavior; fixed and skinned
DOT3 T2 effects have different hemisphere/self-shadow/point topology; fixed
and skinned Glass have distinct source-gain behavior; `_psPhong.fx` is
pow-8 with Diffuse1.a brightness; mirror point specular is pow-16; and the
skinned object-space `_psPhong2.fx` path uses Diffuse1.a to select the generated
PhongMap's pow-4/pow-64 channels. `RgbGen` modulates only `_FFP` SELFLUM,
`AlphaGenValue` modulates only `_FFP`, and coverage is selected per technique
from diffuse alpha, normal-map alpha, vertex diffuse alpha, ReflectColor alpha,
or zero. The PhongMap contents are proved at
`Render_CreateSystemTextures @ 0x58aca0`; OpenNova generates the same 256×256
RGBA8 bytes and samples them with the witnessed linear-clamp state. The
transitive-source golden now pins all 132 object shader entry points: 128
generated policy wrappers plus the four EnvironmentMirrorTextured P3
postmultiply auxiliaries. GLOW is selected inside those production wrappers;
there is no duplicate proxy shader family.

## Witness map

**Boot and registry.** `HLSLEffect_InitAndLoadAll @ 0x5b0080` (renamed from
`HLSLEffect_InitAndLoadAll`): queries device caps, sets `HLSLEffect_PassClassGates @ 0x27e569c`
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
[scr](../../engine/formats/scr/scr.h)); `D3DXCreateEffect` with
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
(`GetTickCount()/rate % count` or the controlled-animation slot), evaluated
RgbGen/AlphaGen channels (`AlphaGenValue`), the complete UV matrix
(`compute_uv_transform_matrix @ 0x5b1990`, time-driven), world/view matrix
family, `FogStart`/`FogRangeRecip`, `MatRotSpecular` from the live light
direction, and `ColorSrcGlobalGain` ← `Render_LightScaleR @ 0x8409f4` (the
env #17 modulator triple's shader-path consumer — REN-5). A control slot is
8 bytes: the even dword at `0x83FCE8 + 8·ordinal` is a **signed `int32`**
value and the odd state dword at `0x83FCEC + 8·ordinal` selects
fractional-frame versus modulo-frame interpretation. There is no blanket
`uint16` clamp: `0x10000` is the exact 16.16 endpoint and negative values are
preserved for extrapolation. No runtime writer to the odd dword was found, and
`[orig: CtrlRegAnimSlot_UpdateAll @ 0x401bf0]` writes only the even value, but
the retail image does not initialize every state to zero: ordinals 0..91 are
zero while `TEX_TEAM` and `TEX_CAMO1/2/3` (92..95, state dwords
`0x83FFCC/0x83FFD4/0x83FFDC/0x83FFE4`) are statically one. Those four texture
selectors therefore take the signed modulo branch at
`apply_shader_parameters @ 0x58DC36..0x58DC42`; ordinary controls take the
fractional branch. The 2026-08-17 PR-503 T3 arm comparison exposed the old
all-zero assumption: RevX02 `IndoArms.3di` has two diffuse frames controlled
by `TEX_CAMO1`, so raw avatar selector 1 must choose frame 1, not frame 0.

The consumer fields do not index a private per-model value array. The on-disk
CTRL list is model-local names; `[orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640; ordinal store
@ 0x5B46E6]` resolves those names against the 96-entry global catalog, and
`ThreediGp_LoadFromFile` patches the material/texture-animation references to
the resolved global ordinals at `0x5B5C80..0x5B5DA2`. The resolver is
case-insensitive and returns zero for both `LOD_FRAC` and a miss
`[orig: CtrlName_ToOrdinal @ 0x57B290]`; an unknown authored model name
therefore aliases global ordinal 0. OpenNova keeps ordinary lookup
unambiguous but reproduces this loader-only alias. The full catalog and the
PANM/light remap sites are recorded in
[3di-gp-format-re.md](../threedi/3di-gp-format-re.md).

For the integer interpolation consumers, retail uses a two-operand 32-bit
`IMUL`, discards the high product, then performs an arithmetic right shift.
The port deliberately reproduces that wrapping low-product arithmetic rather
than widening to 64 bits `[orig: AlphaGen_EvaluateValue @ 0x5B234C;
RgbGen_EvaluateColor @ 0x5B24AC]`. Controlled flipbook uses the same
low-dword/`SAR 16` rule for state-zero controls and signed modulo for the
statically state-one texture-selector controls. Controlled UV instead converts the signed CTRL dword
to a floating fraction; its negative and `0x10000` inputs are likewise not
clamped.

**The GfxShader pass-flag word (anchored at the 2026-07-07 water fidelity
grill).** The `GfxShader_Create*TexDesc` material family applies state
through `CGfxShader_ApplyPass @ 0x683190` (checked wrapper
`GfxShader_ApplyPassChecked @ 0x677020`): `combined = shaderFlags |
passFlags` decodes into the device state block committed by
`CGfxDevice_ApplyRenderStates @ 0x67e230` — **0x10000** =
`D3DRS_SPECULARENABLE(29)`, **0x20000** = `FOGENABLE(28)`, **0x40000** =
`ALPHATESTENABLE(15)`, **0x100000** = z-write OFF (inverted →
`ZWRITEENABLE(14)` `@ 0x683232`), **0x200000** = z-test ALWAYS (inverted →
`ZFUNC(23)` = current-else-ALWAYS `@ 0x683249`), **0x400000** = `CULLMODE
NONE` (else CCW, CW under the mirror flag `@ 0x6832a8..0x6832be`),
**0x1000000/0x2000000** = the stage-clamp pair. `CGfxDevice_SetAlphaTestRef
@ 0x6770a0` latches only ALPHAFUNC (GREATER for ref ≥ 0, LESSEQUAL
negated) + ALPHAREF — the ENABLE rides bit 0x40000 alone, so a
SetAlphaTestRef call without that bit is an inert device latch (the water
surface's `0x20` call is the canonical example — env-tod-re.md #34
re-grade). The technique blend block applies only
ALPHABLENDENABLE/SRCBLEND/DESTBLEND (`GfxBlend_ApplyToDevice @ 0x6817d0`).
Scene projection depth range: `Render_SetProjectionDepthRange @ 0x58abe0`
(ex `sub_58ABE0`) — near pinned 0.2, far = arg (missions pass 0x400 = 1024
`[orig: Game_StartMission @ 0x524721]`), viewport-depth slot 0.99996948 =
1 − 2⁻¹⁵ (`@ 0x58ac32`).

**The mode word (anchored at the 2026-08-21 impact-scar grill).** The
`GfxShader_Create1TexModeId(tex, modeWord) @ 0x679030` family builds its
0xF4 state block from ONE word (`CGfxShader_SetRenderStateByModeId @ 0x6835c0`
→ `RenderState_CacheFindOrAddByModeId @ 0x6832f0` →
`find_or_create_render_state_permutation(word & 0x3FFF | 0x80000) @ 0x681d00`,
the first `ValidateDevice`-passing substate wins). The word's upper bits ARE
the pass-flag word above (the shader stores the whole word at +60, so
`combined = word | passFlags`); the low 14 bits are three fields:
- bits 0-3, the framebuffer blend (`decode_blend_mode_to_d3d_states
  @ 0x680f00`): 0 = blend OFF (ONE/ZERO); 1 = SRCALPHA/INVSRCALPHA;
  2 = ONE/ONE (later variants SRCALPHA/ONE, SRCALPHA/INVSRCALPHA);
  3 = ZERO/INVSRCALPHA; 4 = DESTCOLOR/ONE; 5 = SRCALPHA/ZERO;
  6 = ZERO/SRCCOLOR; 7 = ZERO/ONE; 8 = DESTCOLOR/SRCCOLOR; 9 = SRCCOLOR/ZERO;
  10 = SRCCOLOR/ONE; 11 = ONE/SRCALPHA; 12 = ONE/INVSRCALPHA;
  13 = SRCALPHA/SRCCOLOR.
- bits 4-7, the stage-0 alpha op, first substate (`decode_mode_alpha_stage
  @ 0x680b00`): 0x00/0x10 = SELECTARG2(TFACTOR); 0x20 = SELECTARG2(DIFFUSE);
  0x30 = SELECTARG1(TEXTURE); 0x40 = MODULATE(TEXTURE, TFACTOR);
  0x50 = MODULATE(TEXTURE, DIFFUSE); 0x60 = SELECTARG1(TEXTURE) with the
  CURRENT/stage-1 variants behind it; 0xD0 = ADD; 0xE0 = SUBTRACT;
  0xF0 = MODULATE(CURRENT, TEXTURE).
- bits 8-13, the stage-0 colour op (`decode_mode_color_stage @ 0x681080`;
  "MOD(2X)" = MODULATE2X because `GfxDevice_Modulate2XEnabled` (ex
  `dword_32656AC`, renamed 2026-08-21) is set to 1 unconditionally by
  `CGfxDevice_CreateDevice @ 0x67eb5f` — `CGfxTextOverlay_Draw @ 0x67715d`
  halves clear colours only when it is 0): 0x100 = SELECTARG1(TFACTOR);
  0x200 = SELECTARG2(DIFFUSE); 0x300 = MOD(2X)(DIFFUSE, TFACTOR);
  0x400 = SELECTARG1(TEXTURE); 0x500 = MOD(2X)(TEXTURE, TFACTOR);
  0x600 = MOD(2X)(TEXTURE, DIFFUSE); 0x700 = MODULATE(TEXTURE|ALPHAREPLICATE,
  DIFFUSE); 0x800 = DOT3(TEXTURE, TFACTOR); 0x900 = DOT3(TEXTURE, DIFFUSE);
  0xA00 = ADD; 0xD00 = ADD(DIFFUSE, TEXTURE); 0xE00 = SUBTRACT(CURRENT,
  TEXTURE); 0xF00 = MOD(2X)(CURRENT, TEXTURE); 0x1000 = MODULATE(CURRENT,
  CURRENT); 0x2000 = MOD(2X)(TEXTURE, TEXTURE|ALPHAREPLICATE); 0x80000001 =
  MODULATE(TEXTURE, TEXTURE|ALPHAREPLICATE); family 0 writes nothing.
Worked words: the impact-scar strips' `0x120651` (scorch: blend 1, alpha
0x50, colour 0x600, FOGENABLE, z-write off — no alpha test, CCW cull) and
`0x460651` (bhole1: plus ALPHATESTENABLE and CULLMODE NONE, z-write on)
([world-wac-ai-re.md §24.9](../world/world-wac-ai-re.md)); the terrain's
`0x20200` (blend off, TFACTOR alpha, flat DIFFUSE colour, fog). The port's
decoder is `renderer::decode_scar_strip_mode`
(`engine/runtime/renderer/scar_draw_list.h`).

**Ground truth corpus.** The shipped shader set: 44 `.fx` in JO:CA
`localres.pff` (SCR\x01-wrapped, key 0xA55B1EED), 20 `_`-prefixed includes +
24 effect files → 19 tags (+3 `EffectAlt_UV` twins: VS_DOT3DIFF, VS_PHONGT,
VS_SKBASIC). 24 FF + 18 table file-tags + 3 twins = OED's 45; the runtime
registry additionally carries VS_TRACER. Tracer.fx exposes only TECHNIQUE_NORMAL
(`EffectSpecial=true`, `usevs`/no ps, TexDiffuse1, `RSAlphaMode(TRUE, ONE, ONE)`,
ZMODE_NOWRITE, unlit, `vsTracer` Diff = `|dot(eye, normal)|²` — the soft-edge
facing falloff). Never committed — retail data; re-derive via `engine/formats/scr` +
`engine/formats/pff` from a retail install.

**The capability probe (REN-4, D-RMAT-4 closure).** The flag word
(entry+160) booleans are UNIONS OVER ALL TECHNIQUES — the loader iterates
every technique calling `IsParameterUsed` per probe parameter, reads the
`blending` annotation per technique, and scans every pass's VS bytecode for
the TANGENT input semantic (`D3DXGetShaderInputSemantics`, usage 6)
`[orig: technique loop @ 0x5ae690]`. The 87 named handles live at entry+656
(dword slots 164..250): 165/166 TexDiffuse1/2, 172-176
TexNormal1/2/Horizon/Occlusion/SpecularCtrl, 196-206 the shared texture set
(CubeNormalize, CubeEnvironment, CubeRotSpecular, PhongMap, Clip1D, Spot2D,
DepthGradWrite, DepthGradTest, AngleMap, CurProj, Cookie), 207-217 matrices
(WVP, World, WorldView, ViewProj, WorldInvTrans, CamToWorldRot, RotSpecular,
MatTexClipPlane, VecTexClipPlane, **MatTexCoord1 = 216**, VecDepthMaskPlane),
218/219 FogStart/FogRangeRecip, 221 ReflectColor, 222 SelfLumColor,
**223 AlphaGenValue**, 225-240 camera/lighting/point-light family (pinned at
REN-5: **225 CameraPos, 226 DirLightVector, 227 DirLightColor,
228 HemiGroundColor, 229 HemiSkyColor, 230 AmbientColor, 232
ColorSrcGlobalGain** `[orig: handle stores @ 0x5af3fe..0x5af485; gain bind
@ 0x58e050]`), 241-244
shadow/spot projection, 245 ReflectBumpDepth, 246/247 the skin arrays,
248 DisplaceAmount, **249 FloatTicks**, **250 AlphaTestFlag**; the tail loop
zeroes handles unused by every technique unless D3DX-shared. Replicating the
probe over the shipped text (a static `IsParameterUsed`: identifier
reachability through the technique's pass states + referenced compiled
functions): **14/19 file tags match the OED dump exactly; 5 rows drift** —
the D-RMAT-4 table below. The `0x10000000` dialect is RESOLVED: both the
LUM authorship and the TexCubeRotSpecular probe mean "renders a Q3
glow/bloom copy" (the Q3 gate `[orig: @ 0x5d93b5]`), so at runtime FFP_GLASS
carries it (its GLOW technique samples the specular cube); the self-lum LOOK
is the EMISSIVE bit (0x1). `MATERIAL_FLAG_LUMINANCE` was renamed
`MATERIAL_FLAG_GLOW` accordingly (oed/types.h keeps the byte-faithful OED
dump values; the runtime-corrected words live on
`oed::kMaterialDescriptorTable`).

**The FF technique tables (from `_FFP.fx` text, REN-4).** TECHNIQUE_NORMAL
`TBoringFFP` P0: `TSSColor(0, Modulate2x, Texture, Diffuse)`,
`TSSAlpha(0, Modulate, Texture, Diffuse)` (+ the same on stage 1 vs Current
for `_MT`), FFP `Lighting = TRUE` with MaterialDiffuse `(1,1,1, AlphaGenValue)`
+ MaterialEmissive `AmbientColor` — SELFLUM swaps to Emissive
`SelfLumColor x ColorSrcGlobalGain` with black diffuse/ambient. Blend per
variant: `_OP` FALSE/ONE/ZERO + FOGMODE_NORMAL + usevs (the spotlight
ladder); `_AB` SRCALPHA/INVSRCALPHA; `_AD` ONE/ONE + FOGMODE_NORMALADD
(black fog fades additive out). Opaque-only spotlight decomposition
(passrules): P1a hemi base (`vscFlatBaseHemiArray[CurNumPointLights]`,
color = SelectArg2(Diffuse) — textureless), P2a ONCE_PER_SPOTLIGHT beam
(TexDepthGradTest + projected TexCurProj, ONE/ONE, AMODE_DEPTHTEST,
alpha = Subtract(Texture, Current)), P3a texture post-multiply
(DESTCOLOR/SRCCOLOR — mod2x onto the lit base). TECHNIQUE_CLIP: stage 1 =
`TexClip1D` sampled by CAMERASPACEPOSITION through `MatTexClipPlane`
(COUNT2), `TSSAlpha(1, Modulate, Texture, Current)` — the water-plane clip
multiplies alpha and AMODE_CLIP's ref-128 test cuts it. TECHNIQUE_PROJSHAD:
black lighting (all material colors 0), color = SelectArg2(Diffuse) — the
shadow silhouette. TECHNIQUE_DEPTHMASK (`_tDepth.fx`, opaque only):
`vscDepth` + `TexDepthGradWrite` clamped, color/alpha = SelectArg1(Texture),
fog off — writes the depth gradient into dest alpha for later
AMODE_DEPTHTEST passes (the beam/soft-depth mechanism). Glass.fx
TECHNIQUE_GLOW swaps `TexCubeEnvironment x MatCamToWorldRot` for
`TexCubeRotSpecular x MatRotSpecular` (the sun-aligned specular cube) —
the Q3 copy renders the sun glint for bloom.

**NORMAL channel ownership (2026-08-22 correction).** `AlphaGenValue` is
consumed only by `_FFP.fx` through MaterialDiffuse.a; shader-based effects read
their texture/vertex alpha channels directly. Likewise `SelfLumColor`/RgbGen
modulates only `_FFP` SELFLUM RGB. Consequently standard Phong reads raw
Diffuse1.a as specular brightness, `_psPhong2.fx` reads raw Diffuse1.a as both
brightness and PhongMap interpolation weight, BDiffT2 tests normal-map alpha,
the PhongMap path tests vertex diffuse alpha, mirror/glass techniques use
ReflectColor/cube alpha, and fixed Glass plus SELFLUM produce zero alpha. The
compile-time `rgb_modulation`, `alpha_modulation`, and `coverage_source` fields
in `pipeline_manifest.json` enforce these per-technique contracts.

**Pass execution (FlushBatches pass loop, REN-4).** Pass blocks: +4 = pass
count, +16+8i = rules/z/a flags, +20+8i = FOGMODE
`[orig: @ 0x5d9f50 pass loop]`. Entry flag bit 2 stops after pass 0; entry
bit 3 forces `ZFUNC = ALWAYS` for the entry (submit 0x10 — the z-read-off
override); else pass zmode bit 0x80 picks ALWAYS/LESSEQUAL. Pass gating
(flags & 0x3C vs the entry's ≤3 light handles, spot/point split by
`Light_IsSpotlight @ 0x5a9040`): 0x10 run-if-no-spots, 0x20 run-if-spots,
0x04 run-if-pointlights, 0x08 run-if-spots; then flags & 4 = ONE DRAW PER
POINTLIGHT (`Light_GetPointLightParams @ 0x5a9180` →
PointLightCoord/Color/Atten + CommitChanges per light), flags & 8 = ONE DRAW
PER SPOTLIGHT (`get_light_projection_info @ 0x5aa5c0` →
SpotLightProjMatrix/TexCurProj; the spot plane cached at ctx+824), flags & 2
= POINTLIGHT_VARIATIONS (fills the PointLight*Array set +
CurNumPointLights); tech flag bit 3 `useffplights` enables real D3D lights
(`Light_ApplyAsD3DLight @ 0x5abd50`). MATCHTERRAIN-class entries bind THE
TERRAIN TILE TEXTURE under the object (`floor` of strip world x/z →
`terrain_tile_cache_lookup @ 0x604140`; fallback texture at effect+164).
`setup_entity_lighting_and_shader_constants @ 0x5d98a0` pushes
**AlphaTestFlag (slot 250) ← matdef+514 bit 0x1** (the shader-path alpha
test; the invert bit does not reach the shader path), DirLightColor ←
ctx+116..128 **x the state-stack effectScale** (entry[9] = the per-entity
SUN-VISIBILITY factor `[orig: Entity_ComputeSunVisibility @ 0x5c6800]`), the
Hemi/Ambient blocks from the ctx lighting slots — lerped by entry[10] under
entry flag bit 1, where **entry[10] = the parent INTERIOR's daylight-openness
float** (interior model +536; floor/ceiling ↔ ground/sky — the REN-4
"dual-LOD cross-fade" reading was an erratum, corrected at REN-5; dual-LOD
is the separate 0x10000000 repeat-draw) — and under ctx+841 the mirror-clip
constants (MatTexClipPlane ← base x ctx+756). Full writer/reader decode:
[render-lighting-re.md](render-lighting-re.md).

**UV animation (REN-4).** `apply_shader_parameters` binds MatTexCoord1 (slot
216) from `compute_uv_transform_matrix((tick_ms << 8)/1000, matdef+524)`
`[orig: @ 0x58dd49]` when the effect resolves it (the #UV twins / TEX_UVXFORM
— FFP applies it as TextureTransform COUNT2, VS effects via
`CalcAnimatedUV`). Channel blocks (8 B: `{u8 type, u8 phase, i16 speed,
i16 base, i16 range}`, U at matdef+524, V at +532): phase16 = `(phase<<8) +
time*speed` (wrapping u16); type high nibble = mode — 0x10 time-scroll
(16 +, 17 −; translate = phase16/65536), 0x20 rotation about UV center
(angle = phase16 x 2π/65536; 32 +, 33 −), 0x30/0x40/0x50/0x60 with
type ≤ 0x70 = WAVEFORM set/scroll/shear/scale (value =
`wave_lookup(type, phase16)/65535 x range + base`, base/range signed 8.8),
types 'q'..'u' (113..117) = CONTROLLED-ANIM set/scroll/shear/scale/rotation
(value = base + range x `dword_83FCE8[2*phase]`/65536). `wave_lookup
@ 0x5de6b0` indexes the SAME 2816-byte waveform table PANM uses
(`WaveformTable @ 0x2bf8ed0` = engine/formats/threedi `threedi_panm_wave_table()`);
bands per type {1→0, 2→256, 3→768, 4→1024, 5→1280, 6→rand, 7→1536 lerped,
8→1792, 9→2048, 0xA→2304 lerped, 0xF→2560}. Ported as
`renderer::uv_anim` (vectors section 4). The live bridge feeds the parsed U/V
channel blocks into this port and carries its complete row-vector matrix
through `ObjectData` and the object shader:
`u' = u*m00 + v*m10 + m20`,
`v' = u*m01 + v*m11 + m21`. Two shader `vec3` rows replace the former
offset/scale/rotation decomposition, so controlled set (zero diagonal) and
shear survive intact.

**RgbGen / AlphaGen (REN-4 — closes the channel-remap question).** The
`convert_material_definition` remaps (`src[16]`: file 3..7 → runtime 8..12,
`src[17]` identity) are the RGBGEN/ALPHAGEN TYPE bytes — the file's wave-type
ids shifted to the runtime `wave_lookup` band ids.
`[orig: RgbGen_EvaluateColor @ 0x5b23d0]` evaluates a 12-byte gen block per
channel:
type 24 = constant color; types 113 **and 114** interpolate from
`dword_83FCE8[2*phase]`; every other type uses
`base + (delta × wave_lookup(type, phase16 + speed × (tick<<8)/1000)) >> 16`.
Thus RGB types 115–117 are waveform fallbacks, not controlled
shear/scale/rotation operations. Both the controlled and waveform
interpolations keep the low 32 bits of `delta × fraction` before arithmetic
`SAR 16`; signed CTRL values can therefore extrapolate below the start and
overflow wraps exactly as retail does `[orig: RgbGen_EvaluateColor
@ 0x5B24AC]`.

`[orig: AlphaGen_EvaluateValue @ 0x5b2320]` (matdef+564) feeds the
AlphaGenValue parameter (slot 223), consumed by the FF techniques as
MaterialDiffuse alpha. Type 24 is constant; type 113 interpolates the
base/end window from `dword_83FCE8[2*phase]`; every other type uses the
waveform path. This is a separate dispatch from RGB and UV, not one global
meaning for the five control-style numbers. Alpha uses the same signed,
low-product `IMUL`/`SAR 16` interpolation
`[orig: AlphaGen_EvaluateValue @ 0x5B234C]`.

Point-light color shares the RGB generator contract:
`[orig: Light_TickGenBlock @ 0x5a8ae0]` advances the light generator block and
`[orig: Light_GetPointLightParams @ 0x5a9180]` obtains its color through
`RgbGen_EvaluateColor`. The runtime light bridge therefore resolves a control
value only for RGB styles 113/114; styles 115–117 retain waveform behavior.
`ObjectData::evaluate_lights` now supplies that signed register value, so
point lights share the same endpoint, negative-extrapolation, and wrapping
behavior as material RGB.

**Fog parameter sets (REN-4 — closes the secondary-set question).**
`CD3DDevice_SetFogParameters @ 0x677960` writes TWO blocks: the NORMAL set
@ 0x3262248..5C and the **LITE set @ 0x3262230..44 = the same fog at
one-third density** (linear: end x3; exp: density/3). FOGMODE bit 2
(`_BaseInc.fx` FOGMODE_LITE 4..7) selects the LITE block; bit 3
(FOGMODE_SHADER 8..11) zeroes table/vertex fog so the VS `oFog` drives. The
shipped corpus never uses fogmode 4..7 — LITE is dormant in JO (the
"underwater set" hypothesis is refuted; underwater fog color is the separate
`CD3DDevice_SetActiveFogColor` path).

**The color pipeline (gamma space; witnessed 2026-07-06, the model-parity
slice).** The retail pipeline is **gamma-space end to end** — texture bytes
enter the TSS/shader math raw, every combine runs on gamma-encoded values,
and the framebuffer byte is the displayed value:

- **No sRGB texture sampling.** The full device-layer SetSamplerState
  enumeration (the `mov reg, [vtbl+0x114]` idiom over `.text`, device region)
  sets only ADDRESSU/V/W (1/2/3), MAG/MIN/MIPFILTER (5/6/7), MIPMAPLODBIAS
  (8), MAXMIPLEVEL (9), MAXANISOTROPY (10) — `D3DSAMP_SRGBTEXTURE` (15) is
  never set `[orig: CD3DDevice_InitializeDisplay @ 0x679c1b..0x679d29;
  CD3DDevice_UpdateWindow @ 0x676716..0x6767f5; GfxDevice_ResetDisplayMode
  @ 0x677f91..0x678070; GfxDevice_HandleLostDevice @ 0x678374..0x678453;
  CGfxDevice_ApplyRenderStates @ 0x67e3ec..0x67e4f3]`. Device defaults:
  MAG/MIN LINEAR + **MIP POINT** (bilinear with sharp mip cuts), aniso 2,
  and reset MIPMAPLODBIAS 0.0. The current maximum-video comparison profile
  uses `texfilter_level=3`, whose per-stage selection is anisotropic with
  linear mip filtering and the device capability's maximum anisotropy
  (`CGfxDevice_ApplyRenderStates`; trilinear/aniso = modes 2/3-4, cf.
  `HLSLEffect_TextureFilterMode @ 0x27e5698`). The older cfg-0/driver-forced
  explanation described invalidated captures and is not a publication path.
- **No sRGB framebuffer writes.** The SetRenderState immediate sweep (state
  ids at every `[vtbl+0xE4]`-load site) covers the standard FF set (7, 14,
  15, 19/20, 22-29, 34-38, 48, 53-60, 136-148, 168, 171) —
  `D3DRS_SRGBWRITEENABLE` (194) never appears.
- **No sRGB effect states.** The 44-file shipped `.fx` corpus contains no
  `SRGBTexture`/sRGB pass state (grep over the decrypted localres set).
- **The display transform is the identity at defaults.** The only gamma is
  the user slider: `GLib_SetGammaRamp @ 0x677be0` builds
  `ramp[i] = (i/255)^gamma` (normalized, 16-bit, RGB-identical) and the
  default is **1.0** (static initializer `flt_84F354 @ 0x84f354`; config
  keyword `gamma` `[orig: Config_ParseSettingsLine @ 0x54feb7]`, options
  slider `UI_OnGammaSliderChanged @ 0x55a3d0`, re-applied on display
  reset/lost-device).

**Reimpl mapping (D-RMAT-7/-8).** Retail spatial shaders sample textures RAW
(no `source_color` hint), run the witnessed math on gamma-space values, and
write those numeric values unchanged through `nova_scene_output` into
the floating scene target. All opaque, source-over, additive, particle, and
Q3 draws therefore blend before any color-space conversion. The terminal
`FrameFxCompositorEffect`, ordered last at POST_TRANSPARENT, snapshots the
finished scene and applies the sole piecewise-sRGB inverse immediately before
Godot's host output encoding. Every offscreen device whose texture is
consumed raw needs exactly one decode too (2026-08-23 correction — with an
empty compositor those RGBA8 textures were sRGB-encoded a second time, which
blew out the water reflection and the bloom source): the six
environment-cube faces, the twelve slot-shadow captures, and the isolated Q3
source render into **HDR 2D** targets (Godot skips its sRGB output encode, so
the texture stores the gamma-domain numbers the shaders wrote); the water
mirror keeps a plain RGBA8 target with a decode-only terminal effect on its
camera, because its witnessed 0x40 dim is a canvas multiply over the
finished BYTES and an HDR 2D canvas would run that multiply in linear space.
The first-person viewmodel draws inside the beauty pass (its instances
apply the retail renderfov projection + depth band in the vertex stage,
`nova_viewmodel_pass.gdshaderinc`), so it shares the beauty target's one
terminal transfer and needs no composite of its own. A 3D
view with no `FrameFx` (ONED workspace previews, the menu avatar
preview, probes) installs the decode-only `DisplayDecode` node, which
is the same terminal effect without a Q3 source. The obsolete per-shader
`nova_gamma_to_linear` / `nova_linear_to_gamma` API was deleted in this
cutover.

The final [0,1] clamp is itself witnessed (the byte framebuffer saturates).
The swatch probe's **calibrate mode** runs on the production renderer and
D3D12 driver: all 256 input bytes survive the terminal transfer exactly;
the pinned SRCALPHA/INVSRCALPHA operands resolve to byte 128 and the selected
ONE/ONE operands resolve to byte 96. Run it after any Godot or renderer
change. This closes D-RMAT-8 rather than bounding it to opaque surfaces.

## Divergence catalog

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RMAT-1 | Alpha test kept `a >= t` and INVERTED THE VALUE (`1-a >= t`); threshold fudged `maxf(0.001, byte/255)` | keep `a > ref`; invert flips the COMPARE to `a <= ref` (`[orig: @ 0x5da3a9..0x5da401; @ 0x6770a0]`) | FIXED (this slice: composer + model threshold; T1 re-dump cited) |
| D-RMAT-2 | VS_TRACER unknown (absent from the OED-derived table); soft-edge `vsTracer` look unported | runtime registry carries VS_TRACER; the "soft edge" is the `vsTracer` facing falloff `Diff = \|dot(eye, normal)\|²` over unlit additive `MODULATE(Texture, Diffuse)` — NOT a displacement | **FIXED (REN-4)**: `MATERIAL_DESCRIPTOR_VIEW_FADE` → `OSCAP_VIEW_FADE` composes the per-fragment `\|dot(eye, normal)\|²` fade (`[orig: vsTracer, Tracer.fx]`; per-vertex→per-fragment is the reimpl form of the same formula); vectors re-dumped with this witness |
| D-RMAT-3 | Tag lookup case-SENSITIVE | `stricmp` (`[orig: HLSLEffect_FindByName @ 0x5ade70]`) | FIXED (case-insensitive exact-tag) |
| D-RMAT-4 | File-effect capability/sort words carried verbatim from the OED dump | derived at load by the technique-usage probe, UNIONED over all techniques (`[orig: @ 0x5ae690]`) | **FIXED (REN-4)**: the probe replicated statically over the shipped localres text — 14/19 tags match; 5 drift rows corrected on `kMaterialDescriptorTable` (the OED dump in `oed/types.h` stays byte-faithful): FFP_GLASS `0xb000 → 0x10003000` (no VS ⇒ no TANGENT; GLOW technique uses TexCubeRotSpecular ⇒ GLOW), VS_SKBUMPDIFFT/PHONGT `0x6014 → 0xc014` and VS_SKBUMPDIFFT2 `0x601c → 0xc01c` (read `In.Tangent`, never ReflectColor ⇒ TANGENT not GLASS), VS_SKGLASS `0x7004 → 0x7000` (untextured ⇒ no DIFFUSE). The 0x10000000 dialect resolved = the glow-copy capability (flag renamed `MATERIAL_FLAG_GLOW`); `is_luminance` re-keyed on EMISSIVE. Cited re-dump + `renderer_material_classify` pins |
| D-RMAT-5 | Composer lighting gains were prototype values (hemi fill + ×1.5/×1.6/spec 0.8) | witnessed uniform surface `HemiGroundColor/HemiSkyColor/DirLightVector/DirLightColor/AmbientColor/ColorSrcGlobalGain` with engine-fed values under the FF MODULATE2X model | **FIXED (REN-5)** — the composer emits the witnessed model (saturated hemi+dir ×2; SELFLUM × ColorSrcGlobalGain ×2; the ×1.5/×1.6/spec-0.8 constants deleted); uniforms renamed `u_hemi_sky_color`/`u_hemi_ground_color`/`u_color_src_global_gain` and engine-fed from the env blocks ([render-lighting-re.md](render-lighting-re.md)); T1 re-dump: key set + classification rows identical, all 630 composed hashes re-hashed under this citation, sections 3/4 untouched; T2: 116/120 swatch cells moved, the 4 VS_TRACER cells (unlit MODULATE 1×) byte-identical. Residual reflection/phong stand-ins tracked as D-RLIT-5 |
| D-RMAT-6 | Single-pass reimpl materials lacked CLIP/PROJSHAD/DEPTHMASK/GLOW/MATCHTERRAIN technique-class behavior | six pass classes selected per batch entry (`@ 0x5d9ff3`), CLIP falls back to NORMAL, LUM populates GLOW; source content and execution are pinned in `retail_effect_inventory.json` / `auxiliary_technique_validation.json` | **FIXED (2026-08-22, highest-quality shader audit)** — CLIP, PROJSHAD, GLOW, and MATCHTERRAIN are live with per-technique compile-time contracts and D3D12 raster proofs. DEPTHMASK is source-validated but unreachable because `LightPool_SpawnSpotProjectorEffect @ 0x5a9fd0` has no caller, so omitting a runtime pass is the faithful result. GLOW executes in the isolated Q3 camera and exact native FrameFX sequence |
| D-RMAT-7 | Textures decoded sRGB→linear (`source_color`), witnessed gamma-space formulas evaluated on mixed-space values, result re-encoded by the reimpl blit — an unwitnessed transform stack around every FF shader (compressed lighting contrast, washed color response) | gamma-space end to end: raw texel sampling, gamma-space combines, framebuffer byte = displayed byte, identity display ramp at default gamma 1.0 (§Color pipeline witness above) | **FIXED (2026-07-06; hard cut over 2026-08-22)**: raw sampling + gamma-space math remain, while the old per-shader inverse helpers are replaced by `nova_scene_output` and one terminal display decode; calibrate mode proves 256/256 byte identity plus live blend equations |
| D-RMAT-8 | Framebuffer blending happened on per-shader blit-encoded (linear) values | blending on gamma bytes (`out = src_g op dst_g` per the blend mode tables `@ 0x680f00`) | **FIXED (2026-08-22)** — all retail 3D and particle passes now write and blend gamma-domain numeric values in the scene target; `FrameFxCompositorEffect` performs the only display decode after the final blend. The D3D12 calibration is zero-tolerance: 256/256 transfer bytes, SRCALPHA/INVSRCALPHA byte 128, ONE/ONE byte 96 |
| D-RMAT-9 | Object composer fog was a linear ramp with an invented `smoothstep` for type 3 | the device fog table: type 0 exponential `ln(64)/end`, types 1/2/3 linear with start = 0.5 / `(1−density)·end·0.5` / `(1−density)·end·0.25` (`[orig: Render_SetFogState @ 0x58a950 → CD3DDevice_SetFogParameters @ 0x677960]`; env-tod-re.md §Fog policy) | **FIXED (2026-07-06, the model-parity slice)**: the composer emits the witnessed table (one text with `terrain_lighting.gdshaderinc`/`water.gdshader`); covered by the same T1 re-dump |
| D-RMAT-10 | The `_MT` secondary (detail) stage ran HALF the witnessed combine: the composer emitted `base.rgb *= detail.rgb` — ×1, no alpha touch — so resolved MT surfaces (RckS05's `W_Rck1_o`, gray avg 93/255) modulated ×0.365 where retail runs ×0.73 (MT objects too dark in detail regions, the REN-7 T3 "W_RCK1_O watch item"), and the stage never alpha-modulated; a missing secondary bound a white ×1 fallback (neutral then, a ×2 brightener under the fix) | stage 1 = `TSSColor(1, Modulate2x, Texture, Current)` + `TSSAlpha(1, Modulate, Texture, Current)` (§FF technique tables — "the same on stage 1 vs Current for `_MT`"), and the combine is CORPUS-UNIFORM across every second-diffuse family (REN-7 sweep, the .fx re-derived from retail `localres.pff` via `engine/formats/pff`+`engine/formats/scr`, never committed): `BDiffT2.fx` (`EffectTag "VS_DOT3DIFF2"`) carries the identical stage-1 pair, and `SkBDiffO2.fx` (`EffectTag "VS_SKBUMPDIFFOBJ2"`) applies BOTH diffuses in its NORMAL P3 "post multiply" pass — same TSS pair under `RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)` (the ×2-onto-framebuffer form); a NULL-texture stage is dropped; the sample set is the SECOND authored UV channel — the .3di v8 vertex carries TWO UV sets unconditionally (stride 40 = pos+normal+uv0+uv1; RckS05 uv1 distinct on 48/48 verts, FOUNTAIN M4 on 455/455; FVF 0x212 TEX2 corroborates the D3D FF stage-N→texcoord-N default) | **FIXED (REN-7, 2026-07-07)**: composer emits `base.rgb *= detail.rgb * 2.0; base.a *= detail.a;` `[orig: _FFP.fx TECHNIQUE_NORMAL _MT stage 1]`; the reimpl masks `OSCAP_DETAIL` off the composed key when the secondary fails to resolve (exact stage-drop identity, retail-shaped; the white fallback deleted; `classify()` stays pure — 0 classification rows moved). T1 re-dump: exactly the 224 OSCAP_DETAIL composed hashes moved (+27 bytes each = the two text edits), everything else byte-identical; handoff pins unchanged (`FF_MT_OP/base → 0x00001004`) |
| D-RMAT-11 | Controlled flipbooks treated every CTRL as a state-zero signed 16.16 fraction, so RevX02 `IndoArms.3di` consumed raw `TEX_CAMO1 = 1` as frame zero (`A_Arm1st.tga`, tattooed) | the retail image statically seeds the adjacent state dwords for `TEX_TEAM` and `TEX_CAMO1/2/3` (ordinals 92–95) to one; `apply_shader_parameters` therefore uses signed `value % frame_count` for those four selectors `[orig: @ 0x58DC36..0x58DC42]` | **FIXED (2026-08-17, PR-503 adversarial T3)**: `compute_anim_frame` selects modulo only for exact ordinals 92–95 and preserves the generic 16.16 path; literal pins cover two-frame `IndoArms.3di` and three-frame `APLFP1.3DI` Jflag1/Jflag2/Jflag3, including the signed negative remainder |

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

REN-4 session (the shader/TSS grill):

| Address | Old | New | Basis |
|---|---|---|---|
| 0x683420 | sub_683420 | RenderState_CacheFindOrAdd | the 1024×252-B state cache; memcmp 0xF4 payload; applies via RenderState_ApplyToDevice |
| 0x6832F0 | sub_6832F0 | RenderState_CacheFindOrAddByModeId | mode-id (0x3FFF) lookup; builds via the permutation cache |
| 0x680b00 | decode_blend_state | decode_mode_alpha_stage | writes stage ALPHAOP/ARG1/ARG2 from the 0xF0 nibble |
| 0x681080 | decode_blend_state_extended | decode_mode_color_stage | writes stage COLOROP/ARG1/ARG2 from bits 8-13 |
| 0x683650 | sub_683650 | CGfxShader_SetRenderStateDesc | stores the flags word (+60), resolves the 0xF4 desc through the cache into +68 |
| 0x6835C0 | sub_6835C0 | CGfxShader_SetRenderStateByModeId | same via the mode-id path |
| 0x679030 | sub_679030 | GfxShader_Create1TexModeId | factory: 1 texture slot + mode id |
| 0x6793C0 | sub_6793C0 | GfxShader_Create2TexDesc | factory: 2 texture slots + 0xF4 desc |
| 0x679550 | sub_679550 | GfxShader_Create4TexDesc | factory: 4 texture slots + desc |
| 0x677020 | sub_677020 | GfxShader_ApplyPassChecked | null-guarded CGfxShader_ApplyPass (the old "free-all-by-id" auto comment was wrong) |
| 0x5b2320 | sub_5B2320 | AlphaGen_EvaluateValue | evaluates matdef+564 → the AlphaGenValue param |
| 0x5a9040 | sub_5A9040 | Light_IsSpotlight | the spot/point split in the pass-rules gate |
| 0x5a9180 | sub_5A9180 | Light_GetPointLightParams | fills coord/color/atten for the per-light passes |
| 0x5abd50 | sub_5ABD50 | Light_ApplyAsD3DLight | useffplights path — enables real D3D lights |
| 0x28e0990 | flt_28E0990 | g_MatTexCoord1 | the composed UV matrix block |
| 0x2bf8ed0 | table | WaveformTable | the shared 2816-B wave table (PANM + UV anim + RgbGen) |
| 0x2721a40 | dword_2721A40 | Render_ShaderTickMs | the shader clock (ms); (tick<<8)/1000 feeds the anim paths |

## Open questions

- **Closed at REN-4**: the channel enum remaps (= the RgbGen/AlphaGen wave-type
  ids, §RgbGen/AlphaGen); the UV-scroll path (§UV animation — ported as
  `renderer::uv_anim`); `AlphaTestFlag` (slot 250 ← matdef+514 bit 0x1,
  pushed in `setup_entity_lighting_and_shader_constants @ 0x5d98a0`); the
  secondary fog parameter set (= the dormant LITE 1/3-density block,
  §Fog parameter sets).
- **Closed 2026-07-29**: the parsed MTRL U/V blocks (matdef+524/+532 in
  retail, copied by `[orig: convert_material_definition @ 0x5b03c0]`) now
  feed the full live 2×3 object-shader transform; signed RGB/alpha/flipbook
  consumers and the point-light register feed are also live. This closes the
  consumer integration, not the still-partial set of gameplay CTRL producers
  cataloged in [3di-gp-format-re.md](../threedi/3di-gp-format-re.md).
- The sort word (entry+164) consumer — authored at load
  (`[orig: @ 0x5af14b..0x5af1a4]`) but no runtime reader was found; the batch
  sort uses the registry INDEX, not this word. Likely tooling/dev-sort
  residue; note-only.
- `PolyTrn_UsePixelShaderPath` is force-cleared unless `dword_32655B4` is 80
  or 73 (`[orig: PolyTrn_LoadTerrainConfig @ 0x60e578]`) — what that
  device/format code is ('P'/'I'?); terrain-record scope.
- DirLightVector (slot 226) is pushed only under the mirror gate (ctx+841,
  re-negated with w = 0.8 `[orig: @ 0x5d9967..0x5d99a6]`) — no normal-path
  push was found (the FF path lights via D3D light 0; VS effects that
  resolve the parameter would read the last mirror push). Note-only unless a
  VS-lit artifact surfaces in T3.
- Whether retail's `#UV` TextureTransform (MatTexCoord1) also touches stage 1:
  the reimpl composer applies `obj_transform_uv` to BOTH `v_uv` and `v_uv2`;
  the `_FFP.fx` reading pinned the stage-0 transform only. Identity for every
  non-`#UV` MT material, so invisible today — witness the stage-1 TSS
  TEXTURETRANSFORMFLAGS if a `#UV`+`_MT` artifact surfaces (noted at the
  D-RMAT-10 port, 2026-07-07).
