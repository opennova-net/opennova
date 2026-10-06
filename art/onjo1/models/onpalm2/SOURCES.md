# `onpalm2` sources

`onpalm2.blend` is the source of truth. Its scene `onpalm2` is the game model:
a shorter coconut palm (about 7 m) leaning hard to one side, 18 fronds, four
LODs (1,068 / 544 / 294 / 108 triangles), laid out as `onpalm1` is (one part,
thresholds 384, 128, 32 and 0; three `CB` walk volumes up the trunk; LOD 2's
triangles as the bullet faces, the trunk's Wood and the fronds' Foliage).

It draws with `onpalm1`'s textures, which the game loads once for both: each
material's texture list names the files `onpalm1` writes (`onpalm1_0.tga`
for the fronds; `onpalm1_1.tga`, `onpalm1_1d.tga` and `onpalm1_1n.mdt` for the
trunk), and its image nodes show `onpalm1`'s texture sources in Blender only.

No third-party material is used beyond what `onpalm1` uses
(`art/onjo1/models/onpalm1/SOURCES.md`).
