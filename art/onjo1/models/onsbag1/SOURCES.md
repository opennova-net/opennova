# `onsbag1` sources

`onsbag1.blend` is the source of truth. Its scene `onsbag1` is the game model:
a straight sandbag wall 4.6 m long and 1.1 m high, nine courses in running
bond, three bags deep in the five lower courses and two in the four upper, four
LODs (2,169 / 780 / 280 / 96 triangles, thresholds 128, 44, 8 and 0, LOD type
`bldg`) of one part, two `CB` volumes (the deep lower block and the upper one)
a soldier stands behind, and LOD 1's triangles as the bullet faces, all Dirt,
as the original's `sbag01` (a 4.5 m wall, its LOD 1 the Dirt faces, its walk
volumes a lower and an upper block). Its scene `onsbag1 bake` holds the
high-detail wall the textures are baked from: 184 filled hessian bags, each a
pillow with a tied end, its sand settled in lumps and its cloth creased, sagging
and turned a little at random, and the voxel union of them all from which the
game mesh is decimated.

The textures follow the original's buildings, upgraded as the hut is: one baked
base on the first UV map (`textures/onsbag1_base.png`, the colour bake times
the ambient occlusion bake, 2048, as the player presses against it), a 512
normal map (`onsbag1_normal.png`), and the hessian's weave as a grey tiling
detail on the second UV map (`onsbag1_det.png`, mean 128, every 0.27 m), drawn
with `VS_DOT3DIFF2`.

The model and its layout are our own, made from scratch in the look of the
original game's sandbag walls (`sbag01`, `sbag06`, looked at for size, palette,
LODs, collision and shader only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `hessian_380` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/hessian_380) | Rico Cilliers, colormass | CC0 1.0 | The bags' cloth (box-projected at its real size), the detail |

The source maps sit under `art/onjo1/materials/polyhaven/` as downloaded
(2026-10-06). The cloth is regraded in the bake material toward the original's
muted khaki, each bag tinted on its own and dirtied toward the ground; the base
is graded to a lit texel about half its seen value.
