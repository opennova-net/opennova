# `.ptl` text format — narrative spec

A `.ptl` file is a plain-text effect/particle definition file consumed by the
Joint Operations particle system at game-load time. The runtime parses it via
the same `CConfigReader` machinery used for other text configs (e.g. `.def`).

This document captures the grammar observed across the 77-file corpus at
`~/Desktop/JO_ASSETS_t/*.ptl` and cross-checked against the engine parser
functions enumerated in [`ida_particle_witness.md`](ida_particle_witness.md).

## 1. Encoding

- **Line endings**: CRLF (`\r\n`). The C++ parser strips trailing `\r` after splitting on `\n`.
- **Charset**: ASCII. No multi-byte sequences observed.
- **Trailing NUL**: at least four corpus files (`30MM.ptl`, `airexp.ptl`, `ambfx.ptl`, `df_exp.ptl`) end with a single `\0` byte. The parser treats trailing NULs as whitespace/EOF.

## 2. Section structure

A file is a sequence of sections. Each section has the form:

```
[<section-tag>]
{
    <statement>
    <statement>
    ...
}
```

A section tag and its `{` always live on their own lines. The closing `}` may be
followed by an optional `;` (e.g. `boatwake.ptl:45` writes `};`). Blank lines
between sections are ignored.

### Section tags

Four tags exist in the binary (xref'd to `CEffectWorld_ParseSectionCallback @ 0x5ecb40`):

| Tag | Purpose |
| --- | --- |
| `[effectdef]` | Top-level effect: an `id` and an ordered list of `pdefs` (particle definitions composed into the effect) |
| `[particledef]` | One particle template — emission/physics/visual params + up to 4 graphic layers |
| `[tabledef]` | Curve lookup table (32 rows × 8 `uint8_t` values) referenced by `*_func` keys on particle defs |
| `[tabledef_edithandles]` | Editor metadata for a `[tabledef]` (handle count, tightness). Runtime ignores |

## 3. Statements

Inside `{ ... }`, every non-blank line is a statement of the form:

```
<key> = <value>[, <value> ...] ;
```

Whitespace (tabs and spaces) around `=` and `,` is freely mixed and trimmed.

### Value types

Inferred from per-key handlers in `CParticleDef_ParseFromConfigMap @ 0x5ed210`:

| Type | Example | Parsed by IDA call |
| --- | --- | --- |
| identifier (whitespace-allowed) | `id = Buildup dots;` | `CConfigReader_GetString` |
| flag string | `flags = EMITVECTOR AMBIENTCOLOR;` | `CConfigReader_GetString` then `FlagTable_ParseFromString` |
| int | `emit_burst = 1;` | `CConfigReader_GetInt` |
| float | `scale = 2.000;` | `CConfigReader_GetFloat` |
| packed RGB | `color1 = 220, 183, 140;` | `CConfigReader_GetPackedRGB` |
| vec3 | `gravity_mask = 1.000, 1.000, 1.000;` | `CConfigReader_GetVec3` |
| ID list | `pdefs = beginFlash, chunksNbits, dirtCloud;` | string + comma-split |
| curve ref | `scale_func = table12 reverse;` | string; trailing `reverse` toggles direction |
| graphic decl | `graphic1 = mbFlash2.tga, additive;` | filename + blend-mode lowercased |

Keys not recognised by `ParseFromConfigMap` are silently ignored by the engine
(no entry in its dispatch switch). The C++ parser preserves them in
`ParticleDef::unknown_keys` for forward compatibility with future editor
additions, rather than failing the parse.

## 4. Comments and quoting

- **No comment syntax** — searched corpus for `//`, leading `;`, leading `#`. Zero hits across `30MM`, `airexp`, `ambfx`, `boatwake`, `buildup`, `heathaze`, `stock`, `troytabl`. Will revisit if the smoke test surfaces any.
- **No quoted strings** — values are bare tokens, terminated by `,` or `;`.

## 5. Cross-references

At link time the engine resolves these references by string id:

- `[effectdef].pdefs` → list of `[particledef].id`
- `<particledef>.*_func` (`scale_func`, `alpha_func`, `red_func`, `green_func`, `blue_func`, plus per-graphic `g{1..4}_*_func`) → `[tabledef].id`
- `<particledef>.child_id` → another `[particledef].id` (sub-emitter spawn)
- `[tabledef_edithandles].tableid` → `[tabledef].id`

Resolution lives in `CEffectDef_ResolveAllReferences @ 0x5e9d70` (deferred —
out of scope for the parser; the C++ parser stores ids verbatim).

## 6. `[effectdef]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `id` | identifier | Unique within file |
| `pdefs` | id list | One or more `[particledef]` ids; emitted in order |

## 7. `[particledef]` keys

The full set, transcribed from `CParticleDef_ParseFromConfigMap @ 0x5ed210`
(call sequence numbered as in the decompile). Per-graphic-layer keys are
documented in §8.

| # | Key | Type | Engine offset | Notes |
| --- | --- | --- | --- | --- |
| 0 | `id` | identifier | +0 | |
| 1 | `child_id` | identifier | +132 | Optional sub-emitter |
| 2 | `flags` | flag-string | +136 (bits) | `FlagTable @ 0x846A18` |
| 3 | `move` | enum-string | +140 | `FlagTable @ 0x848800` |
| 4–7 | `emit_dur`, `emit_dur_adj`, `emit_rate`, `emit_rate_adj` | float | +3616/+3620/+3624/+3628 | |
| 8 | `emit_rate_func` | curve ref | (string) | |
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
| 35 | `orientationadj` | vec3 | +3828 | (note: no underscore between `orientation` and `adj`) |
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
| 55–83 (×4) | per-graphic block — see §8 | | +148/+936/+1724/+2512 | |

The `lod` key (e.g. `lod = 0.000;` in `30MM.ptl:34`) is observed in the corpus
but is **not** in `ParseFromConfigMap`'s dispatch switch — it's likely consumed
by the section-line tokenizer `CParticleDef_ParseProperties @ 0x5ea320` before
the config map is built. The C++ parser captures it explicitly on `ParticleDef`
to preserve it.

## 8. Graphic layers (`graphicN`, `gN_*`)

Each `[particledef]` may declare up to 4 graphic layers. A layer is opened by:

```
graphic{1,2,3,4} = <texture>, <blend_mode>;
```

`<texture>` may be empty (e.g. `graphic1 = , blend;` in `stock.ptl:163`); the
runtime renders this as a blank ("invisible-but-spawning") layer.

Subsequent `g{N}_*` keys in the same `{ }` block configure the layer:

| Key | Type | Notes |
| --- | --- | --- |
| `g{N}_flip_frames` | int | default 1 |
| `g{N}_flip_rate` | int | default 8 |
| `g{N}_color1` … `g{N}_color4` | packed RGB | per-graphic override; falls back to particle-level |
| `g{N}_alpha` | float | inherited from particle then overridden |
| `g{N}_scale`, `g{N}_scale_adj` | float | inherited then overridden |
| `g{N}_scale_func`, `g{N}_alpha_func`, `g{N}_red_func`, `g{N}_green_func`, `g{N}_blue_func` | curve ref | |

The engine `qmemcpy`s the particle-level color/alpha/scale defaults into each
layer slot first, then per-graphic keys overwrite. The C++ parser does the same.

## 9. `[tabledef]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `id` | identifier | Looked up by `<particledef>.*_func` |
| `tl1` … `tl32` | 8 × `uint8_t` | 256-byte LUT total |

The corpus uses 32 rows × 8 values exactly. The smoke test asserts this.

## 10. `[tabledef_edithandles]` keys

| Key | Type | Notes |
| --- | --- | --- |
| `tableid` | identifier | References a `[tabledef].id` |
| `handlecount` | int | Editor curve handle count |
| `tightness` | int | Editor smoothing param |

This section is editor-only; the runtime ignores it.

## 11. Validation rules

- `emit_burst < 1` is silently clamped to `1` by the engine. The C++ parser preserves the source value but we may want to surface a normaliser later.
- `orbital_axis` defaults to `{0,1,0}` when absent.
- `flip_frames` defaults to `1` when absent on a graphic layer.
- Duplicate `emit_dur` keys (observed in many files: e.g. `30MM.ptl:36` and `:43`) are last-wins.

## 12. Open questions

See [`open_questions.md`](open_questions.md) for items that need follow-up RE
or runtime work to fully resolve.
