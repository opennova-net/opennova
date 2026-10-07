# `ongrass1` sources

`ongrass1.blend` is the source of truth. Its scene `ongrass1` is the game model:
the island's terrain grass, a foliage definition's graphic (the terrain set's
second `foliage` block, code 253 of the foliage map), not an item. It is laid
out as the original game's terrain foliage (`mveg5`, `mveg5b`): one LOD, one
part, one strip of nine upright cards (a ragged ring of six about 1.5 m out
with three spokes), 36 vertices and 18 triangles, drawn with `FF_ST_OP`,
alpha-tested at 128 and two-sided, its 18 bullet faces Foliage. The cards stand
1.55 to 1.9 m tall in the file: the game draws a terrain foliage model at half
its height (`Foliage_GenerateInstances_0 @ 0x600121`), so the grass stands 0.8
to 0.95 m in a mission. Each card shows a window of the atlas's grass strip as
wide as the card, so the strip keeps its proportions.

Its scene `ongrass1 bake` renders the atlas (`textures/ongrass1_atlas.png`,
1024 a side): an orthographic camera on each of two rows, 2.4 m wide and 1.2 m
tall, lit by a sun and a grey sky, on a transparent film.

- The top half is the grass: 230 blades made from scratch, each a narrow
  V-folded ribbon curving out of the ground and drooping toward its tip, drawn
  with one of `LeafSet013`'s six long leaves stretched along it.
- The bottom half is the undergrowth `onundr1` draws: broad-leaved plants of
  six to eleven `LeafSet022` leaves on stalks rising out of a clump, a few
  blades among them.

A throwaway script grades each half toward the original's muted foliage (its
jungle atlas `mveg5` averages about (83, 95, 62) over its opaque texels; the
game multiplies the texture by the ground's colour and its light, foliage-re.md)
and fills the transparent texels with their nearest leaf's colour so the mips
carry no background.

The model and its layout are our own, made from scratch in the look of the
original game's terrain foliage (`mveg5`, `mveg5b`, looked at for size, layout,
vertex count, shader and texture layout only).

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `LeafSet013` (2K PNG: Color, Opacity) | [ambientCG](https://ambientcg.com/view?id=LeafSet013) | ambientCG (Lennart Demes) | CC0 1.0 | The grass blades |
| `LeafSet022` (2K PNG: Color, Opacity) | [ambientCG](https://ambientcg.com/view?id=LeafSet022) | ambientCG (Lennart Demes) | CC0 1.0 | The undergrowth's leaves |

The source maps sit under `art/onjo1/materials/ambientcg/` as downloaded.
