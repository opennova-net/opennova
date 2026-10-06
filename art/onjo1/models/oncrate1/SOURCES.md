# `oncrate1` sources

`oncrate1.blend` is the source of truth. Its scene `oncrate1` is the game model
(two LODs, one part, the collision box); its scene `oncrate1 bake` is the
high-detail crate the textures are baked from. The baked textures under
`textures/` are what export writes into `assets/` (`oncrate1_0.tga`, the
diffuse with the specular mask in its alpha; `oncrate1_0n.mdt`, the normal map).

The model and its layout are our own, made from scratch in the look of the
original game's wooden crate (looked at for size, palette and collision only).
The stencil marks are our own text in Blender's built-in font.

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `Wood062` (2K JPG: Color, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Wood062) | ambientCG (Lennart Demes) | CC0 1.0 | The boards' grain (most boards) |
| `Wood059` (2K JPG: Color, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Wood059) | ambientCG (Lennart Demes) | CC0 1.0 | The odd bleached board |
| `Metal021` (1K JPG: Color, Metalness, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Metal021) | ambientCG (Lennart Demes) | CC0 1.0 | Corner caps, bolts, nails |
| `Rope002` (1K JPG: Color, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Rope002) | ambientCG (Lennart Demes) | CC0 1.0 | Rope handles |

The source maps are shared by every model and sit under
`art/onjo1/materials/ambientcg/` as downloaded (2026-10-05). They are
regraded in the materials (`crate_wood`, `crate_steel`, `crate_rope`) toward the
original game's muted grey-brown, with per-board variation, edge wear, crevice
darkening and dirt at the base made in the node trees.
