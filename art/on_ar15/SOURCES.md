# `on_ar15` sources

## The carbine's materials

The carbine's four materials `on_ar15_0` .. `on_ar15_3` draw with `VS_PHONGT`, as the original's
first-person `M4_1st`, `M16_1st` and most of its first-person guns do: a Phong highlight whose
strength is the diffuse texture's alpha, through a tangent-space normal map. The normal map is flat
(`textures/on_ar15_n.png`, 64 a side, all four materials share it; export writes it as
`on_ar15_0n.mdt`). The diffuses `textures/on_ar15_{0,1,2,3}_c.tga` are graded for the original's
lighting, which draws `2 x texel x light`: each texel holds half the colour it shows lit, and the
alpha holds the specular mask, 1.3 times the texel's luminance (the original's guns' masks follow
their luminance at 1.2 to 1.6). The tangent frames make a vertex 64 bytes, so the LOD fits the
game's 2 MiB vertex pool buffer only under 32,768 vertices: the magazine and the spare magazine
(`59 Mesh`, `64 Mesh`) are halved (Decimate, collapse 0.5) to 2,960 faces each, which keeps the
whole carbine at 30,365 vertices.

## The arms' textures

`on_arms` (the first-person arms in `on_ar15.blend`, the `Avatars.def` combo's
arms part) wears the player soldier `onsold1`'s woodland fatigues: camouflage
sleeves with a darker hem, and dark leather gloves from 3.2 cm before each wrist.
`on_arms_0_c.tga` and `on_arms_1_c.tga` under `textures/` are baked onto the
mesh's existing UVs at 1024 a side from the materials `on_arms_0_fatigue_src`
and `on_arms_1_fatigue_src` (kept in the `.blend` with a fake user), which use the
same noise, scales and colours as `onsold1`'s bake source
(`art/onjo1/person/onsold1.blend`), with the mesh's own ambient occlusion. The
`on_glove` and `on_hem` vertex attributes on `on_arms Skin` mark the gloves and
the hem. The normal maps `on_arms_{0,1}_n.tga` (exported as `on_arms_{0,1}n.mdt`)
are unchanged.

To bake again: in each material slot of `on_arms Skin`, put its
`*_fatigue_src` material, give that material's `bake_target` node a 1024 image
(Non-Color, so the bake is stored as baked, as `onsold1`'s diffuse is), bake
Emit and Ambient Occlusion, multiply the colour by `0.5 + 0.5 * AO`, and save it
over the `_c.tga` the slot's own material reads. Then put back the slot's own
material.

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `Fabric077` (1K JPG: Color) | [ambientCG](https://ambientcg.com/view?id=Fabric077) | ambientCG (Lennart Demes) | CC0 1.0 | The sleeves' twill |
| `Leather026` (1K JPG: Color) | [ambientCG](https://ambientcg.com/view?id=Leather026) | ambientCG (Lennart Demes) | CC0 1.0 | The gloves' leather |

Both maps sit under `art/onjo1/materials/ambientcg/` as downloaded (2026-10-05).
The camouflage is procedural noise, our own.
