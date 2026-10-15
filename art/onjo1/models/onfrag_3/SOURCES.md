# `onfrag_3` sources

`onfrag_3.blend` is the source of truth: the third-person frag grenade: what `WPN_GRENADEHE` names
as `gfx3` (in another soldier's hand and the chase view) and the `Thrown frag grenade` item (101883,
the round's `frndlyTrcrID 1883`) draws in flight. Its scene `onfrag_3` is the game model: three LODs
(850 / 340 / 64 triangles, thresholds 38, 19 and 0, as the original's `Frag_3RD`) of one part, LOD 2
the collision and bullet faces, and the user point `bullet` on the body, facing up, as `Frag_3RD`
places it.

The geometry is the first-person model's own (`art/on_frag/on_frag.blend`, its parts in their bones'
frame), stood in `Frag_3RD`'s frame: the body's centre 4.5 cm up and 2.2 cm forward, the fuze up,
the spoon forward. LOD 1 is LOD 0 collapsed, keeping its UVs; LOD 2 is a six-sided body and a box
fuze, each face on a quarter-texel copy of itself around the UV of LOD 0's nearest face, so it keeps
that texel's colour and a tangent frame. The material is `on_frag`'s, drawn with `VS_PHONGT`, its
texture list naming the files `on_frag` already writes (`on_frag_0.tga`, read as `on_frag_0.dds`,
and `on_frag_0n.mdt`), which the game loads once for both models.

The original's `Frag_3RD` was looked at for its size, frame, LOD thresholds, collision LOD, user
point and shader only. No third-party material is used.
