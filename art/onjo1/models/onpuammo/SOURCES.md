# `onpuammo` sources

`onpuammo.blend` is the source of truth. Its scene `onpuammo` is the game model: the ammunition
powerup's pickup (`items.def` 102042, `Power Up Ammo Pack`, its powerup `PU_ONAMMO`), two olive drab
steel ammunition cans side by side (29 x 15 x 17.5 cm each), their lids, folded carry handles and
latches, a yellow stencil on each can's outer side (`CTG 5.56MM`, a ball-round line and a lot number,
our own text in Blender's built-in font). Three LODs (248 / 88 / 20 triangles, thresholds 38, 10 and
0; LOD 2 the cans and lids as boxes) of one part, LOD 1's triangles the bullet faces (Metal), laid out
as `onpumed` and the original's `PwrMed`: the origin 0.621 m over the ground, the `Ground` user point,
the 0.5 m pickup box.

The scene `onpuammo bake` holds LOD 0 with three procedural bake materials (the olive paint, chipped
on its edges and dirtier toward the ground, the stencil laid on from a mask kept packed in the
`.blend`; the bare steel latches; the handles). The textures are made as `onpumed`'s
(`onpuammo_base.png` 512, `onpuammo_normal.png` 256, `onpuammo_det.png` 256 every 20 cm), drawn with
`VS_DOT3DIFF2` (`onpuammo_0.dds`, `onpuammo_0d.dds`, `onpuammo_0n.mdt`).

The model and its layout are our own; the original's `PwrMed` was looked at for its size, origin,
user point, collision and LODs only (the original has no ammunition pickup model of its own). No
third-party material is used.
