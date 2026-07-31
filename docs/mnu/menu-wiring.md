# How MNU menus are wired

How a `.mnu` menu drives navigation, fills its lists, and hands game behavior to the
shell. Terms (Menu, Screen, Window, Action, Command, Menu Shell, Tab) are defined in
[CONTEXT.md](../../CONTEXT.md); the Action/Command split is [ADR 0001](../adr/0001-mnu-action-command-boundary.md).

## Buttons and navigation: the `<ACTION>` element

A button is a `<WINDOW type="button">` (or `type="radio"`) carrying one or more
`<ACTION>` children. An Action is authored behavior. Retail parses this complete
type vocabulary `[orig: CUIElement_ParseXMLDefinition @
0x648ee2..0x6490e9]`:

| Action | Effect |
| --- | --- |
| `<ACTION type="screen">NAME</ACTION>` | Show screen `NAME` in this menu (pushes the back stack). |
| `<ACTION type="screen" file="sp.mnu">NAME</ACTION>` | Cross-menu jump: the shell opens `sp.mnu` at screen `NAME`. |
| `<ACTION type="window" state="SHOW\|HIDE\|ENABLE\|DISABLE">NAME</ACTION>` | Change the named Window's shown or enabled state. The separate `TOGGLE` flag inverts that property. |
| `<ACTION type="url" EXTERNAL_BROWSER>www…</ACTION>` | Ask the shell to open a URL; Interactive preview consumes it without side effects. |
| `<ACTION type="form_post" source="…" field="…" target_form="…">…</ACTION>` | Submit authored form data. `TEST="LT\|LE\|EQ\|GE\|GT"` carries its comparison mode. |
| `<ACTION type="tab">NAME</ACTION>` | Select a named focus target on the retail form/tab event. This is not the visibility-based Tab convention below. |
| `<ACTION type="pop_screen">` | Back (pop the screen stack; at the root the shell decides). |
| `GLB_LOAD`, `GLB_LOADANDPING`, `GLB_FILTER`, `GLB_FILTER_NUM`, `GLB_PING`, `GLB_JOIN` | Drive the shell-owned NovaWorld browser workflow. |
| `APPMSG`, `LAN_SEARCH`, `LAN_JOIN`, `MNX` | Dispatch the corresponding application, LAN, or shell-owned compiler operation. |

Retail reads the target from the element text; the permissive authoring parser
also accepts the existing `target`/`screen`/`window` aliases. A button may carry
several Actions; pressing it runs them in order.

**Tabs** are the `window` Action in practice: one button per panel, each one hiding
its sibling panels and showing its own. There is no "tab" widget type.

Runtime path: `NovaMnuButton::on_pressed` → `NovaMnuMenu::dispatch_action` routes
menu-owned Actions directly and hands browser/form/application Actions to their
shell. Same-file `screen` navigates in place; `file` jumps emit `menu_requested`
for the Menu Shell to open. Interactive preview consumes shell-side effects.

**Authoring:** add a widget, then add an Action in the inspector (type + target). It
serializes straight back to `<ACTION>`. Use the Menus workspace **Interactive**
toggle to click through tabs/screens in the preview without leaving the editor.

## What populates a list box

A `<WINDOW type="list">` has two possible content sources:

1. **Static items**, authored in the file as `<ITEMS><ITEM>…</ITEM></ITEMS>`. The
   builder seeds these. Most shipped lists ship empty.
2. **Shell-populated by control NAME.** The `.mnu` only declares the empty list
   (name, position, row height, scrollbar); the game fills it at runtime. The Menu
   Host finds the list by a well-known name and calls `set_items(...)`. Selection
   relays back through the menu's `widget_value_changed` signal.

So the file describes the *shape* of the list; the game decides its *contents*. The
same hook drives the mission browser and the Options → Mods expansion list.

## Shell wiring: Commands by control NAME

Game behavior the format cannot express (launch a mission, mount an expansion, quit)
is a **Command**, supplied by the Menu Shell (`godot/game/nova_menu_shell.gd`) by matching
a control's NAME — never written in the `.mnu` (ADR 0001). The shell holds exported
name sets and connects/sees them after each (re)build:

| Name set | What the shell does with a match |
| --- | --- |
| `start_control_names` (`START_GAME`, `ACCEPT`, …) | Connect `pressed` → launch the selected mission. |
| `exit_control_names` / `return_control_names` | Connect `pressed` → quit / return to menu. |
| `mission_list_names` (`MISSION_LIST`, …) | Fill the list with the resource dir's `.bms` missions; cache the selection. |
| `mod_list_names` (`AVAIL_LIST`, …) / `mod_desc_names` (`MOD_DESC`) | Fill with discoverable expansions; on activate, mount the expansion, refresh content, persist, describe it. |

To support a new game's menu set, point the shell's name sets at its control names;
no engine change is needed.
