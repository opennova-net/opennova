# `onar15_3` sources

`onar15_3.blend` is the source of truth: the third-person AR-15-pattern carbine
`WPN_ON_AR15` names as `gfx3` (what another soldier, the chase view and the
`Power Up AR-15` pickup draw) and the pickup's model. Its scene `Scene` is the
game model: four LODs of one part (thresholds 160, 64, 12 and 0, as the
original's `M4_3RD`), LOD 2 the collision and bullet faces, and the user points
`MFLASH01` (the muzzle), `bullet` (where a round starts, 0.06 m ahead of the
ejection port as the original places it), `bcasing` (the ejection port) and
`scope` (the rear sight).

The geometry is the first-person carbine's own (`art/on_ar15/on_ar15.blend`,
its rig in its rest pose, the spare magazine of the reload left out), moved so
the pistol grip is the model origin and the bore runs 0.09 m above it along the
model's forward, the original's convention, then decimated to LOD 0's 3,999
triangles. LOD 1 (2,687 triangles) is LOD 0 with its coplanar faces merged and
its pieces under 3 cm dropped, keeping LOD 0's corners. LOD 2 (498 triangles, a
closed mesh, the bullet faces too) is a 4 mm voxel shell of LOD 0 collapsed to
500 triangles, each face on the material and each corner on the UV of LOD 0's
nearest face. (The first LOD 1 and LOD 2, LOD 0 decimated to 1,500 and 533
triangles, tore its loose pieces apart: LOD 2 kept 17% of LOD 0's surface,
drawn from 5 m.) LOD 3 is a silhouette of six boxes (72 triangles), every corner
on one texel of `on_ar15_0_c.tga` nearest that texture's mean colour. The four
materials are `on_ar15`'s, and so are their textures
(`art/on_ar15/textures/on_ar15_{0,1,2,3}_c.tga`), which the game loads once for
both models. They draw with `VS_PHONGT`, as the original's `M4_3RD` does: the
Phong highlight whose strength is the diffuse alpha, over `on_ar15`'s flat normal
map (`on_ar15_n.png`, written as `on_ar15_0n.mdt`), each material's texture list
naming the two files `on_ar15` already writes. LOD 3's corners each sit on a
quarter-texel square around their texel, laid out along the face, so the shader
has a tangent frame to light it by while it keeps that texel's colour.

The original game's `M4_3RD` was looked at for its size, origin, LOD
thresholds, collision LOD, user points and shader only.

No third-party material is used beyond what `on_ar15` itself uses.
