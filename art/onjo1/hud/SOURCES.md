# The HUD's sources

The in-mission HUD of OpenNova's own game: our own art, made from scratch in the look of Joint
Operations' HUD (the same layout logic and readability: a soldier panel bottom left, a heading-up map
with a compass bezel bottom right, white icons in a translucent tint, amber text), upgraded as far as
the original engine draws it. The original game's HUD textures were looked at for size, layout and
drawing mode only (in a scratch folder outside the repo); nothing of theirs is here.

## What is the source

- **`hud.blend`**: the 3D-rendered art. Six scenes `stance0_stand` .. `stance5_parachute` pose our
  soldier (a skin-modifier mannequin with helmet, pack and carbine, a bench for sitting, a tripod gun for
  emplaced, a canopy for the parachute), each framed by an orthographic camera at 1024 x 544; the scene
  `weapon_on_ar15` holds our AR-15's parts (copied from `art/on_ar15/on_ar15.blend` with their placement
  baked, the reload's spare magazine left out) under a side camera at 1024 x 320, and `weapon_on_pist`
  our pistol's (from `art/on_pist/on_pist.blend` the same way, at rest) under a side camera at 1024 x 512. The gear's
  scenes hold the first-person models' meshes (from `art/on_knif`, `art/on_frag`, `art/on_smok` and `art/on_medp`, each
  part in its bone's frame) under a side camera at 1024 x 512 framing them: `weapon_on_knif` the knife level, its point
  to the right; `weapon_on_frag` three grenades in a row (the carried count, as the original's `H_frag`) and
  `weapon_on_frag_m` two for the armory; `weapon_on_smok` a canister standing and one lying before it;
  `weapon_on_medp` the pouch turned to show its lid, drawn in two shades by object colour (the pouch 0.62, the patch's
  cross white) so the cross reads in the alpha. All render with Workbench (studio light, one white colour, the object
  outline, a transparent film).
- **`src/<group>/<name>.png`**: each texture's master, two or four times the texture's size (one 1:1
  where the texture is already large): the texels as the game reads them, so a master is the texture at
  a higher resolution. The stance and weapon masters come from the renders above (their alpha is the
  render's coverage times `clamp((luminance - 0.18) / 0.62, 0.22, 1)`; the weapon's `0.15`, `0.6`,
  `0.3`): the HUD keeps only an alpha-mode texture's alpha, so the shading becomes alpha.
- **`fonts/<name>.png` and `fonts/<name>.fntset`**: the two HUD fonts' glyph sheets (16 x 14 cells
  holding the bytes 0x20..0xFF in Windows-1252, white glyphs with a dark one-texel rim, each at its cell's
  left) and the font sets the editor's font importer makes the `.fnt` from (our own FNT writer,
  `engine/formats/fnt`), with the import options `advance ink`, `tracking 0`, `space 5` (`onhudb18`)
  or `4` (`onhud14`), `color sheet` (the rim stays dark), the rest their fallbacks: `opennova-project
  import <project> art/onjo1/hud/fonts/onhudb18.fntset`, those options set on its record, then the
  `.fnt` from the project's import cache copied into `assets/`.

**Export**: a texture is its master area-averaged down to the texture's size (premultiplied), written
as an uncompressed true-colour TGA, bottom row first, no image ID (32-bit, or 24-bit where the table
says so). The editor's image importer does exactly that: import the master with `format tga` (or
`tga24`) and `size <W>x<H>`. No generator script is kept.

## The drawing rules the art is made to

The HUD draws by rules the original engine's HUD loader and its materials fix
(`docs/interface/hud-re.md`, "The HUD texture loader"), so the texels are chosen for them:

- **Colour-mode art** (the crosshairs, the map art, the panel, the boxes, the pips, the tip icons) draws
  through the device's modulate-2x stage: what shows is twice the texel times the draw's colour. Art
  drawn in white (the crosshairs, the map icons, the pips) carries white texels and takes the colour of
  the draw; art drawn in plain white (the panel, the compass bezel) is authored at half the brightness
  it shows at. The boxes draw at a half diffuse and so show as authored; their border shows
  `2 x border x boxtile`.
- **Alpha-mode art** (the stance icons, the AR-15's silhouette, magazine, rounds, reticle and armory
  picture) keeps only its alpha and draws in the hudpos `stanceicon_color`: white texels, the picture
  in the alpha.
- **Sized by its texels**: the panel, the crosshairs, the magazine, the rounds, the weapon silhouette
  (the AR-15's art is at the sizes of the original M4's, which its weapon.def entry is modelled on), the
  stance icons (normalised: the first frame's longer side becomes 128 design units), the box pieces
  (a quarter of the atlas each, at the surface's width over 1600), the tip icon (48 design units). The
  map art, the masks and the gain scale stretch to their rects, so they are made larger.

## The files

| Texture (in `assets/`) | Size | Mode | Master | What it is |
|---|---|---|---|---|
| `cross01.tga` .. `cross25.tga` | 64 x 64, 32-bit | colour | `src/crosshairs/` | The 25 crosshair styles of the options (`cross01` the default; `cross25` empty: no crosshair). White marks with a soft dark rim, each mark inside one of the five regions the spread pulls apart (four edge wedges and the centre square 0.45..0.55). |
| `compring.tga` | 512 x 512, 32-bit | colour | `src/map/compring.png` | The compass bezel around the map: 5-degree ticks, N E S W in amber facing outward; its band starts at the map disc's edge (0.45 x 512 / 1.25 texels from the centre is the map radius). |
| `TSDicon.tga` | 64 x 1920, 32-bit | colour | `src/map/TSDicon.png` | The map's 30 icon cells, white with a dark rim (some carry their own colour): square, dot, flag, person, emplacement, FARP, flag bay, chevron, down, bridge, vehicle, helicopter, twin emplacement, armoury, wounded, boat, medical, ammunition, aircraft, four lettered zones, ride wanted, enemy sighted, motorcycle, attack, defend, speaker, pick-up. |
| `WPIndctr.tga` | 64 x 256, 32-bit | colour | `src/map/WPIndctr.png` | The waypoint altitude nub: above, below, level, blank. |
| `dmgslice.tga`, `dmgslc_n.tga` | 256 x 256, 32-bit | colour | `src/map/` | The map's hit-direction slices (30 and 15 degrees), white, brightest at the map's rim. |
| `onhframe.tga` | 176 x 170, 32-bit | colour | `src/panel/onhframe.png` | The soldier panel (hudpos `StaticFrame`): the weapon pane, the stance pane and the health slot. |
| `h_onar15.tga` | 124 x 75, 32-bit | alpha | `src/weapon/h_onar15.png` | The AR-15's side silhouette along the top of the canvas (its weapon.def `hudicon`), rendered from our model. |
| `h_onclip.tga`, `h_onrnd.tga` | 131 x 16, 4 x 16, 32-bit | alpha | `src/weapon/` | The AR-15's magazine window and one round (its weapon.def `hudclipgfx 0 0` and `hudrndgfx 6 0 4 0 1`: thirty rounds from x 6, a notch every ten). |
| `onxhair.tga` | 64 x 64, 32-bit | alpha | `src/weapon/onxhair.png` | The AR-15's own reticle (its weapon.def `crosshair`): a fine open cross. |
| `m_onar15.tga` | 310 x 90, 32-bit | alpha | `src/weapon/m_onar15.png` | The AR-15 in the armory (its weapon.def `loadout_menu_icon`), rendered from our model. |
| `h_onpist.tga` | 122 x 65, 32-bit | alpha | `src/weapon/h_onpist.png` | The pistol's side silhouette (its weapon.def `hudicon`), rendered from our model, in the box the original's `H_bereta` fills (22, 13 to 81, 52 of the same 122 x 65 canvas). It shares the AR-15's magazine window, round and reticle (`hudclipgfx 0 0 h_onclip.tga`, `hudrndgfx 7 0 8 0 1 h_onrnd.tga`: fifteen rounds from x 7, 8 texels apart), as the original's `WPN_M9Beretta` shares `WPN_M4AUTO`'s. |
| `m_onpist.tga` | 310 x 90, 32-bit | alpha | `src/weapon/m_onpist.png` | The pistol in the armory (its weapon.def `loadout_menu_icon`), rendered from our model, the canvas's full height, centred. |
| `h_onknif.tga`, `m_onknif.tga` | 122 x 65, 310 x 90, 32-bit | alpha | `src/weapon/` | The knife's silhouette (`WPN_KNIFE`'s and `WPN_KNIFE2`'s `hudicon`) in the box the original's `H_knife` fills (8, 19 to 116, 46), and its armory picture (`loadout_menu_icon`), rendered from `weapon_on_knif`. |
| `h_onfrag.tga`, `m_onfrag.tga` | 122 x 65, 310 x 90, 32-bit | alpha | `src/weapon/` | The frag grenade: three grenades in the box the original's `H_frag` fills (16, 6 to 105, 56), from `weapon_on_frag`; two in the armory, in `M_frag`'s box (88, 6 to 211, 85), from `weapon_on_frag_m`. |
| `h_onsmok.tga`, `m_onsmok.tga` | 122 x 65, 310 x 90, 32-bit | alpha | `src/weapon/` | The smoke grenade's two canisters in the box the original's `H_smoke` fills (33, 2 to 83, 52), and in the armory, from `weapon_on_smok`. |
| `h_onmedp.tga`, `m_onmedp.tga` | 122 x 65, 310 x 90, 32-bit | alpha | `src/weapon/` | The medical pouch in the box the original's `H_medpak` fills (35, 3 to 83, 53), and in the armory, from `weapon_on_medp`. |
| `onhstnc0.tga` .. `onhstnc5.tga` | 256 x 136, 32-bit | alpha | `src/stance/` | The stance icons: stand, crouch, prone, sitting, emplaced, parachute. |
| `border.tga` | 128 x 128, 32-bit | colour | `src/box/border.png` | The window box (the scoreboard, objectives, help, briefing, message log): a bevelled frame centred on the fill's inset edge, the title tab on row 3, the scan-lined dark fill in cell (3,0). |
| `boxtile.tga` | 256 x 256, 24-bit | colour | `src/box/boxtile.png` | Our four-colour woodland camouflage, tiling, which the box frame is printed with. |
| `monogram.tga` | 512 x 256, 24-bit | additive | `src/box/monogram.png` | The box's glow pass, black: it adds nothing (the original ships it black too). |
| `border3.tga` | 128 x 128, 32-bit | colour | `src/box/border3.png` | The tip panel: a dark panel, a light keyline, an amber corner; cell (1,1) is its fill. |
| `k_tip.tga`, `g_tip.tga` | 64 x 64, 32-bit | colour | `src/tip/` | The tip icons: a key and our crouching soldier, each with an amber question mark. |
| `Binoculr.tga` | 1024 x 1024, 32-bit | stretched | `src/view/Binoculr.png` | The binocular mask over the screen: two lenses and the rangefinder window. |
| `BinoCH.tga` | 256 x 256, 32-bit | stretched | `src/view/BinoCH.png` | The binocular reticle (drawn over 384,256..640,512). |
| `BNumbers.tga` | 16 x 160, 24-bit | additive | `src/view/BNumbers.png` | The rangefinder's amber seven-segment digits, a 16-texel cell each, kept to the left 10 (the digits step 10). |
| `NVG.tga` | 1024 x 1024, 32-bit | stretched | `src/view/NVG.png` | The night-vision tube mask. |
| `NVGScale.tga` | 16 x 80, 24-bit | stretched | `src/view/NVGScale.png` | The night-vision gain rows (0..4), rising bars. |
| `vignette.tga` | 512 x 512, 32-bit | alpha | `src/view/vignette.png` | The damage vignette's alpha (drawn in red). |
| `neticon1.tga`, `neticon2.tga`, `neticon3.tga` | 32 x 64, 16 x 64, 16 x 32, 32-bit | as stored | `src/net/` | The connection indicators: the T/R link pairs, the quality lamps, the NovaWorld N. |
| `JO_LFP.tga`, `R_LFP.tga`, `N_LFP.tga` | 64 x 256, 32-bit | colour | `src/lfp/` | The objective tiles of each side (our own emblems: a star in a globe, three claw marks; neutral plain), four states each. |
| `lfp_alf.tga`, `lfp_dlf.tga` | 32 x 32, 32-bit | colour | `src/lfp/` | The objective marks: attack, defend. |
| `H_flag.tga` | 32 x 32, 32-bit | colour | `src/misc/H_flag.png` | The carried-flag icon. |
| `rockpip.tga`, `turrpip.tga`, `dirguide.tga` | 64 x 64, 32 x 32, 64 x 64, 32-bit | colour | `src/misc/` | The vehicle pips: the fixed gun's, the turret's lag, the driver's heading guide. |
| `onhudb18.fnt`, `onhud14.fnt` | 256 x 256 pages | font | `fonts/` | The HUD fonts (hudpos `fonthud1_hi`, above 640 wide; `fonthud1_lo`), 18 and 14 texels a line. |

`hudpos.def` beside them is hand-written: every position is our own, in the original's 1024 x 768
design space.

## Third-party material

| Material | Author | Licence | Used for |
|---|---|---|---|
| [Barlow Condensed](https://github.com/google/fonts/tree/main/ofl/barlowcondensed) Bold and SemiBold, v1.4 | The Barlow Project Authors (Jeremy Tribby) | SIL Open Font License 1.1 | Rasterised into the two glyph sheets; the compass letters, the lettered map zones and the tip icons' question marks and key letter |

Everything else is drawn from scratch (shapes, noise and Blender renders of our own scenes).
