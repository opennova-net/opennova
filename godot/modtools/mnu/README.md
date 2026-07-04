# Menus workspace

Author NovaLogic `.mnu` menu screens and the `.mns` stylesheet they share. The
Menus workspace owns both — `.mnu` documents in multi-tab editing, plus the
single shared stylesheet (canonically `menu_style.mns`) the screens reference
as `%NAME%`. The right dock carries a **Properties** tab (per-widget inspector)
and a **Styles** tab (variables / source / per-variable inspector); the canvas
itself IS the live stylesheet preview, so unsaved Styles edits show on every
canvas refresh. Part of the [OpenNova Editor (ONED)](../README.md).

## What you do here

A Menu is one `.mnu` document; inside it, Screens are the full-canvas layouts and
Windows are the nodes of each screen's widget tree (containers or typed Widgets:
buttons, lists, tables, comboboxes, and the rest; the vocabulary lives in the
project glossary, [CONTEXT.md](../../../CONTEXT.md)). The canvas shows the live
menu in Edit mode, inert and click-through, with selection, drag, and resize
gestures; the tree and inspector stay in sync with the canvas selection.

Behavior that the format itself carries is the Action: navigate to a screen or
menu, show or hide a named window, pop, or open a URL. You author Actions in the
inspector and they serialize straight back to `<ACTION>`. The Interactive toggle
puts the preview into a play state: tabs and screen navigation respond to clicks
while external Commands (launch, quit, URL, cross-menu) stay sandboxed as no-ops,
so you can click through a menu's flow without leaving the editor.

Widget sounds audition through the shared sound preview: the inspector's trigger
dropdown reads the menu's `.lwf` profile (`menu.lwf` across shipped JO menus) and
plays the selected set. Text fields pick strings from the game's RTXT tables via
the string picker.

Colors, fonts, and pictures can reference the stylesheet as `%NAME%`: the
inspector resolves the token for its swatch, the "%" dropdown on each row offers
the type-matching variables (the token is what saves, never the baked value), and
"Edit style" flips the right dock to the **Styles** tab and selects that
variable. Menus using a token the stylesheet does not define get an "unresolved
style variable" count in the status bar (the original engine fails on those at
load).

## Styles tab

The shared menu stylesheet (`TRIM_COLOR`, `DEF_FONTNAME`, ...) shows up as
grouped variables (the shipped file's comment header collapses into a "File
header" disclosure; comment runs become group headings), each with a typed
editor: colors carry a live AARRGGBB swatch and picker, fonts a picker plus an
"Edit in Fonts" jump, pictures and plain text stay editable as written. A
"Used by" panel lists the menus referencing the selected variable (with jumps
back to the **Properties** tab), the Source view edits the raw text with
line-clickable diagnostics. Saving is byte-faithful: an untouched open + save
reproduces the file exactly, and a value edit changes only its own line.

The Menus canvas itself IS the preview — every Styles edit propagates to the
open menu's canvas immediately (no save needed), in real context. There is no
separate preview pane.

The first activation auto-opens `menu_style.mns` from the resource folder when
one exists; Save As defaults there too, because a loose stylesheet beside the
menus shadows a PFF-archived one at runtime - the standard modding flow.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.mnu` | [`libs/mnu`](../../../libs/mnu) | menu screens: window tree, widgets, Actions; parsed through the forgiving XML reader [`libs/mnu_xml`](../../../libs/mnu_xml); round-trip preserves the format superset |
| `.mns` | [`libs/mns`](../../../libs/mns) | menu stylesheets: named style variables the screens reference as `%NAME%`; lossless document model (comments, grouping, alignment survive saves - [ADR 0014](../../../docs/adr/0014-mns-lossless-document-model.md)) |

## How it is built

A single workspace: the main viewport hosts the menu editor (tree + canvas);
the right dock hosts a `Properties | Styles` tab strip. Properties is the
per-widget inspector; Styles stacks the stylesheet variables/source editor over
the per-variable inspector.

| File | Role |
|---|---|
| [`mnu_workspace.gd`](mnu_workspace.gd) | Menus adapter: per-menu document tabs, shared stylesheet ownership, right-dock tab routing, save/new/open contract per tab |
| `mnu_editor.gd` | center surface: widget tree + canvas + Edit / Interactive toggle; accepts an in-memory stylesheet so unsaved Styles edits hit the preview |
| `mnu_canvas.gd` | WYSIWYG canvas over the runtime menu node; gestures and the interactive preview |
| `mnu_widget_tree.gd` | the screen / window tree view |
| `mnu_property_inspector.gd` | per-widget property forms, Action editing, sound triggers, stylesheet-aware color/font/texture rows |
| `mnu_editor_document.gd` | menu document model: load / save / dirty state |
| `mnu_list_editor.gd`, `mnu_string_picker.gd`, `mnu_ui_helpers.gd` | static list items, RTXT string picking, shared UI helpers (incl. AARRGGBB color parsing) |
| `mns_editor.gd` | Styles tab body: Variables / Source toggle, whole-file snapshot undo |
| `mns_variable_table.gd` | grouped variable rows with typed editors and swatches |
| `mns_source_view.gd` | raw text view with Apply + line-clickable diagnostics |
| `mns_inspector.gd` | per-variable detail: rename, typed value, comment, Used-by, delete |
| `mns_editor_document.gd` | stylesheet document model: load / save / dirty state |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
- Format and engine-behaviour record: [`docs/mnu/menu-re.md`](../../../docs/mnu/menu-re.md); host wiring: [`docs/mnu/menu-wiring.md`](../../../docs/mnu/menu-wiring.md).
- Decisions: [ADR 0001](../../../docs/adr/0001-mnu-action-command-boundary.md), [ADR 0002](../../../docs/adr/0002-mnu-round-trip-preserves-superset.md), [ADR 0003](../../../docs/adr/0003-no-raw-passthrough-create-from-scratch.md), [ADR 0005](../../../docs/adr/0005-mnu-var-expansion-policy.md), [ADR 0014](../../../docs/adr/0014-mns-lossless-document-model.md).
