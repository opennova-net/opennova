# `assets/`: OpenNova's own game data

OpenNova's own game is not ready yet. Until it is, this directory holds only
what the game needs to boot to a placeholder main menu: a note that the game
is coming soon, and a **PLAY RETAIL** button that plays Joint Operations from
the player's own install (ADR 0048).

Every file is authored from scratch. No retail byte is ever committed here
([docs/asset-gated-tests.md](../docs/asset-gated-tests.md) has the policy).

## Files

| File | What it is |
|---|---|
| `main.mnu` | The placeholder main menu: one `STARTUP` screen with literal text, `PLAY_RETAIL`, `CHANGE_FOLDER` and `EXIT`. Hand-written. |
| `opennova.fnt` | The menu's one font (uppercase 5x7 stroke art drawn at 2x). Minted by `tests/fixtures/minimal_fnt_builder.h`; `minimal_fnt_gen_test --write` regenerates it and the `minimal_fnt_gen` ctest keeps it byte-identical to the builder. |

The menu names its font and colors literally, so it needs no string table,
stylesheet or cursor file (the OS arrow stands in). Adding a file here means
adding it to this table.

## How it runs

- The packaged zip ships this directory beside `opennova.exe`
  (`scripts/package_godot_windows.ps1`). A source run uses the repo's copy.
- The game mounts it as a loose root when launched without `--resource-dir`
  (`BootRootMount.bundled_assets_dir` / `mount_bundled`).
- `PLAY_RETAIL` and `CHANGE_FOLDER` are wired by control name in
  `godot/game/bundled_menu_companion.gd`. `MainGame` mounts the picked install,
  saves it as `[resources] retail_dir` in `user://opennova.cfg`, and switches
  to that install's own menus. `EXIT` quits through the document's own action.
