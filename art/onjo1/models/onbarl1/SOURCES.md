# `onbarl1` sources

`onbarl1.blend` is the source of truth. Its scene `onbarl1` is the game model
(four LODs, one part, the collision box, and LOD 0's own triangles as the
bullet faces: its collision LOD is 0, so a round strikes, and scars, the drawn
staves and hoops, as the original's `Barl01` does with a collision LOD that
repeats its LOD 0's outer shell); its scene `onbarl1 bake` is the high-detail
cask the textures are baked from: eighteen staves, the planked lid and bottom,
four iron hoops with rivets. The baked textures under `textures/` are what
export writes into `assets/` (`onbarl1_0.tga`, the diffuse with the specular
mask in its alpha; `onbarl1_0n.mdt`, the normal map).

The model and its layout are our own, made from scratch in the look of the
original game's wooden barrel (looked at for size, palette and collision only;
`opennova-3di info --verbose` on a scratch copy of `Barl01`).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `Wood059` (2K JPG: Color, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Wood059) | ambientCG (Lennart Demes) | CC0 1.0 | The staves' grain |
| `Wood062` (2K JPG: Color) | [ambientCG](https://ambientcg.com/view?id=Wood062) | ambientCG (Lennart Demes) | CC0 1.0 | Per-stave colour variation |
| `Metal021` (1K JPG: Color, Metalness, NormalGL, Roughness) | [ambientCG](https://ambientcg.com/view?id=Metal021) | ambientCG (Lennart Demes) | CC0 1.0 | Hoops and rivets |

The source maps are shared by every model and sit under
`art/onjo1/materials/ambientcg/` as downloaded (2026-10-05). They are regraded
in the materials (`cask_wood`, `cask_iron`) toward the original game's muted
grey-brown.
