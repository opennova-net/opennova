# `on_medp` sources

The pouch is our own, made from scratch in `on_medp.blend` (its geometry built by a throwaway
script): a padded olive canvas pouch 15 x 9 x 5.5 cm with a zip round its middle and its puller, a
webbing grab loop on one end, and a first-aid patch on its lid (a white cross on a green square, the
ISO 7010 sign's shape); 284 triangles in one part. The original's `Medp_1st` and `Medp_3RD` were
looked at for size, part layout and clips only.

The one material `on_medp_0` draws with `VS_PHONGT`: `textures/on_medp_c.png` (512 a side;
`on_medp_0.dds`) and `textures/on_medp_n.png` (256 a side; `on_medp_0n.mdt`), baked from the scene
`on_medp bake` with three procedural materials (the canvas, its weave in the normal map; the webbing;
the zip's black teeth), graded as the knife's (`art/on_knif/SOURCES.md`). The patch is drawn into the
canvas material's bake from a mask kept packed in the `.blend` (`_patch.png`, our own: a rounded
square and a cross), laid on the lid in the pouch's own frame.

The arms are `on_ar15`'s (`art/on_ar15/SOURCES.md`). No third-party material is used.
