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
directly. The table opens in file order — the order entries sit in on disk, which
is how the game reads them; click a column to sort, and a third click returns to
file order.

The game looks text up by section: it finds the section by name, then takes the
first key match inside it. Validation mirrors that — the same key may appear in
several sections (the shipped game does this), a repeated key inside one section
warns that the later copy can never be reached, and entries that drift out of
their section's run are an error with a one-click "Group entries by section" fix.
Moving an entry to a different section relocates it to that section's end, keeping
the file shaped the way the game expects. The lookup tester in the left inspector
resolves `section:key` exactly like the game, including the visible
`??section:key??` marker for missing text.

CSV import and export round-trip the whole table for translators. Tables saved
unedited reproduce their original bytes exactly, including the original game's
cp1252 accented text. New / Open / Save / Save As are in the action bar; the
workspace reopens your last table when you return. This is a pure data editor: no
3D view and no asset dock.

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
- Format and engine-behaviour record: [`docs/interface/rtxt-strings-re.md`](../../../docs/interface/rtxt-strings-re.md).
