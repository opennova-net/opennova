# `onshed1` sources

`onshed1.blend` is the source of truth. Its scene `onshed1` is the game model,
exported into `expansions/onjo1/models/` (the expansion `onjo1`): a plank
supply shed raised on six stilts, 3.8 by 2.8 metres inside its walls, with a
front porch and two steps, a closed plank door with its batten and hasp, a
shuttered window on its right side, plank gables and a ridge beam, and a gable
roof of two rusty corrugated-iron sheets with a ridge cap, 3.7 metres to the
ridge. Three LODs of one part (464, 372 and 66 triangles, thresholds 96, 24 and
0, LOD type `bldg`): LOD 1 is LOD 0 without its small pieces (the joists, the
batten, the hasp, the shutters and the ridge cap), face for face with LOD 0's
UVs; LOD 2 is a shell (deck, walls, gables, roof and steps) whose UVs are carried
over from LOD 0 by nearest face. LOD 1's triangles are the bullet faces (Wood,
the roof Metal); the walk volumes (`CB`) are the body to the eaves, the porch,
both steps and the roof, and one `VC` volume stops vehicles.

The textures follow the base game's buildings (`onhut1`, `ontower1`): a 1024
base on the first UV map (`textures/onshed1_base.png`), baked in Cycles from the
CC0 materials below on a real-size box projection, the colour bake times the
ambient occlusion bake and graded to a stored mean of 0.22 as the base game's
bases are (a lit texel about half its seen value); a 512 normal map
(`onshed1_normal.png`, the materials' normal maps baked in tangent space and
averaged down from 1024); and a grey tiling detail per material on the second
UV map, mean 128, every 0.9 m (`onshed1_det_wood.png`, `onshed1_det_tin.png`:
each material's own grain, high-passed against a wide blur), drawn with
`VS_DOT3DIFF2`. The bake materials (`onshed1_bake_wood`, `onshed1_bake_tin`)
stay in the file for a re-bake.

The scene `onjo1_m1 loading` renders the mission `onjo1_m1`'s loading screen
(`expansions/onjo1/art/onjo1_m1.png`, 800 x 600): the shed at night under a
storm sky, lit by a lightning flash behind and a lamp in its door, the base
game's palms (`onpalm1`, linked from `art/onjo1/models/onpalm1/onpalm1.blend`,
never copied in) behind it, and the title in the menu's colours, set in
Blender's built-in text font.

The model and its layout are our own, made from scratch in the look of the
original game's jungle buildings (looked at for size, palette and shader only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `wood_planks_grey` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/wood_planks_grey) | Rob Tuytel | CC0 1.0 | The planks (walls, deck, stilts, steps, door, gables), the wood detail |
| `rusty_corrugated_iron` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/rusty_corrugated_iron) | Charlotte Baglioni | CC0 1.0 | The roof sheets, the ridge cap and the hasp, the tin detail |

The source maps sit under `art/onjo1/materials/polyhaven/` as downloaded for the
base game's models.
