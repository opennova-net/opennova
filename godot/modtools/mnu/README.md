# Menus workspace

Author NovaLogic `.mnu` menu screens: a WYSIWYG canvas, a widget tree, and a
property inspector over the exact runtime menu node, so what you see is what the
game renders. Part of the [OpenNova Editor (ONED)](../README.md).

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

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.mnu` | [`libs/mnu`](../../../libs/mnu) | menu screens: window tree, widgets, Actions; parsed through the forgiving XML reader [`libs/mnu_xml`](../../../libs/mnu_xml); round-trip preserves the format superset |
| `.mns` | [`libs/mns`](../../../libs/mns) | menu stylesheets: named style variables the screens reference |

## How it is built

A single-pane workspace: the main viewport hosts the editor (tree + canvas), the
right dock hosts the property inspector.

| File | Role |
|---|---|
| [`mnu_workspace.gd`](mnu_workspace.gd) | adapter: document, selection relay, sound audition wiring |
| `mnu_editor.gd` | center surface: widget tree + canvas + Edit / Interactive toggle |
| `mnu_canvas.gd` | WYSIWYG canvas over the runtime menu node; gestures and the interactive preview |
| `mnu_widget_tree.gd` | the screen / window tree view |
| `mnu_property_inspector.gd` | per-widget property forms, Action editing, sound triggers |
| `mnu_editor_document.gd` | document model: load / save / dirty state |
| `mnu_list_editor.gd`, `mnu_string_picker.gd`, `mnu_ui_helpers.gd` | static list items, RTXT string picking, shared UI helpers |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
- Format and engine-behaviour record: [`docs/mnu/menu-re.md`](../../../docs/mnu/menu-re.md); host wiring: [`docs/mnu/menu-wiring.md`](../../../docs/mnu/menu-wiring.md).
- Decisions: [ADR 0001](../../../docs/adr/0001-mnu-action-command-boundary.md), [ADR 0002](../../../docs/adr/0002-mnu-round-trip-preserves-superset.md), [ADR 0003](../../../docs/adr/0003-no-raw-passthrough-create-from-scratch.md), [ADR 0005](../../../docs/adr/0005-mnu-var-expansion-policy.md).
