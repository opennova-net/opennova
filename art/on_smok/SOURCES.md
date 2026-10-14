# `on_smok` sources

The canister is our own, made from scratch in `on_smok.blend` (its geometry built by a throwaway
script): an olive drab cylinder 6 cm across and 11.3 cm tall with a pale band round its top for the
colour of the smoke, the frag's fuze, spoon (straight down the canister's side) and pull ring; 610
triangles in three parts. The original's `Smok_1st` and `Smok_3RD` were looked at for size and part
layout only.

The one material `on_smok_0` draws with `VS_PHONGT`: `textures/on_smok_c.png` (512 a side;
`on_smok_0.dds`) and `textures/on_smok_n.png` (256 a side; `on_smok_0n.mdt`), baked from the scene
`on_smok bake` as the frag's are (`art/on_frag/SOURCES.md`), the band in its own material.

The arms are `on_ar15`'s (`art/on_ar15/SOURCES.md`). No third-party material is used.
