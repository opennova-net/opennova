# `onradio1` sources

`onradio1.blend` is the source of truth. Its scene `onradio1` is the game model:
a manpack field radio (its receiver-transmitter clamped over the battery box,
knobs, a dial window, connectors, a carry handle and a whip antenna) with its
handset and cord, set on a small wooden ammunition crate; three LODs (908 / 480
/ 288 triangles, thresholds 38, 10 and 0) of one part, the later LODs dropping
the knobs and cord, then the handset and antenna, so they keep LOD 0's UVs. Two
`CB` volumes (the crate and the set) and LOD 1's triangles as the bullet faces:
the crate's Wood and the radio's Metal, two materials on the one texture set
(the second names the files the first writes).

The scene `onradio1 bake` holds the same LOD 0 with the bake materials: one
baked base on the first UV map (`textures/onradio1_base.png`, the colour bake
times the ambient occlusion bake, 1024), a 512 normal map
(`onradio1_normal.png`) and a fine grey grain on the second UV map
(`onradio1_det.png`, mean 128), drawn with `VS_DOT3DIFF2`.

The model and its layout are our own, made from scratch in the look of the
original game's communications radio on its table (`Cradio`, `Radio01`,
`Table01`, looked at for size, palette, LODs, collision and shader only); like
`Cradio` the item is `notarget`.

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `Wood062` (2K JPG: Color, NormalGL) | [ambientCG](https://ambientcg.com/view?id=Wood062) | ambientCG (Lennart Demes) | CC0 1.0 | The crate's boards, under worn olive paint |
| `Metal021` (1K JPG: Color, NormalGL) | [ambientCG](https://ambientcg.com/view?id=Metal021) | ambientCG (Lennart Demes) | CC0 1.0 | The radio's steel under its olive drab, the detail |

The source maps are the crate's and barrel's (`art/onjo1/materials/ambientcg/`,
downloaded 2026-10-05), box-projected and painted in the bake materials: a
muted olive drab with grime and scuffs to bare steel, the crate's paint worn
through to grey wood, black rubber knobs and handset, a pale dial scale.
