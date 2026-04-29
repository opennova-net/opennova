# IDA witness matrix — particle system

**Re-validation pass started**: 2026-04-27 (worktree particles)
**Latest pass**: 2026-04-28 — visual + algorithmic alignment (curve LUT bake,
spawn-flag computation, annular emit-shape, YAWANDPITCH static billboard,
GRAVITATE move, 8-mode shader split). See plan
`C:\Users\taylor\.claude\plans\hey-study-the-particle-cuddly-mochi.md`.
**Truth source**: retail `Jointops.exe` IDB at
`C:\Users\taylor\Development\opennova-godot\RE\Jointops.exe.i64`
**Companion artifacts**: [`ptl_format.md`](ptl_format.md) — narrative grammar;
[`ptl_corpus_catalog.md`](ptl_corpus_catalog.md) — corpus inventory;
[`open_questions.md`](open_questions.md) — unverified items.

## Verdict legend

- **match** — code citation matches IDA, layout/grammar matches IDA. Add inline `@ 0xADDR (IDAName)` if missing.
- **match-relabel** — address correct, layout correct, but human-readable label in code mislabels what the function does. Code stays, comment updates.
- **misaligned** — layout does not match IDA. Code needs fix.
- **missing-citation** — claim has no `@ 0xADDR` reference; needs one added.
- **missing-citation+verified** — no citation in code, but IDA witness is verified in this pass; just needs the citation added.
- **no-witness-found** — searched IDA, no match. Quarantine the call site.
- **pending** — not yet evaluated.
- **deferred** — out of scope for this worktree (runtime / emitter / atlasing).

## Bump / Bumpadd lit-color RE (2026-04-28, port deferred)

The engine's "bump" lighting effect is implemented via a 2-color vertex
format combined with a fixed-function texture stage combiner. Hard data:

**Light direction** (`flt_848D34/D38/D3C` at `0x848D34`, raw bytes
`46 B6 13 BF / 46 B6 13 BF / 46 B6 13 3F`):

```
flt_848D34 = -0.5773503  // = -1/√3
flt_848D38 = -0.5773503  // = -1/√3
flt_848D3C = +0.5773503  // = +1/√3
```

This is the engine's hardcoded global directional light, a unit vector
along the diagonal `(-1, -1, +1)/√3`. Used for ALL bump-shaded particles
across the engine — there's no per-emitter or per-spawn override.

**Lit-color computation in `BuildBillboardQuads @ 0x5e6d60`** (when
`particle.flags & 0x80` (LitColor for Bump=3 and Bumpadd=6) is set):

```
1. Compose per-particle rotation matrix:
   D3DXMatrixRotationX(R, particle.rotation_radians)  // around X axis
   composed = R × parent_matrix  // parent_matrix at emitter+664

2. Compute inverse via D3DXMatrixTranspose (sub_68BF44 → off_8507A8 IAT
   thunk, mirrored at internal `?c_D3DXMatrixTranspose` @ 0x68bf4a):
   inv_rotation = transpose(composed)

3. Build light direction in particle local space:
   light_local.x = bump_scale * (-flt_848D34)   // = +bump_scale * 0.577
   light_local.y = bump_scale * (-flt_848D38)   // = +bump_scale * 0.577
   light_local.z = bump_scale * (+flt_848D3C)   // = +bump_scale * 0.577
   light_local = inv_rotation × light_local      // sub_68B52B = D3DXVec3Transform

4. Encode each component to a byte via remap signed → unsigned:
   byte = clamp((value + 1.0) * 0.5, 0.0, 1.0) * 255

5. Compose primary vertex color:
   primary_RGB = (byte_x << 16) | (byte_y << 8) | byte_z
   primary = (modulated.alpha << 24) | primary_RGB
   // i.e., RGB replaced by encoded light direction; alpha kept from modulated

6. Compose secondary vertex color:
   secondary = modulated_RGB | 0xFF000000
   // i.e., raw modulated RGB with full alpha
```

The resulting vertex carries TWO colors per corner. The fixed-function
texture stage combiner (TSS state — not yet decoded) uses both:
- Bump (mode 3): combiner does `texture × diffuse × specular` (modulate)
  giving a lit-color-tinted modulated texture
- Bumpadd (mode 6): different combiner op (additive blend of stages)

**Vertex layout** at offsets relative to `_ESI` (vertex base) per the
engine code:
- `+0..+8`: position xyz (12 B)
- `+12`: primary color (lit color or modulated color, depending on flags) — D3DFVF_DIFFUSE
- `+16`: secondary color (modulated, full alpha) — D3DFVF_SPECULAR
- `+20..+24`: uv (8 B) — D3DFVF_TEX1
- Stride = 28 B/vertex × 4 verts/quad = 112 B/quad

**FVF anchor** confirmed via `GDynamicVB_FlushAndRender @ 0x5e0b10`:
the engine calls `device->SetFVF(450)` before `DrawIndexedPrimitive`.
`450 = 0x1C2 = D3DFVF_XYZ (0x002) | D3DFVF_DIFFUSE (0x040) | D3DFVF_SPECULAR
(0x080) | D3DFVF_TEX1 (0x100)` — exact match to the 2-color + position +
1-uv layout. This confirms the 2-color path is universal across all
particle blend modes (the FVF doesn't change per mode); the per-mode
visual differentiation comes from how the renderer writes those colors
(LitColor branch vs standard branch in `BuildBillboardQuads`) plus the
texture stage combiner state.

**Port scope** (deferred to a future slice):
1. Add a 2nd Color vertex attribute to our `ArrayMesh` quads (Godot's
   `ARRAY_CUSTOM0..3` for an extra per-vertex value).
2. In `nova_particle_emitter.cpp::_update_meshes`, when
   `particle.flags & particle_runtime_flag::LitColor` is set, compute the
   lit color per-particle:
   - Build rotation matrix from particle.rotation around an axis
     consistent with our billboard's local frame (likely camera forward,
     not engine's X axis convention — needs validation).
   - Multiply by camera basis (or emitter orientation, depending on
     interpretation).
   - Invert via transpose.
   - Transform light direction into local space.
   - Encode to bytes.
3. Update `particle_blend_bump.gdshader` to consume the lit color via
   the custom attribute, producing `texture × secondary × primary` in
   fragment.
4. Same for `particle_blend_bumpadd.gdshader` but with additive
   composition.

**Outstanding RE for the port to be engine-faithful:**
- Texture-stage combiner state (D3DTSS_COLOROP, _COLORARG1, _COLORARG2)
  for Bump vs Bumpadd. Currently we don't know whether the engine uses
  D3DTOP_MODULATE or D3DTOP_ADD (or some 2x variant) for stage 1.
- Whether the per-particle rotation in the engine's matrix is around
  X-axis literally (D3DXMatrixRotationX) or whether the X here is a
  billboard-local "screen normal" axis re-named in the engine's
  coordinate convention. The composition with `parent_matrix` at
  emitter+664 (which IS world-space) suggests the X really is X.

## Kong-rename corrections (RE 2026-04-28)

Kong renamed several engine functions involved in the particle render path
incorrectly. Recording the actual semantics here so future RE doesn't
re-trip:

| Kong name | Address | Actual purpose |
| --- | --- | --- |
| `CEffectChannel_PlaySample` | `0x5e4230` | **`BindRenderStateAndTexture`**. Called from `BuildBillboardQuads @ 0x5e6d60` whenever the bound texture for the next batch differs from the current. Calls `GDynamicVB_FlushAndRender` (drains pending verts), dispatches `CD3DDevice_SetFogAndBlendMode` based on `*sample` (the material struct's first dword), then calls `device->SetTexture(sample[4])` via vtable+368. Has nothing to do with audio. |
| `CD3DDevice_SetFogAndBlendMode` | `0x677740` | **`SetFogStateAndTextureFactor`**. Only writes fog render states (D3DRS_FOGTABLEMODE=35, _FOGSTART=36, _FOGEND=37, _FOGDENSITY=38, _FOGVERTEXMODE=140) + D3DRS_FOGCOLOR (=34) — the latter selected by low 2 bits of `mode` from {self-color, gray 0xFF7F7F7F, black 0xFF000000, white 0xFFFFFFFF}. Never writes D3DRS_SRCBLEND/DESTBLEND/ALPHABLENDENABLE. The "BlendMode" in the kong name is misleading. |
| `Render_ResetFogAndBlendState` | `0x589ad0` | **`Render_ResetFogState`**. Two-step fog reset (`SetFogStateAndTextureFactor(-1)` then `(0)`) plus 3 dirty flags. No alpha-blend reset. |

Implication for the bump/bumpadd/distort RE: alpha-blend state for
particles is NOT switched per-blend-mode in the per-particle render loop.
It's likely set once at the manager level (probably `SrcAlpha + InvSrcAlpha`
or `SrcAlpha + One`) and the per-blend-mode visual differentiation comes
from:

1. The 2-color vertex format that `BuildBillboardQuads` writes when
   `particle.flags & 0x80` (LitColor for Bump/Bumpadd) is set: primary
   color = lit color (composed from `def[+304..+312]` orientation/normal
   × light-direction constants `flt_848D34/D38/D3C`), secondary color =
   raw modulated color OR'd with 0xFF000000 (full alpha).
2. The fixed-function texture stage combiner (D3DTSS_*) which multiplies
   `texture × primary × secondary` in the appropriate combiner mode.
3. Per-mode `flag` bits (0x80 = LitColor for Bump/Bumpadd; 0x100 =
   Distort) drive the spawn-time setup; the renderer then dispatches the
   vertex format accordingly.

## Per-blend-mode render-state binding (RE 2026-04-28)

**Correction to the "alpha-blend set once at manager level" speculation
above** — the alpha-blend state IS switched per-blend-mode, but through a
struct-driven indirection that wasn't immediately obvious from the
particle code alone.

**Call chain** when a particle batch's bound texture changes:

```
CParticleEmitter_BuildBillboardQuads @ 0x5e6d60
  → CEffectChannel_PlaySample @ 0x5e4230   (BindRenderStateAndTexture)
    → CD3DDevice_SetFogAndBlendMode @ 0x677740   (fog + TFACTOR only)
    → sub_677020(sample[1 or 2], 0x500000) @ 0x677020   (thunk)
      → sub_683190 @ 0x683190
        → sub_680760 @ 0x680760    (bind up to 6 textures via SetTexture)
        → GfxBlend_ApplyToDevice @ 0x6817d0   (D3DRS_ALPHABLENDENABLE=27,
            _SRCBLEND=19, _DESTBLEND=20)
        → RenderState_ApplyToDevice @ 0x681920   (per-stage texture
            combiner: D3DTSS_COLOROP=1, _COLORARG1=2, _COLORARG2=3,
            _ALPHAOP=4, _ALPHAARG1=5, _ALPHAARG2=6, _RESULTARG=28
            for stages 0..5)
        → SetVertexShader / SetPixelShader at offsets +244 / +248
            of the bound material (vtable+428 / vtable+368)
```

**The `sample` argument to `PlaySample` is read from the per-frame UV-rect
slot at `graphic+724`**. Each frame entry holds a pointer to a sample
struct + the 5-float UV rect (u_min, u_max, v_min, v_max, inset). The
sample struct itself encodes the blend mode (sample+0), primary
render-state pointer (sample+4), secondary render-state pointer for
compound modes like Distort (sample+8), a global flag dword (sample+12 →
`dword_3266E8C`), and a vertex format / FVF code (sample+16).

**Confirmed not used**: `D3DTOP_BUMPENVMAP` (=22) and
`D3DTOP_BUMPENVMAPLUMINANCE` (=23) appear only in the parser's string-
length comparisons (`CParticleDef_ParseFromConfigMap @ 0x5ed210`); never
in the render path. So **bump and bumpadd modes use plain fixed-function
combiners** (likely `D3DTOP_MODULATE` / `D3DTOP_ADD`), not D3D9 bump-
mapping ops. Our current bump shader (`texture × lit_color`) and bumpadd
shader (`texture + lit_color`) approximate the engine's combiner output;
the bounded deviation around the per-particle rotation transform on
`lit_color` direction stands.

**Static struct layout** (the data that `RenderState_ApplyToDevice` reads,
inferred from the function's per-DWORD access pattern):
- `+0` = `active_stages` (loop bound; loop runs while `v5 < this+0` for
  stages 1..5, so this caps how many stages get configured)
- `+16` (`renderState[4]`) = stage-0 ALPHAOP value
- `+24` (`renderState[6]`) = stage-0 ALPHAARG1 value
- `+28` (`renderState[7]`) = stage-0 ALPHAARG2 value
- `+32` (`renderState[8]`) = stage-0 COLOROP value
- `+40` (`renderState[10]`) = stage-0 COLORARG1 value
- `+44` (`renderState[11]`) = stage-0 COLORARG2 value
- `+48` (`renderState[12]`) = stage-0 RESULTARG selector (sets D3DTSS=28
  to `4*(v3!=0) + 1` → 1 = D3DTA_CURRENT or 5 = D3DTA_TEMP)
- Stages 1..5 follow at stride 36 bytes from offset +56, in the same
  shape (loop body in `RenderState_ApplyToDevice`).

**Static struct location** — the per-blend-mode state structs live in
the read-only data section starting around `0x7e7558`, organised as an
array of indexed structs (one per blend mode 1..8). Each struct entry
begins with a chain pointer + a blend-mode index dword (1..8), followed
by ~15 dwords of stage-state data referencing per-mode sub-structs at
`0x7e94e0..0x7e9510`. Pattern search for the Mod2x signature
`SrcBlend=9 (DESTCOLOR), DestBlend=3 (SRCCOLOR)` confirmed the data
section: byte sequence `09 00 00 00 03 00 00 00` lands at `0x7e7814`,
inside the index-6 (Mod2x) struct.

Full per-blend-mode COLOROP/ALPHAOP value extraction is **not strictly
required to validate our portable shaders**: see the conclusion below.

**Conclusion — engine combiner modes for Bump / Bumpadd**: the search
for `D3DTOP_BUMPENVMAP` (=22) and `D3DTOP_BUMPENVMAPLUMINANCE` (=23) in
the binary's immediate operands returned hits only inside the .ptl
parser (`CParticleDef_ParseFromConfigMap @ 0x5ed210`, used as
string-length compare values). The render path never references these
ops. **`D3DRS_SPECULARENABLE` (=29) is also never set in the particle
code path** — no immediate-29 hits in the 0x5e*/0x5f* range — so the
default value (FALSE) holds and the SPECULAR vertex slot is dead state
for particles.

**Implication**: bump and bumpadd modes use plain fixed-function
combiners (most likely `D3DTOP_MODULATE` for stage 0 with
`COLORARG1=TEXTURE`, `COLORARG2=DIFFUSE`). Since DIFFUSE carries the
encoded `lit_color` (per the FVF=450 layout we already documented), the
engine's per-pixel output is `texture × lit_color` for Bump. Our
existing shaders match: `particle_blend_bump.gdshader` does
`base * lit_color` with `base = COLOR * texture(...)`; `particle_blend_
bumpadd.gdshader` does `(base * COLOR) + lit_color * 0.5` with the
framebuffer-level additive blend. The `0.5` coefficient and the exact
lit-vs-modulated combination are educated guesses, but they fall in the
same fixed-function-modulate ballpark as the engine.

**The bump/bumpadd `D3DTSS_COLOROP` "RE-blocker" is therefore resolved**:
no exotic combiner mode is in play. Remaining bump/bumpadd deviation is
the per-particle rotation transform on the lit-color direction (already
documented as a bounded deviation; would require per-vertex CUSTOM1 with
rotation angle to close).

**Distort blend mode (BlendMode=7)** uses up to 2 textures via the
secondary render-state pointer at `sample+8` (selected when `this+276`
flag is set). The second texture is bound to stage 1 by `sub_680760`.
The specific stage-1 combiner setup is in the index-8 struct at
`~0x7e7858` (16 dwords past the index-7 struct, by stride). Decoding
the exact byte values for the texture-stage state requires resolving the
sub-struct pointers at `0x7e94ec..0x7e94f0` and reading their layout —
deferred. Our shader falls back to a fixed-strength screen-tex UV
offset, which matches the visual intent (sample distorted underlying
buffer based on alpha gradient) without byte-exact stage state.

Full port deferred — see `notes/particle_visual_parity.md` Bounded
Deviations.

## Section-tag dispatch

The four section strings live in `.rdata` and are referenced by the section dispatcher:

| Tag string | Address | Reachable from |
| --- | --- | --- |
| `[effectdef]` | `0x7dd9e8` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecb60` |
| `[particledef]` | `0x7dd9d8` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecb8c` |
| `[tabledef]` | `0x7dd9cc` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecbb8` |
| `[tabledef_edithandles]` | `0x7dccf0` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecbe4`; also `CEffectTableDef_ParseCallback` @ `0x5e4020`, `CParticleTableDef_ParseScriptLine` @ `0x5e92e4`, `CParticleDef_ParseProperties` @ `0x5ea346` (used as an end-of-section sentinel) |

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `libs/particle/src/parser.cpp` (TBD) | dispatcher | `CEffectWorld_ParseSectionCallback` | `@ 0x5ecb40` (size 0x101) | Single function references all 4 section strings (data refs above); branches on tag → per-section parser | match (witness only) | Our C++ parser collapses dispatch + per-section into one switch — this function is the IDA mirror. |
| (none yet) | dispatcher | `CEffectTableDef_ParseCallback` | `@ 0x5e4010` (size 0x1b1) | Alternate path for `[tabledef_edithandles]` | pending | Likely an editor-only callback. Not on the runtime asset-load path. |

## Per-section parsers

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `libs/particle/src/parser.cpp` (apply_particle_key) | parser-fn | `CParticleDef_ParseProperties` | `@ 0x5ea320` (size 0x2525) | Full decomp 2026-04-27: stricmp dispatch on ~80 keys — matches our switch. Sets bit 0x02 on `reverse` trailing token (e.g. scale_func @ 0x5eafdd) and bit 0x01 on `inverse`. `[tabledef_edithandles]` @ 0x5ea346 returns 1 to skip section. | match | **Engine bugs preserved as documented**: g{2,3,4}_color1 falls through to higher-color slots in this dispatcher. Our parser does the *correct* mapping (g{N}_color{M} → graphics[N-1].color[M]); see open_questions.md. |
| `libs/particle/src/parser.cpp` (apply_particle_key) | hydrator-fn | `CParticleDef_ParseFromConfigMap` | `@ 0x5ed210` (size 0x1da5) | Reads ~80 named keys from config map → `CParticleEffectDef` (~5204 B); see field offset table below | match (witness only) | Drives our C++ `ParticleDef` field set. |
| `libs/particle/src/parser.cpp` (apply_table_key) | parser-fn | `CParticleTableDef_ParseScriptLine` | `@ 0x5e92b0` (size 0x266) | `[tabledef]` line driver | pending | Equivalent line-handler not directly decompiled; the corpus 32×8 invariant is verified across 77/77 files via the smoke test. |
| (none yet) | parser-fn | `CParticleTableDef_ParseProperties` | `@ 0x5eefc0` (size 0x228) | Companion property reader for `[tabledef]` | pending | |
| `libs/particle/src/parser.cpp` (apply_particle_key, graphic decl + g_* dispatch) | parser-fn | `CParticleDefEntry_ParseGraphicProperty` | `@ 0x5e3550` (size 0xabe) | Full decomp 2026-04-27: graphic1 resets idx=0, graphicN++ (capped at 3); per-key dispatch via strstr; "reverse"/"inverse" trailing tokens set bits 0x02/0x01 on each func. | match | Engine quirk: g2_color1 / g3_color1 / g3_color2 / g4_color1 / g4_color2 are remapped by the outer dispatcher (CParticleDef_ParseProperties) to higher-color slots before reaching here — see row above. |
| `libs/particle/src/particle.cpp::parse_blend_mode` + `blend_mode_name` | parser-fn | `CParticleDefEntry_ParseBlendMode` | `@ 0x5e29f0` (size 0xc8) | Full decomp 2026-04-27: chained strstr in this exact order: additive=1, blend=0, premult=2, bump=3, mod=4 (`off_7DCBA8`), mod2x=5, bumpadd=6, distort=7. Engine quirk: "bump" matches before "bumpadd" so `bumpadd` resolves to 3 (Bump), not 6. | match | Quirk replicated by our parser (otherwise corpus diverges). |
| (none yet) | parser-fn | `CParticleTableDef_ParseTransformFlags` | `@ 0x5e2950` (size 0x39) | Tabledef flag bits | pending | |
| `libs/particle/src/particle.cpp::parse_flag_table` | helper-fn | `FlagTable_ParseFromString` | `@ 0x5df970` (size 0x45) | Iterates {bit:u32, name:char[260]} entries (264 B stride, 4-byte preamble); ORs all matched bits via `strstr`. | match | Used to parse both flags table @ 0x846A18 (26 named bits / 29 slots) and move table @ 0x848800 (5 bits). |
| `libs/particle/src/particle.cpp::format_flag_table` | helper-fn | `sub_5DF9C0` (BuildFlagString) | `@ 0x5df9c0` (size 0x98) | Inverse of FlagTable_ParseFromString; emits names in table order, single-space separated, with one trailing space. | match | Replicated exactly so writer output matches engine layout. |

## Save / write (round-trip verification gold)

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `libs/particle/src/writer.cpp::write_particle` | writer-fn | `CParticleDef_SaveToFile` | `@ 0x5e4d70` (size 0x9ef) | full decomp 2026-04-27: field-by-field fprintf, `%5.3f` floats, BGR-byte order in memory printed back as R,G,B. Engine writes `emit_dur` **twice** (lines 0x5e4e6a + 0x5e4f30, same field +906) and `lod` (offset +38) regardless of whether ParseProperties consumed it. | match | Round-trip parity verified by `particle_writer_roundtrip_test`. Not byte-exact against corpus (engine writer uses `\n` only; OS may translate). |
| `libs/particle/src/writer.cpp::write_effect` | writer-fn | `CParticleEffectDef_WriteToFile` | `@ 0x5e0fe0` (size 0xc7) | full decomp 2026-04-27: `\tid = %s;`, `\tpdefs = name1, name2;` (separator from word_7CDA14 = ", "). Note: space-equals (not tab-equals like particledef). | match | |
| `libs/particle/src/writer.cpp::write_table` | writer-fn | `CParticleTableDef_WriteToFile` | `@ 0x5e27e0` (size 0xee) | full decomp 2026-04-27: 32 fixed rows of 8 `%u` values; `\ttlN = ...;` lines; "id = " uses space-equals. | match | Our writer zero-fills if parsed table has < 32 rows (defensive; corpus always has 32). |
| (none yet) | writer-fn | (no engine writer found for `[tabledef_edithandles]`) | n/a | format inferred from boatwake.ptl:40-45 | match (corpus-derived) | Implemented in `write_handles`; round-trip verified. |

## `CParticleEffectDef` layout (heap, ~5204 B)

Offsets confirmed live from `CParticleDef_ParseFromConfigMap @ 0x5ed210` decompile, this RE pass.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` | `id` | std::string (heap) | First call: `CConfigReader_GetString("id", …)` |
| `+132` | `child_id` | std::string ptr | Sub-particle reference |
| `+136` | `flags` | u32 bitfield | Parsed via `FlagTable_ParseFromString(@ 0x846A18, …)` |
| `+140` | `move` | u32 enum | Parsed via `FlagTable_ParseFromString(@ 0x848800, …)` |
| `+148` … `+935` | `graphic[0]` | 788 B | First graphic layer |
| `+936` … `+1723` | `graphic[1]` | 788 B | Second graphic layer |
| `+1724` … `+2511` | `graphic[2]` | 788 B | Third graphic layer |
| `+2512` … `+3299` | `graphic[3]` | 788 B | Fourth graphic layer |
| `+3300` | `graphic_count` | u32 | Incremented in the parse loop after each found graphic |
| `+3304` | `alpha` | float | |
| `+3308` … `+3379` | `alpha_func` playback flags | 72 B | |
| `+3380` … `+3451` | `red_func` playback flags | 72 B | |
| `+3452` … `+3523` | `green_func` playback flags | 72 B | (note: parser order red/blue/green/blue is interleaved with strings; offsets verified at write sites) |
| `+3524` … `+3595` | `blue_func` playback flags | 72 B | |
| `+3596` | `color1` | packed RGB (4 B) | |
| `+3600` | `color2` | packed RGB (4 B) | |
| `+3604` | `color3` | packed RGB (4 B) | |
| `+3608` | `color4` | packed RGB (4 B) | |
| `+3612` | `bump_scale` | float | |
| `+3616` | `emit_dur` | float | |
| `+3620` | `emit_dur_adj` | float | |
| `+3624` | `emit_rate` | float | |
| `+3628` | `emit_rate_adj` | float | |
| `+3704` | `emit_delay` | float | |
| `+3708` | `emit_burst` | i32 | Engine clamps to ≥1 post-parse |
| `+3712` | `emit_maxoverride` | i32 | |
| `+3716` | `age` | float | Key string at `off_7DD900` (`"age"`, len 3) |
| `+3720` | `age_adj` | float | |
| `+3724` | `y_offset` | float | |
| `+3728` | `z_offset` | float | |
| `+3732` | `scale` | float | |
| `+3740` | `scale_adj` | float | |
| `+3744` … `+3815` | `scale_func` playback flags | 72 B | |
| `+3816` | `orientation` | vec3 | |
| `+3828` | `orientationadj` | vec3 | |
| `+3840` | `yaw_rot` | float | |
| `+3844` | `yaw_rot_adj` | float | |
| `+3848` | `pitch_rot` | float | |
| `+3852` | `pitch_rot_adj` | float | |
| `+3856` | `roll_rot` | float | |
| `+3860` | `roll_rot_adj` | float | |
| `+3864` | `orbitalspeed` | float | |
| `+3868` | `orbitalspeed_adj` | float | |
| `+3872` | `orbital_axis` | vec3 | Default `{0,1,0}` when missing |
| `+3884` | `spread` | float | |
| `+3888` | `spread_skip` | float | |
| `+3892` | `speed` | float | |
| `+3896` | `speed_adj` | float | |
| `+3900` | `elastic` | float | |
| `+3904` | `gravity` | float | |
| `+3908` | `gravity_mask` | vec3 | |
| `+3920` | `drag` | float | |
| `+3924` | `emit_shape` | i32 | |
| `+3928` | `emit_shape_size` | vec3 | |
| `+3940` | `emit_shape_size_skip` | vec3 | |
| `+3952` … `+4591` | `collide_sound[0..19]` | 32 B each | 20 slots, stride 32 |

Per-graphic-layer (788 B) layout, parsed in the loop at `0x5ee47f`+:

| Offset (within graphic) | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` … `+259` | `texture` | char[260] | e.g. `dirtpuf.tga` |
| `+260` … `+323` | `blend_mode_raw` | char[64] | lowercased; passed to `CParticleDefEntry_ParseBlendMode` |
| `+324` | `blend_mode_id` | i32 | result of ParseBlendMode |
| `+328` | `scale` | float | inherits from particle, then per-graphic override |
| `+332` | `scale_adj` | float | |
| `+336` … `+407` | `scale_func` playback flags | 72 B | qmemcpy'd from particle then per-graphic override parses on top |
| `+408` | `alpha` | float | |
| `+412` … `+483` | `alpha_func` playback flags | 72 B | |
| `+484` … `+555` | `red_func` playback flags | 72 B | |
| `+556` … `+627` | `green_func` playback flags | 72 B | |
| `+628` … `+699` | `blue_func` playback flags | 72 B | |
| `+700` | `color1` | packed RGB | |
| `+704` | `color2` | packed RGB | |
| `+708` | `color3` | packed RGB | |
| `+712` | `color4` | packed RGB | |
| `+716` | `flip_frames` | i32 | Defaults to 1 if not provided |
| `+720` | `flip_rate` | i32 | |

## Per-particle struct (engine `Particle`, stride `emitter[1].field_0x00`)

Offsets confirmed live from `CParticleEmitter_SpawnParticle @ 0x5e7640` and
`CParticleEmitter_UpdateParticles @ 0x5e6980` decompiles, 2026-04-28. Stride
varies by emitter type (`*((_DWORD*)emitter + 63)`); the offsets below are
within the per-particle slot.

| Offset | Field | Type | Source / consumer |
| --- | --- | --- | --- |
| `+0` | `serial` | u8 | `LOBYTE(emitter[1].prevPosZ)++` at spawn; renderer reads for LOD stride `serial / lod_divisor` |
| `+4` | `flags` | u32 | spawn-time bits — see table below |
| `+8` | `packed_color` | DWORD ARGB | random of color1..4 with random alpha; renderer modulates per-channel via LUTs gated on `flags` |
| `+16` | `age` | f32 | initialised to `def.age + def.age_adj * rand10/1024`, decreases each frame by dt unless `def.flags & NeverAge` |
| `+20` | `graphic_def_ptr` | ptr | `def + 148 + 788 * (rand() % graphic_count)` — runtime pointer to chosen graphic layer |
| `+24..+32` | `position` | vec3 | spawn = emitter.pos + def.{y,z}_offset + emit_shape offset; updated `pos += vel*dt` in UpdateParticles |
| `+36..+44` | `velocity` | vec3 | from emit_shape (annular per-axis); updated by gravity/drag/gravitate |
| `+48` | `spread/twist` | f32 | per-particle directional spread accumulator |
| `+52` | `rotation_rate` | f32 | from `def.roll_rot + roll_rot_adj * rand_signed`, scaled by deg2rad |
| `+56` | `scale_velocity` | f32 | `1 / lifetime` so scale reaches 1.0 at end-of-life |
| `+60` | `rotation` | f32 | accumulates from `rotation_rate * dt`; renderer reads (suppressed when YAWANDPITCH set) |
| `+64` | `animation_seed` | f32 | per-particle phase for flipbook UV strip |
| `+68` | initialised flag | u32 | =1 always |

### `Particle::flags` bit table (engine particle+4)

Set in `SpawnParticle @ 0x5e7640` based on per-graphic LUT pointer presence
(curves resolved via `CEffectDef_ResolveAllReferences @ 0x5e9d70`) plus blend
mode and flipbook frame count. The renderer
`BuildBillboardQuads @ 0x5e6d60` only modulates a channel when its bit is set
on the particle.

| Bit | Constant | Set when | Effect at render |
| --- | --- | --- | --- |
| 0x01 | `AlphaCurve` | `graphic+480` (alpha LUT) non-null | alpha modulated by `alpha_lut[age%256]` |
| 0x02 | `RedCurve` | `graphic+552` (red LUT) non-null | red channel modulated |
| 0x04 | `GreenCurve` | `graphic+624` (green LUT) non-null | green channel modulated |
| 0x08 | `BlueCurve` | `graphic+696` (blue LUT) non-null | blue channel modulated |
| 0x10 | `ScaleCurve` | `graphic+404` (scale LUT) non-null | per-particle scale modulated |
| 0x20 | `Flipbook` | `graphic+716` (flip frame count) > 1 | flipbook UV advances over lifetime |
| 0x80 | `LitColor` | `graphic+472` (blend_mode) ∈ {Bump=3, Bumpadd=6} | normal-map vertex coloring path |
| 0x100 | `Distort` | `graphic+472` == Distort=7 | screen-texture distortion path |

Mirrored in our portable runtime as `opennova::particle::particle_runtime_flag::*`
in `libs/particle/include/particle/emitter.h`. The bake step
(`bake_particle_def_curves`) sets `CurveRef::baked = true` so spawn flag
computation reads `layer.<curve>.baked` instead of dereferencing engine
pointers.

## Godot integration (Phase 3, 2026-04-28)

GDExtension wrappers in `godot/engine/particle/` plus modtools workspace in
`godot/modtools/particle/`. The portable simulator drives `NovaParticleEmitter`
(Node3D) → `MultiMeshInstance3D` billboards. Editor exposes effects, particles,
and curve tables via three workflow inspectors with a live SubViewport preview
(FlyCamera + reference grid, mirroring `objects-codex/godot/modtools/object/`).

| Code-loc | Item | Tag/struct/fn | IDA witness | Verdict |
| --- | --- | --- | --- | --- |
| `godot/engine/particle/nova_particle_file.{h,cpp}` | wrapper | `CParticleSystemDef_*` (file-scope) | top-level Resource owns parsed `opennova::particle::ParticleFile`; `load_from_file` / `save_to_file` route through libs/particle. | match (semantic) |
| `godot/engine/particle/nova_particle_def.{h,cpp}` | wrapper | `CParticleEffectDef` (~5204 B) | exposes ~80 fields via inspector groups (Identity / Emission / Lifetime / Visual / Curves / Motion / Sounds + Graphics). flags + move stored as both raw string AND uint32 bitfield (engine has both). | match (semantic) |
| `godot/engine/particle/nova_particle_graphic_layer.{h,cpp}` | wrapper | per-graphic 788 B block (engine offsets +148/+936/+1724/+2512) | inspector enum hint exposes the 8 BlendMode values from CParticleDefEntry_ParseBlendMode @ 0x5e29f0. | match (semantic) |
| `godot/engine/particle/nova_particle_curve_ref.{h,cpp}` | wrapper | curve reference (name + reverse + inverse) | reverse=bit 0x02, inverse=bit 0x01 from CParticleDef_ParseProperties @ 0x5ea320. | match (semantic) |
| `godot/engine/particle/nova_particle_table.{h,cpp}` | wrapper | TableDef LUT | 32 rows × 8 = 256-byte logical curve; `sample(t)` does linear interpolation for editor visualization and future runtime curve evaluation. | match (semantic) |
| `godot/engine/particle/nova_particle_emitter.{h,cpp}` | wrapper | `CParticleEmitter` Node3D | drives MultiMeshInstance3D + ShaderMaterial; runs `opennova::particle::emitter_advance` per-frame. Default StandardMaterial3D (additive) until 8-mode shader lands. | match (semantic) |
| `godot/engine/particle/ptl_resource_format.{h,cpp}` | loader/saver | (no engine analogue — Godot ResourceFormat) | `_get_recognized_extensions()` = `.ptl`; `_load()` returns `NovaParticleFile`; `_save()` round-trips. | match (semantic) |
| `godot/modtools/editor/particle_workspace.gd` | editor adapter | n/a | three workflows (Effects / Particles / Tables); save/save-as/open file actions; mounts ParticlePreview SubViewport into shell `_viewport_lane`. | match (semantic) |
| `godot/modtools/particle/particle_preview.gd` | editor viewport | `CParticleEmitter_BuildBillboardQuads @ 0x5e6d60` (visual surface only) | SubViewport + FlyCamera + grid; one NovaParticleEmitter spawns whatever the inspector selected. | match (semantic) |
| `godot/modtools/particle/shaders/particle_blend_*.gdshader` (8 files) | shader | `CParticleDefEntry_ParseBlendMode @ 0x5e29f0` (blend semantics) | one `.gdshader` per BlendMode (additive/blend/premult/bump/mod/mod2x/bumpadd/distort). Each sets the matching `render_mode` (`blend_add`, `blend_mix`, `blend_premul_alpha`, `blend_mul`) and per-fragment math. Soft-circle fallback when no texture is bound. | match (semantic) — bump/bumpadd/distort use approximations; see Bounded Deviations |

## Runtime — semantic port in `libs/particle/src/emitter.cpp` (Phase 2, 2026-04-27)

This row group is "match (semantic)" — our portable simulator captures the
engine's per-particle behavior (emission, lifetime, integration, RNG resolution)
without claiming byte-exact parity with the DirectX-bound renderer.

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict |
| --- | --- | --- | --- | --- | --- |
| `libs/particle/include/particle/emitter.h::Emitter` | struct | `CParticleEmitter` | runtime instance, ~252 B | parentPos, orient matrix, AABB, particle buffer ptr, stride, spring/drag/damping, force vec — full layout in SpawnParticle decomp | match (semantic) | Our portable struct does NOT mirror byte layout. |
| `libs/particle/src/emitter.cpp::emitter_advance` | runtime-fn | `CParticleEmitter_AdvanceFrame` | `@ 0x5e6570` (size 0x40b) | full decomp 2026-04-27: emit timing (interval = 1/emit_rate), burst loop, expire-by-age | match (semantic) | We collapse AdvanceFrame + UpdateParticles into one function. |
| `libs/particle/src/emitter.cpp::integrate_particle` | runtime-fn | `CParticleEmitter_UpdateParticles` | `@ 0x5e6980` (size 0x3df) | full decomp 2026-04-27: pos+=vel\*dt, vel.y += gravity\*dt, vel-=drag\*dt\*vel, scale+=scale_vel\*dt, age-=dt unless NEVERAGE. Path B revised 2026-04-28: `move & Gravitate` ⇒ delta = pos − emitter.pos, gravity_mask scaled, **D3DXVec3Normalize** (constant-magnitude force), vel += unit_delta * dt * spring. Direction is AWAY from emitter (engine quirk; authors flip via negative gravity_mask). **Kill-plane** at `*(emitter+332)`: `def.flags & 0x08000000` (bit 27) kills if `particle.y > threshold`; `& 0x10000000` (bit 28) kills if `particle.y <= threshold` — engine writes `particle.age = 0` to mark expired (no position clamp). | match (semantic) | Order matches engine (explicit Euler — pos first, then forces). GRAVITATE direction + normalization verified live via `sub_68B032 → off_85072C` thunk. Pinned by `test_gravitate_pushes_away_from_emitter` (constant magnitude across distances), `test_gravitate_negative_mask_inverts_direction` (attractive flip via mask sign), `test_gravitate_respects_zero_mask`. Spring const exposed as `Emitter::spring_const` (default 0 = fall back to `def.gravity`); engine sets the same scalar at emitter+0x308 in `CEffectEmitter_Initialize @ 0x5e6020` at spawn time. Pinned by `test_gravitate_spring_const_overrides_def_gravity`. **Kill-plane** exposed as `Emitter::kill_plane_mode` + `Emitter::kill_plane_y` (mode 0=disabled, 1=KillAbove, 2=KillAtOrBelow). Bits 27/28 are engine-internal — outside the 26-name flag table at 0x846A18 — so neither parser nor corpus carries them; manager populates per spawn site. Pinned by `test_kill_plane_disabled_keeps_particles_alive`, `test_kill_plane_above_kills_when_particle_rises`, `test_kill_plane_below_kills_at_threshold_or_lower` (ctest) + `test_kill_plane_mode_property_clamps_invalid_values` (GUT). |
| `libs/particle/src/emitter.cpp::integrate_particle` (ORBIT branch) | runtime-fn | `CParticleEmitter_UpdateAllParticles` (ORBIT modifier) | `@ 0x5f3be0` (size 0x1224) — `(def.move & 4)` branch | full decomp 2026-04-28: rotate (pos − emitter.pos) and velocity around `def.orbital_axis` via `init_D3DXMatrixRotationAxis(matrix, def.orbital_axis, angle)` then `D3DXVec3TransformCoord`. Engine angle derives from FPU stack chain involving particle.age × emitter basis vectors; we approximate with `def.orbitalspeed * dt` per frame using Rodrigues' formula. | match (semantic) | Pinned by `test_orbit_rotates_around_axis` (full 2π returns near start) + `test_orbit_axis_y_keeps_y_constant`. **Engine bit reorder finding (verified live from raw bytes at 0x848A10/0x848B18/0x848C20)**: the move flag table at 0x848800 stores ORBIT at memory pos 4 with bitmask 0x04, WANDER at memory pos 2 with bitmask 0x08, BUBBLE at memory pos 3 with bitmask 0x10. Our `move_flag::*` constants now reflect this. WANDER and BUBBLE physics paths remain unwitnessed in this UpdateAllParticles dispatch — likely live in a sibling integrator. |
| (none yet) | runtime-fn | `CParticleEmitter_UpdateAllParticles` | `@ 0x5f3be0` (size 0x1224) | combined update (large; orchestrates Update + Submit + Render) | deferred | Renderer-bound; out of scope for the portable simulator. |
| `godot/engine/particle/nova_particle_emitter.cpp::_update_meshes` | runtime-fn | `CParticleEmitter_BuildBillboardQuads` | `@ 0x5e6d60` (size 0x7d7) | full decomp 2026-04-28: vertex 28 B (pos 12 + color 4 + lit color 4 + uv 8), 4 verts/quad, indices 0/1/2/1/3/2; per-particle LOD stride from `defPtrOffset+3F4` divisor; rotation via `D3DXMatrixRotationX(angle)` × parent matrix at emitter+664; LUT byte sample at `(int)(t*256) & 0xFF` from graphic+480/+552/+624/+696/+404; flipbook UV strip from graphic+724 (per-frame rect array, count at +716); manager-level RGB tint at emitter+200..+202 (3 bytes, applied as `(byte * channel) >> 7` per channel — engine byte 128 = 1.0); LOD decimation `divisor = round(1.0 / *(emitter+8 + 0x3F4))` then `if (serial % divisor == 0) render`. | match (semantic) | Godot wrapper builds 4 separate `MeshInstance3D` quads (one per graphic layer), per-emitter depth sort, per-blend-mode `ShaderMaterial` cache, manager-level color tint exposed as `color_tint` Color property (default white = neutral), per-frame UV rects baked via `bake_graphic_uv_rects`, cross-emitter sort handled by Godot's transparent renderer auto-sort, **LOD decimation** exposed as `lod_divisor` Int property (default 1 = render every particle; ≥2 skips particles whose `serial % divisor != 0` at render time — simulator state untouched, matching the engine's render-only check). Pinned by `test_lod_divisor_*` GUT tests + `test_lod_divisor_does_not_affect_simulation` ctest. Atlas baking remains deferred. |
| `godot/engine/particle/nova_particle_emitter.cpp::_update_meshes` (YAWANDPITCH branch) | runtime-fn | `CParticleEmitter_RenderStaticBillboards` | `@ 0x5f4e10` (size 0x80c) | full decomp 2026-04-28: same vertex layout as `BuildBillboardQuads`, but uses `D3DXMatrixScaling` instead of rotation. Selected by `(def.flags & 0x100) == 0 ? rotated : static` dispatch; bit 8 = `YAWANDPITCH`. | match (semantic) | Wrapper uses `effective_rotation = (def.flags & YawAndPitch) != 0 ? 0 : particle.rotation`. Pinned by `test_yaw_and_pitch_skips_billboard_rotation` (effective_rotation == 0) and `test_yaw_and_pitch_clear_keeps_rotation` (effective_rotation > 0 when bit clear). Debug accessor `get_debug_static_billboard()` exposes the branch state. |
| (none yet) | runtime-fn | `CParticleEmitter_BuildOrientationMatrix` | `@ 0x5f3970` (size 0x267) | full decomp 2026-04-28: builds an orthonormal 4×4 matrix at emitter+352 from `def.orbital_axis` (def[+3872..3884]) using cross-product fallback when forward is parallel to `(0,1,0)`. NOT the parent transform — this is the orbital frame consumed by the (deferred) ORBIT move-mode. | match (witness only) | Earlier note speculated this was the parent transform; the live decomp proved otherwise. The portable simulator will need this when ORBIT is implemented. Tracked in open_questions.md as a follow-up. |
| (none yet) | runtime-fn | `CParticleEmitter_TrySubmitForRender` | `@ 0x5e7540` (size 0x33) | submit gate | deferred | Renderer-bound. |
| `libs/particle/src/emitter.cpp::emitter_translate` | runtime-fn | `CParticleEmitter_TranslatePosition` | `@ 0x5efe90` (size 0xad) | full decomp 2026-04-28: `delta = newPos - this[6..8]`; `this[6..8] = newPos`; `this[53..55] += delta` (AABB min); `this[56..58] += delta` (AABB max). Engine seeds the two accumulators from spawn pos in `CEffectEmitter_Initialize @ 0x5e6020`; `UpdateAllParticles @ 0x5f3be0` re-initialises them to ±∞ each frame and rebuilds via per-particle `AABB_ExpandToIncludePoint`. **PositionRelative flag (bit 18 = 0x40000)**: when set on `def.flags`, our `emitter_translate` also shifts every alive `Particle::position` by the same delta, matching the engine's "particles attached to emitter" semantic (default flag clear = particles stay in world space, "left behind" when the emitter moves). | match (semantic) | We expose `Emitter::last_translation_delta` + `Emitter::cumulative_translation` (both Vec3) and the free function `emitter_translate`. AABB tracking itself is deferred (no consumer yet — would feed the cross-emitter sort). Godot wrapper hooks `NOTIFICATION_TRANSFORM_CHANGED` so a parented Node3D's world origin drives the simulator delta in real time. Renderer simulator now world-space by default (`set_as_top_level(true)` on layer meshes; vertices computed with world-space camera basis); `PositionRelative` opts back into local-space by carrying alive particles with the emitter at the simulator level. Pinned by 6 cases in `tests/particle/particle_translate_test.cpp` + 3 GUT tests (`test_emitter_position_change_translates_simulator`, `test_world_space_default_keeps_particles_when_emitter_moves`, `test_position_relative_carries_particles_with_emitter`). |
| `libs/particle/src/emitter.cpp::emit_one_internal` + `apply_emission_shape` | runtime-fn | `CParticleEmitter_SpawnParticle` | `@ 0x5e7640` (size 0xaa9) | full decomp 2026-04-27 + 2026-04-28: emit_shape switch (1=box one-axis dominant + range, 2=sphere annular per-axis lerp(skip,size,rand), 3=cone half-angle around forward + annular per-axis), random color1..4 pick, age = age + age_adj\*rand10, scale_velocity = 1/age, per-particle flags 0x01..0x100 from per-graphic LUT presence + flipbook + blend mode | match (semantic) | Engine RNG resolution `rand() & 0x3FF` mirrored in `emitter_rand10`; we use a portable LCG so test seeds are platform-stable. Annular shape geometry pinned by `tests/particle/particle_emit_shape_test.cpp` (one-axis dominance, OneFrame sign clamp, annular range bounds, cone half-angle). Spawn flag bit table validated by `test_spawn_records_curve_flags` in `particle_emitter_test.cpp`. |
| (none yet) | runtime-fn | `CParticleEmitter_SpawnNewParticle` | `@ 0x5f35b0` (size 0x28c) | wrapper / scheduler around SpawnParticle | deferred | Folded into `emitter_advance`'s emission loop. |
| `libs/particle/src/emitter.cpp::emitter_init` | runtime-fn | `CParticleEmitter_Init` | `@ 0x5419e0` (size 0x86) | emitter ctor / init | match (semantic) | Engine init also handles AnimMap + sound triggers — out of scope for our pure simulator. |
| (none yet) | runtime-fn | `CParticleSystemDef_InitDefaults` | `@ 0x5e14f0` (size 0x3a3) | startup defaults | deferred |
| (none yet) | runtime-fn | `CParticleManager_Construct` | `@ 0x5e87f0` (size 0x1e2) | manager singleton | deferred |
| `libs/particle/src/particle.cpp::bake_atlas_layout` + `nova_particle_emitter.cpp::_rebuild_atlas_texture` | runtime-fn | `CParticleManager_BuildTextureAtlases` | `@ 0x5e8db0` (size 0x44d) | engine bakes textures into a shared atlas + writes a per-graphic UV rect pointer array at graphic+724 (5 floats per rect: u_min, v_max, u_max, v_min, inset). Renderer indexes via `*(int **)(graphic + 724 + 4 * frame)`. | match (semantic) | `bake_atlas_layout` packs all present graphic layers' source textures using a horizontal shelf packer (left-to-right, atlas height = max layer height) and updates each layer's `baked_uv_rects` to atlas coordinates. `_rebuild_atlas_texture` builds the combined `Image` via `Image::blit_rect`, converts to `ImageTexture`, and binds it to all 4 layer materials' `albedo_tex`. Cache-keyed on (width, height, present) per layer. Bounded deviation: engine's exact pack algorithm (shelf vs row vs binary tree) is undecoded; `inset` defaults to 0 (no bleed-prevention padding yet). Pinned by 5 ctest cases in `particle_atlas_test.cpp` (`single_layer_atlas_full_uv_range`, `two_layer_atlas_horizontal_pack`, `atlas_uv_split_by_frames`, `atlas_layout_handles_different_heights`, `absent_layer_keeps_horizontal_strip_default`) and 2 GUT cases (`test_atlas_texture_combines_multiple_layers`, `test_atlas_clears_when_def_unset`). |
| (none yet) | runtime-fn | `CParticleManager_ResolveAllReferences` | `@ 0x5ec850` (size 0xc4) | links effect → pdef → table | deferred |
| Godot transparent renderer (architectural mirror) | runtime-fn | `CParticleManager_RecursiveSortAndRender` | `@ 0x5ec980` (size 0x188) | full decomp 2026-04-28: recursive divide-and-conquer sort over emitter bbox centers, alternating axes (`axisMask` cycles 1→2→4→1) with depth-bin splits; falls back to direct `RenderBatch` when a cluster can't be split further. Performance optimisation for many overlapping emitters; semantic = back-to-front sort by view-space depth. | match (semantic) | Each `NovaParticleEmitter`'s 4 layer meshes are `set_as_top_level(true)` with vertex data in world coordinates. Godot's transparent renderer auto-sorts mesh instances back-to-front by AABB-center view-space depth, achieving the same rendering order without our needing a manager-level coordinator. The recursive divide-and-conquer optimisation is a perf detail — Godot uses QuickSort which is fine for typical scenes (≤ low hundreds of emitters). Pinned by `test_cross_emitter_aabb_centers_at_distinct_world_positions` GUT test. |
| (none yet) | runtime-fn | `CParticleManager_RenderBatch` | `@ 0x5e9890` (size 0x466) | batch draw | deferred — Godot's renderer dispatches the per-MeshInstance3D draw, which corresponds to engine's per-emitter render slot. The engine's shared vertex/index buffer pooling is unimplemented; we use one ArrayMesh per layer per emitter. |
| Godot transparent renderer (architectural mirror) | runtime-fn | `CParticleManager_TransformToViewSpace` | `@ 0x5ecc50` (size 0x31c) | full decomp 2026-04-28: gets device handle + view + projection matrices, projects each child emitter's bbox to view space using the camera basis vectors stored at `this+664`/`+696`/`+700`/`+704`, computes screen-space bounds, builds a sort entry, then calls `RecursiveSortAndRender`. | match (semantic) | Mirrored by Godot's transparent renderer, which projects each `MeshInstance3D`'s world AABB to view space using the active `Camera3D`'s transform — same architectural role. Our world-space rendering setup (top_level=true layer meshes + vertices in world coords) ensures the AABB centers reflect each emitter's actual particle cloud. Pinned by `test_single_emitter_aabb_center_tracks_world_position` GUT test. |
| (none yet) | runtime-fn | `CParticleManager_BeginFrame` | `@ 0x5ecfc0` (size 0xd6) | per-frame begin | deferred |
| (none yet) | runtime-fn | `CParticleManager_FindTableDefByName` | `@ 0x5e9540` (size 0xec) | lookup by id | deferred |
| `libs/particle/src/particle.cpp::bake_curve_lut` + `bake_particle_def_curves` | runtime-fn | `CEffectDef_ResolveTblDefReference` | `@ 0x5e9630` (size 0x4a) | full decomp 2026-04-28: writes `entry+68 = TableDefByName + 328`. The LUT is the tabledef's 32 × 8 byte buffer read row-major as a flat 256-byte array. Renderer indexes `lut[(int)(t*256) & 0xFF]` (no interpolation). | match (semantic) | LUT bake mutates `CurveRef::baked_lut` in `ParticleDef`. `reverse` reads source bytes in reverse index order; `inverse` flips byte values (`255 - src`). Pinned by `tests/particle/particle_curve_lut_test.cpp` (row-major, reverse, inverse, compose, short-table zero-pad, def-bake walk). |
| (none yet) | runtime-fn | `CEffectDef_ResolveAllReferences` | `@ 0x5e9d70` (size 0x28e) | full resolve pass — calls `ResolveTblDefReference` 6× for the 5 particle-level + 1 emit_rate curves + once per graphic layer (5 curves × ≤4 layers); also resolves textures + 20 sound slots. | match (semantic) | Mirrored by `bake_particle_def_curves` (curve resolution only). Texture resolution lives in the Godot wrapper via `texture_path_resolver`; sound resolution remains deferred. |
| (none yet) | runtime-fn | `CEffectDef_AddSubEffect` | `@ 0x5a3020` (size 0x18) | append child to effect | deferred |
| (none yet) | runtime-fn | `CEffectDef_FindByTypeName` | `@ 0x5b01a0` (size 0x34) | lookup by id | deferred |
| (none yet) | runtime-fn | `CEffectDef_Construct` | `@ 0x5b01e0` (size 0x5e) | effectdef ctor | deferred |
| (none yet) | runtime-fn | `CEffectDef_InvokeFactory` | `@ 0x5e19e0` (size 0x4e) | factory dispatch | deferred |
| (none yet) | runtime-fn | `CEffectDef_SetTextureName` | `@ 0x5ef8e0` (size 0x1f) | texture assign | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_Initialize` | `@ 0x5e6020` (size 0x541) | emitter init | deferred |
| `libs/particle/src/emitter.cpp::emitter_advance` (emit_rate scaling) | runtime-fn | `CEffectEmitter_AdvanceEmission` | `@ 0x5e1d30` (size 0x1dc) | full decomp 2026-04-28: when `def.emit_rate_func` resolves to a tabledef, the emission interval is scaled by `lut[(int)(t*256) & 0xFF] / 128.0` per frame. `_EDI[925]` (= def+3700) holds the runtime LUT pointer. Engine byte 128 = 1.0 neutral; 0 = no emission; 255 ≈ 2× faster. | match (semantic) | Pinned by 3 new C++ tests in `particle_emitter_test.cpp`: zero-LUT suppresses emission, neutral LUT (byte 128) matches constant-rate baseline within ±1, LUT 255 doubles emission count. We use `e.age / def.emit_dur` as `t_norm` for finite emit_dur; FOREVEREMIT loops the curve via `e.age - floor(e.age)`. |
| (none yet) | runtime-fn | `CEffectEmitter_SpawnBetweenPositions` | `@ 0x5ea0a0` (size 0xa1) | spawn along a segment | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_Destroy` | `@ 0x5e3460` (size 0x83) | dtor | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_SetOrientationFromDirection` | `@ 0x5e5b00` (size 0x2ae) | orient from facing dir | deferred |
| (none yet) | spawn API | `CEffectWorld_SpawnEmitterAtPosition` | `@ 0x5f6df0` (size 0x182) | top-level spawn entry | deferred |
| (none yet) | spawn API | `CEffect_UpdateEmitterTransform` | `@ 0x5f7410` (size 0x1be) | per-frame transform update | deferred |
| (none yet) | weather | `WeatherParticle_UpdateAllEmitters` | `@ 0x5cb100` (size 0x4b0) | separate weather particle subsystem | deferred |
| (none yet) | weather | `WeatherParticle_LoadTextures` | `@ 0x5de840` (size 0x85) | weather texture load | deferred |
| (none yet) | debug | `Debug_DrawParticleStats` | `@ 0x44c840` (size 0x10e) | debug overlay | deferred |
