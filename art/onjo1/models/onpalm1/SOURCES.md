# `onpalm1` sources

`onpalm1.blend` is the source of truth. Its scene `onpalm1` is the game model: a
coconut palm about 10 m tall on a gently curved trunk, its crown 22 fronds (the
young ones upright, the old ones drooping, the oldest dead and hanging), four
LODs (1,196 / 624 / 342 / 124 triangles, thresholds 384, 128, 32 and 0) of one
part, three `CB` walk volumes up the trunk, and LOD 2's triangles as the bullet
faces: the trunk's Wood (a round stops), the fronds' Foliage (a round goes on
through them at a cost, as the game does for surface 17). Its scene
`onpalm1 bake` holds the four frond sources the frond texture is rendered from:
leaflets of `LeafSet013` laid along a rachis, flat, rendered top-down.

The fronds are V-folded cards, each a strip along one frond of the atlas
(`textures/onpalm1_fronds.png`: a green, a worn, a drying and a dead frond),
alpha-tested at 128 and two-sided, drawn with `FF_ST_OP` as the original game
draws its palm fronds and bushes. The trunk draws with `VS_DOT3DIFF2`, the
normal-mapped member of the `FF_MT` family the original's trunks use: a base
texture along the trunk (`onpalm1_trunk.png`: the bark's low frequencies,
graded, the leaf scars a ring every 10 to 16 cm, damp at the foot, the boot's
fibre under the crown), a tiling detail on the second UV map
(`onpalm1_trunk_det.png`, the bark's high-passed grey, mean 128) and a normal
map (`onpalm1_trunk_n.png`, the bark's normal along the trunk with the scars'
ridges). The trunk runs 0.8 m below the origin, so a slope never shows its end.
`onpalm2` draws with the textures this model writes.

The model and its layout are our own, made from scratch in the look of the
original game's jungle palms (`J_tre13` and `J_tre11`, looked at for size,
palette, LODs, shaders and collision only, with `opennova-3di info --verbose`
on scratch copies).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `LeafSet013` (2K PNG: Color, Opacity) | [ambientCG](https://ambientcg.com/view?id=LeafSet013) | ambientCG (Lennart Demes) | CC0 1.0 | The fronds' leaflets |
| `palm_tree_bark` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/palm_tree_bark) | Dimitrios Savva, Rico Cilliers | CC0 1.0 | The trunk's base, detail and normal |

The source maps sit under `art/onjo1/materials/` as downloaded (2026-10-06).
The leaflets are regraded in the bake scene's material toward the original's
muted olive (a lit texel about half its seen value), each tinted on its own;
the trunk's maps were resampled along the trunk and graded by a throwaway
script.
