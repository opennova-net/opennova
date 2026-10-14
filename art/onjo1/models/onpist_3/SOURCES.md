# `onpist_3` sources

`onpist_3.blend` is the source of truth: the third-person pistol `WPN_colt45` (our WK-11, named by the sidearm id the engine's default kits name) names as `gfx3` (what
another soldier, the chase view and the `Power Up Pistol` pickup draw) and the pickup's model. Its scene
`Scene` is the game model: three LODs of one part (thresholds 108, 25 and 0, as the original's
`M9_3RD`), LOD 2 the collision and bullet faces, and the user points `MFLASH01` (the muzzle), `bullet`
(where a round starts, on the bore 0.05 m ahead of the ejection port, as `onar15_3` places it) and
`bcasing` (the ejection port). `CB-colonly`, the box a soldier walks into to take the pickup
(1.24 m, as `onar15_3`'s and the original's pickups carry one), is `onar15_3`'s.

The geometry is the first-person pistol's own (`art/on_pist/on_pist.blend`, its rig in its rest pose),
moved so the grip is the model origin (4.8 cm ahead of the grip's back) and the bore runs 0.08 m above
it along the model's forward, the original's convention. LOD 0 (700 triangles) is a 2 mm voxel shell of
the pistol collapsed to 700 triangles, LOD 1 (256) the same shell collapsed further, each face on the
material and each corner on the UV of the source's nearest face. LOD 2 (24 triangles, closed) is two
boxes, the slide and the grip at its slant, every corner on one texel of `on_pist_0_c.tga` near that
texture's mean colour. The two materials are `on_pist`'s, and so are their textures
(`art/on_pist/textures/on_pist_{0,1}_c.tga`), which the game loads once for both models. They draw with
`VS_PHONGT`, as the original's `M9_3RD` does, over `on_pist`'s flat normal map (`on_pist_0n.mdt`),
each material's texture list naming the files `on_pist` already writes.

The original game's `M9_3RD` was looked at for its size, origin, LOD thresholds, collision LOD, user
points and shader only.

No third-party material is used beyond what `on_pist` itself uses (`art/on_pist/SOURCES.md`: the
KINEMATION pack, an exception to the CC0 rule).
