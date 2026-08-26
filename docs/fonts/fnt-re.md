# FNT bitmap fonts — reverse-engineering record

Structure-mapping record for the original engine's **`.fnt`** bitmap-font format
(the menu/HUD fonts `Arial12b`/`14n`/`14b`/`16n`/`16b`, `Impac22b`, `Impac38b`,
`Arials18`, `Arial22`, `couri20b`) and its runtime `CGameFont`. The
reimplementation surface is `engine/formats/fnt` (`fnt_font_t` / `fnt_parse`) and the Godot
wrapper `FntResource` (`godot/src/fnt`); rasterization is Godot-side (
`TextServer`, ENG-4). Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-FNT-…` divergence catalog. Produced by a read-only
IDA session (PAR-R4, 2026-07-05); no IDB renames were made. It converts the
**Fonts** system from `UNAUDITED` to tracked (divergence-ledger.md).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| `.fnt` on-disk format (header + glyph table + pages) | **MATCHING** | `engine/formats/fnt` (`FNT_MAGIC` "FNT0", 224 glyphs × 20 B, 256×256×4 RGBA pages, 32-B header) maps field-for-field onto the parser `GameFont_LoadFromBlob @ 0x674740` |
| Font load path | **witnessed** | `HUD_LoadFontIntoSlot @ 0x580400` (renamed 2026-08-15, ex sub_580400; alloc `CGameFont` 0x1318 B → `CGameFont_Init @ 0x673a60` → `File_LoadResource @ 0x75b540` → `GameFont_LoadFromBlob` parse → free the file buffer) |
| Boot font set + slot scales | **witnessed** | `HUD_InitAllFonts @ 0x51ee20 → HUD_LoadFontIntoSlot(path, slot, scaleFP)`; the slot scale is `scaleFP × 0.000015258789` = **scaleFP / 65536** (16.16 fixed) written to slot+4 / slot+8; a null font slot leaves scale 1.0 (graceful, no crash — required-resources.md) |
| Version/design-width handling | **FIXED 2026-07-05** | D-FNT-1/2 — reader reads +4 as the design width, scales `800/dw`, no equality gate; `fnt_roundtrip` pins a non-800 font parsing |
| Reimpl glyph indexing (byte → glyph) | **MATCHING (D-FNT-4 FIXED 2026-07-19)** | retail selects the 20-byte glyph record from the unsigned text byte, skipping controls 0x7F–0x81 `[orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]`; `to_font_file` exposes each remaining record at that byte's decoded cp1252 codepoint and disables system-font fallback, so U+201C draws byte 0x93's bitmap and metric (`strings_encoding_test.gd`) |

## The witnessed format (`GameFont_LoadFromBlob @ 0x674740`)

The parser validates `fontData[0] == 0x30544E46` ("FNT0"; `-1` on mismatch), then:

| Off | Field | Engine use |
|---|---|---|
| `+0` | magic `"FNT0"` | validated == `0x30544E46` |
| `+4` | **design width** | `this+4844 = 800.0 / fontData[1]` — a per-font design-space SCALE reference, **never validated** (D-FNT-1) |
| `+8` | `pageCount` | `this+352`; the number of 256×256×4 texture pages |
| `+12` | **`glyph_spacing`** | copied to `this+356` (the loader only stores it; the text engine consumes it as the inter-glyph advance term — D-FNT-3) |
| `+32` | glyph table | **224 glyphs × 5 DWORDs (20 B)** copied verbatim to `this+364` (`srcGlyph = fontData+8` dword-index; `dstGlyph += 5` × 224). The 5 DWORDs are `[page, u0, v0, u1, v1]` (our `fnt_glyph_t`: `uint32 page` + `fnt_uv_t{u0,v0,u1,v1}`) |
| `+4512` | pixel pages | `pageCount` × **0x40000 B** each (256×256×4 RGBA; `pixelDataPtr += 0x10000` DWORDs/page, backup `memcpy 0x40000`). First-char = glyph 0 = ASCII 32 (space); glyphs 32..255 |

Each page becomes a GPU texture named `GFONT<this>:<NN>` (`GTexture_FindOrCreateFromData
@ 0x676c40`); a CPU backup copy (`operator_new(0x40000)` + memcpy) is retained iff
`this+4848` is set. Glyph pixel size = UV × 256 (our `fnt_uv_to_pixels`).

## Consumers

- `HUD_InitAllFonts @ 0x51ee20` loads the boot set into font slots with per-slot
  16.16 scales (width breakpoints 640/800/1024 select the slot — required-resources.md).
- `CGameFont_DrawText @ 0x6752c0` (the F2 `EngineTextPreview` cites it) draws glyphs
  from the pages using the glyph UV rects; the half-bright right-aligned HUD path is
  `HUD_DrawTextRightAligned_HalfBright @ 0x580850`.

## D-FNT divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FNT-1 | A | **FIXED 2026-07-05** | Offset `+4` is the design-width scale reference (`this+4844 = 800.0 / it`), NOT a version — the engine never validates it. Our reader treated it as a version and rejected `!= 800`. **Fixed:** `fnt_parse_header` drops the equality gate; a non-800 font parses (pinned in `fnt_roundtrip_test`). `engine/formats/fnt` `fnt_font_t.version` renamed to `design_width`. |
| D-FNT-2 | A | **FIXED 2026-07-05** | The per-font design scale `800.0 / designWidth` is now retained: `fnt_parse` stores the file's `+4` word into `design_width`, `fnt_design_scale()` computes `800/dw` `[orig: @ 0x674740]`, and the from-scratch writer emits the font's own design width. The reimpl applies the scale at render (ENG-4). |
| D-FNT-3 | B | **FIXED 2026-08-23** | Offset `+12` (`hdr3`, `this+356`) is the inter-glyph **SPACING** term, not a shadow offset. The loader only stores it, which is why the first pass found no reader; its consumers are the text engine, where both the measurer and the drawer advance by `glyph_width + (glyph_spacing - 1) * scale` and the measured width strips the trailing pad `[orig: CGameFont_MeasureText @ 0x674e70 this+356; CGameFont_DrawText @ 0x6752c0]`. The drawer's shadow is unrelated — it comes from a format flag (`BYTE1(textBuffer)`) plus fixed sub-pixel offsets (`cursorX-0.5`, `y-1.5`), so the old name described a field the draw path never reads. **Fixed:** `fnt_font_t.shadow_offset` → `glyph_spacing` (`engine/formats/fnt/fnt.h`), and the Godot binding followed — `FntResource.get/set_glyph_spacing`, with the editor's spin relabelled "Spacing". `FntResource::to_font_file` applies `glyph_spacing - 1` per glyph and `strings_encoding_test.gd` pins it. Retail's shipped fonts carry both 0 (Serpen24) and -3 (Gunpl22b). |
| D-FNT-4 | A | **FIXED 2026-07-19** | Retail indexes glyphs BY BYTE: printable bytes directly select their 20-byte FNT records, while 0x7F/0x80/0x81 are skipped by both measurement and drawing `[orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]`. The reimpl now single-sources the RTXT and font-boundary mapping in `util/nova_cp1252.h`; `FntResource::to_font_file` keys each printable byte's record at the decoded cp1252 Unicode codepoint, omits the three retail controls, and disables system-font fallback. `strings_encoding_test.gd` pins byte 0x93 → U+201C with the source slot's 9 px metric, the control omissions, and no fallback. This closes the shipped-data-visible substitution (67/98 JO bins carry bytes ≥ 0x80). |

**D-FNT-2 render-scale confirmed:** the same drawer reads `this+4844` (our
`fnt_design_scale` = `800/dw`) and multiplies it into every glyph's width/height
(`v161 = this+4844 * scaleX`, `v163 = this+4844 * scaleY`) — the design scale we
now retain is exactly the engine's glyph render scale `[orig: @ 0x6752c0]`.

## Authoring policy: the glyph rect IS the advance

Consequence of the D-FNT-3 formula, and the rule our rasterizer follows. The format has no
advance table and no side-bearing fields, so everything the engine knows about horizontal
metrics comes from the glyph's rect width plus the one font-wide spacing term. Authoring a
face therefore means baking each glyph's advance into its cell, not its ink box:

- Cell width = the typeface's advance for that glyph (+1, since we emit `glyph_spacing` 0 and
  the engine subtracts one); the coverage is blitted at its left side bearing inside that cell.
- A glyph with **no ink still needs a cell** — space above all. A 0x0 rect advances by
  `0 + (0 - 1)` = -1 px and runs words together. Retail sizes its space like any other glyph
  (9x19 in `Gunpl22b`, 7x17 in `Serpen24`).
- One cell height for the whole font (ascent + descent), which is what keeps baselines aligned;
  retail's shipped faces are uniform this way.

The retired ONED font rasterizer implemented and tested this policy. That
authoring surface was removed by ADR 0037; the policy remains useful for any
future format-specific font tool. Like `fnt_pack_shelf`, it satisfies the
witnessed reader but is not witnessed engine behavior.

## Cross-references

- Reimpl: `engine/formats/fnt` (`fnt.h`/`fnt.c`), `godot/src/fnt/nova_fnt_resource`,
  and `godot/src/util/nova_cp1252.h`.
- The FNT shelf packer remains in `engine/formats/fnt`; this record covers the
  format and load contract. ONED has no font-authoring consumer.
