# The full-screen images' sources

`screens.blend` is the source of truth for the full-screen images of OpenNova's own game: the main
menu's background, the base mission's loading image, the boot splash, and the single-player win and
lose backdrops. Each is a scene of its own with its camera, rendered in Cycles and saved as the image's
master; the editor's image import makes the file the game reads from the master. The text an image
carries is Blender text objects in its scene, parented to its camera, set in Barlow (the menus' face),
whose four fonts are packed into the file. The original game's `CP19.pcx`, `Loading.pcx`, `loadscrn.pcx`,
`JO_epil.tga`, `JO_epil2.tga` and `main_hdr.tga` were looked at for size, composition and how much of the
frame the text takes, in a scratch folder outside the repo; nothing of theirs is here.

## What is in the file

- **Linked models, never copied.** The scene `library` holds one collection per model, `P_<model>`, whose
  objects draw LOD 0 meshes linked from the model's own `.blend`: `onpalm1`, `onpalm2`, `onfern1`,
  `onbush1`, `ongrass1` and `onundr1` (both at half height, as the game draws its terrain foliage),
  `onhut1`, `ontower1`, `onsbag1`, `onradio1`, `oncrate1`, `onbarl1` and `onar15_3`, each from
  `art/onjo1/models/<model>/<model>.blend`. The scenes place them as collection instances. The
  soldier's rig and LOD 0 body (`onsold1 Rig`, `onsold1 Body`) and his clip `on_idle` are linked from
  `art/onjo1/person/onsold1.blend`; a scene's soldier is a local copy of the rig, posed from `on_idle`
  frame 0 (the pose kept on the copy), its body keeping the source's bind, the third-person carbine
  placed in his right hand along the line to his left hand.
- **Ground.** `onjo_m1` stands on the island itself: `assets/art/terrain/onisle1_heightmap.png` as a
  mesh, a vertex every 2 texels at 1 unit a texel with the world's origin at the map's centre (the
  terrain importer's island layout), textured with the set's colour map and detail map (linked as files,
  not copied), the sea a plane at the set's water height (12). Its point attributes, taken from the
  set's surface map (the track and the camp's paths), its foliage map (codes 253 and 254) and the
  clearing at the track's north end, say where palms, undergrowth, grass, ferns and bushes grow; the
  track and the paths keep the colour map's own paint, darkened as wet ground. The camp's hut, tower,
  sandbags, crates, barrels and radio stand where Indigo Shore's building list (`assets/onjo_m1.bms`)
  puts them, at its heights and yaws (a model turned by minus the record's yaw), so the paths and mud
  painted into the colour map meet them. The other scenes stand on
  noise-displaced grids of their own (a wet jungle floor, a beach). The geometry nodes group
  `screens_scatter` places the foliage models over a ground by a named attribute.
- **Weather and grade.** `screens_rain` (thin streaks at random in a box before the camera, none
  within a few metres of it, leaning with the wind), a box of principled volume for the mist, a
  gradient world sky with procedural clouds (and the dawn sun's disc), and in three scenes `grade`, a
  transparent plane before the lens that darkens toward an edge or the corners (a graduated filter).
- **Render settings.** Cycles on the GPU, 512 samples (adaptive, threshold 0.01), OpenImageDenoise, AgX
  with the look `AgX - Medium High Contrast` and each scene's exposure, no metadata stamped into the
  PNG. A text's emission is set for exposure 0 and divided by its scene's exposure, so the title colours
  come out the same in every scene: white titles about (235, 235, 235), amber about (235, 160, 90). In
  `onmnbg` the soldier's fill and rim lights reach him alone (light linking).

## The scenes

| Scene | Master (size) | Game file | What it is | Text |
|---|---|---|---|---|
| `onmnbg` | `art/onjo1/menu/src/onmnbg.png` (1600 x 1200) | `assets/onmnbg.tga`, 800 x 600 (`../menu/SOURCES.md`) | The main menu's background: the camp at dusk in the rain, the soldier in profile with his carbine before the guard tower, the hut's lit door and lamplight, palms over it all; the left third dark and calm for the menu's buttons. | A small `OPENNOVA` mark top right (Barlow Condensed Black, 20-pixel capitals, grey); the menu draws its own title. |
| `onjo_m1` | `assets/art/onjo_m1.png` (800 x 600) | `onjo_m1.pcx` (`format pcx`, `palette median_cut`, as `expansions/onjo1/art/onjo1_m1.png.import`) | The loading image of "Indigo Shore" (`onjo_m1.bms`): the island on a rainy day, looking north from a rise beside the track down its last stretch into the camp: the clearing's trampled floor, the hut, the sandbag walls and the stacked crates, palms both sides. The lower centre is the muddy track, calm for the game's continue line and bar. | Laid out as `onjo1_m1.png`: `INDIGO SHORE` (Barlow Condensed SemiBold, 44-pixel capitals, white) from (95, 135), `OPENNOVA` (Barlow Semi Condensed SemiBold, 16, amber, letter-spaced) below it, and `A REBEL CAMP ON THE ISLAND. CLEAR IT AND GET OUT.` (Barlow Semi Condensed Medium, 11, light grey) at y 485. |
| `loading` | `assets/art/loading.png` (800 x 600) | `loading.pcx` (`format pcx`, `palette median_cut`) | The boot splash the game shows while it starts: palms against a dark night sky over a misty jungle. | Our mark, centred: `OPEN` in white and `NOVA` in amber (Barlow Condensed Black, 64-pixel capitals), a thin amber rule beneath with a four-point star at its middle (meshes in the scene). |
| `jo_Epil` | `assets/art/jo_Epil.png` (800 x 600) | `jo_Epil.tga` (`format tga24`) | The single-player win screen's backdrop: dawn on the north beach, the sun just over a calm sea and its path on the water, a leaning palm, the soldier at the water's edge looking out. The upper middle is plain sky for the score lines the game draws. | None. |
| `jo_Epil2` | `assets/art/jo_Epil2.png` (800 x 600) | `jo_Epil2.tga` (`format tga24`) | The lose screen's backdrop: the dark, wet jungle floor at night in the rain, the carbine dropped on its side in a puddle with a cold light's reflection beside it, the jungle out of focus behind (depth of field on the carbine). | None. |

## Re-rendering

Open `screens.blend` in Blender 5.2 with the linked models beside it as checked in (LFS pulled), pick
the scene, render it (F12) and save the image as PNG (RGB, 8 bit) over the master. The base game's
image imports make `onjo_m1.pcx`, `loading.pcx`, `jo_Epil.tga` and `jo_Epil2.tga` from the masters in
`assets/art/` on the editor's next import pass; `assets/onmnbg.tga` is made again as
`../menu/SOURCES.md` says. No generator script is kept: the file is edited in Blender.

## Third-party material

| Material | Author | Licence | Used for |
|---|---|---|---|
| [Barlow Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowcondensed) SemiBold and Black, [Barlow Semi Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowsemicondensed) Medium and SemiBold, v1.408 (google/fonts at `e9678a5385322c63730122ba464765baad83cfd8`) | The Barlow Project Authors (Jeremy Tribby) | SIL Open Font License 1.1 (`../menu/fonts/OFL.txt`) | Packed into `screens.blend` for its text objects |

The linked models' own third-party material is in their `SOURCES.md` files. Everything else (the
scenes, the procedural grounds, sky, water, mist and rain, the mark) is our own.
