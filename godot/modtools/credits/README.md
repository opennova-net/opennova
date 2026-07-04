# Credits workspace

Compose the scrolling end-credits sequence (`.kda`): text and image blocks, fonts
and colors, and scroll timing. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

Credits are an ordered list of blocks. The Credits workspace has two modes:

- **Visual**: a draggable card list (text, image, and line-break blocks) with
  per-block font, color, alignment, and image settings, plus a live preview
  player you can play, pause, and scrub.
- **Source**: the raw KDA text, with parse validation and an Apply step.

A toolbar exposes the scroll settings: scroll rate, spacing between entries, and
horizontal center. Referenced fonts and images are checked, and missing ones are flagged. It
is a single-pane editor. New / Open / Save / Save As are in the action bar.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.kda` (CBIN) | [`libs/cbin`](../../../libs/cbin) | rolling-credits script: a settings section (scroll rate, spacing between entries, center) plus text, image, color, newline, and justify entries. Authored as text, compiled to an obfuscated CBIN blob |

Credits reference fonts (`.fnt`, see the [Fonts workspace](../fonts/README.md))
and image files.

## How it is built

A single-pane workspace; the adapter
[`credits_workspace.gd`](credits_workspace.gd) builds the
inspector and the Visual / Source toggle.

| File | Role |
|---|---|
| [`credits_workspace.gd`](credits_workspace.gd) | adapter: single-pane inspector, file actions |
| `credits_editor.gd` | editor controller: Visual / Source modes, scroll / spacing / center bar, preview playback |
| `credits_editor_document.gd` | document model: load / save KDA, dirty state, asset checks |
| `credits_editor_block_list.gd` | the block list: selection, drag-reorder, add / delete |
| `credits_editor_block_card.gd` | one block's card (text / image / line break, font, color, alignment) |
| `credits_editor_source_view.gd` | raw KDA text view with parse validation |
| `credits_editor.tscn` | scene layout |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
