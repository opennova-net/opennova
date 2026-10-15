# `onmedp_3` sources

`onmedp_3.blend` is the source of truth: the third-person medical pouch `WPN_MEDPACK` names as
`gfx3`, what another soldier and the chase view draw in the hand. Its scene `onmedp_3` is the game
model: three LODs (284 / 112 / 12 triangles, thresholds 38, 19 and 0, as the original's `Medp_3RD`)
of one part, LOD 2 the collision and bullet faces, and the user point `bullet` by the hand, facing
forward, as `Medp_3RD` places it.

The geometry is the first-person model's own (`art/on_medp/on_medp.blend`, its parts in their bones'
frame), stood in `Medp_3RD`'s frame: the pouch thin across the model, its length forward from the
hand, its patch on the right. LOD 1 is LOD 0 collapsed, keeping its UVs; LOD 2 is the pouch's box,
each face on a quarter-texel copy of itself around the UV of LOD 0's nearest face, so it keeps that
texel's colour and a tangent frame. The material is `on_medp`'s, drawn with `VS_PHONGT`, its texture
list naming the files `on_medp` already writes (`on_medp_0.tga`, read as `on_medp_0.dds`, and
`on_medp_0n.mdt`), which the game loads once for both models.

The original's `Medp_3RD` was looked at for its size, frame, LOD thresholds, collision LOD, user
point and shader only. No third-party material is used.
