# How MNU menus are wired

How a `.mnu` menu drives navigation, fills its lists, and hands game behavior to the
host. Terms (Menu, Screen, Window, Action, Command, Menu Host, Tab) are defined in
[CONTEXT.md](../../CONTEXT.md); the Action/Command split is [ADR 0001](../adr/0001-mnu-action-command-boundary.md).

## Buttons and navigation: the `<ACTION>` element

A button is a `<WINDOW type="button">` (or `type="radio"`) carrying one or more
`<ACTION>` children. An Action expresses **only** navigation and visibility, never
game logic:

| Action | Effect |
| --- | --- |
| `<ACTION type="screen">NAME</ACTION>` | Show screen `NAME` in this menu (pushes the back stack). |
| `<ACTION type="screen" file="sp.mnu">NAME</ACTION>` | Cross-menu jump: the host opens `sp.mnu` at screen `NAME`. |
| `<ACTION type="window" state="SHOW\|HIDE\|TOGGLE">NAME</ACTION>` | Flip the visibility of the named Window in the current screen. |
| `<ACTION type="pop_screen">` | Back (pop the screen stack; at the root the host decides). |
| `<ACTION type="quit">` / `<ACTION type="url">www…</ACTION>` | Host policy: quit / open a link. |

The target is read from the `target`/`screen`/`window` attribute, else the element
text. A button may carry several Actions; pressing it runs them in order.

**Tabs** are the `window` Action in practice: one button per panel, each one hiding
its sibling panels and showing its own. There is no "tab" widget type.

Runtime path: `NovaMnuButton::on_pressed` → `NovaMnuMenu::dispatch_action` routes
`screen`/`window`/`pop`/`quit`/`url`. Same-file `screen` navigates in place;
`file` jumps emit `menu_requested` for the Menu Host to open. (`godot/engine/mnu/`.)

**Authoring:** add a widget, then add an Action in the inspector (type + target). It
serializes straight back to `<ACTION>`. Use the Menus workspace **Interactive**
toggle to click through tabs/screens in the preview without leaving the editor.

## What populates a list box

A `<WINDOW type="list">` has two possible content sources:

1. **Static items**, authored in the file as `<ITEMS><ITEM>…</ITEM></ITEMS>`. The
   builder seeds these. Most shipped lists ship empty.
2. **Host-populated by control NAME.** The `.mnu` only declares the empty list
   (name, position, row height, scrollbar); the game fills it at runtime. The Menu
   Host finds the list by a well-known name and calls `set_items(...)`. Selection
   relays back through the menu's `widget_value_changed` signal.

So the file describes the *shape* of the list; the game decides its *contents*. The
same hook drives the mission browser and the Options → Mods expansion list.

## Host wiring: Commands by control NAME

Game behavior the format cannot express (launch a mission, mount an expansion, quit)
is a **Command**, supplied by the Menu Host (`godot/game/menu_shell.gd`) by matching
a control's NAME — never written in the `.mnu` (ADR 0001). The host holds exported
name sets and connects/sees them after each (re)build:

| Name set | What the host does with a match |
| --- | --- |
| `start_control_names` (`START_GAME`, `ACCEPT`, …) | Connect `pressed` → launch the selected mission. |
| `exit_control_names` / `return_control_names` | Connect `pressed` → quit / return to menu. |
| `mission_list_names` (`MISSION_LIST`, …) | Fill the list with the resource dir's `.bms` missions; cache the selection. |
| `mod_list_names` (`AVAIL_LIST`, …) / `mod_desc_names` (`MOD_DESC`) | Fill with discoverable expansions; on activate, mount the expansion, refresh content, persist, describe it. |

To support a new game's menu set, point the host's name sets at its control names;
no engine change is needed.
