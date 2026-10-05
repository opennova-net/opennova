# `assets/`: OpenNova's own game data

OpenNova's own game is not ready yet. This directory is its base game: an
OpenNova Editor project (`project.opennova`, a standalone Joint Operations
project, ADR 0046) holding every file the game reads to boot, today to a
placeholder main menu (a note that the game is coming soon, and a **PLAY
RETAIL** button that plays Joint Operations from the player's own install,
ADR 0048), and the first pieces of the game's own data.

Every file is OpenNova's own: hand-written, made by the editor's blank
factories (`engine/editor/blank/`, written through our own writers), minted by
our tools, or exported by the Blender add-on (`tools/blender/opennova_3di/`)
from a scene under [`art/`](../art). No retail byte is ever committed here
([docs/asset-gated-tests.md](../docs/asset-gated-tests.md) has the policy).
Models, clips and textures ride Git LFS; everything else is a plain git blob.

The files sit flat in this folder, as a game folder holds its loose files: the
game's loose mount and the original game's loose search find a file by its bare
name in the root (`Vfs`'s loose lookup; retail's CWD-relative `_lopen`), while
the editor finds it anywhere in the project. Keep new files at the top level.

## Files

| File | What it is |
|---|---|
| `project.opennova` | The editor project: open this folder in the OpenNova Editor to edit, build or play the game. Its `.opennova/` cache (builds, runs, imports) ignores itself. |
| `main.mnu` | The placeholder main menu: one `STARTUP` screen with literal text, `PLAY_RETAIL`, `CHANGE_FOLDER` and `EXIT`, its `MAIN` window naming the mouse pointer. Hand-written. |
| `newarow1.tga` | The mouse pointer every screen names (a white arrow outlined in black, 32 by 32). The original game hides the system pointer, so a screen naming none has no pointer at all. The name is the game's own: its start-mission splash draws it too. Made by the editor's blank factory (`engine/editor/blank/blank_texture.cpp`). |
| `opennova.fnt` | The menu's one font (uppercase 5x7 stroke art drawn at 2x). Minted by `engine/editor/blank/blank_font_art.h` (the same art as the editor's blank font); `minimal_fnt_gen_test --write` regenerates it and the `minimal_fnt_gen` ctest keeps it byte-identical to the builder. |
| `Arial12b.fnt`, `Arial14n.fnt`, `Arial14b.fnt`, `Arial16n.fnt`, `Arial16b.fnt`, `Impac22b.fnt`, `Impac38b.fnt` | The seven fonts the game's main menu loads by name. The editor's blank font (the same art as `opennova.fnt`), made by Create Missing. |
| `gametext.bin`, `gameerr.bin`, `vmacros.bin`, `keyhelp.bin`, `game.bin`, `menutxt.bin` | The string tables the game reads as it starts and the main menu reads. Blank tables from Create Missing. |
| `nw_cdata.coo` | The NovaWorld screens' string table the main menu reads. Blank, from Create Missing. |
| `items.def`, `weapon.def`, `charattr.def` | The item, weapon and character-attribute definitions the game reads as it starts, from Create Missing. `items.def` holds the persons (`Player #1, Single player` 105310 and `Player #1, Multiplayer` 105305, both `onsold1` playing `onp1.adm`; `Rebel rifleman` 101800, `onenmy1` playing `one1.adm`), edited in the editor; `charattr.def` (hand-written in the game's form) spawns class 1 as 5310 and classes 5 to 9 as 5305. |
| `Avatars.def` | The player's character: one combo of the head `onsoldh.3di`, the body `onsoldb.3di` and the first-person arms `on_arms.3di`. Hand-written in the game's form. |
| `menu_style.mns`, `brand.mns` | The menu stylesheets: the `%NAME%` fonts and colours the screens use. From Create Missing. |
| `on_ar15.3di` | A first-person AR-15-pattern carbine, 64 parts on one rig. Exported from `art/on_ar15/on_ar15.blend`, like every `on_ar15*` and `on_arms*` file below. |
| `on_arms.3di` | The first-person arms skinned to `on_ar15`'s rig (its first 55 parts). |
| `on_ar15.adm` | `on_ar15`'s animation table: the eight weapon slots and the clip each plays. |
| `on_ar15_{rst,i,f,r,e,swt,swf,swr}.bad` | The clips: reset, idle, fire, reload, empty, switch to, switch from and switch rank. |
| `on_ar15_{0,1,2,3}_c.tga`, `on_arms_{0,1}_c.tga`, `on_arms_{0,1}_n.tga` | The models' diffuse textures and the arms' normal maps. |
| `onsold1.3di` | The player's soldier: fatigues, vest, helmet, three LODs, skinned to the original's 19 person bones in the original's bind with the mesh on a part of its own (the layout of the original's `US01`). Exported from `art/onjo1/person/onsold1.blend`, like every `onsold*`, `onenmy1*`, `onp1*` and `one1*` file below. |
| `onsoldh.3di`, `onsoldb.3di` | `onsold1` cut at the collar into the head and the body the Avatars combo draws together, on `onsold1`'s textures. |
| `onenmy1.3di` | The rebel rifleman: tiger-stripe fatigues, chest rig, boonie hat and the rifle in his right hand (`MFlash01` at its muzzle), on the same rig. |
| `onsold1_0.tga`, `onsold1_0n.mdt`, `onenmy1_0.tga`, `onenmy1_0n.mdt` | Their diffuse textures (the specular mask in the alpha) and normal maps. |
| `onp1.adm`, `onp1_*.bad` | The player's animation table (109 rows) and its clips: idles, the run in eight directions, the sprint, crouched runs, the kneel, prone and crawls, rolls, the dive, jump, swim, reload and three deaths. A slot it leaves out plays the reset. |
| `one1.adm`, `one1_*.bad` | The rebel's table: the player's rows with the AI's own (a walk on `anim_walk_forward`, `anim_run_forward`, `anim_attack` firing on `FIRE_SECONDARY`, `anim_guard`, `anim_cover_idle`). |
| `E_STAND.adm` | The engine's default infantry table (`kDefaultInfantryAdm`, for a person whose item names none), the same rows as `one1.adm`. |

Nothing references the `on_ar15` files yet but `on_arms.3di`, the arms of the
`Avatars.def` combo. Re-export them by opening the
scene with the add-on installed and running Export Model on `on_ar15` and
`on_arms` and Export Animations on `on_ar15`: the scene's output paths point
here. The person re-exports the same way from `art/onjo1/person/onsold1.blend`:
Export Model on `onsold1` and `onenmy1` first (they write the textures), then on
`onsoldh` and `onsoldb`, and Export Animations on `onsold1` and `onenmy1`;
`E_STAND.adm` is a copy of `one1.adm`. The menu names its font and colors literally, and its pointer by name.
Adding a file here means adding it to this table.

## How it runs

- The packaged zip ships this directory beside `opennova.exe`
  (`scripts/package_godot_windows.ps1`, which refuses an unpulled LFS
  pointer; the package jobs pull `assets/**`). A source run uses the repo's
  copy.
- The game mounts it as a loose root when launched without `--resource-dir`
  (`BootRootMount.bundled_assets_dir` / `mount_bundled`).
- The editor's Build packs it into `language.pff`, `localres.pff` and
  `resource.pff`, the archives the original game opens. Played on that build,
  OpenNova (`--resource-dir`) boots to the same menu, and so does the original
  `Jointops.exe` (1.7.5.7) dropped beside the archives with nothing else from
  an install and no `/d`: it writes its own `game.cfg` and saves on its first
  run. That drop-in is the standing proof that our data and the original game
  still agree.
- The web build (ADR 0049) serves it beside the page with an
  `assets/manifest.json` (`scripts/package_godot_web.sh`), and the page copies
  every listed file into the engine's in-memory filesystem before the game
  starts. Every visitor downloads all of it first, so the unreferenced
  `on_ar15`/`on_arms` art stays out of the site until the game uses it (drop
  it from the script's excludes and `game-web.yml`'s LFS pull then); the
  project file stays out too.
- `PLAY_RETAIL`, `CHANGE_FOLDER` and `EXIT` are wired by control name in
  `godot/game/bundled_menu_companion.gd` (retail wires its own `EXIT` by name
  too; its menus have no quit `ACTION`). `MainGame` mounts the picked install,
  saves it as `[resources] retail_dir` in `user://opennova.cfg`, and switches
  to that install's own menus; `EXIT` quits. In the original game the first
  two do nothing.
  On the web build the page's `WebRetailPicker` stages the pick, which is never
  saved, and `EXIT` does nothing (ADR 0049).
