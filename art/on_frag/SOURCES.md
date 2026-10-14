# `on_frag` sources

The grenade is our own, made from scratch in `on_frag.blend` (its geometry built by a throwaway
script): a round 6.2 cm body in olive drab with a thin yellow band under the fuze, the fuze with its
striker housing, the spoon down the body's side and the pull ring with its split pin; 850 triangles in
three parts. The original's `Frag_1st` and `Frag_3RD` were looked at for size, part layout, clips and
timing only.

## The grenade's material

The one material `on_frag_0` draws with `VS_PHONGT`, as the knife's: `textures/on_frag_c.png` (512 a
side; `on_frag_0.dds`) and `textures/on_frag_n.png` (256 a side; `on_frag_0n.mdt`), baked in Cycles
from the scene `on_frag bake` with four procedural materials (the body's olive drab paint, worn on its
edges, and the yellow band; the fuze's grey-green metal; the spoon; the bright steel ring), graded as
the knife's (`art/on_knif/SOURCES.md`).

The arms are `on_ar15`'s (`art/on_ar15/SOURCES.md`). No third-party material is used.
