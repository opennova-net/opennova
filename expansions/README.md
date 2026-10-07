# `expansions/`: OpenNova's own expansions

Each folder here is one expansion of OpenNova's own game, an OpenNova Editor
expansion project on the base game's project in [`assets/`](../assets). This
file lives beside the projects rather than in one, so no project holds a file
the game never reads.

## `onjo1/`: Indigo Storm

`onjo1` is a small expansion of OpenNova's own game: the base game's one
mission, "Indigo Shore", at night in a storm, with a building of its own and a
loading screen of its own. Like the base game in `assets/`, it
is made from scratch (no retail byte; third-party material CC0 only, recorded
in each source's `SOURCES.md`) and runs in OpenNova and in the original
`Jointops.exe` alike.

`onjo1/` is an OpenNova Editor expansion project (`project.opennova`,
ADR 0046): it builds as the expansion `onjo1`, played with `/exp onjo1` (or
`/mod onjo1`, or picked in the Mods list), and its base game is the project in
`../../assets` (the project file's `expansion.base_project`, ADR 0046 T5). The
editor reads the base game from that project's export (`assets/build/export`,
made by its File > Export or `opennova-project export assets`): the expansion
is checked against it, built over it (a file the base serves the same is left
out of the build) and played over it, strict Play in the game install
included. Export the base game first, and again after changing it.

### Files (under `onjo1/`)

| File | What it is |
|---|---|
| `project.opennova` | The editor project, made with the editor's `new_project` as the expansion `onjo1` on the base game's project `../../assets`. |
| `strings/onjo1.bin` | The expansion's text table: `[exp_info]` `EXP_NAME` ("OpenNova: Indigo Storm") and `EXP_DESC`, which the game's Mods list shows. Built loose into `expansion/onjo1/`, the only place the game reads it. Edited in the editor's string table document. |
| `version.txt` | The expansion's version text, whose CRC a server compares with a joiner's. Made by `new_project`. |
| `missions/onjo1_m1.bms`, `missions/onjo1_m1.wac`, `missions/onjo1_m1.til`, `strings/onjo1_m1.bin` | "Indigo Storm": the base game's `onjo_m1` imported from its export and renamed in the editor (the mission's file set renamed together), then made a night storm: the environment `onstorm1`, the start at 21:00 (the header's start time), the title and briefing in `onjo1_m1.bin`, and the supply shed placed at the camp's west edge (building list). Its script starts the rain at full and a quarter of `overcast.def`'s colours as the base mission does, then asks for the storm's lightning itself, as the script command table offers it: each second (the script runs once a second) a far strike one time in 25 (`farflash`) and a near one, with its thunder close by, one time in 70 (`flash`). Objectives, events and rebels are the base mission's. |
| `terrain/onstorm1.env` | The storm's environment: the base game's `onrain1` (imported from its export and renamed) with a nearer fog (300), faster clouds (95), a darker cloud deck, murkier water, a cold blue-white lightning colour, and night keyframes (19:30 to 05:00) of a cold storm light, so the jungle reads in silhouette and is never black. |
| `defs/items.def` | The base game's item table with one item more, `Supply shed` (id 100020, `onshed1`, a landable building). The game opens `items.def` once by name, so an expansion that adds an item ships the whole table. |
| `models/onshed1.3di`, `models/onshed1_0.dds`, `models/onshed1_0d.dds`, `models/onshed1_1d.dds`, `models/onshed1_0n.mdt` | A plank supply shed on stilts with a rusty corrugated-iron roof, a front porch and steps, about 4.5 by 5 metres: three LODs (464, 372 and 66 triangles, thresholds 96, 24 and 0, LOD type `bldg`), LOD 1's triangles its bullet faces (Wood and Metal), `CB` walk volumes for the body, the porch, both steps and the roof, and a `VC` volume for vehicles; drawn with `VS_DOT3DIFF2` (a baked base, a tiling detail per material, a 512 normal map). Exported from `art/onjo1/models/onshed1/onshed1.blend`. |
| `art/onjo1_m1.png` | The mission's loading screen as an import source (800 x 600): the editor's image import makes `onjo1_m1.pcx` from it (8-bit, a median-cut palette), the image the game shows while `onjo1_m1.bms` loads (a mission's `<name>.pcx`, else `loadscrn.pcx`). Rendered from the scene `onjo1_m1 loading` in `onshed1.blend`, the base game's palms linked from their own `.blend`. |

### Build and ship

- The editor's Build, Play and Export, or `opennova-project build expansions/onjo1`
  and `opennova-project export expansions/onjo1 --out <dir>`, make
  `expansion/onjo1/` with `onjo1L.pff` (the mission's text), `onjo1.pff`
  (everything else) and the loose `onjo1.bin` and `version.txt`.
- The game zip (`scripts/package_godot_windows.ps1`) exports the base game and
  this project and ships the expansion as `assets/expansion/onjo1/` beside the
  base game's archives, where both OpenNova (`opennova.exe -- /exp onjo1`, or
  the main menu's MODS) and the original game dropped into `assets/`
  (`Jointops.exe /exp onjo1`) mount it.
