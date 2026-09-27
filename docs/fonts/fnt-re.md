# FNT bitmap fonts — reverse-engineering record

Structure-mapping record for the original engine's **`.fnt`** bitmap-font format
(the menu/HUD fonts `Arial12b`/`14n`/`14b`/`16n`/`16b`, `Impac22b`, `Impac38b`,
`Arials18`, `Arial22`, `couri20b`) and its runtime `CGameFont`. The
reimplementation surface is `engine/formats/fnt` (`fnt_font_t` / `fnt_parse`) and the Godot
wrapper `FntResource` (`godot/src/fnt`). Menu fonts use Godot `TextServer`
(ENG-4); the gameplay HUD uses `engine/runtime/hud` layout and
`godot/src/hud/hud_overlay.cpp` device submission. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-FNT-…` divergence catalog. Initially produced by a read-only
IDA session (PAR-R4, 2026-07-05); the 2026-09-21 submission follow-up appended
an IDB comment, with no renames. It converts the
**Fonts** system from `UNAUDITED` to tracked (divergence-ledger.md).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| `.fnt` on-disk format (header + glyph table + pages) | **MATCHING** | `engine/formats/fnt` (`FNT_MAGIC` "FNT0", 224 glyphs × 20 B, 256×256×4 RGBA pages, 32-B header) maps field-for-field onto the parser `GameFont_LoadFromBlob @ 0x674740` |
| Font load path | **witnessed** | `HUD_LoadFontIntoSlot @ 0x580400` (renamed 2026-08-15, ex sub_580400; alloc `CGameFont` 0x1318 B → `CGameFont_Init @ 0x673a60` → `File_LoadResource @ 0x75b540` → `GameFont_LoadFromBlob` parse → free the file buffer) |
| Boot font set + slot scales | **witnessed** | `HUD_InitAllFonts @ 0x51ee20 → HUD_LoadFontIntoSlot(path, slot, scaleFP)`; the slot scale is `scaleFP × 0.000015258789` = **scaleFP / 65536** (16.16 fixed) written to slot+4 / slot+8; a null font slot leaves scale 1.0 (graceful, no crash — required-resources.md) |
| Version/design-width handling | **FIXED 2026-07-05** | D-FNT-1/2 — reader reads +4 as the design width, scales `800/dw`, no equality gate; `fnt_roundtrip` pins a non-800 font parsing |
| Reimpl glyph indexing (byte → glyph) | **MATCHING (D-FNT-4 FIXED 2026-07-19)** | retail selects the 20-byte glyph record from the unsigned text byte, skipping controls 0x7F–0x81 `[orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]`; `to_font_file` exposes each remaining record at that byte's decoded cp1252 codepoint and disables system-font fallback, so U+201C draws byte 0x93's bitmap and metric (`strings_encoding_test.gd`) |
| Gameplay HUD device batching | **host code / not grillable** | Retail page/vertex-buffer witness below; `hud_frame_compiler`, `hud_overlay_test.gd`, and pixel-identical baseline/fixed HUD capture. |

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
- `CGameFont_GetCharExtent @ 0x674dc0` (witnessed 2026-09-12): the single-byte extent
  the chat word-wrap walks with (`HUD_WordWrapText @ 0x580980`, `charSize[0]`
  `@ 0x5809db`, plus 1 per byte `@ 0x5809e4`): `out[0] = floor(((u1 - u0) * 256
  [flt_7D1D70] + glyph_spacing [this+356] - 1 [flt_7C3280]) * (800 / design_width)
  [this+4844] + 0.5 [flt_7C3B94])` `@ 0x674de4..0x674e25`, the `(glyph_spacing - 1)`
  pad INCLUDED (the measurer strips one trailing pad from its final width, this does
  not); `out[1] = (space.v1 - space.v0) * (800 / design_width) * 256`
  `@ 0x674e2c..0x674e49`. No scale argument. A tab (`ch == 9`) returns the font's
  **tab width `this+0x168`** when nonzero, else measures the SPACE glyph `@ 0x674dd8`;
  any other byte below 0x20 measures 0 and returns -1 `@ 0x674e55`. `this+0x168` is
  a runtime field (no `.fnt` header word feeds it); our `GameFontState.tab_width`
  models the same stop for the measurer/drawer, and `GameFont::char_width` takes it
  as an argument. Port: `GameFont::char_width` (`tests/hud/hud_frame_compiler_test.cpp`
  `test_char_extent` / `test_chat_wrap_slots`).

## D-FNT divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FNT-1 | A | **FIXED 2026-07-05** | Offset `+4` is the design-width scale reference (`this+4844 = 800.0 / it`), NOT a version — the engine never validates it. Our reader treated it as a version and rejected `!= 800`. **Fixed:** `fnt_parse_header` drops the equality gate; a non-800 font parses (pinned in `fnt_roundtrip_test`). `engine/formats/fnt` `fnt_font_t.version` renamed to `design_width`. |
| D-FNT-2 | A | **FIXED 2026-07-05** | The per-font design scale `800.0 / designWidth` is now retained: `fnt_parse` stores the file's `+4` word into `design_width`, `fnt_design_scale()` computes `800/dw` `[orig: @ 0x674740]`, and the from-scratch writer emits the font's own design width. The reimpl applies the scale at render (ENG-4). |
| D-FNT-3 | B | **FIXED 2026-08-23** | Offset `+12` (`hdr3`, `this+356`) is the inter-glyph **SPACING** term, not a shadow offset. The loader only stores it, which is why the first pass found no reader; its consumers are the text engine, where both the measurer and the drawer advance by `glyph_width + (glyph_spacing - 1) * scale` and the measured width strips the trailing pad `[orig: CGameFont_MeasureText @ 0x674e70 this+356; CGameFont_DrawText @ 0x6752c0]`. The drawer's shadow is unrelated — it comes from a format flag (`BYTE1(textBuffer)`) plus fixed sub-pixel offsets (`cursorX-0.5`, `y-1.5`), so the old name described a field the draw path never reads. **Fixed:** `fnt_font_t.shadow_offset` → `glyph_spacing` (`engine/formats/fnt/fnt.h`), and the Godot binding followed — `FntResource.get/set_glyph_spacing`, with the editor's spin relabelled "Spacing". `FntResource::to_font_file` applies `glyph_spacing - 1` per glyph and `strings_encoding_test.gd` pins it. Retail's shipped fonts carry both 0 (Serpen24) and -3 (Gunpl22b). |
| D-FNT-4 | A | **FIXED 2026-07-19** | Retail indexes glyphs BY BYTE: printable bytes directly select their 20-byte FNT records, while 0x7F/0x80/0x81 are skipped by both measurement and drawing `[orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]`. The reimpl now single-sources the RTXT and font-boundary mapping in `util/cp1252.h`; `FntResource::to_font_file` keys each printable byte's record at the decoded cp1252 Unicode codepoint, omits the three retail controls, and disables system-font fallback. `strings_encoding_test.gd` pins byte 0x93 → U+201C with the source slot's 9 px metric, the control omissions, and no fallback. This closes the shipped-data-visible substitution (67/98 JO bins carry bytes ≥ 0x80). |

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

## Retail font submission batches (2026-09-21)

The live retail `Jointops.exe.kong.i64` witness is **anchored** by the FNT
loader, its page shaders, and the drawer's calls to the dynamic vertex buffer:

| Surface | Witness and behavior |
|---|---|
| Glyph buffer | `[orig: CGameFont_DrawText @ 0x6752C0]` appends six triangle-list vertices per glyph for the current texture page. It flushes at 384 vertices (`@ 0x675A6E..0x675AC7`), submits the remainder (`@ 0x675CFA..0x675D39`), then advances pages (`@ 0x675D8A..0x675DAA`). |
| Underline buffer | The same drawer appends two line vertices per underline, flushes at 128 (`@ 0x675BB3..0x675C17`), and submits remaining lines after remaining glyphs (`@ 0x675D43..0x675D85`). Capacity flushes can interleave these kinds. |
| Device submission | `[orig: GDynamicVB_DrawPrimitive @ 0x6788E0]` copies 40-byte vertices into a dynamic buffer and submits a primitive list (`@ 0x67892C..0x6789B9`), with a `DrawPrimitiveUP` fallback. |
| Font blend | `[orig: GameFont_LoadFromBlob @ 0x674740]` selects page shader mode `0x651` (`@ 0x674830..0x67483B`); `[orig: RenderState_DecodeBlendModeToD3DStates @ 0x680F00]` selects source-alpha / inverse-source-alpha (`@ 0x680F2C..0x680F3A`). Material modulation is documented in [render-material-re.md](../render/render-material-re.md). |

`~/Development/jo-c` at `f2f7c22dbec6c3d1dab31ad1c6a516a6edb45a3c`
corroborates these instructions: `Jointops.exe.kong.c:645826` (drawer),
`:646301` (batch flushes), and `app/reconstruction_ui_native.inc:202831`
(relocated drawer), `:203887` (capacity flush), `:204287` (remainder/page walk).
The source export was checked against live disassembly; it is corroboration,
not a second independent retail execution test.

The Godot device path now emits one indexed triangle array per **consecutive
font-page run** and per consecutive HUD triangle texture run. Glyph geometry,
UVs, italic corners, per-vertex color modulation, and the compiler's existing
quads / triangles / lines / glyphs / underlines order are retained. It does not
sort across page runs. Four indexed corners replace six duplicated glyph
vertices; matching retail's historical buffer capacity has no device benefit.
The retail crosshair's own strips are witnessed at
`[orig: HUD_DrawCrosshairCornerQuad @ 0x590F50]` (submission
`@ 0x591509..0x59152B`); this change batches the compiler's existing triangles.

This is **host code / not grillable** as an exact device implementation.
Retail page iteration and capacity-triggered underline interleaving differ
from the existing `HudDrawList` kind-separated representation; this change
makes no claim of full retail cross-kind ordering parity. The engine compiler
and existing material/blend state are unchanged.

Validation: `hud_frame_compiler`, `hud_overlay_test.gd`, and a rendered
1024 x 768 comparison with the actual `revx02` fonts, colored messages,
weapon/waypoint text and reticle: **122 glyphs, 14 triangles, 4 quads, zero
differing pixels** between baseline and fixed DLLs. The fixture-dependent
health-bar assertion and timing results are recorded in
[03TR frame costs](../perf/03tr-frame-costs.md).

IDB changes: appended an anchored entry comment at `0x6752C0` describing both
buffer capacities, page iteration and the reimplementation boundary. No
renames or type changes; saved `Jointops.exe.kong.i64`.

## Cross-references

- Reimpl: `engine/formats/fnt` (`fnt.h`/`fnt.cpp`), `godot/src/fnt/fnt_resource`,
  and `godot/src/util/cp1252.h`.
- The FNT shelf packer remains in `engine/formats/fnt`; this record covers the
  format and load contract. ONED has no font-authoring consumer.
