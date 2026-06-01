# OpenNova

Glossary of the project's domain language. Definitions only: what a term *is*, not how it's implemented. Pick one canonical word per concept; alternatives go under _Avoid_.

## Menu UI (MNU)

The vocabulary for NovaLogic's `.mnu` menu system and OpenNova's runtime + editor support for it.

**Menu**:
A single `.mnu` document: one screen or a set of related screens authored together (e.g. main, options, loadout).
_Avoid_: dialog, page, form

**Screen**:
A top-level, full-canvas layout inside a menu. Only one screen is visible at a time; navigation moves between them.
_Avoid_: page, view (when you mean the whole canvas)

**Window**:
Any node in a screen's widget tree, container or leaf (the format element is `<WINDOW>`). A Window is either a grouping container or an interactive widget.
_Avoid_: panel, control (when you mean the tree node)

**Widget**:
A Window of a specific interactive/visual type (button, combobox, table, spinlist...). Use Widget for the typed sense, Window for the raw tree node.
_Avoid_: control, element

**Action**:
A behavior the `.mnu` file itself can express, via an `<ACTION>` element: navigate to a screen or menu, show/hide a named window, pop, or open a URL. This is the *entire* behavior vocabulary the format carries.
_Avoid_: command, event, handler

**Command**:
Game-semantic behavior a button performs that the format *cannot* express (start a mission, apply video settings, connect to a server). The host/engine supplies a Command by matching a widget's **name**; it is never written in the `.mnu`.
_Avoid_: action (reserve that strictly for the `<ACTION>` element)

**Menu Host**:
The runtime front-end that loads a menu set, drives a live interactive menu, plays its audio, and supplies Commands by control name. The menu counterpart to the world runtime.
_Avoid_: menu manager, controller

**Menus workspace**:
The OpenNova Editor (ONED) surface for authoring `.mnu` files (WYSIWYG canvas + tree + inspector).
_Avoid_: menu editor (ambiguous with the runtime menu)

**Edit mode**:
The flag that makes a live menu inert and click-through so the editor can reuse the exact runtime node as a WYSIWYG preview. Off = fully interactive runtime.
_Avoid_: preview mode, design mode
