# `ontower1` sources

`ontower1.blend` is the source of truth. Its scene `ontower1` is the game model:
a three-level timber guard tower laid out as the original's `Wgrdtwr1` (a 6 m
body on four corner posts, floors at 3.3, 6.6 and 9.9 m, the top platform 8.4 m
across behind a 1.1 m plank parapet, a hip roof of corrugated tin at 12.3 m),
with X braces in its open bays and switchback stairs inside. Four LODs (2,636 /
2,204 / 1,076 / 344 triangles, thresholds 172, 44, 12 and 0) of one part; the
later LODs drop parts of LOD 0 (the stair treads and rails, then the braces and
the lower floors, then the beams), so they keep its UVs.

It is climbed as the original's tower is, by its collision, with no user point
and no ladder: the floors are `CB` volumes that leave the well over each flight,
each flight a `CB` ramp (a slab whose top is the stairs' slope) from one floor
to the next, the four corner posts, the parapet and the roof are `CB` volumes,
and two `VC` volumes (the body and the top) are what vehicles meet. LOD 1's
triangles are the bullet faces, all Wood, as the original's LOD 1 is.

The scene `ontower1 bake` holds the same LOD 0 with the bake materials: the
textures follow the original's buildings, upgraded as the hut is, one baked base
on the first UV map (`textures/ontower1_base.png`, the colour bake times the
ambient occlusion bake, 2048), a 512 normal map (`ontower1_normal.png`) and the
planks' grain as a grey tiling detail on the second UV map (`ontower1_det.png`,
mean 128, every 0.5 m), drawn with `VS_DOT3DIFF2`.

The model and its layout are our own, made from scratch in the look of the
original game's wooden guard tower (`Wgrdtwr1`, looked at for size, levels,
LODs, collision and shader only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `wood_planks_grey` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/wood_planks_grey) | Rob Tuytel | CC0 1.0 | Timbers, planks and treads; the detail |
| `rusty_corrugated_iron` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/rusty_corrugated_iron) | Charlotte Baglioni | CC0 1.0 | The roof |

The source maps sit under `art/onjo1/materials/polyhaven/` (the hut's, downloaded
2026-10-05). They are box-projected at their real-world sizes and regraded toward
the original game's grey-brown, with damp, dirt toward the ground and a muted
rust made in the node trees and the grade.
