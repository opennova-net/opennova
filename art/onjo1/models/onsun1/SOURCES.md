# `onsun1` sources

`onsun1.blend` is the source of truth. Its scene `onsun1` holds the two models
an environment names for its sun (`docs/env/env-tod-re.md`, "The sun and glare
models a game supplies"):

- **`onsun1`**, the sun (`sun_3di`): one quad 3.94 units across, 0.78 units
  toward the eye, on a part that turns to face the camera (PANM 0x300), drawn
  `FF_ST_AD_LUM` (added, glowing, two-sided). The game puts it 64 units from
  the eye along the sun's direction and writes its brightness into
  `UPL_INTENSITY`, on which the material's colour runs from black to white.
  Its texture `onsun1_0` is the disc (white to about 0.4 of the quad's
  half-width, a sharp limb) and a warm corona, 64 a side, made into the DXT5
  `.dds` of seven levels the game reads (`textures/onsun1_0.png` the source,
  `opennova-3di texture --format dxt5 --alpha opaque`).
- **`onglare1`**, the glare (`glare_3di`): two parts that face the camera and
  grow with `UPL_INTENSITY` (PANM 0x301, a uniform `scalex` track), each a
  2 x 2 grid of quads 5.66 units across showing one quarter texture mirrored
  about the centre: the far part the soft golden glow with faint rays
  (`onglare1_0`, black to (255, 206, 128), scale 10 to 12), the near part the
  bright halo around the disc (`onglare1_1`, (40, 40, 40) to (150, 236, 255),
  scale 1 to 5). Both 256 a side, 24-bit TGAs stored bottom row first, the
  model's Texture files set to TGA so export copies them as they are.

The layout, sizes, shaders, parts, tracks and texture sizes and formats follow
the original's `msun.3di` and `mglare.3di`, looked at with `opennova-3di info`
in a scratch folder and never imported here. The textures are our own,
painted procedurally by a throwaway script (radial falloffs, random rays from
a fixed seed), with no third-party material.
