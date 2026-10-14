# `on_knif` sources

The knife is our own, made from scratch in `on_knif.blend` (its geometry built by a throwaway script):
a 30 cm fighting knife, a 17 cm drop-point blade with a swedge and a coated finish whose edge bevel is
bare steel, a steel guard, a ribbed olive polymer grip and a steel pommel; 826 triangles in one part.
The original's `M9k_1st` and `M9K_3RD` were looked at for size, part layout, clips and timing only.

## The knife's material

The one material `on_knif_0` draws with `VS_PHONGT`, as the AR-15's, the pistol's and the original's
first-person `M9k_1st` do: a Phong highlight whose strength is the diffuse texture's alpha, through a
tangent-space normal map. `textures/on_knif_c.png` (1024 a side; export writes it as `on_knif_0.dds`,
DXT5) and `textures/on_knif_n.png` (512 a side; `on_knif_0n.mdt`) are baked in Cycles from the scene
`on_knif bake`, the same mesh with four procedural materials: the dark coating (blade flats and
spine), the grind (the coating, then bare steel over the last third toward the edge, by the
`edge_t` vertex attribute, 0 at the grind line and 1 at the edge), the guard's and pommel's dark
steel, and the grip's polymer (a fine stipple in the normal map). The colour bake times `0.55 + 0.45 x`
the ambient occlusion bake is halved, so a lit texel shows the colour (the original's `2 x texel x
light`), and the alpha holds the specular mask: each material's specular weight times `0.25 + 1.3 x`
the texel's luminance.

The arms are `on_ar15`'s (`art/on_ar15/SOURCES.md`). No third-party material is used.
