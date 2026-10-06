# `on_ar15` sources: the arms' textures

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
