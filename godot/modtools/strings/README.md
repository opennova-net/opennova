# Strings workspace

Edit the game's localized text tables (RTXT string tables): browse by section,
search and filter, edit entries in place, and import or export CSV for
translation. Part of the [OpenNova Editor (ONED)](../README.md).

## What you do here

A string table is a set of text entries organized into sections. Each entry has a
key, its text (with an optional `{hot}` accelerator marker), a screen position
hint, and a section. The Strings workspace shows a searchable, sortable table with
a section filter, and an always-visible detail editor that sits in a split next to
the table rather than in a separate dock, so you can edit the selected entry
directly. Entries are validated (duplicate or empty keys, bad section references).
CSV import and export round-trip the whole table for translators. New / Open /
Save / Save As are in the action bar; CSV import and export are in the left
inspector. This is a pure data editor: no 3D view and no asset dock.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| RTXT (`.bin`) | [`libs/rtxt`](../../../libs/rtxt) | localized string table: a text blob, a per-entry table (text offset, X/Y position, section), and trailing section and key metadata. Binary, little-endian |

## How it is built

A single-pane workspace whose adapter
[`strings_workspace.gd`](strings_workspace.gd) keeps the detail panel always
visible and opts out of the right asset dock.

| File | Role |
|---|---|
| [`strings_workspace.gd`](strings_workspace.gd) | adapter: table plus always-on detail panel |
| `strings_editor.gd` | document model: load / save RTXT, sections, entries, validation, CSV, undo / redo |
| `ui/strings_editor_view.gd` | center surface: table on the left, detail editor on the right |
| `ui/strings_table_view.gd` | searchable, sortable entry table |
| `ui/strings_detail_dock.gd` | per-entry key / text / section / position editor |
| `ui/strings_inspector.gd` | left inspector: search, section filter, validation, CSV actions |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
