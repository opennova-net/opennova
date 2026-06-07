# MNU expresses only navigation; game behavior is wired by control name

The MNU `<ACTION>` vocabulary covers navigation and visibility only: go to a screen or another `.mnu`, show/hide a named window, pop the screen, open a URL. It has no verb for game-semantic behavior — start a mission, apply video settings, connect to a server — and neither do the shipped JO menus.

We deliberately keep `libs/mnu` free of any such command verb, staying faithful to the real format, and instead wire game behavior in the runtime host (`NovaMenuHost` / `menu_shell.gd`) by matching a widget's control **name** (`START_GAME`, `ACCEPT`, `EXIT`, ...). This is the Action vs Command distinction recorded in CONTEXT.md.

## Consequences

- A functional button is inert until the host recognizes its name; behavior is never data-driven from the `.mnu`.
- A future reader who finds the host scanning for magic control-name strings should read this: it is the intended boundary, not a shortcut.
- Supporting a different game's menu set means supplying that game's control-name sets, not extending `libs/mnu`.
