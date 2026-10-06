# `onbush1` sources

`onbush1.blend` is the source of truth. Its scene `onbush1` is the game model:
a broad-leaved jungle shrub about 1.5 m tall and 2.5 m across, 48 leafy sprays
rising out of a dome, three LODs (576 / 192 / 48 triangles, thresholds 64, 16
and 0) of one part. As the original game's bushes are, it has no walk volume
and its last LOD's triangles are its bullet faces, all Foliage. Its scene
`onbush1 bake` holds the eight leaf sprays the atlas is rendered from: twigs of
`LeafSet022` leaves in two layers (the back one shadowed), laid flat and
rendered top-down.

Each spray is a gently V-folded card along one spray of the atlas
(`textures/onbush1_leaves.png`), alpha-tested at 128 and two-sided, drawn with
`FF_ST_OP` as the original's foliage is.

The model and its layout are our own, made from scratch in the look of the
original game's jungle bushes (`J_bsh1`, `J_bgrp1`, looked at for size,
palette, LODs, shader and collision only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `LeafSet022` (2K PNG: Color, Opacity) | [ambientCG](https://ambientcg.com/view?id=LeafSet022) | ambientCG (Lennart Demes) | CC0 1.0 | The sprays' leaves |

The source maps sit under `art/onjo1/materials/ambientcg/` as downloaded
(2026-10-06), regraded in the bake scene's material toward the original's muted
green (a lit texel about half its seen value), each leaf tinted on its own.
