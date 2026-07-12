# Particles (.ptl) - format + system RE record

> **Status**: the blueprint-graph workspace + curve tables redesign landed with the runtime
> effect world (`NovaEffectWorld`) on the 2026-07-10 particles train. **Re-grilled
> 2026-07-12 for the extraction-train slice (branch `particles-ida-parity`)**: the SIZE
> model resolved (phase clock + graphic-scale base size + /128 lerped scale LUT), the
> emission shapes corrected to position shells, the full 29-entry flag table dumped, the
> YAWANDPITCH Euler path witnessed, z_offset re-homed to the render-side camera pull.
> This format and witness record is the durable reference.

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

Layout re-witnessed instruction-level 2026-07-12 (the earlier `+48..+64` row
assignments were wrong — there is no spread accumulator, no scale ramp, and no
animation seed in this block):

| Offset | Field | Type | Source / consumer |
| --- | --- | --- | --- |
| `+0` | `serial` | u8 | incremented at spawn; renderer reads for LOD stride `serial % lod_divisor` |
| `+4` | `flags` | u32 | spawn-time bits — table below |
| `+8` | `packed_color` | DWORD ARGB | random pick of graphic color1..4 (`rand & 3`); **alpha byte = `graphic.alpha (+408) × 255`** — authored, not random `[orig: SpawnParticle @ 0x5e77a0]` |
| `+16` | `age` | f32 | init `emitter+0x128 + emitter+0x12C × rand_signed` (the emitter's def-seeded age/age_adj pair; the adj term is **signed**, `rand10/1023 − 0.5` doubled `[orig: @ 0x5e77e2]`); non-positive/non-finite resolved lifetimes are rejected rather than clamped alive; decreases by dt unless `def.flags & NEVERAGE` |
| `+20` | `graphic_def_ptr` | ptr | `def + 148 + 788 * (rand() % graphic_count)` |
| `+24..+32` | `position` | vec3 | spawn = emitter.pos + `(0, y_offset, 0)` + emit-shape offset (`z_offset` is NOT positional — §4 render pull) `[orig: @ 0x5e78a1]`; `pos += vel*dt` per tick |
| `+36..+44` | `velocity` | vec3 | independently bounded yaw/pitch rotations around the emitter forward, each signed magnitude in `[spread_skip, spread]`, × `(speed + speed_adj × rand_signed)` for EVERY shape `[orig: the post-switch vtable-direction receives def+3884/+3888, then def+3892/+3896 multiply]`; updated by gravity/drag/gravitate |
| `+48` | `curve_phase` | f32 | **the LUT index clock**: starts 0 (`+ timeOffset × rate` for mid-frame spawns), advances `+0x34 × dt`, sweeps 0→256 over the life; color LUTs read `lut[(int)phase % 256]`, the scale LUT lerps `[orig: @ 0x5e7898; BuildBillboardQuads @ 0x5e6d60]` |
| `+52` | `phase_rate` | f32 | `256 / age` `[orig: @ 0x5e788c — flt_7D1D70 / age]` |
| `+56` | `base_size` | f32 | `graphic.scale (+328) + graphic.scale_adj (+332) × rand_signed` — the per-particle world-unit draw size, fixed for life `[orig: @ 0x5e7862]` |
| `+60` | `rotation` (roll, deg) | f32 | init `def.orientation.z (+3824) + orientationadj.z (+3836) × rand01` `[orig: @ 0x5e7803]`; accumulates `+0x40 × dt`; deg→rad at render (`× flt_7DCB00`) |
| `+64` | `rotation_rate` (roll, deg/s) | f32 | `roll_rot (+3856) × sign + roll_rot_adj (+3860) × rand_signed`; sign is random `±1` unless `SIGNEDROTATIONS` (0x800000) pins `+1` `[orig: @ 0x5e782a..0x5e7889]` |
| `+68` | initialised flag | u32 | =1 (written by the renderer on first submit) |

Two PARALLEL per-particle arrays ride beside the main buffer (same stride,
same index):

- **`emitter+0x150`** — the YAWANDPITCH Euler channel: `+0` yaw angle =
  `orientation.x (+3816) + orientationadj.x (+3828) × rand01`, `+4` yaw rate =
  `yaw_rot (+3840) × sign + yaw_rot_adj (+3844) × rand_signed` (same
  SIGNEDROTATIONS gate), pitch pair follows `[orig:
  CParticleEmitter_SpawnNewParticle @ 0x5f3663/0x5f36a5]`. The static
  renderer feeds `(aux+0, aux+8, particle+0x3C) × π/180` into the Euler
  matrix `[orig: RenderStaticBillboards @ 0x5f5068]`. Allocated by the
  oriented-emitter init `[orig: @ 0x5f399f]`.
- **`emitter+0xF0`** — per-particle CHILD-EMITTER state (`emitter+0xC` holds
  the child emitter, `+4` its def): spawn scheduling scalars seeded from the
  child def's emit fields; `ONMYDEATH` (0x10) re-seeds the slot with the
  particle's age + the child def age `[orig: SpawnParticle tail
  @ 0x5e7fe2..0x5e80a6]`. Runtime semantics unported (§8).

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

- **`flags` table @ `0x846A18`** — **29 named bits**, dumped raw 2026-07-12 (4-byte count
  preamble, entries at `0x846A1C`, 264 B stride `{bit:u32, name:char[260]}`). The earlier
  26-name reading mislabeled several bits — `POSITIONRELATIVE` is bit 19 (`0x80000`), NOT
  bit 18; bit 18 (`0x40000`) is `FOREVEREMIT`; and the kill-plane bits 27/28 are NAMED
  (`BELOWH20`/`ABOVEH20` — the water-plane cull), not engine-internal. Full table (bit →
  name): 0 `NOVISNOUPDATE`, 1 `INITIALYCLIP`, 2 `NEVERAGE`, 3 `TOPALIGN`, 4 `ONMYDEATH`,
  5 `USEPARENTSCALE`, 6 `USEPARENTCOLOR`, 7 `USEPARENTALPHA`, 8 `YAWANDPITCH`
  (world-oriented Euler quad dispatch), 9 `HAZE`, 10 `GLOBALWIND`, 11 `FOCALWIND`,
  12 `FOCALWINDFORCEAGING`, 13 `COLLIDEBOUNCE`, 14 `COLLIDESLIDE`, 15 `COLLIDEKILL`,
  16 `EMITVECTOR`, 17 `POSITIONINTERPOLATE`, 18 `FOREVEREMIT`, 19 `POSITIONRELATIVE`,
  20 `USEPARENTROTATIONS`, 21 `GFXFLIPRAND`, 22 `CONTROLEDALLIGNMENT` (sic),
  23 `SIGNEDROTATIONS` (pins the random `±1` sign on rotation rates AND the box-shape
  dominant axis `[orig: @ 0x5e790f/0x5e7832]`), 24 `ONEFRAME`, 25 `BURSTDISTRIBUTE`,
  26 `AMBIENTCOLOR`, 27 `BELOWH20`, 28 `ABOVEH20`. Mirrored 1:1 in
  `libs/particle/include/particle/particle.h::particle_flag` (which was already correct —
  cross-witnessed from ParticleEdit_v1_1.exe `@ 0x5ba500`; this dump confirms the JO table).
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
| `CParticleEmitter_AdvanceFrame` | `0x5e6570` (0x40b) | expiry pass runs before `UpdateParticles`, then emit timing (`interval = 1/emit_rate`) and burst work; a particle whose age crosses zero (or whose kill plane writes age=0) remains for that terminal frame and is reclaimed on the next advance | `emitter_advance` (collapses AdvanceFrame + UpdateParticles); rejects non-finite dt/intervals, bounds burst work at capacity, and applies a host hard ceiling of 4096 particles (retail corpus max override 400) |
| `CEffectEmitter_Initialize` | `0x5e6020` | consumes signed 10-bit draws for `emit_dur ± emit_dur_adj` and `emit_rate ± emit_rate_adj`; seeds the shared gravity/GRAVITATE slot as `gravity × -0.09803897` (`flt_7DC738`) and drag slot as `drag × 0.01` | `emitter_init`: `emit_dur_total`, `emit_rate`, `gravity_accel`, `drag_coefficient`; signed-randomized `orbit_speed` also consumes `orbitalspeed_adj` when authored |
| `CParticleEmitter_UpdateParticles` | `0x5e6980` (0x3df) | explicit Euler, position first: `pos += vel×dt`; NORMAL then adds the shared gravity slot to Y (**no gravity_mask**); GRAVITATE normalizes `pos−emitter.pos`, applies `gravity_mask` without renormalizing, then multiplies the same shared slot; drag writes `vel -= drag_slot×dt×vel`; curve phase and age advance. Because Initialize negates/scales authored gravity, positive authored gravity sinks in NORMAL and attracts in GRAVITATE. Kill-plane bits 27/28 write age=0 without clamping position | `integrate_particle`; `spring_const` is an explicit GRAVITATE-only override, otherwise `gravity_accel` is shared; `kill_plane_mode`/`kill_plane_y` supply the site-specific threshold. Pinned by gravity/drag conversion, gravitate-mask, terminal-frame, and kill-plane ctests |
| `CParticleEmitter_UpdateAllParticles` (ORBIT branch) | `0x5f3be0` (0x1224) | `(def.move & 4)`: rotates `(pos − emitter.pos)` and velocity around `def.orbital_axis` via `D3DXMatrixRotationAxis` + `D3DXVec3TransformCoord`; engine angle derives from an FPU chain over particle age × emitter basis | **Approximate:** `integrate_particle` uses emitter-randomized `orbitalspeed ± orbitalspeed_adj`, then `orbit_speed × dt` via Rodrigues. The retail orientation-matrix/particle-age chain remains unported. Pinned by adjustment, radius, and Y-axis tests |
| `CParticleEmitter_SpawnParticle` | `0x5e7640` (0xaa9) | **Re-witnessed instruction-level 2026-07-12.** Every emit_shape displaces spawn POSITION (not velocity): 1 = hollow-box shell, 2 = sphere shell (direction helper gets 360°), 3 = cone shell (fixed 90°). Velocity is seeded AFTER the switch for every shape: vtable direction helper receives `spread` and `spread_skip`, then multiplies by `(speed + speed_adj × rand±)`. Non-positive/non-finite lifetime is rejected. RNG resolution is `rand() & 0x3FF`, normalized **/1023** (`flt_7DC73C`) | `emit_one_internal` + `apply_emission_shape`; direction uses real independently bounded yaw/pitch rotations and preserves two draws; invalid lifetime completes the RNG path but is not inserted. Portable LCG remains deliberate for stable seeds. Pinned by `particle_emit_shape` and `particle_emitter` |
| `CParticleEmitter_TranslatePosition` | `0x5efe90` (0xad) | `delta = newPos − pos`; shifts AABB min/max accumulators. **PositionRelative** (flags bit 19): particles travel with the emitter; default clear = world-space, particles "left behind" | `emitter_translate` + `last_translation_delta`/`cumulative_translation`; Godot wrapper hooks `NOTIFICATION_TRANSFORM_CHANGED`. AABB tracking itself deferred. Pinned by `particle_translate_test.cpp` + GUT tests |
| `CEffectEmitter_AdvanceEmission` | `0x5e1d30` (0x1dc) | when `emit_rate_func` resolves, emission interval is scaled by `lut[(int)(t*256) & 0xFF] / 128.0` per frame (LUT ptr at def+3700); byte 128 = neutral, 0 = no emission, 255 ≈ 2× | `emitter_advance` emit-rate scaling; `t_norm = age/emit_dur`, FOREVEREMIT loops via `age − floor(age)`. Pinned by 3 LUT-rate ctest cases |
| `CEffectDef_ResolveTblDefReference` | `0x5e9630` (0x4a) | writes `entry+68 = TableDefByName + 328`; LUT = the tabledef's 32×8 bytes read row-major as a flat 256-byte array; renderer indexes `lut[(int)(t*256) & 0xFF]`, **no interpolation** | `bake_curve_lut` + `bake_particle_def_curves`; `reverse` reads source in reverse index order, `inverse` flips values (`255 − src`). Pinned by `particle_curve_lut_test.cpp` |
| `CEffectDef_ResolveAllReferences` | `0x5e9d70` (0x28e) | full resolve pass: 6 particle-level curves (5 color/scale + emit_rate) + 5 curves × ≤4 layers + textures + 20 sound slots | `bake_particle_def_curves` (curves only); texture resolution via the Godot wrapper's `texture_path_resolver`; sound resolution deferred |
| `CParticleEmitter_BuildOrientationMatrix` | `0x5f3970` (0x267) | builds an orthonormal 4×4 at emitter+352 from `def.orbital_axis`, cross-product fallback when forward is parallel to `(0,1,0)`. **NOT** the parent transform (earlier speculation refuted by the live decomp) — it is the orbital frame for the ORBIT move mode | needed for full ORBIT frame fidelity; not yet ported |
| `CParticleEmitter_Init` | `0x5419e0` (0x86) | emitter ctor/init (also AnimMap + sound triggers, out of scope) | `emitter_init` |

### Render-side correspondence (Godot host)

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CParticleEmitter_BuildBillboardQuads` | `0x5e6d60` (0x7d7) | vertex 28 B (pos 12 + color 4 + lit color 4 + uv 8), 4 verts/quad, indices 0/1/2/1/3/2; roll matrix (labelled `D3DXMatrixRotationX`, a `(M,f)` FLIRT-collision family) x manager camera matrix (+664) through the PSGP multiply; **quad half-extent = `particle.base_size (+0x38) x (ScaleCurve ? lerp(scaleLUT[i], scaleLUT[i+1], frac(phase)) / 128 : 1) x 0.5`** (`flt_7C3DD4` = 1/128, `flt_7C3B94` = 0.5 `@ 0x5f51ab`-analog; the scale LUT LERPS and reads `lut[i+1]` one byte past the LUT at i=255); color/alpha LUTs read the RAW byte at `(int)phase % 256`, channel x byte / 256, no lerp; **quad center += `emitter+0x140` (= `-def.z_offset`) x the per-frame view-axis globals `flt_2C06578/7C/80`** (`CParticleManager_BeginFrame @ 0x5ecfe8`) — the z_offset camera-ward pull `@ 0x5e71c9`; **flipbook clock = `(256/phase_rate) x flip_rate x phase / 64` = 4 x flip_rate x elapsed-seconds** `@ 0x5e6f17`, GFXFLIPRAND adds a particle-ptr-derived start offset; per-frame UV entry at graphic+724 = **6 dwords `{material_ptr, u_min, v_min, u_max, v_max, inset}`** — inset ADDS on min edges, SUBTRACTS on max; material changes flush + rebind via `CParticleBatch_FlushAndBindMaterial @ 0x5e4230`; manager RGB tint at emitter+200..+202, applied `(byte * channel) >> 7` (byte 128 = 1.0); **LOD decimation** `divisor = round(1.0 / *(emitter+8 + 0x3F4))`, render only when `serial % divisor == 0` (render-only; sim untouched) | `nova_particle_emitter.cpp::_update_meshes` (size/curve/z-offset/flipbook paths rewritten 2026-07-12 to the witnessed model): 4 `MeshInstance3D` quad sets (one per layer), per-emitter depth sort, per-blend-mode `ShaderMaterial` cache, `color_tint` Color property (white = neutral), `lod_divisor` Int property (1..16). Pinned by `test_lod_divisor_*` GUT + `test_lod_divisor_does_not_affect_simulation` ctest + the rewritten rotation/flipbook GUT cases |
| `CParticleEmitter_RenderStaticBillboards` | `0x5f4e10` (0x80c) | same vertex layout; selected by `(def.flags & 0x100) == 0 ? rotated : static` (bit 8 = YAWANDPITCH). **NOT rotation-suppressed: the path renders WORLD-ORIENTED quads** — `(yaw, pitch, roll) = (aux+0, aux+8, particle+0x3C) x pi/180` into the `(M,f,f,f)` Euler builder `@ 0x5f5068..0x5f508d` (the `init_D3DXMatrixScaling` import label is a FLIRT prototype collision — scaling by angle-sized factors would collapse the +-half corners fed through the PSGP corner transforms; the semantics are RotationYawPitchRoll), corners `(+-half, +-half, 0)` transformed then translated by the pulled center. Per-particle yaw/pitch live in the parallel array at `emitter+0x150` (SS2.3) | Euler-basis quad path in `_update_meshes` (`RenderParticle::oriented`); pinned by the rewritten `test_yaw_and_pitch_renders_world_oriented_quads` GUT + `get_debug_static_billboard()` |
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

All rows verdict **match (semantic)**. The ONED workspace (`godot/modtools/particle/`) mounts the
blueprint screen (node graph + live preview) over these wrappers.

### Runtime load & spawn chain (game integration, witnessed 2026-07-10)

The game-side effect world: who loads the `.ptl` set and how effects spawn by name at runtime.
Reimpl: `godot/engine/world/effect_world.gd` (`NovaEffectWorld`, host-side render service — a
dedicated host is headless and never draws) + the `game_world.gd` fx routing.

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CEffectSystem_Init` | `0x5f6070` (0x53f) | called from `Game_StartMission @ 0x524980`; creates `g_EffectWorld` (a `CParticleManager`, 0x454 B) once, registers the static+rotated billboard renderers, texture dir = `<exe>\tga\`; then parses **every** loose `ptl\*.ptl`, the `.ptu`/`.ptg` alternate set (`byte_24D4DF9` selects `.ptg`), and **every** `.ptl`/`.ptu` entry of **every** mounted PFF volume through `CEffectWorld_ParseSectionCallback` — no fixed file list; post-load resolve `sub_5DF7B0(g_EffectWorld, 1)` | `NovaEffectWorld.load_from_resource_root`: every `.ptl` in the mounted root (loose overrides + all PFF volumes via the resource index), tables merged globally (§1.5). Pinned by `effect_world_test.gd` |
| `CEffectWorld_LoadDefinitionFile` | `0x5ecf70` (0x45) | single-file entry: copies the path into two static buffers, then `File_ParseASCIIFile(path, ParseSectionCallback, 710577837)` | per-file `NovaParticleFile.load_from_buffer` |
| `CEffectWorld_InternEffectHandle` | `0x5f7310` (0xfc) | (renamed 2026-07-10 from kong `CEffect_FindOrCreateMaterial` misnomer) interns an effect NAME → stable **1-based handle**: linear `stricmp` scan of the interned pool (`dword_2C25B18`, count `dword_2C25CE0`); miss → `CEffectWorld_FindDefByName` + append; still missing → clone `stockeffect` (vtable+28) under the requested name. WAC `fx` params resolve through this at script compile (`WacScript_ResolveParameter @ 0x4f2920`) | `NovaEffectWorld.intern_effect` (case-insensitive, 1-based, first-registration-wins; unknown names clone `stockeffect` under the requested name — D-PTL-8 CLOSED 2026-07-12) |
| `CEffectWorld_FindDefByName` | `0x5e34f0` | by-name effect lookup over the parsed set | `_effects_by_name` lower-cased map |
| `CEffectWorld_SpawnEmitterAtPosition` | `0x5f6df0` (0x182) | spawn descriptor (14 dwords): +0 flags (bit0/1 = orientation-in-descriptor; bit2 inverted into the spawn call), +4 interned handle (≤0 → +8 name ptr), +12 owner/tag (stored at emitter+0), +16..24 fixed-point position and +28..36 fixed-point orientation (both through `Math_FixedPointToFloat3_YNegated @ 0x611210`), +40 attenuation 16.16, +44 blend 16.16, +48/+52 sample params (action-slot coupling); spawns via `sub_5EA200(g_EffectWorld, 0, def, pos, orient, flag)` | `spawn_effect` / `spawn_effect_by_handle` (Godot-space positions; one `NovaParticleEmitter` per `pdefs` entry, expiry sweep frees finished finite groups) |
| `WacScript_SpawnEffectAtSsnEntity` | `0x4f23a0` (0x13f) | (renamed from kong `WacScript_SpawnSoundAtEntity` — it spawns a particle emitter) WAC `fx2ssn`: resolves the `(pool<<12)\|slot` handle, **detaches any live emitter at entity+460 first**, descriptor at the entity position, orientation = **terrain surface normal** at its grid cell (`outMillis`/`off_849934` tables), new handle → entity+460 | `game_world._route_mission_effects` resolves the live registry SSN, then `spawn_effect_owned`; replacement detaches the previous group, each sweep follows the entity position/forward, and registry removal stops emission while live world-space particles drain. Initial orientation remains up until the terrain-normal read lands (D-PTL-7) |
| `WacScript_SpawnEffectAtTargetMarker` | `0x4f7fd0` (0x122) | (renamed from kong `WacScript_PlaySoundAtEmitter`) WAC `fx2tgt`: pool-3 walk for `itemDef+80 == 6088` (placed target marker, ids 1..99) with the matching target id; same descriptor + entity+460 handle protocol | unrouted: which `.bms` record field carries the target number is unwitnessed (§8) |
| `ActionSlot_SpawnEffect` | `0x401f20` (0x17f) | weapon-action effect spawn (the ACTION block `particle` key = ActionDef+16, a 1-based interned handle): resolves the firing entity through vehicle parent chains, `Entity_ComputeActionTransform @ 0x401310` fills descriptor position/orientation from the action bone, descriptor dwords 12/13 couple the emitter back to the action slot, handle stored at slot+24 for attach mode 2 | partially routed by `LocalPlayerHost`: local FIRE begins spawn the ACTION particle at the named static viewmodel userpoint (camera fallback), pass its direction, suppress scoped first-person flashes except the vehicle-attack proxy, and key the live-group guard by viewmodel generation/action. Live subobject pose, exact vehicle-parent capability/transform, and third-person model sourcing remain open (§8) |

`CEffectDef_FindByTypeName @ 0x5b01a0` / `CEffectDef_Construct @ 0x5b01e0` are **NOT particle
functions** — that family is the `.3DI` model-def cache (`sub_5B6160` appends `.3DI` to the name
before the lookup); a kong naming trap, recorded in §5.5.

Deferred / unported function index (witnessed addresses, no port yet — renderer- or manager-bound):

| Function | Addr | Function | Addr |
| --- | --- | --- | --- |
| `CParticleEmitter_TrySubmitForRender` | `0x5e7540` | `CEffectDef_AddSubEffect` | `0x5a3020` |
| `CParticleEmitter_SpawnNewParticle` | `0x5f35b0` | `CEffectDef_InvokeFactory` | `0x5e19e0` |
| `CParticleSystemDef_InitDefaults` | `0x5e14f0` | `CEffectDef_SetTextureName` | `0x5ef8e0` |
| `CParticleManager_Construct` | `0x5e87f0` | `CEffectEmitter_Initialize` | `0x5e6020` |
| `CParticleManager_ResolveAllReferences` | `0x5ec850` | `CEffectEmitter_SpawnBetweenPositions` | `0x5ea0a0` |
| `CParticleManager_RenderBatch` | `0x5e9890` | `CEffectEmitter_Destroy` | `0x5e3460` |
| `CParticleManager_BeginFrame` | `0x5ecfc0` | `CEffectEmitter_SetOrientationFromDirection` | `0x5e5b00` |
| `CParticleManager_FindTableDefByName` | `0x5e9540` | `WeatherParticle_UpdateAllEmitters` | `0x5cb100` |
| `CEffect_UpdateEmitterTransform` | `0x5f7410` | `WeatherParticle_LoadTextures` | `0x5de840` |
| `Debug_DrawParticleStats` | `0x44c840` | | |

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
  → CParticleBatch_FlushAndBindMaterial @ 0x5e4230 (renamed in the IDB 2026-07-12, §10)
    → CD3DDevice_SetFogAndBlendMode @ 0x677740  (fog states + FOGCOLOR only, §5.5)
    → GfxShader_ApplyPassChecked @ 0x677020 (thunk) → CGfxShader_ApplyPass @ 0x683190
        → apply_texture_stages @ 0x680760       (binds up to 6 textures via SetTexture)
        → GfxBlend_ApplyToDevice @ 0x6817d0     (D3DRS_ALPHABLENDENABLE=27, _SRCBLEND=19, _DESTBLEND=20)
        → RenderState_ApplyToDevice @ 0x681920  (per-stage combiner: D3DTSS_COLOROP=1, _COLORARG1=2,
            _COLORARG2=3, _ALPHAOP=4, _ALPHAARG1=5, _ALPHAARG2=6, _RESULTARG=28, stages 0..5)
        → SetPixelShader / SetVertexShader      (entry +244 = PIXEL shader, +248 = VERTEX shader —
            REN-4 erratum: the original note had the pair transposed; when +244 is set the TSS
            table is SKIPPED and only the blend states apply [orig: CGfxShader_ApplyPass @ 0x683190])
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
`+0` = active stage count (loop bound); `+4/+8/+12` = SRCBLEND / DESTBLEND / ALPHABLENDENABLE
(consumed by `GfxBlend_ApplyToDevice`); stage-0 values at `+16` ALPHAOP, `+24` ALPHAARG1,
`+28` ALPHAARG2, `+32` COLOROP, `+40` COLORARG1, `+44` COLORARG2, `+48` RESULTARG selector
(writes `4*(v!=0) + 1` → 1 = D3DTA_CURRENT or 5 = D3DTA_TEMP); stages 1..5 follow at stride
36 B **from `+52`** (REN-4 erratum — this note previously said +56; the per-stage shape is
`{ALPHAOP, ALPHAARG0, ALPHAARG1, ALPHAARG2, COLOROP, COLORARG0, COLORARG1, COLORARG2,
RESULTARG}`, the ARG0 slots applied only when non-zero — the LERP third arguments). The full
struct is 0xF4 bytes with `+236` = pixel-shader and `+240` = vertex-shader handles, deduped
through a 1024-entry cache (`RenderState_CacheFindOrAdd @ 0x683420`; entries are
`{-1 sentinel, pad, the 0xF4 struct}`, so entry+244/+248 = struct+236/+240). The engine's
mode-word decoders that BUILD these structs are decoded in
[render/render-material-re.md](../render/render-material-re.md).

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
| `CParticleBatch_FlushAndBindMaterial` | `0x5e4230` | (kong `CEffectChannel_PlaySample`; renamed in the IDB 2026-07-12.) Called from the quad builders when the bound material changes: drains pending verts via `GDynamicVB_FlushAndRender`, dispatches blend/fog state by material type, applies the shader pass, then `device->SetTexture(material[4])`. Nothing to do with audio. |
| `CD3DDevice_SetFogAndBlendMode` | `0x677740` | **SetFogStateAndTextureFactor**. Only writes fog render states (D3DRS 35/36/37/38/140) + D3DRS_FOGCOLOR (34), the color picked by the low 2 bits of `mode` from {self-color, gray `0xFF7F7F7F`, black `0xFF000000`, white `0xFFFFFFFF`}. Never touches SRCBLEND/DESTBLEND/ALPHABLENDENABLE. |
| `Render_ResetFogAndBlendState` | `0x589ad0` | **Render_ResetFogState**. Two-step fog reset (`SetFogStateAndTextureFactor(-1)` then `(0)`) + 3 dirty flags. No alpha-blend reset. |
| `CEffect_FindOrCreateMaterial` | `0x5f7310` | **CEffectWorld_InternEffectHandle** (renamed in the IDB 2026-07-10). Interns effect names → 1-based spawn handles; no materials involved. |
| `WacScript_PlaySoundAtEmitter` | `0x4f7fd0` | **WacScript_SpawnEffectAtTargetMarker** (renamed 2026-07-10). The WAC `fx2tgt` handler — spawns a particle emitter at a placed type-6088 target marker; audio-free. |
| `WacScript_SpawnSoundAtEntity` | `0x4f23a0` | **WacScript_SpawnEffectAtSsnEntity** (renamed 2026-07-10). The WAC `fx2ssn` handler — spawns a particle emitter at an SSN entity; audio-free. |
| `CEffectDef_FindByTypeName` / `CEffectDef_Construct` | `0x5b01a0` / `0x5b01e0` | **The `.3DI` model-def cache**, not particles: `sub_5B6160` appends `.3DI` to the name before this lookup and loads via `ThreediGp_LoadFromFile`. The `CEffectDef_*` prefix on this family is a kong trap — particle effect defs resolve through `CEffectWorld_FindDefByName @ 0x5e34f0`. |
| `CGameConfig_SetWindowClassName` | `0x5df8a0` | sets the effect manager's **texture search dir** (`<exe>\tga\` from `CEffectSystem_Init`); nothing to do with window classes. Comment-only — not yet renamed. |
| `CNapiTransport_DetachFromSession` | `0x5f75d0` | detaches a live effect **emitter** from its owner entity (entity+460 handle protocol, called before a respawn in `WacScript_SpawnEffectAtSsnEntity`); not networking. Comment-only — not yet renamed. |

## 6. Visual parity — implemented features

Renderer alignment against the RE render chain (verdicts per §3/§4 tables):

- **Camera-facing billboards** with per-particle rotation, depth-sorted back-to-front per
  emitter `[orig: BuildBillboardQuads @ 0x5e6d60; ComputeViewDepths @ 0x5e7580]`.
- **YAWANDPITCH static billboards** (flags bit 8) — non-rotating quad path
  `[orig: RenderStaticBillboards @ 0x5f4e10]`.
- **8 blend modes** — parsed values select one of 8 dedicated `particle_blend_*.gdshader`
  files (additive / blend / premult / bump / mod / mod2x / bumpadd / distort) with matching
  `render_mode` (`blend_add` / `blend_mix` / `blend_premul_alpha` / `blend_mul`), depth tests,
  and scene fog. A blank or unresolved runtime graphic stays invisible-but-simulating like
  retail; only the editor preview opts into the diagnostic soft-circle fallback
  `[orig: ParseBlendMode @ 0x5e29f0]`.
- **Curve LUT bake** — per-graphic 256-byte LUTs from the 32×8 tabledef, row-major;
  `reverse`/`inverse` baked into the LUT at resolve `[orig: ResolveTblDefReference @ 0x5e9630]`.
  Sampling (re-witnessed 2026-07-12): the per-particle CURVE PHASE (§2.3) indexes
  `lut[(int)phase % 256]`; color/alpha channels read the raw byte (`channel × byte / 256`,
  no interpolation), the SCALE curve lerps between adjacent bytes with the phase fraction
  and normalizes at `/128` (byte 128 = 1.0) `[orig: the flag-0x10 blocks in both render
  paths]`. `bake_particle_def_curves` is wired into the Godot wrapper's `_refresh_emitter`
  (fixed a latent bug where editor-preview spawn flags silently stayed 0 because nothing
  invoked the bake); the renderer samples the baked LUTs directly.
- **The draw-size model** (re-witnessed 2026-07-12) — per-particle base size =
  `graphic.scale ± scale_adj` seeded once at spawn; quad half-extent =
  `0.5 × size × scale-LUT multiplier`; no lifetime ramp `[orig: SpawnParticle @ 0x5e7862;
  BuildBillboardQuads @ 0x5e6d60]`.
- **z_offset camera-ward pull** — the quad center moves toward the camera by
  `def.z_offset` along the view axis at render time (never a spawn offset)
  `[orig: Initialize @ 0x5e6349; BuildBillboardQuads @ 0x5e71c9]`.
- **YAWANDPITCH world-oriented quads** — per-particle (yaw, pitch, roll) Euler state
  seeded from `orientation`/`orientationadj` + the `yaw/pitch_rot` rate pairs, rendered
  as world-space quads `[orig: SpawnNewParticle @ 0x5f3663; RenderStaticBillboards
  @ 0x5f5068]`.
- **`stockeffect` clone at intern** — unknown effect names clone the stock def under the
  requested name (D-PTL-8 closed) `[orig: InternEffectHandle @ 0x5f7310]`.
- **Spawn flag bits** gating per-channel curve modulation (§2.4)
  `[orig: SpawnParticle @ 0x5e7640]`.
- **Emit shapes** — POSITION shells (re-witnessed 2026-07-12): hollow box (dominant axis
  one-sided `[skip/2, size/2]`, others `±size/2`), annular sphere shell
  `lerp(skip, size, rand)` per axis, 90°-cap cone shell; velocity = independently bounded
  `[spread_skip, spread]` yaw/pitch × `speed ± speed_adj` for every shape `[orig: SpawnParticle @ 0x5e7640]`.
- **GRAVITATE** constant-magnitude masked force using the converted shared gravity slot (§4) `[orig: UpdateParticles @ 0x5e6980]`.
- **ORBIT** adjusted-speed Rodrigues approximation around `orbital_axis`; the retail basis/age chain remains open (§4) `[orig: UpdateAllParticles @ 0x5f3be0]`.
- **Kill-plane** modes (§4): authored `BELOWH20` / `ABOVEH20` bind to the active mission
  water height; plus **LOD decimation** (`serial % divisor`, render-only).
- **Cross-emitter spatial sort** via world-space top-level meshes + Godot's transparent
  renderer auto-sort `[orig: TransformToViewSpace @ 0x5ecc50; RecursiveSortAndRender @ 0x5ec980]`.
- **World-space rendering default** (particles left behind when the emitter moves) with
  **PositionRelative** (flags bit 19) opting back into carried particles
  `[orig: TranslatePosition @ 0x5efe90]`.
- **Manager-level RGB tint** (`(byte * channel) >> 7`, byte 128 = 1.0) as `color_tint`; used by
  screen-flash effects `[orig: BuildBillboardQuads @ 0x5e6d60]`.
- **Flipbooks** — `flip_frames`/`flip_rate` select a horizontal UV frame from elapsed lifetime.
- **Atlas packing** — combined per-emitter atlas + per-layer baked UV rects (§4)
  `[orig: BuildTextureAtlases @ 0x5e8db0]`.
- **Emit-rate curves** scaling the emission interval (§4) `[orig: AdvanceEmission @ 0x5e1d30]`.
- **Bounded host pools**: the Godot wrapper defaults an emitter to 256 particles, honors
  authored `emit_maxoverride`, and caps either path at 4096; the retail corpus maximum override
  is 400. The native scheduler also rejects non-finite timing and bounds burst work (D-PTL-12).
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

- **The billboard SIZE formula — RESOLVED 2026-07-12 (was the top open item).**
  The earlier "spawn-pop scale ramp" reading was wrong: particle+0x30/+0x34 is the
  CURVE PHASE clock (§2.3), the base draw size is `graphic.scale ± scale_adj` seeded
  once at spawn (+0x38), and the draw half-extent is
  `0.5 × size × (ScaleCurve ? scaleLUT lerp / 128 : 1)` (§4 render rows). The
  "graphic+0/+8 × 0.0174533" fragment was the YAWANDPITCH Euler ANGLES (yaw/pitch from
  the emitter+0x150 aux array), not size — the `D3DXMatrixScaling` import label is a
  FLIRT prototype collision. z_offset re-homed to the render-side camera pull (ported);
  the flipbook counter decoded as `4 × flip_rate × elapsed` (ported, ×4 and the
  GFXFLIPRAND offset included).
- **fx2tgt target-id record field**: which `.bms` type-6088 record field carries the 1..99
  target number the runtime matches (`WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0`; the
  MED-side picker `Med_ParamTeleportTargetNum @ 0x449b00` lives in `dfx2med.exe`, a different
  image). Blocks routing `fx2tgt` in `game_world.gd`.
- **Scripted-spawn initial orientation**: both WAC handlers orient the descriptor to the terrain
  surface normal at the entity's grid cell (`outMillis` / `off_849934` tables). The host starts
  at up, then its attached fx2ssn group follows the live entity basis; the terrain-normal read
  remains unported (D-PTL-7).
- **`.ptu`/`.ptg` alternate set**: `CEffectSystem_Init @ 0x5f6070` loads `*.ptu` — or `*.ptg`
  when `byte_24D4DF9` is set — alongside `*.ptl`; the selector byte's meaning (gore toggle?)
  is unwitnessed, and the runtime port loads only `.ptl`.
- **Weapon-action effect completion** (muzzle flash chain): local FIRE now consumes the ACTION
  `particle` and `particleuserpoint`, applies scoped first-person suppression, forwards a static
  userpoint/camera direction, and uses a per-viewmodel-generation live handle. Full parity still
  needs the live action-bone/subobject pose from `Entity_ComputeActionTransform @ 0x401310`, the
  exact `Player_IsVehicleHasAttackCapability` predicate/vehicle-parent transform, a third-person
  model source, and descriptor dwords 12/13 attach coupling. Other spawn sites remain unrouted:
  projectile travel/explosions, vehicle physics dust, bone trails, death effects,
  `Weapon_RaycastAndSpawnImpact @ 0x4e8460` impacts, and weather.
- **Descriptor +40/+44 consumers** (attenuation / blend 16.16 fields): the spawner forwards
  them through kong-misnamed calls (`SoundWorld_UpdateChannelAttenuation @ 0x5e5db0`,
  `CEffectWorld_UpdateBlendValues @ 0x5e5df0`); semantics unwitnessed.
- Atlas pack algorithm `[orig: BuildTextureAtlases @ 0x5e8db0]` (shelf vs row vs binary tree)
  and `inset` semantics.
- Lit-color rotation axis convention (§5.3): 4×4 matrix port + side-by-side reference capture
  to validate `D3DXMatrixRotationX`-vs-view-Z.
- Distort stage-1 combiner byte layout (index-8 struct ~`0x7e7858`, sub-struct ptrs
  `0x7e94ec..0x7e94f0`).
- WANDER (`move & 0x08`) and BUBBLE (`move & 0x10`) physics: engine-vestigial in JO retail
  (zero xrefs; never authored). Preserved for round-trip parsing; rendered as NORMAL.
- **Child-emitter chain unported**: `emitter+0xC` (child emitter) + the per-particle
  scheduling array at `emitter+0xF0` (§2.3) and the `ONMYDEATH`/`USEPARENT*` inherit
  family [orig: SpawnParticle tail @ 0x5e7fe2..0x5e80a6] — per-particle sub-effects
  (the def's `child_id`). The effect-level `pdefs` composition IS ported; the
  per-particle spawner is not.
- **EMITVECTOR direction variant**: the spawn velocity helper has two vtable entries
  (+28/+32) selected by EMITVECTOR (0x10000) [orig: @ 0x5e7640 LABEL_45]; the
  witnessed port uses one spread-cone builder for both.
- **BURSTDISTRIBUTE / mid-frame spawn offsets**: SpawnParticle takes a `timeOffset`
  that pre-ages the particle and pre-advances the phase (sub-tick emission accuracy);
  the port spawns whole bursts at frame boundaries.
- **HAZE / GLOBALWIND / FOCALWIND(FORCEAGING) / COLLIDE\*/ TOPALIGN / NOVISNOUPDATE /
  INITIALYCLIP / POSITIONINTERPOLATE / CONTROLEDALLIGNMENT / ONEFRAME** runtime
  consumers: named flags (§2.5) with unwitnessed runtime semantics (ONEFRAME touches
  the renderer age write @ 0x5f4fd3-family; the rest unexplored).
- **YAWANDPITCH pitch pair offsets**: the yaw angle/rate seeds are witnessed at
  aux+0/+4 [orig: @ 0x5f3663/0x5f36a5]; the pitch pair follows the same shape (the
  renderer reads aux+8 as the pitch angle) — the +8/+12 rate layout is inferred from
  the read side, not the full seed disasm.
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
| D-PTL-7 | Scripted-spawn initial orientation (§4 runtime chain): the WAC fx handlers pass the terrain surface normal at the entity's grid cell; the host starts at up, then follows the attached entity basis | **OPEN** — port the terrain-normal read (§8). [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0] |
| D-PTL-8 | Unknown effect name at intern (§4 runtime chain): the engine clones `stockeffect` under the requested name | **CLOSED 2026-07-12** — `NovaEffectWorld.intern_effect` clones the mounted `stockeffect` entry under the requested name (0 only when no stockeffect is mounted). [orig: CEffectWorld_InternEffectHandle @ 0x5f7310] |
| D-PTL-9 | Direction sampling (spawn velocity + sphere/cone shape caps): the host reconstructs the retail helper's independently bounded signed yaw/pitch rotations on a perpendicular basis, including `spread_skip`; it does not call the original orientation-helper vtable (`CEffectEmitter_SetOrientationFromDirection @ 0x5e5b00` family) | **PERMANENT (bounded)** — authored component bounds and two-draw cadence match; portable LCG and exact DirectX/FPU basis construction are not byte-identical. [orig: SpawnParticle @ 0x5e7640] |
| D-PTL-10 | GFXFLIPRAND start offset: the engine derives the per-particle flipbook offset from the particle SLOT POINTER (`(ptr + (ptr>>3)) % frames`); the port derives it from the particle serial | **PERMANENT (bounded)** — same distribution intent; the engine's value is address-dependent and unreproducible by design. [orig: BuildBillboardQuads @ 0x5f4f6e-family] |
| D-PTL-11 | Scale-LUT lerp upper byte: the engine reads `lut[i+1]` unguarded — one byte PAST the 256-byte LUT at i=255 (adjacent heap memory); the port clamps to `lut[255]` | **PERMANENT (bounded)** — the engine's overread value is heap-layout-dependent; clamping bounds the final 1/256th of the curve. [orig: the flag-0x10 lerp block @ 0x5f51ab-analog in both render paths] |
| D-PTL-12 | Emitter pool capacity: the host default is 256 and every authored/direct override is capped at 4096; the exact retail manager-wide ceiling is unwitnessed (shipped corpus maximum override 400) | **PERMANENT (bounded safety)** — keeps substantial authored headroom while preventing hostile rate/burst inputs from allocating or looping without bound. |

WANDER/BUBBLE (engine-vestigial, zero xrefs), the emitter AABB accumulators, the full ORBIT
orientation-matrix/age-chain port, collision sounds, and the pending parser decompiles remain
§8 research items.

## 10. IDB changes (session log)

2026-07-10 session: `CEffectWorld_InternEffectHandle @ 0x5f7310` renamed from kong
`CEffect_FindOrCreateMaterial`; `WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0` /
`WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0` renamed from the kong `*Sound*` misnomers.

2026-07-12 session (the extraction-slice re-grill):

- `CParticleBatch_FlushAndBindMaterial @ 0x5e4230` renamed from kong
  `CEffectChannel_PlaySample` — it is the particle batch flush + material bind
  (VB flush, blend/fog mode by material type, shader pass, `SetTexture`), not audio.
- Witness comments appended at `0x5e7862` (size seed), `0x5e788c` (phase clock),
  `0x5e78a1` (spawn position), `0x5e7900` (box shell), `0x5e77a0` (alpha source),
  `0x5e7803` (roll seed), `0x5e6f17` (flipbook clock), `0x5f5068` (Euler args + the
  FLIRT-collision note), `0x5f3663` (aux yaw seed), `0x5f51ab` (scale-LUT lerp /128),
  `0x5f5296` (z_offset camera pull).
- IDB saved.
