# `onpumed` sources

`onpumed.blend` is the source of truth. Its scene `onpumed` is the game model: the health powerup's
pickup (`items.def` 102041, `Power Up Med Pack`, its powerup `PU_ONHEALTH`), a field medical kit: an
olive canvas bag 40 x 25 x 21 cm, its lid's front lip, two webbing straps with their buckles, a carry
handle, and a first-aid patch (a white cross on a green square, the ISO 7010 sign's shape) on the lid
and the front. Three LODs (264 / 66 / 12 triangles, thresholds 38, 10 and 0; LOD 2 the bag's box) of
one part, LOD 1's triangles the bullet faces (Cloth). It is laid out as the original's `PwrMed`: the
origin 0.621 m over the ground, a `Ground` user point there facing up (where the editor's drop puts
the terrain), the bag standing on it, and the pickup's `CB` box 0.5 m square from the ground to
1.25 m, which a soldier walks into to take it.

The scene `onpumed bake` holds LOD 0 with three procedural bake materials (the canvas, worn and dirtier
toward the ground, the patch laid on its lid and front from a mask kept packed in the `.blend`; the
webbing; the black buckles). `textures/onpumed_base.png` (512) is the colour bake times
`0.4 + 0.6 x` the ambient occlusion bake, `textures/onpumed_normal.png` (256) the tangent normal bake,
and `textures/onpumed_det.png` (256, mean 128) a fine grey grain on the second UV map (box-projected,
every 12 cm), drawn with `VS_DOT3DIFF2` as the radio's (`onpumed_0.dds`, `onpumed_0d.dds`,
`onpumed_0n.mdt`).

The model and its layout are our own; the original's `PwrMed` was looked at for its size, origin,
user point, collision and LODs only. No third-party material is used.
