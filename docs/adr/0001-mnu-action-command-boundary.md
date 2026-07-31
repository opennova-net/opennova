# MNU Actions stay authored; Commands stay shell-bound by control name

The MNU `<ACTION>` vocabulary is the behavior explicitly authored in a Menu.
Retail parses sixteen types: `SCREEN`, `WINDOW`, `URL`, `FORM_POST`, the
`GLB_*` browser family, `TAB`, `POP_SCREEN`, `APPMSG`, `LAN_SEARCH`,
`LAN_JOIN`, and `MNX`
`[orig: CUIElement_ParseXMLDefinition @ 0x648ee2..0x6490e9]`. The common JO
menus use the navigation/visibility/URL subset; the remaining types belong to
the shell's form, focus, browser, LAN, or application handling.

Behavior not authored as an `<ACTION>` is a Command. The runtime shell supplies
Commands by matching a widget's control **name** (`START_GAME`, `ACCEPT`,
`EXIT`, ...). This is the Action versus Command distinction recorded in
`CONTEXT.md`; it is an authored-versus-shell-bound seam, not a
navigation-versus-gameplay taxonomy.

## Consequences

- Actions remain typed, ordered data in `libs/mnu`; shell-owned Action types are
  preserved and dispatched through a shell seam rather than reclassified as
  Commands.
- A Window with neither an Action nor a recognized control name is inert.
- A future reader who finds the shell scanning for control-name strings should
  read this as the intended Command seam, not a shortcut.
- Supporting another title can require both its control-name Command bindings
  and shell adapters for authored browser/form Actions. Neither justifies
  inventing new MNU verbs.
