# `onfern1` sources

`onfern1.blend` is the source of truth. Its scene `onfern1` is the game model:
a sword-fern clump about 0.9 m tall and 1.9 m across, 28 fronds arching out of
its middle, three LODs (448 / 112 / 40 triangles, thresholds 48, 12 and 0) of
one part. As the original game's bushes are, it has no walk volume (a soldier
walks through it) and its last LOD's triangles are its bullet faces, all
Foliage: a round goes on through it at a cost.

Each frond is a gently V-folded card along one frond of the atlas
(`textures/onfern1_fronds.png`), alpha-tested at 128 and two-sided, drawn with
`FF_ST_OP` as the original's foliage is.

The model and its layout are our own, made from scratch in the look of the
original game's jungle bushes (`J_bsh1`, `J_bgrp1`, looked at for size,
palette, LODs, shader and collision only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `fern_02` (2K: Diffuse, Alpha) | [Poly Haven](https://polyhaven.com/a/fern_02) | Rico Cilliers, Rob Tuytel | CC0 1.0 | The five scanned fronds of the frond atlas |

The source maps sit under `art/onjo1/materials/polyhaven/` as downloaded
(2026-10-06). The atlas keeps the five fronds where the source lays them out,
drops its stray bits, and is graded toward the original's muted olive (a lit
texel about half its seen value) by a throwaway script.
