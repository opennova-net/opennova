# `assets/`: OpenNova's own game data

OpenNova's own game is not ready yet. Until it is, this directory holds what
the game needs to boot to a placeholder main menu (a note that the game is
coming soon, and a **PLAY RETAIL** button that plays Joint Operations from the
player's own install, ADR 0048) and the first pieces of the game's own data.

Every file is OpenNova's own: hand-written, minted by our tools, or exported
by the Blender add-on (`tools/blender/opennova_3di/`) from a scene under
[`art/`](../art). No retail byte is ever committed here
([docs/asset-gated-tests.md](../docs/asset-gated-tests.md) has the policy).
Models, clips and textures ride Git LFS; everything else is a plain git blob.

## Files

| File | What it is |
|---|---|
| `main.mnu` | The placeholder main menu: one `STARTUP` screen with literal text, `PLAY_RETAIL`, `CHANGE_FOLDER` and `EXIT`. Hand-written. |
| `opennova.fnt` | The menu's one font (uppercase 5x7 stroke art drawn at 2x). Minted by `engine/editor/blank/blank_font_art.h` (the same art as the editor's blank font); `minimal_fnt_gen_test --write` regenerates it and the `minimal_fnt_gen` ctest keeps it byte-identical to the builder. |
| `on_ar15.3di` | A first-person AR-15-pattern carbine, 64 parts on one rig. Exported from `art/on_ar15/on_ar15.blend`, like every `on_ar15*` and `on_arms*` file below. |
| `on_arms.3di` | The first-person arms skinned to `on_ar15`'s rig (its first 55 parts). |
| `on_ar15.adm` | `on_ar15`'s animation table: the eight weapon slots and the clip each plays. |
| `on_ar15_{rst,i,f,r,e,swt,swf,swr}.bad` | The clips: reset, idle, fire, reload, empty, switch to, switch from and switch rank. |
| `on_ar15_{0,1,2,3}_c.tga`, `on_arms_{0,1}_c.tga`, `on_arms_{0,1}_n.tga` | The models' diffuse textures and the arms' normal maps. |

Nothing references the `on_ar15` files yet. Re-export them by opening the
scene with the add-on installed and running Export Model on `on_ar15` and
`on_arms` and Export Animations on `on_ar15`: the scene's output paths point
here. The menu names its font and colors literally, so it needs no string
table, stylesheet or cursor file (the OS arrow stands in). Adding a file here
means adding it to this table.

## How it runs

- The packaged zip ships this directory beside `opennova.exe`
  (`scripts/package_godot_windows.ps1`, which refuses an unpulled LFS
  pointer; the package jobs pull `assets/**`). A source run uses the repo's
  copy.
- The game mounts it as a loose root when launched without `--resource-dir`
  (`BootRootMount.bundled_assets_dir` / `mount_bundled`).
- `PLAY_RETAIL`, `CHANGE_FOLDER` and `EXIT` are wired by control name in
  `godot/game/bundled_menu_companion.gd` (retail wires its own `EXIT` by name
  too; its menus have no quit `ACTION`). `MainGame` mounts the picked install,
  saves it as `[resources] retail_dir` in `user://opennova.cfg`, and switches
  to that install's own menus; `EXIT` quits.
