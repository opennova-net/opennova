# The `.o3d` scene text

`opennova-3di build <scene.o3d> -o <out.3di>` mints a `.3di` from this text
through the engine's construction API (`engine/formats/threedi/threedi_build.h`)
and the parity writer ([ADR 0047](../adr/0047-blender-3di-exporter.md)). The
Blender add-on (`tools/blender/opennova_3di`) writes it; any other exporter may.

## Conventions

- One record per line, whitespace-separated; `#` starts a comment. The first
  record is `o3d 1`.
- Positions, normals and directions are **mission axes**: x forward, y left,
  z up, metres (game units). The CLI converts to model axes `(-y, z, x)`.
- Triangles (`t`) and collision faces (`cf`) wind **counter-clockwise about the
  outward normal** in mission axes. The CLI flips to retail's order (model
  axes mirror mission axes).
- UVs are D3D: `v` runs down (origin top-left).
- Indices are 0-based and local to the enclosing strip / collision object.

## Records

| Record | Fields | Meaning |
| --- | --- | --- |
| `model` | name | GHDR name, at most 15 characters |
| `tangents` | 0/1 | vertices carry tangent/bitangent (tangent-reading shaders) |
| `register` | NAME | a CTRL register; must be in `threedi_ctrl_catalog.h` |
| `material` | SHADER | opens a material (shader tag, e.g. `FF_ST_OP`, `FFP_GLASS`) |
| `texture` | name [slot type flags] | a texture on the open material (16 characters max; slot 1 diffuse) |
| `matflags` / `alphatest` / `glass` / `emissive` | value | material flag byte, alpha-test threshold, glass, emissive type |
| `rgbgen` | style reg rate r g b r g b | RGB generator (styles > 112 read CTRL index `reg`; colours 0..255) |
| `alphagen` / `ugen` / `vgen` | style reg rate start end [phase] | alpha / U / V generators |
| `lod` | threshold [type] | opens a render LOD (projected-radius threshold; 0 = coarsest) |
| `part` | parent x y z | opens a part in the LOD; parent is an earlier part or itself (root); the pivot |
| `strip` | material [alpha] | opens a triangle-list strip on the open part |
| `v` | x y z nx ny nz u v | a strip vertex (at most 65535 per strip) |
| `t` | a b c | a strip triangle |
| `panm` | part parent | a part-animation row in the open LOD |
| `track` | target style REG\|- rate start end [axis] | a track on the last `panm`: target `rotx roty rotz scalex scaley scalez trans`; rotations in 1/16384 turn, others 8.8; `axis` 1/2/3 for `trans` |
| `userpoint` | name x y z dx dy dz part [type] | a USRP point (15 characters; part -1 = none; type 71 G / 83 S) |
| `cobj` | parent [ox oy oz] | opens collision section i (pairs with LOD0 part i) |
| `cv` | x y z | a collision vertex |
| `cf` | a b c [poly_type flags] | a bullet face (poly_type = impact material; the effect row is material + 4) |
| `cvol` | type flags minx miny minz maxx maxy maxz | an axis-box volume (six planes) |
| `cvolume` | type flags minx miny minz maxx maxy maxz | a convex volume whose planes follow as `cp` lines |
| `cp` | nx ny nz d [flags] | a plane of the open `cvolume`: outward normal, `n . p + d == 0` on it; retail flags seams 1 |

Volume `type` is the collidable type (1 `CB` solid, 4 `CL` ladder, 7 `VC`
vehicle contact, 19 `CP` player-only, ...; docs/world/world-wac-ai-re.md §15).

## Validation

The build fails, naming the line, on an unknown record or register, a malformed
field, an index outside its strip or collision object, a strip over 65535
vertices or indices, more than 255 parts, more than 8 `sitex` seats, a volume
with fewer than 4 planes, or a model the writer refuses. The minted bytes are
read back before the file is written. The ctest fixture is
`tests/fixtures/threedi/o3d/spinner.o3d`.
