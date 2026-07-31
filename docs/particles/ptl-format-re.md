# Particles (.ptl) - format + system RE record

> **Status**: the blueprint-graph workspace + curve tables redesign landed with the runtime
> effect world (`NovaEffectWorld`) on the 2026-07-10 particles train. **Re-grilled
> 2026-07-12 for the extraction-train slice (branch `particles-ida-parity`)**: the SIZE
> model resolved (phase clock + graphic-scale base size + /128 lerped scale LUT), the
> emission shapes corrected to position shells, the full 29-entry flag table dumped, the
> YAWANDPITCH Euler path witnessed, z_offset re-homed to the render-side camera pull.
> **Deep-dive grill 2026-07-13 (the PR #237 weapon/vehicle particle pass)**: the recoil-row
> casing leg, the ADS settle gate (the `g_weaponScopeActive` promoter), the muzzle
> suppression window (the `ActionSlot_ClearEffectHandle` group-death callback), the ammo
> `effects_table` impact chain, and the ITEMS.DEF per-item userpoint effects (`particlefx*`)
> witnessed. Their event/resolution pipelines and generic transient presentation are routed;
> remaining impact-tag fidelity is D-WPN-15 and the Knife-only instant-kill-zone family is
> D-WPN-16.
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

The dispatcher compares section tokens with `_stricmp`; the per-section property parsers do
the same for known keys. Section tags, `graphicN`/`gN_*` keys, table rows, and edit-handle keys
are therefore ASCII case-insensitive. A scan of the 77-file retail corpus found no mixed-case
section tags or known keys, so this matters to tolerant/mod-authored input rather than shipped
content. The reimpl folds known tokens while preserving the authored spelling of unknown keys.

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
| 48 | `orbitalspeed` | float | +3864 | Authored degrees/second |
| 49 | `orbitalspeed_adj` | float | +3868 | Signed adjustment in degrees/second |
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
- **Curve-ref modifiers** `[orig: CParticleDef_ParseProperties @ 0x5ea320]`: one trailing token
  is consumed; `reverse` sets bit `0x02`, `inverse` sets bit `0x01` on the curve reference (e.g.
  the `scale_func` site @ `0x5eafdd`). The retail writer's suffix helper instead emits bit `0x01`
  as `invert`, then bit `0x02` as `reverse` (`CurveRef_ModifierSuffix @ 0x42bf60`). Our writer now
  matches that spelling and order; the parser accepts both `invert` and `inverse` so its own output
  round-trips. It deliberately composes both trailing modifiers in either order while retail
  consumes only the last token. Shipped files use at most one modifier, so that remaining
  difference is a mod-syntax superset (D-PTL-20).
- **Flip-frame bounds**: retail carries the authored `flip_frames` count into its frame registrar
  without the reimpl's normalization. The reimpl forces non-positive counts to `1` and caps larger
  counts at `256` across parse, bake, preview, and runtime seams to bound allocation/work
  (D-PTL-19). The shipped corpus does not approach the cap.
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
| `CEffectWorld_ParseSectionCallback` | `0x5ecb40` (0x101) | references all 4 section strings; `_stricmp` branches tag → per-section parser | `libs/particle/src/parser.cpp` folds all four section tokens before one switch | match (witness; mixed-case contract) |
| `CEffectTableDef_ParseCallback` | `0x5e4010` (0x1b1) | alternate `[tabledef_edithandles]` path; likely editor-only, not on the runtime load path | — | pending |
| `CParticleDef_ParseProperties` | `0x5ea320` (0x2525) | `_stricmp` dispatch on ~80 keys; one trailing `reverse`/`inverse` token; edithandles sentinel @ `0x5ea346`; `gN_colorM` remap bug (§1.11) | `parser.cpp::apply_particle_key`; known keys folded, unknown spelling retained | match except the documented dual-modifier superset (D-PTL-20) |
| `CParticleDef_ParseFromConfigMap` | `0x5ed210` (0x1da5) | hydrates ~80 named keys → `CParticleEffectDef` (§2.1) | drives the `ParticleDef` field set | match (witness) |
| `CParticleTableDef_ParseScriptLine` | `0x5e92b0` (0x266) | `[tabledef]` line driver | `parser.cpp::apply_table_key`; 32×8 invariant verified 77/77 via smoke test | pending |
| `CParticleTableDef_ParseProperties` | `0x5eefc0` (0x228) | companion property reader | — | pending |
| `CParticleDefEntry_ParseGraphicProperty` | `0x5e3550` (0xabe) | `graphic1` resets idx=0, `graphicN`++ capped at 3; case-insensitive key dispatch; reverse/inverse bits per func | folded graphic decl + `g_*` dispatch in `apply_particle_key` | match |
| `CParticleDefEntry_ParseBlendMode` | `0x5e29f0` (0xc8) | chained strstr; bumpadd→3 quirk (§1.11); `mod` table at `off_7DCBA8` | `particle.cpp::parse_blend_mode` / `blend_mode_name` | match |
| `CParticleTableDef_ParseTransformFlags` | `0x5e2950` (0x39) | tabledef flag bits | — | pending |
| `FlagTable_ParseFromString` | `0x5df970` (0x45) | §2.5 | `particle.cpp::parse_flag_table` | match |
| `BuildFlagString` (`sub_5DF9C0`) | `0x5df9c0` (0x98) | §2.5 | `particle.cpp::format_flag_table` | match |

Writers (round-trip verification gold):

| Original | Addr (size) | Behavior witnessed | Reimpl | Verdict |
| --- | --- | --- | --- | --- |
| `CParticleDef_SaveToFile` | `0x5e4d70` (0x9ef) | field-by-field fprintf, `%5.3f` floats, BGR byte order in memory printed back as R,G,B; writes `emit_dur` **twice** (fprintf sites `0x5e4e6a` + `0x5e4f30`, same field), `lod` unconditionally, and curve bits as `invert reverse` | `writer.cpp::write_particle`; round-trip parity via `particle_writer_roundtrip_test` (engine writer uses `\n` only) | match; curve suffix spelling/order closed 2026-07-22 |
| `CParticleEffectDef_WriteToFile` | `0x5e0fe0` (0xc7) | `\tid = %s;`, `\tpdefs = a, b;` (separator `", "` from `word_7CDA14`); space-equals, not tab-equals | `writer.cpp::write_effect` | match |
| `CParticleTableDef_WriteToFile` | `0x5e27e0` (0xee) | 32 fixed rows of 8 `%u`; `\ttlN = ...;`; `id = ` space-equals | `writer.cpp::write_table`; zero-fills if parsed table < 32 rows (defensive) | match |
| (no engine writer for `[tabledef_edithandles]`) | n/a | format corpus-derived (`boatwake.ptl:40-45`) | `writer.cpp::write_handles`; round-trip verified | match (corpus-derived) |

## 4. Runtime witness matrix (simulator)

The portable simulator in `libs/particle/src/emitter.cpp` is a **faithful core with recorded
gaps**: it captures the witnessed per-particle behavior (emission, lifetime, integration, curve
clocks) without claiming complete or byte-exact parity with the DirectX-bound runtime. ORBIT,
platform RNG/basis construction, capacity bounds, and the other §8/D-PTL rows remain explicit.
The engine `CParticleEmitter` runtime
instance is ~252 B (parent pos, orient matrix, AABB, particle buffer ptr + stride, spring/drag/
damping, force vec); our struct does not mirror byte layout.

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CParticleEmitter_AdvanceFrame` | `0x5e6570` (0x40b) | expiry pass runs before `UpdateParticles`, then emit timing (`interval = 1/emit_rate`) and burst work; a particle whose age crosses zero (or whose kill plane writes age=0) remains for that terminal frame and is reclaimed on the next advance | `emitter_advance` (collapses AdvanceFrame + UpdateParticles); rejects non-finite dt/intervals, bounds burst work at capacity, and applies a reimpl hard ceiling of 4096 particles (retail corpus max override 400) |
| `CEffectEmitter_Initialize` | `0x5e6020` | consumes signed 10-bit draws for `emit_dur ± emit_dur_adj` and `emit_rate ± emit_rate_adj`; seeds the shared gravity/GRAVITATE slot as `gravity × -0.09803897` (`flt_7DC738`) and drag slot as `drag × 0.01` | `emitter_init`: `emit_dur_total`, `emit_rate`, `gravity_accel`, `drag_coefficient`; signed-randomized `orbit_speed` also consumes `orbitalspeed_adj` when authored and stores the converted radians/second rate |
| `CParticleEmitter_UpdateParticles` | `0x5e6980` (0x3df) | explicit Euler, position first: `pos += vel×dt`; NORMAL then adds the shared gravity slot to Y (**no gravity_mask**); GRAVITATE normalizes `pos−emitter.pos`, applies `gravity_mask` without renormalizing, then multiplies the same shared slot; drag writes `vel -= drag_slot×dt×vel`; curve phase and age advance. Because Initialize negates/scales authored gravity, positive authored gravity sinks in NORMAL and attracts in GRAVITATE. Kill-plane bits 27/28 write age=0 without clamping position | `integrate_particle`; `spring_const` is an explicit GRAVITATE-only override, otherwise `gravity_accel` is shared; `kill_plane_mode`/`kill_plane_y` supply the site-specific threshold. Pinned by gravity/drag conversion, gravitate-mask, terminal-frame, and kill-plane ctests |
| `CParticleEmitter_SpawnNewParticle` / `UpdateAllParticles` (ORBIT branch) | `0x5f35b0` / `0x5f3be0` | Spawn randomizes `(orbitalspeed ± orbitalspeed_adj)` in authored degrees/second, then multiplies by `π/180` at `0x5f37bd` before storing the runtime rate. Update `(def.move & 4)` rotates `(pos − emitter.pos)` and velocity around `def.orbital_axis` via `D3DXMatrixRotationAxis` + `D3DXVec3TransformCoord`; its angle also derives from an FPU chain over particle age × emitter basis | **Approximate:** `emitter_init` randomizes at emitter scope and converts the result to radians/second; `integrate_particle` applies `orbit_speed × dt` via Rodrigues. The retail per-particle randomization and orientation-matrix/particle-age chain remain unported. Pinned by base/adjustment conversion, HE 30°/sec cadence, radius, and Y-axis tests |
| `CParticleEmitter_SpawnParticle` | `0x5e7640` (0xaa9) | **Re-witnessed instruction-level 2026-07-12.** Every emit_shape displaces spawn POSITION (not velocity): 1 = hollow-box shell, 2 = sphere shell (direction helper gets 360°), 3 = cone shell (fixed 90°). Velocity is seeded AFTER the switch for every shape: vtable direction helper receives `spread` and `spread_skip`, then multiplies by `(speed + speed_adj × rand±)`. Non-positive/non-finite lifetime is rejected. RNG resolution is `rand() & 0x3FF`, normalized **/1023** (`flt_7DC73C`) | `emit_one_internal` + `apply_emission_shape`; direction uses real independently bounded yaw/pitch rotations and preserves two draws; invalid lifetime completes the RNG path but is not inserted. Portable LCG remains deliberate for stable seeds. Pinned by `particle_emit_shape` and `particle_emitter` |
| `CParticleEmitter_TranslatePosition` | `0x5efe90` (0xad) | `delta = newPos − pos`; shifts AABB min/max accumulators. **PositionRelative** (flags bit 19): particles travel with the emitter; default clear = world-space, particles "left behind" | `emitter_translate` + `last_translation_delta`/`cumulative_translation`; Godot wrapper hooks `NOTIFICATION_TRANSFORM_CHANGED`. AABB tracking itself deferred. Pinned by `particle_translate_test.cpp` + GUT tests |
| `CEffectEmitter_AdvanceEmission` | `0x5e1d30` (0x1dc) | when `emit_rate_func` resolves, emission interval is scaled by `lut[(int)(t*256) & 0xFF] / 128.0` per frame (LUT ptr at def+3700); byte 128 = neutral, 0 = no emission, 255 ≈ 2× | `emitter_advance` emit-rate scaling; `t_norm = age/emit_dur`, FOREVEREMIT loops via `age − floor(age)`. Pinned by 3 LUT-rate ctest cases |
| `CEffectDef_ResolveTblDefReference` | `0x5e9630` (0x4a) | writes `entry+68 = TableDefByName + 328`; LUT = the tabledef's 32×8 bytes read row-major as a flat 256-byte array; renderer indexes `lut[(int)(t*256) & 0xFF]`, **no interpolation**. On a lookup miss it logs the "not found" format `@ 0x7dd420` (via `@ 0x5df7e0`), writes `entry+0x44 = 0`, and returns 0 — an unresolved curve is a null LUT, i.e. the channel renders un-curved | `bake_curve_lut` + `bake_particle_def_curves`; `reverse` reads source in reverse index order, `inverse` flips values (`255 − src`). Unresolved names leave `baked = false`. Pinned by `particle_curve_lut_test.cpp` |
| `CParticleManager_FindTableDefByName` (the find-or-transform-cache walk called by `ResolveTblDefReference`) | `0x5e9540` (0xf0) | walks the table list at `ctx+0x170`, compares names with **`_stricmp @ 0x76fdf6`**, and takes transform flags (`inverse=1`, `reverse=2`). **TableDef+0x248 is the applied transform mask, not an owner field.** Flags 0 return the FIRST matching base. Modified lookup returns an already-cached matching transform; otherwise it remembers the LAST matching base, clones it (0x250-byte alloc → copy ctor `@ 0x5e80f0` → list append `@ 0x5f85c0`), applies the transform at `0x5e2700`, and caches that mask at +0x248. Case-insensitive lookup is shipped-visible (`Table11Alt` vs `table11Alt`) | `ParticleFile::find_table` / Godot lookup remain case-insensitive. `bake_one_curve` returns the first base for unmodified refs and transforms the last base for modified refs; immutable baked LUTs need no mutable clone cache. Pinned by case-fold and duplicate-selection `particle_curve_lut` cases. The corpus has 102 duplicate table names and 7 names with differing duplicate data, but none of those 7 is referenced with a modifier, so the corrected duplicate rule is generic/mod fidelity rather than a shipped visual change |
| `CEffectDef_ResolveAllReferences` | `0x5e9d70` (0x28e) | full resolve pass: 6 particle-level curves (5 color/scale + emit_rate) + 5 curves × ≤4 layers + textures + 20 sound slots | `bake_particle_def_curves` (curves only); texture resolution via the Godot wrapper's `texture_path_resolver`; sound resolution deferred |
| `CParticleEmitter_BuildOrientationMatrix` | `0x5f3970` (0x267) | builds an orthonormal 4×4 at emitter+352 from `def.orbital_axis`, cross-product fallback when forward is parallel to `(0,1,0)`. **NOT** the parent transform (earlier speculation refuted by the live decomp) — it is the orbital frame for the ORBIT move mode | needed for full ORBIT frame fidelity; not yet ported |
| `CParticleEmitter_Init` | `0x5419e0` (0x86) | emitter ctor/init (also AnimMap + sound triggers, out of scope) | `emitter_init` |

### Render-side correspondence (Godot layer)

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CParticleEmitter_BuildBillboardQuads` | `0x5e6d60` (0x7d7) | vertex 28 B (pos 12 + color 4 + lit color 4 + uv 8), 4 verts/quad, indices 0/1/2/1/3/2; roll matrix (labelled `D3DXMatrixRotationX`, a `(M,f)` FLIRT-collision family) x manager camera matrix (+664) through the PSGP multiply; **quad half-extent = `particle.base_size (+0x38) x (ScaleCurve ? lerp(scaleLUT[i], scaleLUT[i+1], frac(phase)) / 128 : 1) x 0.5`** (`flt_7C3DD4` = 1/128, `flt_7C3B94` = 0.5 `@ 0x5f51ab`-analog; the scale LUT LERPS and reads `lut[i+1]` one byte past the LUT at i=255); color/alpha LUTs read the RAW byte at `(int)phase % 256`, channel x byte / 256, no lerp; **quad center += `emitter+0x140` (= `-def.z_offset`) x the per-frame view-axis globals `flt_2C06578/7C/80`** (`CParticleManager_BeginFrame @ 0x5ecfe8`) — the z_offset camera-ward pull `@ 0x5e71c9`; **flipbook clock = `(256/phase_rate) x flip_rate x phase / 64` = 4 x flip_rate x elapsed-seconds** `@ 0x5e6f17`, GFXFLIPRAND adds a particle-ptr-derived start offset; per-frame UV entry at graphic+724 = **6 dwords `{material_ptr, u_min, v_min, u_max, v_max, inset}`** — inset ADDS on min edges, SUBTRACTS on max; material changes flush + rebind via `CParticleBatch_FlushAndBindMaterial @ 0x5e4230`; manager RGB tint at emitter+200..+202, applied `(byte * channel) >> 7` (byte 128 = 1.0); **LOD decimation** `divisor = round(1.0 / *(emitter+8 + 0x3F4))`, render only when `serial % divisor == 0` (render-only; sim untouched) | `renderer::ParticleFrameCompiler` emits a value-owned immutable quad packet with four exact 28-byte vertices per quad and applies size/LUT/z-offset/flipbook/tint/LOD. The Godot scene-to-quad adapter applies the exact bump-color matrix and expands each quad to triangle vertices `0/1/2/1/3/2`; `NovaParticleCompositorEffect` streams those vertices through one persistent growable RD vertex buffer and executes every adjacent state run in packet order. Contracts pin stride, expansion, domain filtering, ordering, bounds, and command runs |
| `CParticleEmitter_RenderStaticBillboards` | `0x5f4e10` (0x80c) | same vertex layout; selected by `(def.flags & 0x100) == 0 ? rotated : static` (bit 8 = YAWANDPITCH). **NOT rotation-suppressed: the path renders WORLD-ORIENTED quads** — `(yaw, pitch, roll) = (aux+0, aux+8, particle+0x3C) x pi/180` into the `(M,f,f,f)` Euler builder `@ 0x5f5068..0x5f508d` (the `init_D3DXMatrixScaling` import label is a FLIRT prototype collision — scaling by angle-sized factors would collapse the +-half corners fed through the PSGP corner transforms; the semantics are RotationYawPitchRoll), corners `(+-half, +-half, 0)` transformed then translated by the pulled center. Per-particle yaw/pitch live in the parallel array at `emitter+0x150` (SS2.3) | the portable compiler's oriented-quad branch consumes the same per-particle Euler state and writes it into the shared packet; it is no longer a per-emitter mesh path |
| `CParticleEmitter_ComputeViewDepths` | `0x5e7580` | computes camera-space depth for every particle before a shared batch sort | `ParticleFrameCompiler` computes the rendered-center depth for every visible particle before packet construction |
| `CParticleManager_TransformToViewSpace` | `0x5ecc50` (0x31c) | projects each child emitter's bbox to view space via camera basis at this+664/+696/+700/+704, builds sort entries, calls `RecursiveSortAndRender` | the adapter derives value-owned emitter bounds for diagnostics and ordering input |
| `CParticleManager_RecursiveSortAndRender` | `0x5ec980` (0x188) | recursively separates non-overlapping emitter sets using alternating axes (`axisMask` cycles 1→2→4→1); an irreducibly overlapping leaf is sent to `RenderBatch` | the reimpl does not reproduce the recursive leaf partition: it places all selected emitters in one shared depth list (D-PTL-21). Spatially disjoint quads cannot affect one another visually, but exact retail tie/partition order remains an open algorithmic-parity gap |
| `CParticleManager_RenderBatch` | `0x5e9890` | builds one shared `{depth, emitter_id, particle}` list for every emitter in an overlapping leaf, globally sorts it back-to-front (`@ 0x5e9b63`), then dispatches adjacent emitter-id runs. Overlapping emitters therefore CAN interleave particle-by-particle; emitter contiguity is not guaranteed | `ParticleFrameCompiler` globally sorts all visible particles back-to-front with deterministic emitter/particle source-index ties, then forms adjacent render-state runs. This reproduces the required cross-emitter interleaving and prevents reimpl material sorting from undoing it; only the broader recursive batch partition differs (D-PTL-21) |
| `CParticleManager_BuildTextureAtlases` | `0x5e8db0` (0x44d) | bakes textures into a shared atlas; the per-graphic array at graphic+724 holds per-frame texture-ENTRY pointers, and the entry's rect floats sit behind its material ptr — `{+0 page/material ptr, +4 u_min, +8 v_min, +12 u_max, +16 v_max, +20 inset = 2.5/side}` (the allocator's success writes `@ 0x5e2d00..0x5e2d3b`; an earlier note here had v_min/v_max swapped); collection takes only probe-sized entries (+288 > 0), placement order is a stable width-descending bubble sort, pages are typed by their first entry (types 0–2 → 1024², 3–7 → 256² `@ 0x5e8f1e`; pages 1/2 shared, others exact `@ 0x5e2bef`), type-1 rows get alpha cleared at blit `@ 0x5e9116`, and the skyline placer (`CParticleAtlas_TryPlaceEntry @ 0x5e2be0`, ex kong `CEffectChannel_TryAssignSlot`) carries a persistent scan minimum and rewrites covered columns to `skyline[best]+height` — it can LOWER taller columns and overlap earlier rects; page finalize (`CParticleTexture_InitTextureAndChannels @ 0x5e8210`) converts type-3/6 pages via `Texture_GenerateNormalMapFromHeight(…, 0.125, 0)` and type-7 via `(…, 0.03125, forceBlue=1)` (the generator's real 5th arg), encoding `(n+1)×127.5` | `renderer::ParticleAtlasBuilder`: shared mission/preview catalog, exact frame registration, type page families, stable width-descending placement, witnessed skyline allocator, exact rects and 2.5-pixel inset, type-1 alpha clear, and whole-page type-3/6/7 normal preprocessing. Every flipbook frame is an independently packed registered entry/rect; frames are not assumed to be horizontal cells. Raw RGBA pages cross one narrow Godot-upload seam |

### Godot wrapper correspondence

| Wrapper | Engine analogue | Notes |
| --- | --- | --- |
| `godot/engine/particle/nova_particle_file.{h,cpp}` | file-scope `CParticleSystemDef_*` | top-level Resource owning the parsed `ParticleFile`; load/save route through `libs/particle` |
| `nova_particle_def.{h,cpp}` | `CParticleEffectDef` (§2.1) | ~80 fields via inspector groups; `flags`/`move` stored as both raw string and u32 bitfield (engine has both) |
| `nova_particle_graphic_layer.{h,cpp}` | per-graphic 788 B block (§2.2) | inspector enum exposes the 8 blend modes |
| `nova_particle_curve_ref.{h,cpp}` | curve reference | name + `reverse` (bit 0x02) + `inverse` (bit 0x01) |
| `nova_particle_table.{h,cpp}` | tabledef LUT | 32×8 logical curve; `sample(t)` linearly interpolates for editor visualization only |
| `nova_effect_scene.{h,cpp}` | `CEffectWorld` value ownership | Resource adapter over the portable catalog/group/emitter scene; the 62.5 Hz mission tick is the only runtime simulation owner |
| `nova_particle_renderer.{h,cpp}` + `nova_particle_compositor.{h,cpp}` | `CParticleManager` compile/upload/draw seam | one Node facade publishes immutable packet generations to one POST_TRANSPARENT RD compositor, with persistent growable buffers and renderer-owned value diagnostics |
| `nova_particle_emitter.{h,cpp}` | `CParticleEmitter` | legacy single-def authoring/test convenience wrapper; neither GameWorld nor ONED uses it for runtime presentation |
| `ptl_resource_format.{h,cpp}` | (no engine analogue) | Godot ResourceFormat loader/saver for `.ptl`, round-trips |

These rows map responsibilities; they are not a blanket parity verdict. The ONED workspace
(`godot/modtools/particle/`) mounts the blueprint screen (node graph + live preview) over these
wrappers. Behavior-level matches and remaining gaps are recorded in the witness matrices,
§8, and D-PTL catalog below.

### Runtime load & spawn chain (game integration, witnessed 2026-07-10)

The game-side effect world: who loads the `.ptl` set and how effects spawn by name at runtime.
Reimpl: `godot/engine/world/effect_world.gd` (`NovaEffectWorld`, Godot-side render service — a
dedicated host is headless and never draws) + the `game_world.gd` fx routing.

| Original | Addr (size) | Behavior witnessed | Reimpl + pinning |
| --- | --- | --- | --- |
| `CEffectSystem_Init` | `0x5f6070` (0x53f) | called from `Game_StartMission @ 0x524980`; creates `g_EffectWorld` (a `CParticleManager`, 0x454 B) once, registers the static+rotated billboard renderers, texture dir = `<exe>\tga\`; then parses **every** loose `ptl\*.ptl`, the `.ptu`/`.ptg` alternate set (`byte_24D4DF9` selects `.ptg`), and **every** `.ptl`/`.ptu` entry of **every** mounted PFF volume through `CEffectWorld_ParseSectionCallback` — no fixed file list; post-load resolve `sub_5DF7B0(g_EffectWorld, 1)` | `NovaEffectWorld.load_from_resource_root`: every `.ptl` in the mounted root (loose overrides + all PFF volumes via the resource index), tables merged globally (§1.5). Pinned by `effect_world_test.gd` |
| `CEffectWorld_LoadDefinitionFile` | `0x5ecf70` (0x45) | single-file entry: copies the path into two static buffers, then `File_ParseASCIIFile(path, ParseSectionCallback, 710577837)` | per-file `NovaParticleFile.load_from_buffer` |
| `CEffectWorld_InternEffectHandle` | `0x5f7310` (0xfc) | (renamed 2026-07-10 from kong `CEffect_FindOrCreateMaterial` misnomer) interns an effect NAME → stable **1-based handle**: linear `stricmp` scan of the interned pool (`dword_2C25B18`, count `dword_2C25CE0`); miss → `CEffectWorld_FindDefByName` + append; still missing → clone `stockeffect` (vtable+28) under the requested name. WAC `fx` params resolve through this at script compile (`WacScript_ResolveParameter @ 0x4f2920`) | `NovaEffectWorld.intern_effect` (case-insensitive, 1-based, first-registration-wins; unknown names clone `stockeffect` under the requested name — D-PTL-8 CLOSED 2026-07-12) |
| `CEffectWorld_FindEffectDefByName` (renamed 2026-07-16 from `CEffectWorld_FindDefByName`) | `0x5e34f0` | by-name effect lookup over the parsed set (the world's +70 def list); compares each def's vtable+0 name with **`_stricmp` @ 0x5e352c** — effect names resolve case-insensitively | `EffectScene::effect_by_name` `fold_ascii` keys (pinned by `catalog_and_stock_alias_contract`) |
| `CEffectWorld_FindParticleDefByName` (renamed 2026-07-16 from kong `CEffectWorld_FindTableDefByName` — a trap; the true TBLDEF find is `@ 0x5e9540`) | `0x5e41d0` | by-name PARTICLE-def lookup over the world's +82 def list; compares with **`_stricmp` @ 0x5e420c**. The effect-def pdefs member resolve (`@ 0x5e4920`, 64-byte name slots from +340) resolves through it and logs `UNRESOLVED: EFFDEF %s missing PARDEF %s` on a miss | `EffectScene` `definition_by_name` `fold_ascii` keys + `ParticleFile::find_particle` / `NovaParticleFile::find_particle`/`find_effect` case-folded (pinned by `pdef_reference_resolution_is_case_insensitive_contract` + the minimal-effect `find_particle` fold check) |
| `CEffectBank_ResolveAllEntries` (the EFFDEF vtable+8 resolve, slot `@ 0x7dca2c`) | `0x5e4920` (0xab) | resolves the effect's pdefs members into its entry buffer (+2388) **all-or-nothing**: each 64-byte name (from +340) finds its PARDEF via `@ 0x5e41d0`, then runs the PARDEF's own vtable+8 resolve; the FIRST miss breaks (`@ 0x5e495d`), logs `UNRESOLVED: EFFDEF %s missing PARDEF %s` once, `CCircularBuffer_ClearAll`s the whole buffer (`@ 0x5e49be`), and returns 0 with the resolved flag (+332) left 0 — a partially-resolvable effect keeps NO members. The def stays in the world list (`CParticleManager_ResolveAllReferences @ 0x5ec850` tracks the failure but never unlinks), so intern/find still return it and a spawn allocates a group whose child-spawn walks the empty buffer — zero children, reaped on the next update. Zero authored pdefs short-circuits to success | `EffectScene::open` clears `definition_indices` and stops at the first unresolved pdefs member; the effect stays registered by name and `spawn` returns `EmptyEffect` (the zero-child group that reaps immediately, expressed as a rejection receipt). Pinned by `effect_resolve_is_all_or_nothing_contract` |
| `CEffectWorld_SpawnEmitterAtPosition` | `0x5f6df0` (0x182) | spawn descriptor (14 dwords): +0 flags (bit0/1 = orientation-in-descriptor; bit2 inverted into the spawn call), +4 interned handle (≤0 → +8 name ptr), +12 owner/tag (stored at emitter+0), +16..24 fixed-point position and +28..36 fixed-point orientation (both through `Math_FixedPointToFloat3_YNegated @ 0x611210`), +40 attenuation 16.16, +44 blend 16.16, +48/+52 sample params (action-slot coupling); spawns via `CEffectWorld_AllocGroupAndSpawn(g_EffectWorld, 0, def, pos, orient, flag)` | `NovaEffectWorld.spawn_effect_request`: one value-owned `EffectScene` group with one pooled emitter value per `pdefs` entry; the mission fixed tick owns lifetime, while the shared renderer consumes immutable draw packets. No emitter renderer Nodes cross the facade |
| `WacScript_SpawnEffectAtSsnEntity` | `0x4f23a0` (0x13f) | (renamed from kong `WacScript_SpawnSoundAtEntity` — it spawns a particle emitter) WAC `fx2ssn`: resolves the `(pool<<12)\|slot` handle, **detaches any live emitter at entity+460 first**, descriptor at the entity position, orientation = **terrain surface normal** at its grid cell (`outMillis`/`off_849934` tables), new handle → entity+460 | `game_world._route_mission_effects` resolves the live registry SSN, then `spawn_effect_owned`; replacement detaches the previous group, each sweep follows the entity position/forward, and registry removal stops emission while live world-space particles drain. Initial orientation remains up until the terrain-normal read lands (D-PTL-7) |
| `WacScript_SpawnEffectAtTargetMarker` | `0x4f7fd0` (0x122) | (renamed from kong `WacScript_PlaySoundAtEmitter`) WAC `fx2tgt`: pool-3 walk for `itemDef+80 == 6088` (placed target marker, ids 1..99) with the matching target id; same descriptor + entity+460 handle protocol | unrouted: which `.bms` record field carries the target number is unwitnessed (§8) |
| `ActionSlot_SpawnEffect` | `0x401f20` (0x17f) | weapon-action effect spawn (the ACTION block `particle` key = ActionDef+16, a 1-based interned handle): resolves the firing entity through vehicle parent chains, `Entity_ComputeActionTransform @ 0x401310` fills descriptor position/orientation from the action bone, and passes descriptor dwords 12/13 to the group spawn. The FIRE path supplies `ActionSlot_ClearEffectHandle @ 0x53f760`; `CEffectGroup_SetDeathCallback @ 0x5e1940` stores it at group+0x5C/+0x60. The recoil path passes 0 and never records a guarded handle. While the group handle lives, the pump re-anchors it to the recorded action bone every tick and releases it underwater [orig: `WeaponAction_ProcessFrame @ 0x540edf` → `CEffectEmitter_UpdatePositionAndParams @ 0x5f6810`] | routed by `PlayerWeaponEffects` for local FIRE: resolve the live viewmodel userpoint, apply the settled-scope gate, and key the owner-bound live-group guard by viewmodel generation/action. `GameWorld.register_effect_anchor` supplies the pump tracker's reimpl analog. Every witnessed weapon particle enters the global World-domain pass; exact vehicle-parent capability/transform and third-person model sourcing remain open (§8) |
| `ActionSlot_ClearEffectHandle` / `CEffectGroup_Destroy` | `0x53f760` / `0x5e3460` | `CEffectGroup_Destroy` invokes the callback stored at group+0x5C/+0x60. Only then does `ActionSlot_ClearEffectHandle` compare the dying GROUP handle with slot+24 and clear slot+24/+40, re-arming the `!slot+24` guard `@ 0x5418c8`. The suppression window is the whole group lifetime, not the first child's lifetime; the 2026-07-15 first-child reading was disproved (D-WPN-19) | `spawn_effect_unless_alive` retains its admission mapping until the final child is gone, then releases the group and clears the owner key. Pinned by `child_reaping_and_group_suppression_lifetime_contract` |
| `CEffectGroup_AdvanceChildrenAndReap` | `0x5e59a0` | advances each child, immediately unlinks and destroys an individually dead child, and destroys the group only after its child list becomes empty. A finished short child therefore neither occupies the 4096-emitter pool nor appears in later snapshots while a forever sibling continues | `EffectScene` reaps finished emitter slots after each fixed tick and releases the group/admission mapping only when no child remains. The same `child_reaping_and_group_suppression_lifetime_contract` pins capacity and snapshots |
| `WeaponAction_Recoil` (the direct effect leg) | `0x542dd0` (gate `@ 0x542efa`, spawn `@ 0x542f64`) | the recoil-row DIRECT effect spawn at the arbiter tick (counter reaches 0 after delaystart): for the LOCAL player gated on `ActionDef+16 && currentAction==3 && g_FpWeaponViewFlags&1` — NO scope gate and param7=0, so no handle records and no live-predecessor suppression. The payload is data-defined: most rows eject at `bcasing`, but REVX02 `WPN_M4AUTO` authors `EFFECT_M16MF` at `MFLASH01` here while its FIRE row authors the casing. muzzleBone select: def default (WeaponDef+0x2D4, itself resolved on **gfx3**), ActionDef+**57** (the 3P index — corrected 2026-07-27, this row had +56; +56 is the gfx1/1P index, see `WeaponDef_ResolveAllReferences @ 0x540270` stores `@ 0x54039e` vs `@ 0x54040f`) when slot flags&2, else `Entity_GetWeaponSlotByte` | PORTED generically: `weapon_fsm.cpp handler_recoil` emits ordered `action_effect` values; `LocalPlayerPresenter` submits every authored payload through the live action-bone transform as an independent `Always` World-domain transient. Casing, muzzle, smoke, and mod-authored names follow the same path—no `mflash*` distinction or direct-row guard. Pinned by `weapon_fsm_test.cpp`, `nova_simulation_test.gd`, and `local_player_presenter_test.gd` |
| the FP gates: `g_FpWeaponViewFlags` + the settle promoter | `0x24d20c0` / `0x4de4f7` | `g_FpWeaponViewFlags` (ex `dword_24D20C0`, renamed 2026-07-13) bit 0 = draw the FP weapon AND its FP fire effects (seeded from settings + profile+1484: 0 writes 2, 1 writes 3, 2 selects third-person; toggled by an input binding; readers `Player_RenderFirstPersonViewModel @ 0x4dedd4`, `ActionSlot_ExecuteActionTick @ 0x541aa1`, `WeaponAction_Recoil @ 0x542eef`, `HUD_RenderAllOverlays @ 0x5a820d`). `g_weaponScopeActive` is promoted to 1 ONLY when the ADS camera ease completes (`Player_UpdatePerFrame @ 0x4de4f7`, the settle promoter — the sole 1-writer; `Player_ToggleWeaponScope @ 0x4df0c0` sets `g_scopeEngaged` and leaves it 0), so the muzzle gate `@ 0x541aba` suppresses at FULL RAISE, not from the toggle | the weapon-view bit is treated always-on (§8). `NovaSimulation` runs the view promoter before the weapon pump, then snapshots settled/third-person/vehicle routing state into every presentation event; a multi-tick catch-up cannot apply the final view state retroactively. Pinned by `local_player_presenter_test.gd` and `nova_simulation_test.gd` |
| `Weapon_RaycastAndSpawnImpact` | `0x4e8460` (0x520) | Knife-only instant-kill-zone EFFECT presenter: `RoundData_SpawnRound @0x4ec1ed` gates on `AmmoDef.flags & 0x400`, then `@0x4ec216..0x4ec21f` calls this leaf only for `kztype == Knife`; its second argument is the AmmoDef, `+0x38` is `kz_maxradius`, and `+0x68` is the effects-table pointer. Ordinary bullets never call it | NOT PORTED: the non-ballistic Knife/instant-kill-zone family is D-WPN-16. D-WPN-14 records and closes the former false bullet-timing reading |
| `Projectile_SpawnImpactEffect` | `0x4e9b80` | physical ballistic impact presenter reached from `Projectile_UpdatePhysics -> Projectile_Handle*Impact`; chooses the AmmoDef effects-table row for the resolved terrain/material/water tag and presents particle + sound at collision time | PORTED for host/SP: `RoundSim` stages tick/order/direction, `NovaSimulation::drain_round_impacts` resolves the row, and `game_world._route_round_impacts` submits a generic `Always` World transient plus 3D sound. D-WPN-15 carries tag-source residuals and D-WPN-8 carries client presentation |
| `AmmoDef_ParseProperty` (effects_table) + `AmmoDef_InitEffectsTable` | `0x40a2d0` / `0x409f20` | per-surface rows stage by tag name against `g_AmmoEffectTagTable @ 0x813420` (28 canonical tags scanned from index 1; the array index IS the impact tag id: null/move/player/zip/obj/dirt/grass/snow/cement/sand/packeddirt/water/railroad/mud/ice/quicksand/stone/wood/metal/glass/cloth/foliage/hmetal/flesh/bodyarmor/uwater×3), effect + sound interned (`none` → 0) at parse, duplicates refused (`redefining the effect` `@ 0x40a502`), the 4th count column parsed and DISCARDED (`@ 0x40a587`); block end compacts the staging into the ammo record's 16-B-stride table (+104) | PORTED: `world/ammo_table.h kImpactEffectTagNames` + the `np::build_ammo_table` bake (first-dup-wins, `none` → empty, count dropped). Pinned by `npruntime_round_sim` |
| `resolve_item_materials_and_spawn_bone_trails` | `0x522ee0` (0x3e1) | mission-start resolve of the ITEMS.DEF per-item effect slots (names → interned handles; userpoint names → 16-bit masks via `ItemDef_GetBoneMaskByName @ 0x49ea40` — EXACT stricmp over the model's first 16 userpoints, duplicate names all match; death/h2odeath/fire/other mask the HUSK model's fixed `Dead`/`Fire`/`Other` point names), then spawns slot A (`particlefx`) for every live entity in pools 1-3 (pool 1 skips attrib 0x42, pools 2/3 skip attrib 0x2 powerups). The skipped PlayerControl (0x40) items get the RUNTIME occupancy chain instead (`entity_update_damage_accumulator_and_shadow` row below) — but only the chel/cpln classes ever reach it, so the shipped ctank/cbike/cveh `particlefx` authors are dead data in JO retail | PORTED for animated and static-batched entities, slot A only: `ItemEffectDirector.reattach` (godot/engine/world/item_effect_director.gd) applies the original gates and first-16 matching. Animated groups follow a live owner; static sources retain object data + base transform as values and submit World-bound persistent groups. The `fxs`/`fxw*` tiers + death family remain §8 threads |
| `Entity_SpawnBoneTrailEffect` | `0x43bef0` (0x1ca) | one mode-2 entity-attached emitter per masked userpoint: descriptor position = the userpoint, direction = the userpoint's direction vector, both through the entity basis; the handle lands at entity+0x1CC (the IDB field name `ownerSession` is a misnomer — `attachedEffectHandle` rename proposed, §5.5); ZERO masked points → ONE emitter at the entity origin (`@ 0x43c097` → `submit_effect_descriptor @ 0x43c0a4`). The twin `entity_spawn_bone_trail_effect @ 0x43f8f0` is xref-dead. +0x1CC is SHARED with the heavy-damage smoke: `Entity_UpdateVehiclePhysics` spawns `g_FxHandleSmkSigB` into it when hp < max/4 (`@ 0x48b0f1`, guard +0x1CC == 0) and `g_FxHandleVehicleFireMed` into +0x400 at critical hp (`@ 0x48b0bc`) | PORTED: live/animated sources use `spawn_effect_attached`; static-batched sources compose the same local point/direction once with their base transform and use `spawn_effect_request(Always, World)`. Both preserve the origin fallback. The damage smoke/fire family remains a §8 thread |
| `entity_update_damage_accumulator_and_shadow` (the PlayerControl occupancy spawner) | `0x48fa70` (0x41a) | **witnessed 2026-07-15.** Reached per-tick ONLY from the CHel/cpln class updater `Entity_UpdateAircraftPhysics @ 0x490310` (`@ 0x4905a6`; class fn table `@ 0x82ac00`, `{tag, pad, fn}` rows: `"CHel"` direct, `"cpln"` via the thunk `@ 0x45d6f0` — the ground classes cveh/ctank/cbike/cbot/ctrn route to `Entity_UpdateVehiclePhysics @ 0x48af00`, which never calls it). Lazy on a render/LOD state == 1; gate `@ 0x48faad`: `def.attrib & 0x40` (PlayerControl) && `occupantEntity` (+368, the vehicle side of the dual-role +0x170 "linked entity" field — a carried child's +0x170 is its carrier `[orig: Entity_AttachToVehicle @ 0x43c130]`) && the once-latch clear → engine-start sound `soundProfile defaultRes+120` (gated: engine accumulator ≤ 5% of 0xCCCCCC0, `!(renderArg+792 & 1)`, above `Env_WaterHeightFixed`), then `Entity_SpawnBoneTrailEffect` (slot A → +0x1CC), latch = 186413; the latch resets to 0 whenever +368 empties. The same function integrates the engine-runtime accumulator (+latch/tick to 0xCCCCCC0 occupied, −46603 empty; non-PlayerControl latches a random 139809/163110/186413) that drives the ground decal + the dust/spray/skid dispatch (`update_vehicle_effect_emissions @ 0x528f20`, vehicle-def curve words 220..231) | The claimant lifecycle is PORTED as `vehicle_control_started/stopped` world effects consumed by `game_world`; the port intentionally runs it for EVERY PlayerControl item so the authored-but-dead cveh/ctank/cbike exhausts present (D-PTL-17). The ground detach slot-31 stop is now ported (D-SND-17); the aircraft-only slot-30 start, visible-lazy gate, and accumulator/wake-emission intensities remain §8 threads |
| the +368 claim/release protocol | `0x4946d0` / `0x4355f0` | **witnessed 2026-07-15.** `Entity_AttachToVehicleSlot @ 0x4946d0` claims +368: ctrlx(2) `@ 0x4947b3..0x4947d2` and drvrx(5) `@ 0x4948b9..0x4948d8` when empty-or-same (both require `attrib & 0x40`); UseGun(3) only when EMPTY `@ 0x494944..0x49495e` (an occupied gun refuses the attach — UseGun targets emplaced-gun entities carrying their own +368); sitex(1) writes only the `vehicle[400+2*slot]` seat table. `Entity_DetachFromVehicle @ 0x4355f0` runs the stop leg ONLY when the detacher IS the claimant (`@ 0x4356e9`): engine-state dword = 7, movement-sound reset, engine-stop sound `defaultRes+124` above water (`@ 0x435716..0x43573e`), `CEffectEmitter_ReleaseSafe(+0x1CC)` + null (`@ 0x435746..0x435759`), then +368 = null (`@ 0x43577c`). A surviving second control occupant does NOT inherit the claim — it re-arms only on a fresh attach. (The prior IDA auto-comment "2=driver, 3=passenger, 5=gunner" on 0x4946d0 was wrong; the seat-name witness `@ 0x4351f0` is sitex=1, ctrlx=2, UseGun=3, drvrx=5) | PORTED 2026-07-15: `Entity.primary_occupant` + `vehicle_claim_primary_occupant` / `vehicle_release_primary_occupant` (`libs/world`), claimed on both attach paths, released on detach/dismount, stale-validated per motor tick; pinned by `vehicle_motor_test.cpp` (claimant-stop-despite-second-controller, silent second-departure, gunner-first claim) |

`CEffectDef_FindByTypeName @ 0x5b01a0` / `CEffectDef_Construct @ 0x5b01e0` are **NOT particle
functions** — that family is the `.3DI` model-def cache (`sub_5B6160` appends `.3DI` to the name
before the lookup); a kong naming trap, recorded in §5.5.

Additional witnessed function index (address cross-reference only; port status is stated in the
behavior rows above):

| Function | Addr | Function | Addr |
| --- | --- | --- | --- |
| `CParticleEmitter_TrySubmitForRender` | `0x5e7540` | `CEffectDef_AddSubEffect` | `0x5a3020` |
| `CParticleEmitter_SpawnNewParticle` | `0x5f35b0` | `CEffectDef_InvokeFactory` | `0x5e19e0` |
| `CParticleSystemDef_InitDefaults` | `0x5e14f0` | `CEffectDef_SetTextureName` | `0x5ef8e0` |
| `CParticleManager_Construct` | `0x5e87f0` | `CEffectEmitter_Initialize` | `0x5e6020` |
| `CParticleManager_ResolveAllReferences` | `0x5ec850` | `CEffectEmitter_SpawnBetweenPositions` | `0x5ea0a0` |
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
of stage state referencing sub-structs at `0x7e94e0..0x7e9510`. The decoded destination-
dependent modes are:

| Mode | Texture-stage source | Framebuffer blend |
|---|---|---|
| Bump (3) | stage 0 `DOTPRODUCT3(TEXTURE, DIFFUSE)`; stage 1 preserves that RGB and restores `TEXTURE.a × DIFFUSE.a` | source-over |
| Mod (4) | `TEXTURE × DIFFUSE` | `SRC=DESTCOLOR, DST=ZERO` |
| Mod2x (5) | `TEXTURE × DIFFUSE` | `SRC=DESTCOLOR, DST=SRCCOLOR` → `2 × source × destination` |
| Bumpadd (6) | the same DOT3 + alpha-restore program as Bump | `SRC=SRCALPHA, DST=ONE` |
| Distort (7) | decoded normal/projective scene lookup (§5.4) | replacement sample with particle alpha |

The Mod2x signature `SrcBlend=9 (DESTCOLOR), DestBlend=3 (SRCCOLOR)` (bytes
`09 00 00 00 03 00 00 00`) lands at `0x7e7814`, inside the index-6 (Mod2x) struct.

**The full per-mode factor set (witnessed 2026-07-15)** — the atlas-page channel descs built
by `CParticleTexture_InitTextureAndChannels @ 0x5e8210` are the state
`CParticleBatch_FlushAndBindMaterial` actually applies (material[1]/[2] = these channels):

| Mode | SRCBLEND / DESTBLEND | Stage program (color AND alpha) |
|---|---|---|
| blend (0) | `SRCALPHA / INVSRCALPHA` (`@ 0x5e8347` + LABEL_16 `@ 0x5e85a9`) | `MODULATE(TEXTURE, DIFFUSE)` |
| additive (1), premult (2) | `ONE / INVSRCALPHA` (`@ 0x5e8380` + LABEL_16) | `MODULATE(TEXTURE, DIFFUSE)` |
| bump (3) | `SRCALPHA / INVSRCALPHA` (`@ 0x5e83ee/0x5e83f6`) | DOT3 (COLOROP 24 `@ 0x5e83d6`) |
| mod (4) | `DESTCOLOR / ZERO` (`@ 0x5e8474` via LABEL_11 + `@ 0x5e843f`) | `MODULATE` |
| mod2x (5) | `DESTCOLOR / SRCCOLOR` (`@ 0x5e8474` + `@ 0x5e8490`) | `MODULATE` |
| bumpadd (6) | `SRCALPHA / ONE` (`@ 0x5e84dc/0x5e84d8`) | DOT3 |
| distort (7) | dual channel, `EffectWorld_WaterReflectVS/PS` pair (`@ 0x5e8527/0x5e856b`) | §5.4 |

Additive is NOT the classic `SRCALPHA/ONE`: it shares premult's `ONE/INVSRCALPHA`, and the
type-1 atlas alpha clear (§4 atlas row, `@ 0x5e9116`) zeroes the fragment alpha
(`TEXTURE.a × DIFFUSE.a = 0`), which turns that pair into a pure add — **an additive
layer's DIFFUSE alpha (the authored alpha curve) never affects its on-screen result**;
additive fades ride the color curves. Premult keeps its authored texture alpha, so
DIFFUSE alpha there attenuates only the destination. The port's RD pipelines and the
scene-preview shaders carry these exact pairs (fixed 2026-07-15 from a `SRCALPHA/ONE` +
shader-side alpha approximation).

**Confirmed not used**: `D3DTOP_BUMPENVMAP` (22) and `D3DTOP_BUMPENVMAPLUMINANCE` (23) appear
only as string-length compare immediates inside the parser
(`CParticleDef_ParseFromConfigMap @ 0x5ed210`) — never in the render path. Bump/Bumpadd use
the ordinary `D3DTOP_DOTPRODUCT3` program above, not D3D9 bump-environment mapping.

### 5.3 Bump / Bumpadd lit-color path

The "bump" lighting effect = 2-color vertex format + fixed-function combiner. Hard data:

**Light constants** (`flt_848D34/D38/D3C @ 0x848D34`, raw bytes
`46 B6 13 BF / 46 B6 13 BF / 46 B6 13 3F`) are `(-k,-k,+k)`, `k=0.5773503`. The builder
negates the first two and keeps the third, so the vector entering the transform is exactly
`bump_scale × (+k,+k,+k)`. There is no per-emitter or per-spawn light override.

**Lit-color computation** in `[orig: BuildBillboardQuads @ 0x5e6d60]` when
`particle.flags & 0x80` (LitColor) is set:

1. Compose `M = Rx(particle.rotation_radians) × view`, where `view` is the parent matrix at
   emitter+664. Rotation is stored in degrees and converted via `flt_7DCB00 = π/180`.
2. Transpose that exact product (`sub_68BF44 → off_8507A8` = `D3DXMatrixTranspose`).
3. Transform `bump_scale × (+k,+k,+k)` by `transpose(M)` (`sub_68B52B` =
   `D3DXVec3Transform`).
4. Encode each component with the original truncating conversion and retain its low byte;
   there is no reimpl-side saturation clamp.
5. Primary vertex color = `(modulated.alpha << 24) | (bx << 16) | (by << 8) | bz` — RGB replaced
   by the encoded light direction, alpha kept from the modulated color.
6. Secondary vertex color = `modulated_RGB | 0xFF000000` — raw modulated RGB, full alpha.

Bump/Bumpadd then run `DOT3(texture.rgb, DIFFUSE.rgb)` and restore alpha as
`texture.a × DIFFUSE.a`; the secondary/SPECULAR color is carried by the universal FVF but
is not consumed by these stage programs. Mode 3 source-over blends, while mode 6 uses
`SRCALPHA/ONE`.

The shared packet renderer carries the same DIFFUSE/SPECULAR ordering and evaluates this
literal matrix operation before packing each quad. Our simulator already stores radians, so
the retail degree→radian conversion is not repeated.

### 5.4 Distort (blend mode 7)

Uses the secondary render-state pointer at `sample+8`; the captured scene is bound by
`apply_texture_stages`. Type-7 atlas preprocessing converts the blue height byte to a wrapped normal
with scale `0.03125` and forces B=255 (§4). At draw time, let `n = 2·tex.rgb−1`,
`θ = GetTickCount()·0.004`, and `w = (sin θ, cos θ, −sin θ) / 51.2`. With particle alpha `a`
and the projective scene coordinate `p`, retail samples:

```
u = n.x*(a*w.x) + n.y*(a*w.y) + n.z*p.x
v = n.x*(a*w.y) + n.y*(a*w.z) + n.z*p.y
```

The RD backend copies scene color once before the ordered particle draws and samples that
immutable copy for Distort. Godot's render-buffer UV is the reimpl projective-coordinate map;
that API mapping, rather than the decoded particle equation, remains a bounded reimpl detail.

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
| `CEffectDef_FindByTypeName` / `CEffectDef_Construct` | `0x5b01a0` / `0x5b01e0` | **The `.3DI` model-def cache**, not particles: `sub_5B6160` appends `.3DI` to the name before this lookup and loads via `ThreediGp_LoadFromFile`. The `CEffectDef_*` prefix on this family is a kong trap — particle effect defs resolve through `CEffectWorld_FindEffectDefByName @ 0x5e34f0`. |
| `CEffectWorld_FindTableDefByName` | `0x5e41d0` | **CEffectWorld_FindParticleDefByName** (renamed in the IDB 2026-07-16). It walks the +82 PARTICLE-def list, not the table list — the EFFDEF pdefs resolve (`@ 0x5e4920`) uses it and logs `missing PARDEF` on a miss. The true TBLDEF find is `CParticleManager_FindTableDefByName @ 0x5e9540` over the manager's +0x170 table list. |
| `CGameConfig_SetWindowClassName` | `0x5df8a0` | sets the effect manager's **texture search dir** (`<exe>\tga\` from `CEffectSystem_Init`); nothing to do with window classes. Comment-only — not yet renamed. |
| `CNapiTransport_DetachFromSession` | `0x5f75d0` | detaches a live effect **emitter** from its owner entity (entity+460 handle protocol, called before a respawn in `WacScript_SpawnEffectAtSsnEntity`); not networking. Comment-only — not yet renamed. |
| `GamePlayerEntity.ownerSession` (field) | entity `+0x1CC` | the entity's ATTACHED-EFFECT emitter handle, written by `Entity_SpawnBoneTrailEffect @ 0x43bef0` and released via `CEffectEmitter_ReleaseSafe @ 0x5f69f0`; not a session pointer. Rename to `attachedEffectHandle` PROPOSED 2026-07-13 (human-curated struct field; needs the rename-everywhere pass). |
| `CEffectChannel_TryAssignSlot` | `0x5e2be0` | **CParticleAtlas_TryPlaceEntry** (renamed in the IDB 2026-07-15). The atlas SKYLINE PLACER, not audio: the kong "channel/slot/timestamp" vocabulary is the page/column-height array. Type-compat gate, the persistent scan minimum, and the rect/inset writes into the texture entry (§4 atlas row). |
| `CParticleTableDef_ReloadAllTextures` | `0x5e4bb0` | **CParticleDef_ReloadGraphicFrameTextures** (renamed in the IDB 2026-07-15). It iterates a PARTICLE DEF's graphic layers (788-B stride), not a tabledef: releases each frame's texture entry and re-interns it under the derived flipbook name (D-PTL-14's registrar). |
| `Entity_UpdateAircraftPhysics` (caveat, name kept) | `0x490310` | correctly the aircraft-family updater — but it serves BOTH the `"CHel"` and (via the thunk `@ 0x45d6f0`) `"cpln"` class-table rows, and it is the ONLY caller of the PlayerControl occupancy spawner `@ 0x48fa70`; do not expect ground-vehicle classes to pass through it. |

## 6. Visual parity — implemented features

Renderer alignment against the RE render chain (verdicts per §3/§4 tables):

- **Camera-facing billboards** with per-particle rotation, submitted to the shared
  back-to-front particle-depth batch `[orig: BuildBillboardQuads @ 0x5e6d60;
  ComputeViewDepths @ 0x5e7580; RenderBatch @ 0x5e9890]`.
- **YAWANDPITCH static billboards** (flags bit 8) — non-rotating quad path
  `[orig: RenderStaticBillboards @ 0x5f4e10]`.
- **8 blend modes** — parsed values select one of eight RD pipelines in the ordered compositor:
  additive, source-over, premultiplied, DOT3 bump, destination modulation, exact 2x
  destination modulation, DOT3 bump-add, and captured-scene distortion. Blend factors are
  pipeline state rather than approximated in a Godot material. A blank or unresolved runtime
  graphic stays invisible-but-simulating like retail; only ONED opts into the diagnostic
  soft-circle fallback `[orig: ParseBlendMode @ 0x5e29f0]`.
- **Per-material retail fog** - Blend/Bump/Distort converge on the live scene fog color;
  Additive/Premult/Bumpadd converge on black, Mod on white, and Mod2x on gray 127.
  Type 0 uses eye depth with `exp(-depth * ln(64) / end)`; types 1-3 use radial
  distance and the authored start/end linear curve. The compositor snapshots
  `NovaEnvironment` on the main thread, fogs RGB before fixed-function blending, and
  leaves alpha unchanged. The Forward+ backend test compiles the shader, executes a live
  draw, and verifies the supplied fog tint in the framebuffer
  `[orig: Render_SetFogState @ 0x58a950; CParticleBatch_FlushAndBindMaterial @
  0x5e4230; CD3DDevice_SetFogAndTextureFactor @ 0x677740]`.
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
- **ORBIT** authored speed and signed adjustment are randomized in degrees/second, then converted with `π/180`; the reimpl uses a Rodrigues approximation around `orbital_axis`, while retail's per-particle randomization and basis/age chain remain open (§4) `[orig: SpawnNewParticle @ 0x5f37a2..0x5f37c3; UpdateAllParticles @ 0x5f3be0]`.
- **Kill-plane** modes (§4): authored `BELOWH20` / `ABOVEH20` bind to the active mission
  water height; plus **LOD decimation** (`serial % divisor`, render-only).
- **Cross-emitter shared-depth sort** — retail recursively partitions emitter AABBs, then an
  overlapping leaf uses one shared particle-depth list and can interleave emitters. The reimpl
  uses one global list for all selected emitters, records adjacent state runs, and executes
  them sequentially, reproducing the visible interleaving while retaining the exact recursive
  partition as D-PTL-21
  `[orig: RecursiveSortAndRender @ 0x5ec980; RenderBatch @ 0x5e9890]`.
- **World-space rendering default** (particles left behind when the emitter moves) with
  **PositionRelative** (flags bit 19) opting back into carried particles
  `[orig: TranslatePosition @ 0x5efe90]`.
- **Manager-level RGB tint** (`(byte * channel) >> 7`, byte 128 = 1.0) as `color_tint`; used by
  screen-flash effects `[orig: BuildBillboardQuads @ 0x5e6d60]`.
- **Flipbooks** — `flip_frames`/`flip_rate` select an independently registered atlas-frame entry from elapsed lifetime; successive frames may occupy unrelated packed rectangles.
- **Atlas packing** — one shared catalog with exact retail frame names, page families,
  stable width ordering, skyline placement, 2.5-pixel UV inset, and blend-type pixel
  preprocessing (§4) `[orig: BuildTextureAtlases @ 0x5e8db0]`.
- **Emit-rate curves** scaling the emission interval (§4) `[orig: AdvanceEmission @ 0x5e1d30]`.
- **Bounded reimpl pools**: the Godot wrapper defaults an emitter to 256 particles, honors
  authored `emit_maxoverride`, and caps either path at 4096; the retail corpus maximum override
  is 400. The native scheduler also rejects non-finite timing and bounds burst work (D-PTL-12).
- Finite preview emitters do not auto-repeat after all particles expire; FOREVEREMIT keeps the
  emitter eligible for continuous spawning. Loose-texture lookup routes through the shared
  texture path resolver (PFF lookup intentionally out of scope).

### Bounded deviations

- **Flip-frame count** (D-PTL-19): the reimpl normalizes to `[1,256]`; retail has no witnessed
  equivalent normalization/cap. This prevents hostile counts from driving unbounded frame-name,
  atlas, and preview work.
- **Curve-ref modifier syntax** (D-PTL-20): the reimpl composes both trailing modifiers; retail
  consumes one. Shipped content uses no combined modifier.
- **Sort partition** (D-PTL-21): the reimpl globally depth-sorts all selected particles instead of
  recreating retail's recursive AABB leaf partition. Overlapping-particle order matches the
  retail batch requirement; exact tie/partition order for spatially disjoint emitters remains open.
- **Distort scene coordinates** (§5.4): the decoded normal/wave/projective equation and
  immutable pre-particle scene copy match; Godot's render-buffer UV convention supplies the
  reimpl mapping for retail's D3D projective coordinate.
- **Global-pass placement**: the shared compositor is a main-camera POST_TRANSPARENT pass,
  preserving packet order and depth-test/no-write behavior. The exact internal split around
  retail's far-water pass remains the render-order residual (D-RORD-7); it is no longer a
  reason to classify or suppress individual effects.

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
  surface normal at the entity's grid cell (`outMillis` / `off_849934` tables). The reimpl starts
  at up, then its attached fx2ssn group follows the live entity basis; the terrain-normal read
  remains unported (D-PTL-7).
- **`.ptu`/`.ptg` alternate set**: `CEffectSystem_Init @ 0x5f6070` loads `*.ptu` — or `*.ptg`
  when `byte_24D4DF9` is set — alongside `*.ptl`; the selector byte's meaning (gore toggle?)
  is unwitnessed, and the runtime port loads only `.ptl`.
- **The flip-frame NAME registrar — RESOLVED 2026-07-14**: N=1 preserves the authored
  literal. N>1 lowercases, truncates at the first `.tga`, and appends `_01.tga` through
  `_09.tga`, then `_10.tga`+; identity is case-insensitive `(name,type)` and there is no
  alternate-name fallback (`CParticleDef_ReloadGraphicFrameTextures @0x5e4bb0`; D-PTL-14).
- **The missing-texture on-screen result**: a never-packed entry renders its quads under the
  null-material default pass (§11) — pin what that actually draws (invisible? last-texture?)
  for the 32 shipped absent-texture layers; the user-observed retail result on the same data
  is "nothing visible".
- **The debug overlay's 3D bounding boxes — RESOLVED 2026-07-13** (§11): the render-batch
  flag `mgr[182]` gates per-emitter red AABBs + footprint quads inside
  `CParticleManager_RenderBatch @ 0x5e9890`. Remaining thread: the SETTER of `mgr[182]`
  (likely one of the `g_ParticlesDisabled`-gated facades) and the WRITER of the emitter's
  accumulated AABB words (the reader is now witnessed).
- **Weapon-action effect completion** (muzzle flash chain) — **direct and impact
  presentation RESOLVED 2026-07-14**:
  local FIRE consumes the ACTION
  `particle`/`particleuserpoint` with SETTLE-gated scoped suppression (the
  `g_weaponScopeActive` promoter `@ 0x4de4f7`), and its suppression window is pinned to the
  group-death callback (`CEffectGroup_SetDeathCallback @ 0x5e1940` →
  `CEffectGroup_Destroy @ 0x5e3460` → `ActionSlot_ClearEffectHandle @ 0x53f760`).
  The recoil arbiter emits the witnessed data-defined direct event (`@ 0x542efa` /
  `@ 0x542f64`), and every payload is submitted as an independent generic `Always`
  transient through its authored action userpoint. REVX02 `WPN_M4AUTO` therefore works when
  its muzzle is authored on RECOIL/`MFLASH01`, without classifying that name or point.
  Local/SP ballistic impacts resolve at the physical collision tick through the authoritative
  `RoundSim`, drain destructively, and submit both particle and 3D sound payloads. The shared
  scene, atlas, immutable packet, and ordered persistent-buffer compositor remove the former
  transient-churn workaround (D-PTL-16 fixed). D-WPN-8 carries the joiner/client presentation
  gap; D-WPN-15 carries selection gaps: the charmap
  sampler `Terrain_GetSurfaceTypeAtPosition @ 0x606510`, the `.TIL` override remap
  `byte_319F7D8`, the water plane, and the entity/building material legs. D-WPN-16 separately
  carries the Knife/instant-kill-zone path; D-WPN-14 records the disproven fire-time-bullet
  reading. The local first-person
  action userpoint now follows the live fake-skinned weapon subobject/bone pose from
  `Entity_ComputeActionTransform @ 0x401310` (fixed 2026-07-14 after a `00TRa.bms` AK gameplay
  witness; pinned by `local_player_presenter_test.gd`). Still open: the exact
  `Player_IsVehicleHasAttackCapability` predicate/vehicle-parent transform, a third-person model
  source, and the `g_FpWeaponViewFlags` weapon-view option (bit 0 treated always-on; the
  profile+1484 default is unwitnessed). Spawn sites still unrouted: projectile travel/explosions,
  the movement wake tiers (`particlefxs`/`particlefxw1..4` — consumer
  `Entity_SpawnBoneEffectsAtMask @ 0x458750`, called from the infantry/vehicle movement
  updaters), the damage-state death/fire/other family (`Entity_UpdateBoneTrailEffects
  @ 0x4589c0`, `Entity_SpawnDeathEffectsAtBones @ 0x4944c0`), the hardcoded vehicle damage set
  (`VehicleEffect_InitAll @ 0x455c20`: `Effect_smkSigB` + `Effect_vehicleFireLarge/Med/Small`,
  globals renamed 2026-07-13), terrain-object userpoint effects
  (`Terrain_SpawnSurfaceEffectsAtUserPoints @ 0x5cea90` / `Terrain_SpawnEffectsAtUserPoint
  @ 0x5cee20`), infantry body effects (`Entity_UpdateInfantryPlayerBody @ 0x4b40e0` sites),
  tracers (`ammo.def tracer_type`/`tracerRate`), the `Mf_Light`/`light_impact` light legs,
  and weather.
- **Descriptor +40/+44 consumers** (attenuation / blend 16.16 fields): the spawner forwards
  them through kong-misnamed calls (`SoundWorld_UpdateChannelAttenuation @ 0x5e5db0`,
  `CEffectWorld_UpdateBlendValues @ 0x5e5df0`); semantics unwitnessed.
- **The PlayerControl occupancy chain's remaining companions** (witnessed 2026-07-15, §4
  occupancy rows): the aircraft-only engine-start sound (`soundProfile defaultRes+120`,
  water-gated; the ground detach `defaultRes+124` stop is D-SND-17); the retail LAZY spawn gate (the spawner
  runs from the render/LOD updater only when the vehicle's render state == 1 — the port
  starts the effect on the claim edge regardless of visibility); the engine-runtime
  accumulator (`+latch` per occupied tick to `0xCCCCCC0`, `−46603` empty) that drives the
  ground-decal intensity and the dust/spray/skid wake dispatch
  (`update_vehicle_effect_emissions @ 0x528f20`, vehicle-def curve words 220..231 —
  effect ids 1=dust/11=spray/21=skid into the global effect-slot system); and the
  engine-state 7 write on detach (the D-NET-157 engine-state model).
- **The heavy-damage vehicle smoke into the shared +0x1CC slot**: `g_FxHandleSmkSigB` at
  hp < max/4 (`Entity_UpdateVehiclePhysics @ 0x48b0f1`, only when +0x1CC is empty) and
  `g_FxHandleVehicleFireMed` into +0x400 at critical hp (`@ 0x48b0bc`) — the port's
  occupancy groups do not yet share a slot with a damage-smoke leg (the
  `VehicleEffect_InitAll` family above).
- **The normal-map gradient sign convention** (`Texture_GenerateNormalMapFromHeight
  @ 0x5e2df0`): the port computes `b(x−1)−b(x+1)` / `b(x,y+1)−b(x,y−1)`; the retail FPU
  chain's operand order is unreduced — pin the sign pair if bump lighting ever looks
  inverted on authored height textures.
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
- Retail's persistent emitter AABB accumulator/write/reset lifecycle (§4, `TranslatePosition`) is
  unported. The reimpl reconstructs bounds from the current particle snapshot for sorting and
  diagnostics, so empty-emitter and reset-timing details can still differ from retail's manager.
- `CParticleEmitter_BuildOrientationMatrix @ 0x5f3970` port for full ORBIT frame fidelity.
- Collision sounds (20 `collide_sound` slots) and `elastic` bounce behavior — resolve pass
  witnessed, runtime unported.
- **Effect/particledef NAME lookup case semantics — RESOLVED 2026-07-16**: every
  by-name walk in the effect system is `_stricmp`, witnessed at all three legs —
  the interned-pool scan (`CEffectWorld_InternEffectHandle @ 0x5f7310`, compare
  `@ 0x5f736b`), the effect-def find (`CEffectWorld_FindEffectDefByName
  @ 0x5e34f0`, compare `@ 0x5e352c`), and the particle-def find used by the
  EFFDEF→PARDEF `pdefs` resolve (`CEffectWorld_FindParticleDefByName @ 0x5e41d0`,
  compare `@ 0x5e420c`, caller `@ 0x5e4920`). `ParticleFile::find_particle`,
  the `EffectScene` catalog maps, and `NovaParticleFile::find_effect`/
  `find_particle` now fold case to match (§4 witness rows).

## 9. Divergence catalog (D-PTL)

Stable IDs for the behavior gaps described above (the §1 intentional parse divergence and the
§6 bounded deviations); dispositions in the canonical vocabulary of
[divergence-ledger.md](../divergence-ledger.md). Pure "not yet researched" items with no
witnessed behavior gap stay in §8.

| ID | Divergence | Disposition |
|---|---|---|
| D-PTL-1 | `g{N}_color{M}` dispatch (§1): the engine's outer dispatcher remaps `g2_color1`/`g3_color1`/`g3_color2`/`g4_color1`/`g4_color2` into higher color slots; the reimpl maps `g{N}_color{M}` → `graphics[N-1].color[M]` correctly | **PERMANENT** — a recorded intentional divergence from an original parse bug. [orig: CParticleDef_ParseProperties @ 0x5ea320] |
| D-PTL-2 | The former reimpl path created one independently sorted/uploaded mesh surface per packet command, allowing material/surface limits and reimpl transparent sorting to violate the engine-wide particle order under transient churn | **FIXED 2026-07-14** — one immutable four-vertex quad packet, renderer-side `0/1/2/1/3/2` triangle expansion, one persistent growable RD vertex buffer, and sequential command draws preserve the compiler's deterministic order without per-emitter Nodes. [orig: CParticleManager_RenderBatch @ 0x5e9890] |
| D-PTL-3 | `mod2x` formerly approximated `DESTCOLOR`/`SRCCOLOR` with a doubled `blend_mul` shader | **FIXED 2026-07-14** — the RD pipeline uses the witnessed `SRC=DESTCOLOR, DST=SRCCOLOR` factors directly; the reimpl render target owns only the platform color-space convention |
| D-PTL-4 | `bump`/`bumpadd` formerly rotated the light around view-Z and saturated its byte encoding | **FIXED 2026-07-14** — the Godot scene-to-quad adapter evaluates `transpose(Rx(roll) × view)`, transforms `bump_scale × (+k,+k,+k)`, and retains the original truncating conversion's low byte before the DOT3 pipelines; the portable compiler owns ordering and batching of the authored quads (§5.3) |
| D-PTL-5 | `distort` formerly used an arbitrary fixed-strength `SCREEN_UV` offset | **FIXED 2026-07-14** — the exact decoded normal/wave/projective equation samples one immutable pre-particle scene-color copy; Godot render-buffer UV remains the bounded reimpl mapping (§5.4) |
| D-PTL-6 | Atlas registration, page allocation, preprocessing, and inset were formerly approximated by a per-emitter horizontal shelf | **FIXED 2026-07-14** — the portable shared builder implements the witnessed name identity, 1024/256 type families, type-1/2 sharing, stable width sort, skyline allocator quirks, exact rect, 2.5-pixel inset, alpha clear, and type-3/6/7 conversions [orig: BuildTextureAtlases @ 0x5e8db0] |
| D-PTL-7 | Scripted-spawn initial orientation (§4 runtime chain): the WAC fx handlers pass the terrain surface normal at the entity's grid cell; the reimpl starts at up, then follows the attached entity basis | **OPEN** — port the terrain-normal read (§8). [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0] |
| D-PTL-8 | Unknown effect name at intern (§4 runtime chain): the engine clones `stockeffect` under the requested name | **CLOSED 2026-07-12** — `NovaEffectWorld.intern_effect` clones the mounted `stockeffect` entry under the requested name (0 only when no stockeffect is mounted). [orig: CEffectWorld_InternEffectHandle @ 0x5f7310] |
| D-PTL-9 | Direction sampling (spawn velocity + sphere/cone shape caps): the reimpl reconstructs the retail helper's independently bounded signed yaw/pitch rotations on a perpendicular basis, including `spread_skip`; it does not call the original orientation-helper vtable (`CEffectEmitter_SetOrientationFromDirection @ 0x5e5b00` family) | **PERMANENT (bounded)** — authored component bounds and two-draw cadence match; portable LCG and exact DirectX/FPU basis construction are not byte-identical. [orig: SpawnParticle @ 0x5e7640] |
| D-PTL-10 | GFXFLIPRAND start offset: the engine derives the per-particle flipbook offset from the particle SLOT POINTER (`(ptr + (ptr>>3)) % frames`); the port derives it from the particle serial | **PERMANENT (bounded)** — same distribution intent; the engine's value is address-dependent and unreproducible by design. [orig: BuildBillboardQuads @ 0x5f4f6e-family] |
| D-PTL-11 | Scale-LUT lerp upper byte: the engine reads `lut[i+1]` unguarded — one byte PAST the 256-byte LUT at i=255 (adjacent heap memory); the port clamps to `lut[255]` | **PERMANENT (bounded)** — the engine's overread value is heap-layout-dependent; clamping bounds the final 1/256th of the curve. [orig: the flag-0x10 lerp block @ 0x5f51ab-analog in both render paths] |
| D-PTL-12 | Emitter pool capacity: the reimpl default is 256 and every authored/direct override is capped at 4096; the exact retail manager-wide ceiling is unwitnessed (shipped corpus maximum override 400) | **PERMANENT (bounded safety)** — keeps substantial authored headroom while preventing hostile rate/burst inputs from allocating or looping without bound. |
| D-PTL-13 | Parser strictness: our parser hard-failed a WHOLE file on any unrecognized top-level line or `=`-less in-block line, where retail IGNORES lines its parsers don't claim — RevX02's `joAmmoHit.ptl` carries a `//====` divider between blocks, so the file was rejected and its 18 `Effect_AmHit*` impact effects all fell to the invisible `stockeffect` clone (the D-PTL-8 path): every bullet impact silently vanished | **FIXED 2026-07-13** — both sites skip-and-continue (`particle_lenient_lines` ctest pins the divider + `=`-less stray). [orig: CEffectWorld_ParseSectionCallback @ 0x5ecb40] |
| D-PTL-14 | Flipbook frame-texture naming formerly guessed fallback names and missed shipped frames. The retail registrar is now exact: one-frame graphics keep the authored name verbatim; multi-frame graphics lowercase the authored string, truncate at the first `.tga`, and append `_01.tga`…`_09.tga`, then `_10.tga` and above. Identity is case-insensitive `(name,type)`. There is no literal-base probe or trailing-letter fallback | **FIXED 2026-07-14** — the shared atlas registrar implements the exact routine and CI contracts pin N=1, N>1, case/type identity, 9→10 formatting, and the absence of fallback probes. One bounded edge: a multi-frame authored name WITHOUT `.tga` null-derefs retail (`*strstr(...) = 0` unguarded `@ 0x5e4c91`); the port skips the truncation and derives normally — original-crash class. [orig: CParticleDef_ReloadGraphicFrameTextures @ 0x5e4bb0, ex kong `CParticleTableDef_ReloadAllTextures`] |
| D-PTL-15 | Static-batch entities formerly had no per-node model data, so their authored `particlefx` never spawned | **FIXED 2026-07-14** — `MissionObjectPlacer` retains one value descriptor per successfully batched entity (item id, immutable object data, base world transform); `GameWorld` applies the original pool gates and first-16 duplicate-name mask, then submits persistent World-bound groups at the composed user point or entity-origin fallback. No vehicle render node or follow-owner workaround is introduced. The `fxs`/`fxw*` movement tiers and death/fire/other family remain separate §8 threads. [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 → Entity_SpawnBoneTrailEffect @ 0x43bef0] |
| D-PTL-16 | Casing-point recoil and ballistic-impact groups were suppressed as a guard against per-emitter Node/material/atlas/upload churn; direct muzzle rows were temporarily name/point classified and guarded, violating retail's generic unsuppressed direct leg | **FIXED 2026-07-14** — every authored direct row and every resolved impact row now enters the same value-owned EffectScene as a generic `Always` transient. One shared atlas and ordered persistent-buffer compositor absorb the churn without changing weapon/FSM timing or creating vehicle render Nodes. [orig: WeaponAction_Recoil @ 0x542dd0; Projectile_SpawnImpactEffect @ 0x4e9b80] |
| D-PTL-17 | The PlayerControl occupancy effect (`particlefx` on an occupied vehicle) runs class-wide in the port: every `attrib & 0x40` item starts its slot-A effect on the +368 claim and stops on the claimant's departure. Retail reaches the spawner ONLY through the `CHel`/`cpln` class updater — the shipped ctank/cbike/cveh `Effect_heloHeat1`/`Effect_whiteExhaust` authors are dead data in JO retail (§4 occupancy row) | **PERMANENT (intentional, small)** — presenting authored-but-unreachable retail data on ground vehicles is the point of the port's generic routing; the claimant protocol, start/stop edges, and per-vehicle single-claim semantics are the witnessed ones. Revisit only if a retail-parity scene comparison needs helicopter-only behavior. [orig: the class fn table @ 0x82ac00; entity_update_damage_accumulator_and_shadow @ 0x48fa70] |
| D-PTL-18 | The witnessed skyline placer rewrites covered columns to `skyline[best]+height`, which can LOWER a taller column and let a later rect overlap an earlier one; its 2.5-pixel inset is uncapped and inverts the UV window on rects ≤ 5 px | **PERMANENT (bounded, original-garbage class)** — the port keeps the witnessed allocator verbatim but treats its result as a proposal: an occupied-rect intersection guard retries the next page and covered columns restore via `max()`, and the inset caps at half the rect extent. Reproducing the overlap would manufacture heap-layout-dependent garbage (ADR 0022). [orig: CParticleAtlas_TryPlaceEntry @ 0x5e2be0, ex kong `CEffectChannel_TryAssignSlot`] |
| D-PTL-19 | Flipbook frame count: retail carries the authored count into frame registration with no witnessed reimpl-style normalization; the port forces non-positive values to 1 and caps counts at 256 | **PERMANENT (bounded safety)** — bounds parse/bake/atlas/preview work and avoids hostile or nonsensical counts. Shipped content is below the cap. [orig: CParticleDef_ReloadGraphicFrameTextures @ 0x5e4bb0] |
| D-PTL-20 | Curve-ref modifier syntax: retail consumes one trailing `reverse` OR `inverse` token; the port consumes all trailing modifier tokens and composes both in either order | **PERMANENT (bounded compatibility superset)** — shipped content uses at most one modifier; combined modifiers remain useful and deterministic for mods. [orig: CParticleDef_ParseProperties @ 0x5ea320] |
| D-PTL-21 | Cross-emitter sorting: retail recursively partitions non-overlapping emitter AABBs, then globally particle-sorts each overlapping leaf; the reimpl globally particle-sorts the whole selected domain | **OPEN (exact algorithmic parity)** — overlapping emitter particles now interleave correctly and spatially disjoint differences are normally invisible, but exact retail leaf/tie order is not claimed. [orig: CParticleManager_RecursiveSortAndRender @ 0x5ec980; CParticleManager_RenderBatch @ 0x5e9890] |
| D-PTL-22 | Section tags and known property keys were compared case-sensitively by the port even though the retail parser family uses `_stricmp` throughout | **FIXED 2026-07-16** — all four tags and effect/particle/graphic/table/edit-handle keys fold ASCII case; unknown keys retain authored spelling. Mixed-case regression in `particle_lenient_lines`; no shipped corpus trigger. |
| D-PTL-23 | Duplicate table resolution treated TableDef+0x248 as an owner and always chose the first name match. Retail uses +0x248 as the inverse/reverse transform mask: unmodified refs choose the first base; modified refs reuse a cached transform or clone/transform the last base | **FIXED 2026-07-16** — native and legacy Godot lookup reproduce first-unmodified/last-modified selection; duplicate-selection ctest pins it. The 7 differing duplicate-name sets have no modified shipped reference. [orig: CParticleManager_FindTableDefByName @ 0x5e9540; transform @ 0x5e2700] |

WANDER/BUBBLE (engine-vestigial, zero xrefs), persistent emitter-AABB lifecycle, the full ORBIT
orientation-matrix/age-chain port, exact recursive sort partition (D-PTL-21), collision sounds,
and the pending parser decompiles remain §8/research items. The fixed rows above establish the
witnessed slices; they do not make the subsystem a blanket byte-exact retail port.

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

2026-07-13 session (the PR #237 deep-dive grill — weapon/vehicle particle chains):

- Renames (auto-named globals, anchored): `dword_24D20C0` → `g_FpWeaponViewFlags` (bit 0
  gates the FP viewmodel draw + local FP fire effects); `off_813420` →
  `g_AmmoEffectTagTable` (the 28-entry effects_table tag table); the ammo effects_table
  staging set `dword_A2E95C/dword_A2E960/dword_A2E964/word_A2E968/dword_A2E96C/word_A2E970`
  → `g_ammoParseInFxTable/g_ammoFxStageCount/g_ammoFxStageTagId/g_ammoFxStageEffectHandle/`
  `g_ammoFxStageSoundId/g_ammoFxStageZero`; the vehicle damage-effect handles
  `dword_AE0764/dword_AE0760/dword_AE075C/dword_AE0758` → `g_FxHandleSmkSigB/`
  `g_FxHandleVehicleFireLarge/g_FxHandleVehicleFireMed/g_FxHandleVehicleFireSmall`.
- Witness comments: `0x53f760` (the bogus "sun direction" comment replaced with callback
  semantics; the 2026-07-16 follow-up below pins it specifically to GROUP death),
  `0x402577` (ActionDef+16 particle handle),
  `0x4025b3` (ActionDef+186 particleuserpoint name → +57/+56 resolve), `0x4e88c3`
  (impact-table row shape + the canonical tag order + selection rules), `0x40a587`
  (the effects_table count column discarded), `0x522ee0` (the ITEMS.DEF per-item
  effect-slot offset map), `0x606573` (unmapped sector → surface 7 → water).
- PROPOSED, not applied (human-curated struct field): `GamePlayerEntity.ownerSession`
  (entity+0x1CC) → `attachedEffectHandle` (§5.5).
- IDB saved.

2026-07-15 session (the #237 rewrite grill — occupancy lifecycle, atlas/registrar,
blend factors):

- Renames (kong misnomers, anchored): `CEffectChannel_TryAssignSlot @ 0x5e2be0` →
  `CParticleAtlas_TryPlaceEntry` (the atlas skyline placer); `CParticleTableDef_ReloadAllTextures
  @ 0x5e4bb0` → `CParticleDef_ReloadGraphicFrameTextures` (it iterates a particle def's
  graphic layers).
- Retype: `Texture_GenerateNormalMapFromHeight @ 0x5e2df0` → 5 args (the trailing
  `char forceBlueOpaque` the kong prototype hid; type-7 passes 1).
- Witness comments: `0x48faad` (the PlayerControl occupancy gate), `0x4356f4` (the
  claimant-only stop leg), `0x5e2d00` (the atlas entry rect-write order), `0x5e4c91`
  (the flipbook derivation + the missing-`.tga` null-deref).
- Comment corrections: `0x4946d0` (the wrong `2=driver/3=passenger/5=gunner` auto-comment
  → the witnessed sitex/ctrlx/UseGun/drvrx codes), `0x5e8283` (the "Type 7" note sat on
  the type-3/6 0.125 call).
- IDB saved.

2026-07-15 session 2 (the greenish fire-barrel regression — curve-table name resolve;
witnessed by direct disassembly of the on-disk image, the IDA MCP bridge being wedged):

- Write-backs APPLIED later the same day (the bridge recovered): `0x5e9540` already
  carried the accurate curated name `CParticleManager_FindTableDefByName` — kept (the
  clone side effect lives in the comments); witness comments landed at `0x5e95ac`
  (`_stricmp` compare — table names are case-insensitive), `0x5e95b8` (then given the
  now-disproved ownership interpretation), `0x5e9652` (the resolve-miss log +
  `entry+0x44 = 0`);
  idb_save run. **The +0x248 owner label was wrong and is superseded by the 2026-07-16
  adversarial re-read below.**
- Port fix landed with the witness: `bake_one_curve` + every `find_table` seam were
  exact-case; ambfx.ptl's `Wood_AmbFB[s]` authors `green_func = Table11Alt` against
  `id = table11Alt`, so green never baked (constant 255) while red/alpha faded with
  `table11Alt` — aging flame sprites tint green, unmasked when the witnessed
  MODULATE fragment replaced the invented premult RGB-by-alpha rescale. Verified by
  before/after `firebarrel_visual_probe.gd` captures (Effect_FireBarrelS: green wisps →
  retail orange) and the new case-fold ctest/GUT cases.

2026-07-16 session (the effect/particledef name-lookup case grill — closes the
last §8 case-semantics thread):

- Witnessed: all three remaining by-name walks compare with `_stricmp` — the
  interned-pool scan (`CEffectWorld_InternEffectHandle @ 0x5f7310`, compare
  `@ 0x5f736b`), the effect-def find (`@ 0x5e34f0`, compare `@ 0x5e352c`, the
  world's +70 list), and the particle-def find (`@ 0x5e41d0`, compare
  `@ 0x5e420c`, the +82 list) that the EFFDEF pdefs resolve (`@ 0x5e4920`,
  64-byte name slots from +340, `UNRESOLVED: EFFDEF %s missing PARDEF %s`)
  routes through.
- Renames APPLIED (idb saved): `CEffectWorld_FindDefByName @ 0x5e34f0 →
  CEffectWorld_FindEffectDefByName`; `CEffectWorld_FindTableDefByName
  @ 0x5e41d0 → CEffectWorld_FindParticleDefByName` (kong trap — it walks the
  particle-def list, not the table list; §5.5 row added). Witness comments at
  `0x5e41d0`, `0x5e34f0`, `0x5e4920`.
- Port fix landed with the witness: `ParticleFile::find_particle` →
  `strutil::iequals`; `EffectScene` `definition_by_name` keys + the pdefs
  probe → `fold_ascii` (identity AND duplicate detection);
  `NovaParticleFile::find_effect`/`find_particle` → `nocasecmp_to`. Pinned by
  `pdef_reference_resolution_is_case_insensitive_contract` (effect-scene
  contract ctest) and the minimal-effect `find_particle` fold check.
- Same session, the ONED preview emitter's flipbook resolution aligned with the
  witnessed registrar: the pre-witness interim probes (literal-base first, then
  `<stem>_NN` with the authored case/extension, then a trailing-letter variant
  strip) are deleted — `NovaParticleEmitter` now derives frame names through
  the shared `retail_particle_frame_name` (D-PTL-14's exact routine), so the
  editor preview resolves exactly what the game runtime resolves.
- Same session, the resolve SEMANTICS: the EFFDEF vtable+8 resolve
  (`CEffectBank_ResolveAllEntries @ 0x5e4920`, slot `@ 0x7dca2c`) is
  **all-or-nothing** — the first missing PARDEF (or a failing PARDEF
  self-resolve) breaks `@ 0x5e495d`, logs once, clears the whole entry buffer
  `@ 0x5e49be`, and returns 0; `CParticleManager_ResolveAllReferences
  @ 0x5ec850` (the +94 table transform walk + vtable+8 over the +70/+82
  lists, reached from the world-ready gate `sub_5F68E0 @ 0x5f68e0`) keeps
  failed defs in the lists. Our port had kept the resolvable subset —
  fixed: `EffectScene::open` now clears and stops at the first miss
  (`effect_resolve_is_all_or_nothing_contract`). Also observed:
  `CEffectWorld_IsNameUnresolved @ 0x5ea150` (tri-table name probe) has
  zero xrefs — dead code in retail.

2026-07-16 adversarial PR #237 follow-up (corrections to prior witness claims):

- `CParticleManager_FindTableDefByName @ 0x5e9540` takes transform flags and
  TableDef+0x248 stores the applied mask (`inverse=1`, `reverse=2`); it is not ownership.
  Flags 0 return the first case-insensitive base, while a modified miss clones/transforms the
  last base unless the matching transformed clone is already cached. The native and legacy
  Godot lookup paths now reproduce that selection (D-PTL-23). The stale IDB owner comment is
  recorded here as superseded; this read-only review did not claim another IDB save.
- The section dispatcher and property parser family compare tags/keys with `_stricmp`.
  `parser.cpp` now folds all known section/effect/particle/graphic/table/edit-handle tokens and
  retains unknown authored spelling (D-PTL-22). The retail corpus has no mixed-case trigger.
- `CParticleManager_RenderBatch @ 0x5e9890` constructs a shared particle-depth list for an
  overlapping recursive leaf and sorts it at `0x5e9b63`; emitters may interleave. The compiler
  now uses a shared global depth list. Exact recursive leaf partition remains D-PTL-21.
- `CEffectGroup_SetDeathCallback @ 0x5e1940` stores the action callback on the GROUP and
  `CEffectGroup_Destroy @ 0x5e3460` invokes it. `CEffectGroup_AdvanceChildrenAndReap`
  at `0x5e59a0` destroys dead children individually without firing the group callback. The prior
  first-child suppression inference is disproved; the port now keeps `SuppressWhileOwned`
  until the final child is gone and immediately recycles each finished child (D-WPN-19).

## 11. The retail debug family (witnessed 2026-07-13)

Retail ships an intact particle debug-page pair (the whole `Debug_Draw*` page
family is UNREFERENCED in the retail image — the page dispatcher/key wiring is
stripped, the functions and their globals survive; mimic the CONTENT, not the
wiring). Witnessed from the kong IDB:

- **`Debug_DrawParticleStats @ 0x44c840`** — the counts + emitter-list page:
  - Header: `"Current Particle Count:  %ld / %ld"` — current vs PEAK. Current
    comes through the checked facade `EffectWorld_GetActiveEntryCountChecked` (0 when the global disable
    byte `byte_24D261D` is set, the world is absent, or its init probe
    `sub_5DF7A0` fails; otherwise `CEffectWorld_GetEntryCount(g_EffectWorld)` = the world's
    active-entry count, the used size of the circular buffer at `world+116`).
    The peak global (`dword_A895E0`) latches the max and RESETS TO ZERO when
    the current count hits 0.
  - Body: iterates from the shared debug scroll global (`dword_A895B4`, also
    written by `Debug_ScrollPageUp @ 0x44a390` / `Debug_ScrollPageDown
    @ 0x44a900`), listing names via `EffectWorld_GetEntryNameByIndex(group, index)` → the entry's
    vtable slot-0 name getter; first row of a group prints `"%02ld   %s"`,
    subsequent rows indent `"      %s"`. Rows draw at x=566 from y=148, step
    20, clipped at y>588.
- **`Debug_DrawEffectBrowser @ 0x44c950`** — the interactive browser page:
  - Header rows at (25, 20/40/60/80), color 0xFFC10DFF-style (-4128769):
    `"Total: %d"` (registered effect count via `CEffectWorld_GetEffectDefCount(world)`),
    `"Current: %d"` (the selection global), `"Effect: %s"` (selected
    effect's vtable slot-0 NAME), `"File: %s"` (vtable +20 — **each effect
    knows its source file**; the browser displays which .ptl provided it).
  - A 20-row scrolling window (`"   %d  %s"`) with per-row color: RED
    (0xFFFF0000) for the row whose debug instance is live, GREEN
    (0xFF00FF00) for the selection.
  - The spawn latch `dword_A895B0` is armed by `Debug_CheckPageIsSix
    @ 0x44a8e0` (`page global dword_A895A4 == 6`); when set, the browser
    DESPAWNS the previous debug instance (`sub_5E57C0`, handle
    `dword_A895E8`, validity probe `sub_5E5FF0`) or SPAWNS the selected
    effect at `camera_pos + camera_fwd >> 3` (fixed-point camera globals →
    `Math_FixedPointToFloat3_YNegated @ 0x611210` → `CEffectWorld_AllocGroupAndSpawn(world, …,
    effect, origin, dir, 1)`).
- **Registration-time miss log**: `CEffectManager_RegisterAllMaterials
  @ 0x5f79c0` interns a fixed 78-pair table of engine-referenced effect
  names (`off_8490D0/off_8490D4`) and logs
  `"Particle Effect Not Found! (%s)\n"` via `ErrorLog_WriteTimestamped
  @ 0x53c6d0` for every miss, then runs `CParticleManager_ResolveAllReferences
  @ 0x5ec850`.
- **The 3D bounding boxes — WITNESSED 2026-07-13** (the user's pointer:
  `Render_DrawDebugBoundingBox @ 0x5e06e0`): the boxes are drawn INSIDE the
  particle render pass, not by a debug page. `CParticleManager_RenderBatch
  @ 0x5e9890`, gated on ONE manager flag (`mgr[182]`, dword offset +0x2D8),
  draws after each emitter's run dispatch: the emitter's flat footprint
  rectangle (`Render_DrawDebugExtents @ 0x5e0cc0` — center words 2,3 ±
  half-extent words 5,6 via `Render_DrawDebugBoundingQuad @ 0x5e0520`) plus
  the emitter's ACCUMULATED 6-float AABB (def+212 word + emitter words
  54..58) as a wireframe box in **opaque red `0xFFFF0000`** — every box the
  same color; and once per batch the batch-total AABB (±3.0-padded per
  emitter) as a bounding quad. `Render_DrawDebugBoundingBox` itself: 8
  corners from a min/max pair, 24 line-list segments, unlit/untextured with
  D3D state save/restore. `CParticleEmitter_DrawDebugAABB @ 0x5e0d40`
  (ex `sub_5E0D40`, whose auto-comment was wrong) is the standalone
  per-emitter convenience wrapper.

### Texture entries, the atlas exclusion gate, and the null-material path

- A texture ENTRY carries its (already per-frame) file name at +24; the size
  probe `CParticleTextureEntry_ProbeSizeFromDisk` builds `path = manager+732 dir ⧺ name` (`CParticleManager_BuildTexturePath` —
  plain concat, NO frame derivation here), header-loads via
  `CTextureData_LoadTGA @ 0x5f7b20`, and stores width/height at +288/+292;
  a load failure leaves the size unset and returns 0.
- `CParticleManager_BuildTextureAtlases @ 0x5e8db0` collects ONLY entries
  with `+288 > 0` — **a missing texture's entry is never packed into any
  atlas**.
- The graphic object: `+0x2CC` flip-frame COUNT, `+0x2D0` flip rate,
  `+0x2D4` the per-frame ARRAY of texture-entry pointers (the billboard
  renderers index it per frame; GFXFLIPRAND offsets `(ptr + ptr>>3) %
  frames` — D-PTL-10's form). The renderer dereferences `entry[0]` = the
  MATERIAL object and rebinds on change via `CParticleBatch_FlushAndBindMaterial
  @ 0x5e4230`: material TYPE byte selects fog/blend mode (1/2/6→2, 4→3,
  5→1, else 0), shader pass from material[1]/[2], `SetTexture(material[4])`.
  **A NULL material** takes the else path: default render state
  (`CD3DDevice_GetRenderStateByIndex(…, 2)` + a validated default pass) and
  the channel's current-material cleared — the quads are still emitted under
  that default state. What that renders on screen for the shipped
  missing-texture layers (SMOKE1-3 etc.) was not pinned before the bridge
  died; the user-observed retail behavior on the same data is "nothing
  visible" (§8 follow-up).

### IDB write-backs (APPLIED 2026-07-13, idb saved)

Applied at anchored confidence (plus `sub_5E0D40 → CParticleEmitter_DrawDebugAABB`, `byte_24D261D → g_ParticlesDisabled`, and the mislabeled `newSize → g_DebugBrowserSelection` / `group_index → g_DebugPageScroll`): `EffectWorld_GetActiveEntryCountChecked →
EffectWorld_GetActiveEntryCountChecked`, `EffectWorld_GetEntryNameByIndex →
EffectWorld_GetEntryNameByIndex`, `sub_5DFED0 → CEffectWorld_GetEntryCount`,
`sub_5DFAA0 → CParticleTextureEntry_ProbeSizeFromDisk`, `CParticleManager_BuildTexturePath →
CParticleManager_BuildTexturePath`, `CEffectWorld_GetEffectDefCount →
CEffectWorld_GetEffectDefCount` (probable), plus entry comments on
`Debug_DrawParticleStats` / `Debug_DrawEffectBrowser` globals
(`dword_A895A4` = debug page index, `dword_A895B0` = page-6 action latch,
`dword_A895E0` = particle peak, `dword_A895E8` = browser debug-spawn
handle). The misnomer pair `CEffectWorld_ResizeAndGetBufferSize` /
`CEffectWorld_ResizeCircularBuffer` (both are get-entry-by-index readers)
needs the rename-everywhere treatment.
