# `onknif_3` sources

`onknif_3.blend` is the source of truth: the third-person knife `WPN_KNIFE` and `WPN_KNIFE2` name as
`gfx3`, what another soldier and the chase view draw in the hand. Its scene `onknif_3` is the game
model: three LODs (826 / 288 / 28 triangles, thresholds 96, 25 and 0, as the original's `M9K_3RD`)
of one part, LOD 2 the collision and bullet faces, and the user point `bullet` 3.4 cm up the grip,
facing forward, as `M9K_3RD` places it.

The geometry is the first-person model's own (`art/on_knif/on_knif.blend`, its parts in their bones'
frame), stood in `M9K_3RD`'s frame: the grip's centre at the origin, the blade up and the edge
forward. LOD 1 is LOD 0 collapsed, keeping its UVs; LOD 2 is a silhouette of a wedge blade, a box
guard and a box grip, each face on a quarter-texel copy of itself around the UV of LOD 0's nearest
face, so it keeps that texel's colour and a tangent frame. The material is `on_knif`'s, drawn with
`VS_PHONGT`, its texture list naming the files `on_knif` already writes (`on_knif_0.tga`, read as
`on_knif_0.dds`, and `on_knif_0n.mdt`), which the game loads once for both models.

The original's `M9K_3RD` was looked at for its size, frame, LOD thresholds, collision LOD, user
point and shader only. No third-party material is used.
