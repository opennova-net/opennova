# `onundr1` sources

`onundr1.blend` is the source of truth. Its scene `onundr1` is the game model:
the island's jungle undergrowth, a foliage definition's graphic (the terrain
set's first `foliage` block, code 254 of the foliage map), not an item. It is
laid out as `ongrass1` and the original game's terrain foliage (`mveg5b`): one
LOD, one part, one strip of nine upright cards, 36 vertices and 18 triangles,
`FF_ST_OP`, alpha-tested at 128 and two-sided, Foliage bullet faces. Seven
cards show the atlas's undergrowth strip and two its grass, as the original's
`mveg5` mixes its atlas's two strips. The cards stand 1.9 to 2.35 m tall in the
file, so 0.95 to 1.2 m in a mission (the game halves a terrain foliage model's
height, `Foliage_GenerateInstances_0 @ 0x600121`).

It draws with `ongrass1`'s atlas, which the game loads once for both: the
material's texture list names `ongrass1_0.tga` (the file `ongrass1` writes, its
`.dds` read first), and its image node shows `ongrass1`'s atlas source in
Blender only. The original's `mveg5` and `mveg5b` share `mveg5.tga` the same
way.

No third-party material is used beyond what `ongrass1` uses
(`art/onjo1/models/ongrass1/SOURCES.md`).
