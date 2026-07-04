# Fonts workspace

Edit NovaLogic bitmap fonts (`.fnt`): paint and place individual glyphs, set the
drop-shadow offset, and generate glyphs from a TTF/OTF or system font. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

A font is one or more page images (glyph sheets) plus a table mapping characters
to rectangles on those pages. The Fonts workspace shows a glyph grid on the left
and a zoomable page canvas on the right with the selected glyph's rectangle
highlighted. Pencil, eraser, fill, and move tools (with an adjustable brush size)
edit the glyph pixels directly; a per-glyph editor sets each glyph's page,
position, and size; and a shared control sets the drop-shadow offset. A live
sample-text field previews the font as you edit. You can also generate a whole
font from a TTF/OTF or installed system font at a chosen pixel size with bold,
italic, outline, and shadow options. It is a single-pane editor with no 3D view.
New / Open / Save / Save As are in the action bar.

The Credits workspace can jump straight here for a referenced font through the
`open_font_name` entry point.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.fnt` | [`libs/fnt`](../../../libs/fnt) | bitmap font (FNT0, version 800): glyphs 32-255, 256x256 RGBA pages, per-glyph metrics, shadow offset |

## How it is built

A single-pane workspace: the adapter
[`fonts_workspace.gd`](fonts_workspace.gd) overrides
`build_inspector(host)` and has no workflow rail.

| File | Role |
|---|---|
| [`fonts_workspace.gd`](fonts_workspace.gd) | adapter: single-pane inspector, file actions, `open_font_name` |
| `fnt_editor.gd` | the editing UI: glyph grid, page view, pixel canvas, per-glyph metrics, shadow offset |
| `fnt_editor_document.gd` | document model: load / save `.fnt`, dirty state |
| `fnt_rasterizer.gd` | rasterize glyphs from a TTF/OTF or system font |
| `fnt_generate_dialog.gd` | the generate-from-font dialog (size, bold / italic / outline / shadow) |

## Related

- Editor framework: [`../README.md`](../README.md); contract in
  [`../framework/editor_workspace.gd`](../framework/editor_workspace.gd).
- Project overview: [top-level README](../../../README.md).
