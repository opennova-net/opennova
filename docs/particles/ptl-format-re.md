# Particles (.ptl) - format + system RE record

> **Status**: the particle runtime/editor redesign (blueprint-graph workspace + curve tables) is
> in flight in a worktree. This format and witness record is the durable reference; reimpl code
> locations cited below may move with that redesign.

Consolidated 2026-06-10 from the scratch notes `ptl_format.md`, `ida_particle_witness.md`,
`particle_visual_parity.md`, and `ptl_corpus_catalog.md` (RE passes 2026-04-27/28).

Binary: `Jointops.exe` (retail JO:CA), imagebase `0x400000`; all addresses absolute in that
image. Reimpl: `libs/particle` (portable parser / writer / simulator), `godot/engine/particle`
(GDExtension wrappers), `godot/modtools/particle` (ONED workspace).

---

## 1. Text format

A `.ptl` file is a plain-text effect/particle definition consumed at game-load time via the
same `CConfigReader` machinery as other text configs (e.g. `.def`). Grammar observed across the
77-file retail corpus (JO:CA `.ptl` set) and cross-checked against the parser functions in §3.

### 1.1 Encoding

- **Line endings**: CRLF (`\r\n`). The reimpl parser strips trailing `\r` after splitting on `\n`.
- **Charset**: ASCII. No multi-byte sequences observed.
- **Trailing NUL**: at least four corpus files (`30MM.ptl`, `airexp.ptl`, `ambfx.ptl`,
  `df_exp.ptl`) end with a single `\0` byte. Treated as whitespace/EOF.

### 1.2 Section structure

A file is a sequence of sections:

```
[<section-tag>]
{
    <statement>
    <statement>
    ...
}
```

A section tag and its `{` always live on their own lines. The closing `}` may be followed by an
optional `;` (`boatwake.ptl:45` writes `};`). Blank lines between sections are ignored.

Four tags exist in the binary (string xrefs from the section dispatcher
`[orig: CEffectWorld_ParseSectionCallback @ 0x5ecb40]`):

| Tag | Purpose |
| --- | --- |
| `[effectdef]` | Top-level effect: an `id` and an ordered list of `pdefs` (particle definitions composed into the effect) |
| `[particledef]` | One particle template — emission/physics/visual params + up to 4 graphic layers |
| `[tabledef]` | Curve lookup table (32 rows × 8 `uint8_t` values) referenced by `*_func` keys |
| `[tabledef_edithandles]` | Editor metadata for a `[tabledef]` (handle count, tightness). Runtime ignores |

Tag string addresses in `.rdata`:

| Tag string | Address | Referenced from |
| --- | --- | --- |
| `[effectdef]` | `0x7dd9e8` | dispatcher data ref @ `0x5ecb60` |
| `[particledef]` | `0x7dd9d8` | dispatcher data ref @ `0x5ecb8c` |
| `[tabledef]` | `0x7dd9cc` | dispatcher data ref @ `0x5ecbb8` |
| `[tabledef_edithandles]` | `0x7dccf0` | dispatcher data ref @ `0x5ecbe4`; also `CEffectTableDef_ParseCallback @ 0x5e4020`, `CParticleTableDef_ParseScriptLine @ 0x5e92e4`, and `CParticleDef_ParseProperties @ 0x5ea346` (end-of-section sentinel) |

### 1.3 Statements

Inside `{ ... }`, every non-blank line is:

```
<key> = <value>[, <value> ...] ;
```

Whitespace (tabs and spaces) around `=` and `,` is freely mixed and trimmed.

Value types, inferred from the per-key handlers in
`[orig: CParticleDef_ParseFromConfigMap @ 0x5ed210]`:

| Type | Example | Engine parse call |
| --- | --- | --- |
| identifier (whitespace-allowed) | `id = Buildup dots;` | `CConfigReader_GetString` |
| flag string | `flags = EMITVECTOR AMBIENTCOLOR;` | `CConfigReader_GetString` + `FlagTable_ParseFromString` |
| int | `emit_burst = 1;` | `CConfigReader_GetInt` |
| float | `scale = 2.000;` | `CConfigReader_GetFloat` |
| packed RGB | `color1 = 220, 183, 140;` | `CConfigReader_GetPackedRGB` |
| vec3 | `gravity_mask = 1.000, 1.000, 1.000;` | `CConfigReader_GetVec3` |
| ID list | `pdefs = beginFlash, chunksNbits, dirtCloud;` | string + comma-split |
| curve ref | `scale_func = table12 reverse;` | string; trailing `reverse` toggles direction |
| graphic decl | `graphic1 = mbFlash2.tga, additive;` | filename + blend-mode lowercased |

Keys not recognised by `ParseFromConfigMap` are silently ignored by the engine (no entry in its
dispatch switch). The reimpl parser preserves them in `ParticleDef::unknown_keys` for forward
compatibility instead of failing the parse.

### 1.4 Comments and quoting

- **No comment syntax** — corpus searched for `//`, leading `;`, leading `#`: zero hits.
- **No quoted strings** — values are bare tokens, terminated by `,` or `;`.

### 1.5 Cross-references

The engine resolves these references by string id after parse (see §4,
`CEffectDef_ResolveAllReferences @ 0x5e9d70`):

- `[effectdef].pdefs` → list of `[particledef].id`
- `<particledef>.*_func` (`scale_func`, `alpha_func`, `red_func`, `green_func`, `blue_func`,
  plus per-graphic `g{1..4}_*_func`) → `[tabledef].id`
- `<particledef>.child_id` → another `[particledef].id` (sub-emitter spawn)
- `[tabledef_edithandles].tableid` → `[tabledef].id`

Resolution is global across all loaded files — table-only corpus files (e.g. `table.ptl`,
hosting the shared `table1..table13` set) exist solely to serve `*_func` references from other
files. The reimpl parser stores ids verbatim; the bake step resolves them (§4).

### 1.6 `[effectdef]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `id` | identifier | Unique within file |
| `pdefs` | id list | One or more `[particledef]` ids; emitted in order |

### 1.7 `[particledef]` keys

The full set, transcribed from `[orig: CParticleDef_ParseFromConfigMap @ 0x5ed210]` (call
sequence numbered as in the decompile). Per-graphic-layer keys in §1.8; struct offsets in §2.1.

| # | Key | Type | Engine offset | Notes |
| --- | --- | --- | --- | --- |
| 0 | `id` | identifier | +0 | |
| 1 | `child_id` | identifier | +132 | Optional sub-emitter |
| 2 | `flags` | flag-string | +136 (bits) | flag table @ `0x846A18` |
| 3 | `move` | enum-string | +140 | move table @ `0x848800` |
| 4–7 | `emit_dur`, `emit_dur_adj`, `emit_rate`, `emit_rate_adj` | float | +3616/+3620/+3624/+3628 | |
| 8 | `emit_rate_func` | curve ref | (string) | runtime LUT ptr at +3700 |
| 9 | `emit_delay` | float | +3704 | |
| 10 | `emit_burst` | int | +3708 | Engine clamps `<1` to `1` |
| 11 | `emit_maxoverride` | int | +3712 | |
| 12 | `emit_shape` | int | +3924 | |
| 13 | `emit_shape_size` | vec3 | +3928 | |
| 14 | `emit_shape_size_skip` | vec3 | +3940 | |
| 15 | `y_offset` | float | +3724 | |
| 16 | `z_offset` | float | +3728 | |
| 17 | `age` | float | +3716 | |
| 18 | `age_adj` | float | +3720 | |
| 19 | `scale` | float | +3732 | |
| 20 | `scale_adj` | float | +3740 | |
| 21 | `scale_func` | curve ref | +3744 (LUT) | |
| 22 | `alpha` | float | +3304 | |
| 23 | `alpha_func` | curve ref | +3308 (LUT) | |
| 24 | `red_func` | curve ref | +3380 (LUT) | |
| 25 | `blue_func` | curve ref | +3524 (LUT) | parser order is unusual: red, blue, green, blue (engine quirk; final blue wins) |
| 26 | `green_func` | curve ref | +3452 (LUT) | |
| 27–30 | `color1` … `color4` | packed RGB | +3596/+3600/+3604/+3608 | |
| 31 | `bump_scale` | float | +3612 | |
| 32 | `spread` | float | +3884 | |
| 33 | `spread_skip` | float | +3888 | |
| 34 | `orientation` | vec3 | +3816 | |
| 35 | `orientationadj` | vec3 | +3828 | (no underscore between `orientation` and `adj`) |
| 36–41 | `yaw_rot`, `yaw_rot_adj`, `pitch_rot`, `pitch_rot_adj`, `roll_rot`, `roll_rot_adj` | float | +3840 → +3860 | |
| 42 | `speed` | float | +3892 | |
| 43 | `speed_adj` | float | +3896 | |
| 44 | `elastic` | float | +3900 | |
| 45 | `gravity` | float | +3904 | |
| 46 | `gravity_mask` | vec3 | +3908 | |
| 47 | `drag` | float | +3920 | |
| 48 | `orbitalspeed` | float | +3864 | |
| 49 | `orbitalspeed_adj` | float | +3868 | |
| 50 | `orbital_axis` | vec3 | +3872 | Defaults to `{0,1,0}` when missing |
| 51 (×20) | `collide_sound0` … `collide_sound19` | identifier | +3952, stride 32 | 20 slots |
| 55–83 (×4) | per-graphic block — see §1.8 | | +148/+936/+1724/+2512 | |

The `lod` key (e.g. `lod = 0.000;` in `30MM.ptl:34`) is observed in the corpus but is **not** in
`ParseFromConfigMap`'s dispatch switch — likely consumed by the section-line tokenizer
`CParticleDef_ParseProperties @ 0x5ea320` before the config map is built. The reimpl parser
captures it explicitly on `ParticleDef`, and the engine writer emits it unconditionally (§3).

### 1.8 Graphic layers (`graphicN`, `gN_*`)

Each `[particledef]` may declare up to 4 graphic layers. A layer is opened by:

```
graphic{1,2,3,4} = <texture>, <blend_mode>;
```

`<texture>` may be empty (`graphic1 = , blend;` in `stock.ptl:163`); the runtime renders this as
a blank ("invisible-but-spawning") layer. Subsequent `g{N}_*` keys in the same block configure
the layer:

| Key | Type | Notes |
| --- | --- | --- |
| `g{N}_flip_frames` | int | default 1 |
| `g{N}_flip_rate` | int | default 8 |
| `g{N}_color1` … `g{N}_color4` | packed RGB | per-graphic override; falls back to particle-level |
| `g{N}_alpha` | float | inherited from particle then overridden |
| `g{N}_scale`, `g{N}_scale_adj` | float | inherited then overridden |
| `g{N}_scale_func`, `g{N}_alpha_func`, `g{N}_red_func`, `g{N}_green_func`, `g{N}_blue_func` | curve ref | |

The engine `qmemcpy`s the particle-level color/alpha/scale defaults into each layer slot first,
then per-graphic keys overwrite. The reimpl parser does the same.

### 1.9 `[tabledef]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `id` | identifier | Looked up by `<particledef>.*_func` |
| `tl1` … `tl32` | 8 × `uint8_t` | 256-byte LUT total |

The corpus uses 32 rows × 8 values exactly — verified 77/77 by the corpus smoke test.

### 1.10 `[tabledef_edithandles]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `tableid` | identifier | References a `[tabledef].id` |
| `handlecount` | int | Editor curve handle count |
| `tightness` | int | Editor smoothing param |

Editor-only; the runtime ignores it. No engine writer exists for this section — the write format
is corpus-derived (`boatwake.ptl:40-45`) and round-trip verified.

### 1.11 Validation rules and parse quirks

- `emit_burst < 1` is silently clamped to `1` by the engine. The reimpl parser preserves the
  source value.
- `orbital_axis` defaults to `{0,1,0}` when absent; `flip_frames` defaults to `1` per layer.
- Duplicate keys are last-wins (duplicate `emit_dur` is common, e.g. `30MM.ptl:36` and `:43` —
  an artifact of the engine writer emitting it twice, §3).
- **Blend-mode token quirk** `[orig: CParticleDefEntry_ParseBlendMode @ 0x5e29f0]`: chained
  `strstr` in this exact order — `additive`=1, `blend`=0, `premult`=2, `bump`=3, `mod`=4,
  `mod2x`=5, `bumpadd`=6, `distort`=7. Because `bump` matches before `bumpadd`, a literal
  `bumpadd` token resolves to 3 (Bump), not 6. Replicated by the reimpl parser (otherwise the
  corpus diverges).
- **Curve-ref modifiers** `[orig: CParticleDef_ParseProperties @ 0x5ea320]`: trailing `reverse`
  sets bit `0x02`, trailing `inverse` sets bit `0x01` on the curve reference (e.g. the
  `scale_func` site @ `0x5eafdd`).
- **`gN_colorM` dispatch bug** `[orig: CParticleDef_ParseProperties @ 0x5ea320]`: the engine's
  outer dispatcher remaps `g2_color1`, `g3_color1`, `g3_color2`, `g4_color1`, `g4_color2` into
  higher color slots before they reach the graphic-property handler. The reimpl parser does the
  *correct* mapping (`g{N}_color{M}` → `graphics[N-1].color[M]`) — a recorded, intentional
  divergence.

## 2. Engine structures

### 2.1 `CParticleEffectDef` (heap, ~5204 B)

Offsets confirmed live from the `[orig: CParticleDef_ParseFromConfigMap @ 0x5ed210]` decompile.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` | `id` | std::string (heap) | |
| `+132` | `child_id` | std::string ptr | Sub-particle reference |
| `+136` | `flags` | u32 bitfield | via `FlagTable_ParseFromString` over table @ `0x846A18` |
| `+140` | `move` | u32 enum | via table @ `0x848800` |
| `+148` … `+3299` | `graphic[0..3]` | 4 × 788 B | layers at +148/+936/+1724/+2512 |
| `+3300` | `graphic_count` | u32 | incremented per parsed `graphicN` |
| `+3304` | `alpha` | float | |
| `+3308`/`+3380`/`+3452`/`+3524` | `alpha/red/green/blue_func` playback flags | 72 B each | |
| `+3596` … `+3608` | `color1..4` | packed RGB ×4 | |
| `+3612` | `bump_scale` | float | |
| `+3616` … `+3628` | `emit_dur`, `emit_dur_adj`, `emit_rate`, `emit_rate_adj` | float ×4 | |
| `+3700` | emit-rate LUT ptr | ptr | runtime, set by resolve pass |
| `+3704` | `emit_delay` | float | |
| `+3708` | `emit_burst` | i32 | clamped ≥1 post-parse |
| `+3712` | `emit_maxoverride` | i32 | |
| `+3716`/`+3720` | `age`, `age_adj` | float | key string `"age"` at `off_7DD900` |
| `+3724`/`+3728` | `y_offset`, `z_offset` | float | |
| `+3732`/`+3740` | `scale`, `scale_adj` | float | |
| `+3744` | `scale_func` playback flags | 72 B | |
| `+3816`/`+3828` | `orientation`, `orientationadj` | vec3 | |
| `+3840` … `+3860` | `yaw/pitch/roll_rot` + `_adj` | float ×6 | |
| `+3864`/`+3868` | `orbitalspeed`, `orbitalspeed_adj` | float | |
| `+3872` | `orbital_axis` | vec3 | default `{0,1,0}` |
| `+3884`/`+3888` | `spread`, `spread_skip` | float | |
| `+3892`/`+3896` | `speed`, `speed_adj` | float | |
| `+3900`/`+3904` | `elastic`, `gravity` | float | |
| `+3908` | `gravity_mask` | vec3 | |
| `+3920` | `drag` | float | |
| `+3924`/`+3928`/`+3940` | `emit_shape` (i32), `emit_shape_size`, `emit_shape_size_skip` (vec3) | | |
| `+3952` … `+4591` | `collide_sound[0..19]` | 32 B each | 20 slots, stride 32 |

### 2.2 Per-graphic layer (788 B)

Parsed in the loop at `0x5ee47f`+:

| Offset (within graphic) | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` … `+259` | `texture` | char[260] | e.g. `dirtpuf.tga` |
| `+260` … `+323` | `blend_mode_raw` | char[64] | lowercased; fed to `CParticleDefEntry_ParseBlendMode` |
| `+324` | `blend_mode_id` | i32 | |
| `+328`/`+332` | `scale`, `scale_adj` | float | inherits from particle, then overridden |
| `+336` | `scale_func` playback flags | 72 B | qmemcpy'd from particle then overridden |
| `+408` | `alpha` | float | |
| `+412`/`+484`/`+556`/`+628` | `alpha/red/green/blue_func` playback flags | 72 B each | resolved LUT ptrs land at +480/+552/+624/+696 |
| `+700` … `+712` | `color1..4` | packed RGB ×4 | |
| `+716` | `flip_frames` | i32 | default 1 |
| `+720` | `flip_rate` | i32 | |
| `+724` | per-frame UV-rect ptr array | ptr | written by atlas bake (§4); 5 floats per rect |

### 2.3 Per-particle struct

Offsets confirmed live from `[orig: CParticleEmitter_SpawnParticle @ 0x5e7640]` and
`[orig: CParticleEmitter_UpdateParticles @ 0x5e6980]`. Stride varies by emitter type
(`*((DWORD*)emitter + 63)`); offsets are within the per-particle slot.

| Offset | Field | Type | Source / consumer |
| --- | --- | --- | --- |
| `+0` | `serial` | u8 | incremented at spawn; renderer reads for LOD stride `serial % lod_divisor` |
| `+4` | `flags` | u32 | spawn-time bits — table below |
| `+8` | `packed_color` | DWORD ARGB | random pick of color1..4 with random alpha; renderer modulates per channel via LUTs gated on `flags` |
| `+16` | `age` | f32 | init `def.age + def.age_adj * rand10/1024`; decreases by dt unless `def.flags & NEVERAGE` |
| `+20` | `graphic_def_ptr` | ptr | `def + 148 + 788 * (rand() % graphic_count)` |
| `+24..+32` | `position` | vec3 | spawn = emitter.pos + `{y,z}_offset` + emit-shape offset; `pos += vel*dt` per tick |
| `+36..+44` | `velocity` | vec3 | from emit shape (annular per-axis); updated by gravity/drag/gravitate |
| `+48` | `spread/twist` | f32 | per-particle directional spread accumulator |
| `+52` | `rotation_rate` | f32 | `def.roll_rot + roll_rot_adj * rand_signed`, deg→rad scaled |
| `+56` | `scale_velocity` | f32 | `1 / lifetime` so scale reaches 1.0 at end-of-life |
| `+60` | `rotation` | f32 | accumulates `rotation_rate * dt`; suppressed when YAWANDPITCH set |
| `+64` | `animation_seed` | f32 | per-particle flipbook phase |
| `+68` | initialised flag | u32 | =1 always |

### 2.4 `Particle::flags` bits (particle+4)

Set in `[orig: CParticleEmitter_SpawnParticle @ 0x5e7640]` from per-graphic LUT presence
(resolved by `CEffectDef_ResolveAllReferences @ 0x5e9d70`), blend mode, and flipbook count.
The renderer `[orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60]` only modulates a channel
when its bit is set.

| Bit | Constant | Set when | Effect at render |
| --- | --- | --- | --- |
| 0x01 | `AlphaCurve` | `graphic+480` (alpha LUT) non-null | alpha modulated by `alpha_lut[age%256]` |
| 0x02 | `RedCurve` | `graphic+552` non-null | red channel modulated |
| 0x04 | `GreenCurve` | `graphic+624` non-null | green channel modulated |
| 0x08 | `BlueCurve` | `graphic+696` non-null | blue channel modulated |
| 0x10 | `ScaleCurve` | `graphic+404` non-null | per-particle scale modulated |
| 0x20 | `Flipbook` | `graphic+716` (flip frames) > 1 | flipbook UV advances over lifetime |
| 0x80 | `LitColor` | blend mode ∈ {Bump=3, Bumpadd=6} | lit-color vertex path (§5.3) |
| 0x100 | `Distort` | blend mode == Distort=7 | screen-texture distortion path (§5.4) |

Mirrored as `opennova::particle::particle_runtime_flag::*` in
`libs/particle/include/particle/emitter.h`; the bake step (`bake_particle_def_curves`) sets
`CurveRef::baked` so spawn-flag computation reads baked state instead of engine pointers.

### 2.5 Flag tables

`[orig: FlagTable_ParseFromString @ 0x5df970]` iterates `{bit:u32, name:char[260]}` entries
(264 B stride, 4-byte preamble) and ORs all `strstr`-matched bits. The inverse
`[orig: BuildFlagString @ 0x5df9c0]` emits names in table order, single-space separated, with
one trailing space — replicated exactly so writer output matches engine layout.

- **`flags` table @ `0x846A18`** — 26 named bits (29 slots). Known consumers: `EMITVECTOR`,
  `AMBIENTCOLOR`, `NEVERAGE`, `FOREVEREMIT`, `YAWANDPITCH` (bit 8 = `0x100`, static-billboard
  dispatch), `PositionRelative` (bit 18 = `0x40000`). Kill-plane bits 27/28
  (`0x08000000`/`0x10000000`) are **engine-internal** — outside the named table, populated per
  spawn site by the manager, never authored in the corpus.
- **`move` table @ `0x848800`** — 5 bits, with a memory-order/bitmask reorder verified live from
  raw bytes at `0x848A10`/`0x848B18`/`0x848C20`: ORBIT sits at memory pos 4 with mask `0x04`,
  WANDER at pos 2 with mask `0x08`, BUBBLE at pos 3 with mask `0x10`. WANDER and BUBBLE are
  engine-vestigial in JO retail (zero code xrefs to their rows; never authored in the corpus —
  only NORMAL / GRAVITATE / NORMAL+ORBIT appear).

## 3. Parse / write witness matrix

Verdicts: **match** = layout/grammar verified against IDA; **match (semantic)** = behavior
ported without byte-layout claims; **pending** = not yet decompiled.

| Original | Addr (size) | Behavior witnessed | Reimpl | Verdict |
| --- | --- | --- | --- | --- |
| `CEffectWorld_ParseSectionCallback` | `0x5ecb40` (0x101) | references all 4 section strings; branches tag → per-section parser | `libs/particle/src/parser.cpp` collapses dispatch + per-section into one switch | match (witness) |
| `CEffectTableDef_ParseCallback` | `0x5e4010` (0x1b1) | alternate `[tabledef_edithandles]` path; likely editor-only, not on the runtime load path | — | pending |
| `CParticleDef_ParseProperties` | `0x5ea320` (0x2525) | stricmp dispatch on ~80 keys; `reverse`/`inverse` bits; edithandles sentinel @ `0x5ea346`; `gN_colorM` remap bug (§1.11) | `parser.cpp::apply_particle_key` | match |
| `CParticleDef_ParseFromConfigMap` | `0x5ed210` (0x1da5) | hydrates ~80 named keys → `CParticleEffectDef` (§2.1) | drives the `ParticleDef` field set | match (witness) |
| `CParticleTableDef_ParseScriptLine` | `0x5e92b0` (0x266) | `[tabledef]` line driver | `parser.cpp::apply_table_key`; 32×8 invariant verified 77/77 via smoke test | pending |
| `CParticleTableDef_ParseProperties` | `0x5eefc0` (0x228) | companion property reader | — | pending |
| `CParticleDefEntry_ParseGraphicProperty` | `0x5e3550` (0xabe) | `graphic1` resets idx=0, `graphicN`++ capped at 3; strstr per-key dispatch; reverse/inverse bits per func | graphic decl + `g_*` dispatch in `apply_particle_key` | match |
| `CParticleDefEntry_ParseBlendMode` | `0x5e29f0` (0xc8) | chained strstr; bumpadd→3 quirk (§1.11); `mod` table at `off_7DCBA8` | `particle.cpp::parse_blend_mode` / `blend_mode_name` | match |
| `CParticleTableDef_ParseTransformFlags` | `0x5e2950` (0x39) | tabledef flag bits | — | pending |
| `FlagTable_ParseFromString` | `0x5df970` (0x45) | §2.5 | `particle.cpp::parse_flag_table` | match |
| `BuildFlagString` (`sub_5DF9C0`) | `0x5df9c0` (0x98) | §2.5 | `particle.cpp::format_flag_table` | match |

Writers (round-trip verification gold):

| Original | Addr (size) | Behavior witnessed | Reimpl | Verdict |
| --- | --- | --- | --- | --- |
| `CParticleDef_SaveToFile` | `0x5e4d70` (0x9ef) | field-by-field fprintf, `%5.3f` floats, BGR byte order in memory printed back as R,G,B; writes `emit_dur` **twice** (fprintf sites `0x5e4e6a` + `0x5e4f30`, same field) and `lod` unconditionally | `writer.cpp::write_particle`; round-trip parity via `particle_writer_roundtrip_test` (engine writer uses `\n` only) | match |
| `CParticleEffectDef_WriteToFile` | `0x5e0fe0` (0xc7) | `\tid = %s;`, `\tpdefs = a, b;` (separator `", "` from `word_7CDA14`); space-equals, not tab-equals | `writer.cpp::write_effect` | match |
| `CParticleTableDef_WriteToFile` | `0x5e27e0` (0xee) | 32 fixed rows of 8 `%u`; `\ttlN = ...;`; `id = ` space-equals | `writer.cpp::write_table`; zero-fills if parsed table < 32 rows (defensive) | match |
| (no engine writer for `[tabledef_edithandles]`) | n/a | format corpus-derived (`boatwake.ptl:40-45`) | `writer.cpp::write_handles`; round-trip verified | match (corpus-derived) |

## 4. Runtime witness matrix (simulator)

The portable simulator in `libs/particle/src/emitter.cpp` is **match (semantic)**: it captures
per-particle behavior (emission, lifetime, integration, RNG resolution) without claiming
byte-exact parity with the DirectX-bound renderer. The engine `CParticleEmitter` runtime
instance is ~252 B (parent pos, orient matrix, AABB, particle buffer ptr + stride, spring/drag/
damping, force vec); our struct does not mirror byte layout.

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CParticleEmitter_AdvanceFrame` | `0x5e6570` (0x40b) | emit timing (interval = 1/emit_rate), burst loop, expire-by-age | `emitter_advance` (collapses AdvanceFrame + UpdateParticles) |
| `CParticleEmitter_UpdateParticles` | `0x5e6980` (0x3df) | explicit Euler, pos first then forces: `pos += vel*dt`; `vel.y += gravity*dt`; `vel -= drag*dt*vel`; `scale += scale_vel*dt`; `age -= dt` unless NEVERAGE. **GRAVITATE** (`move & Gravitate`): `delta = pos − emitter.pos`, masked by `gravity_mask`, `D3DXVec3Normalize` (constant-magnitude force; verified via the `sub_68B032 → off_85072C` thunk), `vel += unit_delta * dt * spring`; direction is AWAY from the emitter — authors flip via negative mask components. Spring scalar at emitter+0x308, set in `CEffectEmitter_Initialize @ 0x5e6020`. **Kill-plane** at `*(emitter+332)`: flags bit 27 kills `y > threshold`, bit 28 kills `y <= threshold`; engine writes `age = 0` (no position clamp) | `integrate_particle`; `Emitter::spring_const` (default 0 → fall back to `def.gravity`), `kill_plane_mode`/`kill_plane_y`. Pinned by gravitate + kill-plane ctest cases in `tests/particle/` and GUT clamp tests |
| `CParticleEmitter_UpdateAllParticles` (ORBIT branch) | `0x5f3be0` (0x1224) | `(def.move & 4)`: rotates `(pos − emitter.pos)` and velocity around `def.orbital_axis` via `D3DXMatrixRotationAxis` + `D3DXVec3TransformCoord`; engine angle derives from an FPU chain over particle age × emitter basis | `integrate_particle` ORBIT branch: `orbitalspeed * dt` per frame via Rodrigues' formula. Pinned by `test_orbit_rotates_around_axis`, `test_orbit_axis_y_keeps_y_constant` |
| `CParticleEmitter_SpawnParticle` | `0x5e7640` (0xaa9) | emit_shape switch: 1 = box one-axis dominant + range, 2 = sphere annular per-axis `lerp(skip, size, rand)`, 3 = cone half-angle around forward + annular per-axis; random color1..4 pick; `age = age + age_adj*rand10`; `scale_velocity = 1/age`; flag bits §2.4. RNG resolution `rand() & 0x3FF` | `emit_one_internal` + `apply_emission_shape`; `emitter_rand10` uses a portable LCG for platform-stable seeds. Pinned by `particle_emit_shape_test.cpp` + `test_spawn_records_curve_flags` |
| `CParticleEmitter_TranslatePosition` | `0x5efe90` (0xad) | `delta = newPos − pos`; shifts AABB min/max accumulators (seeded at spawn by `CEffectEmitter_Initialize @ 0x5e6020`; `UpdateAllParticles` re-inits to ±∞ per frame and rebuilds per particle). **PositionRelative** (flags bit 18): particles travel with the emitter; default clear = world-space, particles "left behind" | `emitter_translate` + `last_translation_delta`/`cumulative_translation`; Godot wrapper hooks `NOTIFICATION_TRANSFORM_CHANGED`. AABB tracking itself deferred (no consumer yet). Pinned by `particle_translate_test.cpp` (6 cases) + 3 GUT tests |
| `CEffectEmitter_AdvanceEmission` | `0x5e1d30` (0x1dc) | when `emit_rate_func` resolves, emission interval is scaled by `lut[(int)(t*256) & 0xFF] / 128.0` per frame (LUT ptr at def+3700); byte 128 = neutral, 0 = no emission, 255 ≈ 2× | `emitter_advance` emit-rate scaling; `t_norm = age/emit_dur`, FOREVEREMIT loops via `age − floor(age)`. Pinned by 3 LUT-rate ctest cases |
| `CEffectDef_ResolveTblDefReference` | `0x5e9630` (0x4a) | writes `entry+68 = TableDefByName + 328`; LUT = the tabledef's 32×8 bytes read row-major as a flat 256-byte array; renderer indexes `lut[(int)(t*256) & 0xFF]`, **no interpolation** | `bake_curve_lut` + `bake_particle_def_curves`; `reverse` reads source in reverse index order, `inverse` flips values (`255 − src`). Pinned by `particle_curve_lut_test.cpp` |
| `CEffectDef_ResolveAllReferences` | `0x5e9d70` (0x28e) | full resolve pass: 6 particle-level curves (5 color/scale + emit_rate) + 5 curves × ≤4 layers + textures + 20 sound slots | `bake_particle_def_curves` (curves only); texture resolution via the Godot wrapper's `texture_path_resolver`; sound resolution deferred |
| `CParticleEmitter_BuildOrientationMatrix` | `0x5f3970` (0x267) | builds an orthonormal 4×4 at emitter+352 from `def.orbital_axis`, cross-product fallback when forward is parallel to `(0,1,0)`. **NOT** the parent transform (earlier speculation refuted by the live decomp) — it is the orbital frame for the ORBIT move mode | needed for full ORBIT frame fidelity; not yet ported |
| `CParticleEmitter_Init` | `0x5419e0` (0x86) | emitter ctor/init (also AnimMap + sound triggers, out of scope) | `emitter_init` |

### Render-side correspondence (Godot host)

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CParticleEmitter_BuildBillboardQuads` | `0x5e6d60` (0x7d7) | vertex 28 B (pos 12 + color 4 + lit color 4 + uv 8), 4 verts/quad, indices 0/1/2/1/3/2; rotation via `D3DXMatrixRotationX(angle)` × parent matrix at emitter+664; LUT byte sample `(int)(t*256) & 0xFF` from graphic+480/+552/+624/+696/+404; flipbook UV strip from graphic+724 (count at +716); manager RGB tint at emitter+200..+202, applied `(byte * channel) >> 7` (byte 128 = 1.0); **LOD decimation** `divisor = round(1.0 / *(emitter+8 + 0x3F4))`, render only when `serial % divisor == 0` (render-only; sim untouched) | `nova_particle_emitter.cpp::_update_meshes`: 4 `MeshInstance3D` quad sets (one per layer), per-emitter depth sort, per-blend-mode `ShaderMaterial` cache, `color_tint` Color property (white = neutral), `lod_divisor` Int property (1..16). Pinned by `test_lod_divisor_*` GUT + `test_lod_divisor_does_not_affect_simulation` ctest |
| `CParticleEmitter_RenderStaticBillboards` | `0x5f4e10` (0x80c) | same vertex layout, `D3DXMatrixScaling` instead of rotation; selected by `(def.flags & 0x100) == 0 ? rotated : static` (bit 8 = YAWANDPITCH) | `effective_rotation = (flags & YawAndPitch) ? 0 : particle.rotation`; pinned by 2 GUT tests + `get_debug_static_billboard()` |
| `CParticleEmitter_ComputeViewDepths` | `0x5e7580` | per-particle camera-space depth before batching, back-to-front sort | per-emitter depth sort in `_update_meshes` |
| `CParticleManager_TransformToViewSpace` | `0x5ecc50` (0x31c) | projects each child emitter's bbox to view space via camera basis at this+664/+696/+700/+704, builds sort entries, calls RecursiveSortAndRender | architectural mirror: layer meshes are `set_as_top_level(true)` with world-space vertices, so Godot's transparent renderer projects world AABBs with the active `Camera3D` |
| `CParticleManager_RecursiveSortAndRender` | `0x5ec980` (0x188) | recursive divide-and-conquer sort over emitter bbox centers, alternating axes (`axisMask` cycles 1→2→4→1), depth-bin splits, falls back to direct `RenderBatch`; semantic = back-to-front by view-space depth | Godot transparent-renderer auto-sort (QuickSort) — the recursion is a perf detail for many overlapping emitters. Pinned by cross-emitter AABB GUT tests |
| `CParticleManager_BuildTextureAtlases` | `0x5e8db0` (0x44d) | bakes textures into a shared atlas; writes a per-graphic UV-rect ptr array at graphic+724 — 5 floats per rect: `u_min, v_max, u_max, v_min, inset`; renderer indexes `*(int**)(graphic + 724 + 4*frame)` | `bake_atlas_layout` (horizontal shelf packer: layers left-to-right, atlas height = max layer height) + `_rebuild_atlas_texture` (`Image::blit_rect` → `ImageTexture` bound to all 4 layer materials). Cache-keyed on per-layer (width, height, present). Pinned by 5 ctest + 2 GUT cases. Exact engine pack algorithm undecoded; `inset` = 0 (§7) |

### Godot wrapper correspondence

| Wrapper | Engine analogue | Notes |
| --- | --- | --- |
| `godot/engine/particle/nova_particle_file.{h,cpp}` | file-scope `CParticleSystemDef_*` | top-level Resource owning the parsed `ParticleFile`; load/save route through `libs/particle` |
| `nova_particle_def.{h,cpp}` | `CParticleEffectDef` (§2.1) | ~80 fields via inspector groups; `flags`/`move` stored as both raw string and u32 bitfield (engine has both) |
| `nova_particle_graphic_layer.{h,cpp}` | per-graphic 788 B block (§2.2) | inspector enum exposes the 8 blend modes |
| `nova_particle_curve_ref.{h,cpp}` | curve reference | name + `reverse` (bit 0x02) + `inverse` (bit 0x01) |
| `nova_particle_table.{h,cpp}` | tabledef LUT | 32×8 logical curve; `sample(t)` linearly interpolates for editor visualization only |
| `nova_particle_emitter.{h,cpp}` | `CParticleEmitter` | Node3D driving the portable simulator + per-layer meshes/materials |
| `ptl_resource_format.{h,cpp}` | (no engine analogue) | Godot ResourceFormat loader/saver for `.ptl`, round-trips |

All rows verdict **match (semantic)**. The ONED workspace (`godot/modtools/particle/`) mounts a
SubViewport preview over these wrappers; its shape is owned by the in-flight redesign.

Deferred / unported function index (witnessed addresses, no port yet — renderer- or manager-bound):

| Function | Addr | Function | Addr |
| --- | --- | --- | --- |
| `CParticleEmitter_TrySubmitForRender` | `0x5e7540` | `CEffectDef_AddSubEffect` | `0x5a3020` |
| `CParticleEmitter_SpawnNewParticle` | `0x5f35b0` | `CEffectDef_FindByTypeName` | `0x5b01a0` |
| `CParticleSystemDef_InitDefaults` | `0x5e14f0` | `CEffectDef_Construct` | `0x5b01e0` |
| `CParticleManager_Construct` | `0x5e87f0` | `CEffectDef_InvokeFactory` | `0x5e19e0` |
| `CParticleManager_ResolveAllReferences` | `0x5ec850` | `CEffectDef_SetTextureName` | `0x5ef8e0` |
| `CParticleManager_RenderBatch` | `0x5e9890` | `CEffectEmitter_Initialize` | `0x5e6020` |
| `CParticleManager_BeginFrame` | `0x5ecfc0` | `CEffectEmitter_SpawnBetweenPositions` | `0x5ea0a0` |
| `CParticleManager_FindTableDefByName` | `0x5e9540` | `CEffectEmitter_Destroy` | `0x5e3460` |
| `CEffectWorld_SpawnEmitterAtPosition` | `0x5f6df0` | `CEffectEmitter_SetOrientationFromDirection` | `0x5e5b00` |
| `CEffect_UpdateEmitterTransform` | `0x5f7410` | `WeatherParticle_UpdateAllEmitters` | `0x5cb100` |
| `Debug_DrawParticleStats` | `0x44c840` | `WeatherParticle_LoadTextures` | `0x5de840` |

## 5. Render chain

### 5.1 Vertex format

FVF anchor confirmed via `[orig: GDynamicVB_FlushAndRender @ 0x5e0b10]`: the engine calls
`SetFVF(450)` before `DrawIndexedPrimitive`. `450 = 0x1C2 = D3DFVF_XYZ | D3DFVF_DIFFUSE |
D3DFVF_SPECULAR | D3DFVF_TEX1` — a 2-color + position + 1-uv layout, **universal across all
particle blend modes** (the FVF never changes per mode):

- `+0..+8` position xyz (12 B)
- `+12` primary color (lit color or modulated, per flags) — DIFFUSE
- `+16` secondary color (modulated, full alpha) — SPECULAR
- `+20..+24` uv (8 B) — TEX1
- Stride 28 B/vertex × 4 verts/quad = 112 B/quad

Per-mode visual differentiation comes from how the renderer writes those colors (LitColor branch
vs standard branch in `BuildBillboardQuads`) plus the texture-stage combiner state below.
`D3DRS_SPECULARENABLE` (=29) is never set on the particle render path (no immediate-29 hits in
the `0x5e*`/`0x5f*` range), so the SPECULAR slot is dead fixed-function state — it is purely a
second color carrier for the combiner.

### 5.2 Per-blend-mode render-state binding

Alpha-blend state IS switched per blend mode, through a struct-driven indirection. Call chain
when a particle batch's bound texture changes:

```
CParticleEmitter_BuildBillboardQuads @ 0x5e6d60
  → CEffectChannel_PlaySample @ 0x5e4230        (actually BindRenderStateAndTexture, §5.5)
    → CD3DDevice_SetFogAndBlendMode @ 0x677740  (fog states + FOGCOLOR only, §5.5)
    → sub_677020 (thunk) → sub_683190 @ 0x683190
        → sub_680760 @ 0x680760                 (binds up to 6 textures via SetTexture)
        → GfxBlend_ApplyToDevice @ 0x6817d0     (D3DRS_ALPHABLENDENABLE=27, _SRCBLEND=19, _DESTBLEND=20)
        → RenderState_ApplyToDevice @ 0x681920  (per-stage combiner: D3DTSS_COLOROP=1, _COLORARG1=2,
            _COLORARG2=3, _ALPHAOP=4, _ALPHAARG1=5, _ALPHAARG2=6, _RESULTARG=28, stages 0..5)
        → SetVertexShader / SetPixelShader      (material +244 / +248, vtable+428 / +368)
```

The `sample` argument is read from the per-frame UV-rect slot at `graphic+724`. Each frame entry
holds a pointer to a **sample (material) struct** + the 5-float UV rect:

| Sample offset | Field |
| --- | --- |
| `+0` | blend-mode id |
| `+4` | primary render-state ptr |
| `+8` | secondary render-state ptr (compound modes, e.g. Distort) |
| `+12` | global flag dword (→ `dword_3266E8C`) |
| `+16` | vertex format / FVF code |

**Render-state struct** layout (per-DWORD access pattern of `RenderState_ApplyToDevice`):
`+0` = active stage count (loop bound); stage-0 values at `+16` ALPHAOP, `+24` ALPHAARG1,
`+28` ALPHAARG2, `+32` COLOROP, `+40` COLORARG1, `+44` COLORARG2, `+48` RESULTARG selector
(writes `4*(v!=0) + 1` → 1 = D3DTA_CURRENT or 5 = D3DTA_TEMP); stages 1..5 follow at stride
36 B from `+56` in the same shape.

The per-blend-mode static structs live in read-only data starting around `0x7e7558` — an
indexed array (blend modes 1..8), each entry a chain ptr + blend-mode index dword + ~15 dwords
of stage state referencing sub-structs at `0x7e94e0..0x7e9510`. Confirmed by pattern search:
the Mod2x signature `SrcBlend=9 (DESTCOLOR), DestBlend=3 (SRCCOLOR)` (bytes
`09 00 00 00 03 00 00 00`) lands at `0x7e7814`, inside the index-6 (Mod2x) struct.

**Confirmed not used**: `D3DTOP_BUMPENVMAP` (22) and `D3DTOP_BUMPENVMAPLUMINANCE` (23) appear
only as string-length compare immediates inside the parser
(`CParticleDef_ParseFromConfigMap @ 0x5ed210`) — never in the render path. So Bump and Bumpadd
use **plain fixed-function combiners** (most likely `D3DTOP_MODULATE` with `COLORARG1=TEXTURE`,
`COLORARG2=DIFFUSE`), not D3D9 bump-mapping ops. This closed the bump/bumpadd combiner
"RE-blocker": no exotic combiner mode is in play, and the engine's per-pixel output for Bump is
`texture × lit_color`.

### 5.3 Bump / Bumpadd lit-color path

The "bump" lighting effect = 2-color vertex format + fixed-function combiner. Hard data:

**Light direction** (`flt_848D34/D38/D3C @ 0x848D34`, raw bytes
`46 B6 13 BF / 46 B6 13 BF / 46 B6 13 3F`): `(-0.5773503, -0.5773503, +0.5773503)` =
`(-1, -1, +1)/√3`. This is the engine's hardcoded global directional light for ALL bump-shaded
particles — no per-emitter or per-spawn override.

**Lit-color computation** in `[orig: BuildBillboardQuads @ 0x5e6d60]` when
`particle.flags & 0x80` (LitColor) is set:

1. Compose a per-particle rotation matrix: `D3DXMatrixRotationX(particle.rotation_radians)` ×
   the parent matrix at emitter+664. Rotation is stored in degrees and converted via
   `flt_7DCB00 = π/180`.
2. Invert via transpose (`sub_68BF44 → off_8507A8` IAT thunk = `D3DXMatrixTranspose`, internal
   mirror @ `0x68bf4a`).
3. Build the light direction in particle-local space: each component =
   `bump_scale × (negated flt_848D34/D38, positive flt_848D3C)`, transformed by the inverse
   rotation (`sub_68B52B` = `D3DXVec3Transform`).
4. Encode each component to a byte: `clamp((value + 1.0) × 0.5, 0, 1) × 255`.
5. Primary vertex color = `(modulated.alpha << 24) | (bx << 16) | (by << 8) | bz` — RGB replaced
   by the encoded light direction, alpha kept from the modulated color.
6. Secondary vertex color = `modulated_RGB | 0xFF000000` — raw modulated RGB, full alpha.

Bump (mode 3) then modulates `texture × lit_color`; Bumpadd (mode 6) composes additively.

**Reimpl** (`nova_particle_emitter.cpp::_update_meshes` LitColor branch +
`particle_blend_bump.gdshader` / `particle_blend_bumpadd.gdshader`): builds the particle local
frame from the camera basis + `rp.rotation` (rotating right/up around the view direction),
inverse-transforms the engine light direction via dot products, scales by `bump_scale`, encodes
per channel. Per-channel `lit_color` varies with rotation (pinned by
`test_lit_color_channels_differ_at_nonzero_bump_scale`,
`test_lit_color_varies_with_particle_rotation`). Our simulator stores radians, so the engine's
degree→radian conversion is skipped. **Bounded deviation**: the engine rotates around the X axis
of a composite view-space matrix (`D3DXMatrixRotationX`); we rotate around the view direction
(Z of our billboard frame). Both produce direction-dependent variation, but exact per-channel
values differ — closing needs a 4×4 matrix port + axis-convention RE + side-by-side capture.

### 5.4 Distort (blend mode 7)

Uses up to 2 textures via the secondary render-state pointer at `sample+8` (selected when the
`this+276` flag is set); the second texture is bound to stage 1 by `sub_680760`. The stage-1
combiner setup lives in the index-8 static struct at ~`0x7e7858` (sub-struct ptrs
`0x7e94ec..0x7e94f0`) — byte layout undecoded. Our shader falls back to a fixed-strength
screen-texture UV offset, matching the visual intent (distort the underlying buffer by the
alpha gradient) without byte-exact stage state.

### 5.5 Kong-rename corrections (durable warnings)

These functions in the particle render path carry misleading kong names; do not re-trip:

| Kong name | Address | Actual purpose |
| --- | --- | --- |
| `CEffectChannel_PlaySample` | `0x5e4230` | **BindRenderStateAndTexture**. Called from `BuildBillboardQuads` when the bound texture changes: drains pending verts via `GDynamicVB_FlushAndRender`, dispatches fog state by `*sample`, then `device->SetTexture(sample[4])`. Nothing to do with audio. |
| `CD3DDevice_SetFogAndBlendMode` | `0x677740` | **SetFogStateAndTextureFactor**. Only writes fog render states (D3DRS 35/36/37/38/140) + D3DRS_FOGCOLOR (34), the color picked by the low 2 bits of `mode` from {self-color, gray `0xFF7F7F7F`, black `0xFF000000`, white `0xFFFFFFFF`}. Never touches SRCBLEND/DESTBLEND/ALPHABLENDENABLE. |
| `Render_ResetFogAndBlendState` | `0x589ad0` | **Render_ResetFogState**. Two-step fog reset (`SetFogStateAndTextureFactor(-1)` then `(0)`) + 3 dirty flags. No alpha-blend reset. |

## 6. Visual parity — implemented features

Renderer alignment against the RE render chain (verdicts per §3/§4 tables):

- **Camera-facing billboards** with per-particle rotation, depth-sorted back-to-front per
  emitter `[orig: BuildBillboardQuads @ 0x5e6d60; ComputeViewDepths @ 0x5e7580]`.
- **YAWANDPITCH static billboards** (flags bit 8) — non-rotating quad path
  `[orig: RenderStaticBillboards @ 0x5f4e10]`.
- **8 blend modes** — parsed values select one of 8 dedicated `particle_blend_*.gdshader`
  files (additive / blend / premult / bump / mod / mod2x / bumpadd / distort) with matching
  `render_mode` (`blend_add` / `blend_mix` / `blend_premul_alpha` / `blend_mul`); soft-circle
  fallback when no texture is bound `[orig: ParseBlendMode @ 0x5e29f0]`.
- **Curve LUT bake** — per-graphic 256-byte LUTs from the 32×8 tabledef, row-major, sampled
  `lut[(int)(t*256) & 0xFF]` with no interpolation; `reverse`/`inverse` baked into the LUT so
  the runtime read is a single byte lookup `[orig: ResolveTblDefReference @ 0x5e9630]`.
  `bake_particle_def_curves` is wired into the Godot wrapper's `_refresh_emitter` (fixed a
  latent bug where editor-preview spawn flags silently stayed 0 because nothing invoked the bake).
- **Spawn flag bits** gating per-channel curve modulation (§2.4)
  `[orig: SpawnParticle @ 0x5e7640]`.
- **Emit shapes** — box (one-axis dominant), annular sphere `lerp(skip, size, rand)` per axis,
  cone half-angle around forward `[orig: SpawnParticle @ 0x5e7640]`.
- **GRAVITATE** constant-magnitude repulsive force (§4) `[orig: UpdateParticles @ 0x5e6980]`.
- **ORBIT** Rodrigues rotation around `orbital_axis` (§4) `[orig: UpdateAllParticles @ 0x5f3be0]`.
- **Kill-plane** modes (§4) and **LOD decimation** (`serial % divisor`, render-only).
- **Cross-emitter spatial sort** via world-space top-level meshes + Godot's transparent
  renderer auto-sort `[orig: TransformToViewSpace @ 0x5ecc50; RecursiveSortAndRender @ 0x5ec980]`.
- **World-space rendering default** (particles left behind when the emitter moves) with
  **PositionRelative** (flags bit 18) opting back into carried particles
  `[orig: TranslatePosition @ 0x5efe90]`.
- **Manager-level RGB tint** (`(byte * channel) >> 7`, byte 128 = 1.0) as `color_tint`; used by
  screen-flash effects `[orig: BuildBillboardQuads @ 0x5e6d60]`.
- **Flipbooks** — `flip_frames`/`flip_rate` select a horizontal UV frame from elapsed lifetime.
- **Atlas packing** — combined per-emitter atlas + per-layer baked UV rects (§4)
  `[orig: BuildTextureAtlases @ 0x5e8db0]`.
- **Emit-rate curves** scaling the emission interval (§4) `[orig: AdvanceEmission @ 0x5e1d30]`.
- Finite preview emitters do not auto-repeat after all particles expire; FOREVEREMIT keeps the
  emitter eligible for continuous spawning. Loose-texture lookup routes through the shared
  texture path resolver (PFF lookup intentionally out of scope).

### Bounded deviations

- **Batching** `[orig: CParticleManager_RenderBatch @ 0x5e9890]`: one mesh batch per graphic
  layer instead of the engine's shared vertex/index buffer pooling.
- **mod2x** approximates `SrcBlend=DESTCOLOR + DestBlend=SRCCOLOR` by pre-multiplying source
  RGB ×2 in fragment over Godot's `blend_mul`; the darken/brighten-around-0.5 midpoint is
  preserved, the gamma curve is not byte-exact.
- **bump / bumpadd** lit-color axis convention (§5.3): rotation about view-Z instead of the
  engine's composite-matrix X. Combiner topology confirmed matching.
- **distort**: fixed-strength screen-tex UV offset; engine stage-1 combiner bytes undecoded (§5.4).
- **Atlas pack algorithm**: engine layout strategy (shelf vs row vs binary tree) undecoded; our
  shelf packer matches the UV-rect data shape. `inset` (bleed padding) defaults to 0 — would be
  `0.5 / atlas_dim` if artifacts appear in dense scenes.

## 7. Corpus

77 retail `.ptl` files (JO:CA install), ~3.0 MB total: **495 `[effectdef]`, 1405
`[particledef]`, 495 `[tabledef]`, 251 `[tabledef_edithandles]`** sections.

Notable outliers:

- **Largest**: `ambfx.ptl` (280 KB; 55/142/33/20), `df_exp.ptl` (211 KB; 33/115/9/0),
  `veh_joexp.ptl` (200 KB; 30/118/7/6).
- **Table-only files** (zero effects/particles — shared curve libraries proving cross-file id
  resolution, §1.5): `blduptab.ptl`, `boatwake.ptl`, `table.ptl` (the generic
  `table1..table13` set), `troytabl.ptl`.
- `waterExp.ptl` carries **more edithandles (26) than tabledefs (20)** — orphaned editor
  metadata, tolerated by the parser.
- `stock.ptl` declares 7 effects over only 2 particledefs and contains the empty-texture
  graphic decl (`graphic1 = , blend;` line 163).
- Trailing-NUL files (§1.1): `30MM.ptl`, `airexp.ptl`, `ambfx.ptl`, `df_exp.ptl`.

The full per-file catalog (sections + first ids per file) is a mechanical section-count scan
over the retail `.ptl` set and can be regenerated from retail data on demand; it is not
carried here.

## 8. Open RE work

- Atlas pack algorithm `[orig: BuildTextureAtlases @ 0x5e8db0]` (shelf vs row vs binary tree)
  and `inset` semantics.
- Lit-color rotation axis convention (§5.3): 4×4 matrix port + side-by-side reference capture
  to validate `D3DXMatrixRotationX`-vs-view-Z.
- Distort stage-1 combiner byte layout (index-8 struct ~`0x7e7858`, sub-struct ptrs
  `0x7e94ec..0x7e94f0`).
- WANDER (`move & 0x08`) and BUBBLE (`move & 0x10`) physics: engine-vestigial in JO retail
  (zero xrefs; never authored). Preserved for round-trip parsing; rendered as NORMAL.
- Pending parser decompiles: `CEffectTableDef_ParseCallback @ 0x5e4010`,
  `CParticleTableDef_ParseScriptLine @ 0x5e92b0`, `CParticleTableDef_ParseProperties @ 0x5eefc0`,
  `CParticleTableDef_ParseTransformFlags @ 0x5e2950`.
- Emitter AABB accumulators (§4, `TranslatePosition`) — unimplemented in the portable simulator;
  no consumer until a manager-level cross-emitter sort is needed.
- `CParticleEmitter_BuildOrientationMatrix @ 0x5f3970` port for full ORBIT frame fidelity.
- Collision sounds (20 `collide_sound` slots) and `elastic` bounce behavior — resolve pass
  witnessed, runtime unported.

## 9. Divergence catalog (D-PTL)

Stable IDs for the behavior gaps described above (the §1 intentional parse divergence and the
§6 bounded deviations); dispositions in the canonical vocabulary of
[divergence-ledger.md](../divergence-ledger.md). Pure "not yet researched" items with no
witnessed behavior gap stay in §8.

| ID | Divergence | Disposition |
|---|---|---|
| D-PTL-1 | `g{N}_color{M}` dispatch (§1): the engine's outer dispatcher remaps `g2_color1`/`g3_color1`/`g3_color2`/`g4_color1`/`g4_color2` into higher color slots; the reimpl maps `g{N}_color{M}` → `graphics[N-1].color[M]` correctly | **PERMANENT** — a recorded intentional divergence from an original parse bug. [orig: CParticleDef_ParseProperties @ 0x5ea320] |
| D-PTL-2 | Batching (§6): one mesh batch per graphic layer instead of the engine's shared vertex/index buffer pooling | **PERMANENT** — a host renderer architecture choice, visually equivalent. [orig: CParticleManager_RenderBatch @ 0x5e9890] |
| D-PTL-3 | `mod2x` (§6): approximates `SrcBlend=DESTCOLOR + DestBlend=SRCCOLOR` by pre-multiplying source RGB ×2 over Godot `blend_mul`; the 0.5 midpoint is preserved, the gamma curve is not byte-exact | **OPEN** (approximation) |
| D-PTL-4 | `bump`/`bumpadd` lit-color axis (§5.3, §6): rotation about view-Z instead of the engine's composite-matrix X (combiner topology confirmed matching) | **OPEN** — pending the 4×4 matrix port + a side-by-side reference capture (§8). |
| D-PTL-5 | `distort` (§5.4, §6): fixed-strength screen-tex UV offset; the engine stage-1 combiner bytes are undecoded | **NEEDS-RE** — decode the index-8 combiner layout (§8). |
| D-PTL-6 | Atlas pack (§6): the engine layout strategy (shelf vs row vs binary tree) is undecoded; our shelf packer matches the UV-rect data shape; `inset` bleed padding defaults to 0 | **NEEDS-RE** [orig: BuildTextureAtlases @ 0x5e8db0] |

WANDER/BUBBLE (engine-vestigial, zero xrefs), the emitter AABB accumulators, the ORBIT
orientation-matrix port, collision sounds, and the pending parser decompiles remain §8 research
items — no witnessed behavior gap in the shipped render/format port.
