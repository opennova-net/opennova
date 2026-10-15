# `onsmok_3` sources

`onsmok_3.blend` is the source of truth: the third-person smoke grenade: what `WPN_GRENADESM` names
as `gfx3` and the `Thrown smoke grenade` item (101875, the round's `frndlyTrcrID 1875`) draws in
flight. Its scene `onsmok_3` is the game model: three LODs (610 / 244 / 32 triangles, thresholds 44,
12 and 0, as the original's `Smok_3RD`) of one part, LOD 2 the collision and bullet faces, and the
user point `bullet` on the fuze, facing up, as `Smok_3RD` places it.

The geometry is the first-person model's own (`art/on_smok/on_smok.blend`, its parts in their bones'
frame), stood in `Smok_3RD`'s frame: the canister standing on the origin, the fuze up, the spoon
forward. LOD 1 is LOD 0 collapsed, keeping its UVs; LOD 2 is a six-sided canister and a box fuze,
each face on a quarter-texel copy of itself around the UV of LOD 0's nearest face, so it keeps that
texel's colour and a tangent frame. The material is `on_smok`'s, drawn with `VS_PHONGT`, its texture
list naming the files `on_smok` already writes (`on_smok_0.tga`, read as `on_smok_0.dds`, and
`on_smok_0n.mdt`), which the game loads once for both models.

The original's `Smok_3RD` was looked at for its size, frame, LOD thresholds, collision LOD, user
point and shader only. No third-party material is used.
