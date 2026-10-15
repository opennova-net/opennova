# `onbino_3` sources

`onbino_3.blend` is the source of truth. Its scene `onbino_3` is the game model: the binoculars the
`Binoculars` item (`items.def` 101424, the engine's fixed id: a person's binoculars draw with this
item's model, in the left hand, `engine/runtime/world/person_overlays.h` draw 4) points at. A pair of
rubber-armoured roof-prism binoculars, the objectives forward, the eyecups and the focus wheel on the
bridge; two LODs (704 / 278 triangles, thresholds 102 and 0, as the original's `Bino_3rd`) of one
part, LOD 1 without the strap lugs and the focus wheel, collapsed so it keeps LOD 0's UVs, and LOD 1's
triangles as the bullet faces. It is laid out in `Bino_3rd`'s frame, since the overlay's placement is a
fixed turn and nudge off the left hand bone tuned to it: the tubes along the model's forward, the pair
from the origin to 0.21 m on the model's right, centred on its height (bounds within 1 cm of
`Bino_3rd`'s).

The scene `onbino_3 bake` holds LOD 0 with four procedural bake materials (the olive rubber armour, a
fine pebble in the normal map; the coated lenses, deep violet; the dark metal hinge; the black eyecups
and focus wheel, knurled). `textures/onbino_3_c.png` (256 a side; `onbino_3_0.dds`) is the colour bake
times the ambient occlusion bake, halved for the original's lighting, with the specular mask in its
alpha (the lenses full); `textures/onbino_3_n.png` (256; `onbino_3_0n.mdt`) the tangent normal bake.
It draws with `VS_PHONGT`, as our first-person gear does (the original's `Bino_3rd` uses
`VS_DOT3DIFF`, an effect the base game does not ship).

The model and its layout are our own; the original's `Bino_3rd` was looked at for its size, frame,
LODs, collision and shader only. No third-party material is used.
