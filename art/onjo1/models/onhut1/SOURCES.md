# `onhut1` sources

`onhut1.blend` is the source of truth. Its scene `onhut1` is the game model: a
single-storey plastered mud-brick hut with a small room on the roof, four LODs
(4,289 / 3,017 / 518 / 226 triangles), one part, 32 collision volumes in LOD 0
and LOD 2's bullet faces. Its scene `onhut1 bake` holds the same LOD 0 with the
procedural materials the textures are baked from.

The model and its layout are our own, made from scratch in the look of the
original game's adobe huts (`DHut01`/`DHut02`, looked at for size, palette and
the way they texture only). Its door and windows are open and its rooms can be
entered. Its damage (plaster spalls, bites at the
corners and along the parapet, rubble at the base) is real geometry, the brick
under the plaster its own material.

The textures follow the original's buildings: one baked base on the first UV map
(`textures/onhut1_base.png`, the colour bake times the ambient occlusion bake,
2048), a 512 normal map (`onhut1_normal.png`), and per material a grey tiling
detail on the second UV map (`onhut1_det_*.png`, mean 128, which `VS_DOT3DIFF2`
multiplies in at 2x), each the high-passed grey of its source below.

| Material | Source | Author | Licence | Used for |
|---|---|---|---|---|
| `plastered_wall_05` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/plastered_wall_05) | Charlotte Baglioni | CC0 1.0 | The plaster skin, tinted tan; the plaster detail |
| `rough_plaster_brick_04` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/rough_plaster_brick_04) | Rob Tuytel | CC0 1.0 | Weathered patches and chips in the plaster |
| `brick_wall_13` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/brick_wall_13) | Rob Tuytel | CC0 1.0 | The mud brick under the plaster; the brick detail |
| `concrete_wall_008` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/concrete_wall_008) | Charlotte Baglioni, Dario Barresi | CC0 1.0 | Plinth, step and roof slabs; the concrete detail |
| `wood_planks_grey` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/wood_planks_grey) | Rob Tuytel | CC0 1.0 | Door, shutters, lintels, beams, posts, ladder; the wood detail |
| `rusty_corrugated_iron` (2K: Diffuse, nor_gl) | [Poly Haven](https://polyhaven.com/a/rusty_corrugated_iron) | Charlotte Baglioni | CC0 1.0 | The awning sheet; the metal detail |

The source maps are shared by every model and sit under
`art/onjo1/materials/polyhaven/` as downloaded (2026-10-05, through the MCP for
Blender library). They are box-projected at their real-world sizes and regraded
toward the original game's stored brightness (its huts' lit plaster about
96, 92, 72), with ground damp, rain streaks, stains and chalky patches made in
the node trees. Two generated models (MCP for Blender Premium: Tripo, Hyper3D
Rodin) were tried as references and not used.
