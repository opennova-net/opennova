# The `.o3d` scene text

`opennova-3di build <scene.o3d> -o <out.3di>` mints a `.3di` from this text
through the engine's construction API (`engine/formats/threedi/threedi_build.h`)
and the parity writer ([ADR 0047](../adr/0047-blender-3di-exporter.md)).
`opennova-3di scene <model.3di> -o <scene.o3d>` writes any `.3di`, retail ones
included, back out as this text: it is build's exact inverse, so
`build(scene(x))` re-mints a builder-made model byte for byte. The Blender
add-on (`tools/blender/opennova_3di`) writes it to export and reads it to
import; any other front end may.

## Conventions

- One record per line, whitespace-separated. `#` starts a comment at the start
  of a line or after whitespace (retail shader tags such as `VS_PHONGT#UV`
  hold a `#`). The first record is `o3d 1`.
- A name field (model, shader, texture, register, user point) is a bare token,
  or `"quoted"` when it holds spaces or is empty (retail ships `"FLARE 01"`,
  `"ground "`, an empty CTRL name). Numbers read `nan` and `inf` (retail ships
  them in occlusion planes and light matrices).
- Positions, normals and directions are **mission axes**: x forward, y left,
  z up, metres (game units). The CLI converts to model axes `(-y, z, x)`.
- Render triangles (`t`) wind **counter-clockwise about the outward normal**
  in mission axes; the CLI flips them to retail's order (retail winds
  counter-clockwise in model axes, the mirror of mission).
- Collision faces (`cf`) and occlusion faces (`of`) wind counter-clockwise
  about their outward normal in mission axes, **the order retail stores**
  (Dtruck2: 905 of 906 bullet faces, Armry01: all 250; Armry01's OCCL faces),
  so the CLI keeps them as given.
- UVs are D3D: `v` runs down (origin top-left).
- Indices are 0-based and local to the enclosing strip, collision object or
  occlusion record.

## Records

| Record | Fields | Meaning |
| --- | --- | --- |
| `model` | name | GHDR name, at most 15 characters |
| `tangents` | 0/1 | vertices carry tangent/bitangent. Optional: a material whose shader reads the TANGENT semantic (VS_DOT3DIFF, VS_PHONGT, VS_SKBUMPDIFFT, ...) turns the layout on itself. The values are never carried: the builder derives them from the UVs by the OED rule (below) |
| `skinned` | 0/1 | GHDR mesh type 2 (before the first `lod`): parts are bones, strips carry bone tables, vertices weights |
| `uv1` | 0/1 | every `v` carries a second UV set (the detail stage of FF_MT shaders) after its first (before the first `lod`) |
| `register` | NAME | a CTRL register, declared in order; a name outside `threedi_ctrl_catalog.h` is a note (the loader reads LOD_FRAC for it) |
| `mtrx` | r00 .. r22 | a PANM rotation frame, MTRX row 1, 2, ... (row 0 is the identity build writes itself): a 3x3 rotation in mission axes, row-major, `p' = p R` |
| `material` | SHADER | opens a material (a shader tag of the engine's table, `opennova-3di catalog`; another tag is a note) |
| `texture` | name [slot type flags frame] | a texture on the open material (16 characters max; slot 1 diffuse, 2 detail, 3/4 normal) |
| `texanim` | frames type time | the material's texture flipbook |
| `reflect` | r g b a | the glass reflection colour (0..255) |
| `matflags` / `alphatest` / `glass` / `emissive` | value | material flag byte, alpha-test threshold, glass, emissive type |
| `rgbgen` | style reg rate r g b r g b [phase] | RGB generator (styles above 112 read the declared register `reg`; colours 0..255) |
| `alphagen` / `ugen` / `vgen` | style reg rate start end [phase] | alpha / U / V generators |
| `lod` | threshold [type] | opens a render LOD (projected-radius threshold, 0 = coarsest; type `gnrc`, `bldg`, `door`, `veh0`). A LOD may hold no parts |
| `part` | parent x y z | opens a part in the LOD: its parent (itself for the root, -1 for none, any part of the LOD) and pivot |
| `strip` | material [alpha] | opens a triangle-list strip on the open part |
| `bones` | p0 p1 ... | a skinned strip's bone table (1 to 16 part indices; before its vertices) |
| `v` | x y z nx ny nz u v [u1 v1] [i0 i1 i2 w0 w1 w2] | a strip vertex (at most 65535 per strip); `u1 v1` with `uv1 1`; skinned: three bone-table slots and weights |
| `t` | a b c | a strip triangle, three distinct vertices (the loader drops one that repeats a corner) |
| `panm` | part parent [flags [matrix]] | a part-animation row in the open LOD; `flags` (a word, `0x` allowed) replaces the flags the tracks imply, `matrix` selects an `mtrx` frame |
| `track` | target style REG\|-\|param rate start end [axis] | a track on the last `panm`: target `rotx roty rotz scalex scaley scalez trans`. Styles above 0x70 name a declared register; the others may carry an integer phase param. Rotations in 1/16384 turn, others 8.8, all int16; `axis` 1/2/3 for `trans` |
| `userpoint` | name x y z dx dy dz part [type] | a USRP point (15 characters; part -1 = none; type 71 G / 83 S) |
| `light` | part x y z atten_start atten_end style rate phase\|reg r g b r g b flags [dx dy dz falloff] | a LGHT light owned by `part`: style and rate (units per second) of its colour generator, `phase` (styles up to 0x70) or a declared register index (above), start and end colours 0..255, the flag byte (1 no corona, 2 no terrain light, 4 no object light, 8 spot). An omni light omits the axis: it keeps retail's default (straight down, no cone); a spot light gives its local +Z axis and cone half-angle in degrees |
| `occ` | type section connecting | opens an occlusion record: type 0 occluder, 1 open, 2 window, 3 portal, 4 (OH); `section` its parent section, `connecting` the section a window or portal leads to |
| `ov` | x y z | a vertex of the open `occ` (at most 128: 7-bit edge words) |
| `op` | nx ny nz d | a plane of the open `occ`; without any, the OED rule picks them (below) |
| `of` | a b c [plane] | a face of the open `occ`; its plane index with explicit `op` planes, none without |
| `cobj` | parent [ox oy oz] | opens collision section i (pairs with part i of the collision LOD: one section per part, as WriteCOBJ walks it): its parent part and offset (the part pivot, as retail stores it) |
| `cv` | x y z | a collision vertex |
| `cf` | a b c [poly_type flags [nx ny nz]] | a bullet face (poly_type = impact material; the effect row is material + 4; flags 1 both sides, 0x100 bullets pass, 0x800 hit from behind). The normal comes from the given (unquantized) corners; an explicit one is stored as given, for a face the 8.8 grid collapses or whose normal disagrees with its winding (`scene` writes retail's for both). The plane distance is `-(n . v0)` |
| `csphere` | cx cy cz r [minx miny minz maxx maxy maxz] | the open section's hit sphere (a skinned model's bone sections) and the bounds of the vertices the bone moves (without them, the sphere's cube) |
| `cvol` | type flags minx miny minz maxx maxy maxz | an axis-box volume (six planes) |
| `cvolume` | type flags minx miny minz maxx maxy maxz | a convex volume whose planes follow as `cp` lines |
| `cp` | nx ny nz d [flags] | a plane of the open `cvolume`: outward normal, `n . p + d == 0` on it, the seam flag |
| `cvmesh` | type flags [label] | a volume authored as triangles (the exporter's form): `vv` vertices and `vf` faces follow, and the builder derives its box, planes and seam flags by the OED rule (below). `label` names it in notes |
| `vv` | x y z | a vertex of the open `cvmesh` |
| `vf` | a b c | a triangle of the open `cvmesh`, counter-clockwise about its outward normal |
| `cxlt` | [x y z] | a CXLT row (mission axes, the frame of the `cobj` offsets), in order; `scene` writes them after the collision records. The table is WriteCXLT's, the collision LOD's attach points, truncated to 16.16 (retail rows are the author's helpers, not the section offsets: Oiltnk2X's 15 sit near the origin while its sections reach 24 m out). A bare `cxlt` declares an empty table. Without any `cxlt`, build derives one row per non-root section (every section on a skinned model) at its offset: our rule, the retail row count in 917 of the 958 JO models |
| `texfile` | name path\|- | written by `scene`: the file a texture name resolves to beside the model, by the runtime's candidate order (`engine/base/resource_index/texture_candidates.h`); `build` ignores it |

Volume `type` is the collidable type (1 `CB` solid, 4 `CL` ladder, 7 `VC`
vehicle contact, 19 `CP` player-only, ...; docs/world/world-wac-ai-re.md §15).

The OED occlusion plane rule, used when an `occ` record gives no `op`: the
record's six bounding-box planes first (+x -x +y -y +z -z), then each face's
own plane unless one already matches it (normal within 0.005 per axis,
distance within 0.03; the last match wins), at most 32
([orig: ConvertToInternal @ 0x4268B3]; it reproduces Armry01's OCCL records).

The OED volume rule, for a `cvmesh`, is the same plane rule over its triangles
(a triangle whose edge cross product is at most 0.0001 long takes plane 0),
with no plane limit (retail ships volumes of up to 61). The volume is the
solid all those planes bound, so a mesh must be convex: `build` notes one
whose vertices reach more than a centimetre outside it. A ladder (type 4)
swaps plane 0 with the plane its last triangle took: the runtime reads plane
0 as the ladder's facing. Seam flags: in section then volume order, each
triangle clears its plane's flag, then sets it when the triangle's box shrunk
by 0.01 lies inside another type-1 volume's box in any section; the last
triangle on a plane decides ([orig: ConvertToInternal @ 0x4268B3]).

The derived collision values follow OED's writer (5fc5b4f6a^
`engine/formats/oed/export_3di.cpp`), truncated as it truncates them: CVRT on
the 8.8 grid; CNRM Q14 with the dominant axis chosen on those integers (z,
then y, only when strictly largest); a section's offset (the part pivot) and
bounds over its vertices and volume boxes, its midpoint the floor of the
bounds' mean (all 5,046 odd-sum axes of the JO corpus round down), its radius
the farthest vertex from that midpoint (a volume-only section's is 0); the
CMDL box over every bullet face and LOD 0's triangles, its radii and height
(`radii[2]`) over the bullet faces alone (retail CNet01, with none, stores 0,
0 and -20000; OED's port folded LOD 0 into the radii too, which the corpus
does not); a bullet face's plane distance and box. Those last words come from
the stored corners and normals, our rule: retail took them from the authored
corners (97.7% of the JO CFAC box words lie off the 8.8 grid the stored
corners sit on), which the file does not keep, so no scene could carry them,
and deriving from what is stored lets `build(scene(x))` re-mint a built model
byte for byte.

The render words: GHDR's radius is the farthest render vertex from the origin,
truncated (932 of the 958 JO models; rounding gives 486); a part's sphere is
its vertex box's centre and the farthest vertex from it (5,168 of 5,932 rigid
parts and 244 of 256 attributable skinned ones; the box's half-diagonal gives
704 and none); a part's `rel` is its pivot less its parent's in float, the
root's (-0, 0, 0) and a parentless part's its own pivot (40,863 of 40,935
words); user points and section offsets truncate to 16.16. Tangents: each
triangle's dP/du and dP/dv from its UVs, summed over the triangles sharing a
vertex of the same part, position and normal, normalized (a triangle with
degenerate UVs reuses the previous one's).

## Validation

The build fails, naming the line, on an unknown record, a malformed field, an
index outside its strip, collision object or occlusion record, a strip over
65535 vertices or indices, more than 255 parts, a part parent the LOD lacks, a
skinned strip without a bone table or naming a missing part, a track value
outside int16, a generator or light register that is not declared, more than 8
`sitex` seats, a volume with fewer than 4 planes, an occlusion record over 128
vertices or 32 planes, or a model the writer refuses. Notes (not errors): a
register outside the catalog, a shader outside the engine's table, a volume
mesh that is not convex, more than 16 user points (the item-effect scan
reads 16), weighted bone slots past their strip's table (retail FSldr03 ships
them), collision faces whose corners are collinear. The minted bytes are read
back before the file is written. The ctest fixtures are
`tests/fixtures/threedi/o3d/spinner.o3d`, `skinned.o3d` and `building.o3d`.

A skinned model's strips are all owned by the root ROBJ while each part keeps
the bounds of the geometry authored on it (the retail layout; the builder
applies it). `scene` writes each strip back on the tightest part whose sphere
holds all its vertices, among the skinned mesh parts (the parts no bone table
names that carry bounds: FSldr03 part 19, ArmsG part 37), or, for a model
authored on its bones (dM1A1's hull), among every part that carries bounds.

A spot light's cone is written as the float half-angle that gives back the
stored byte, cosine and `view_proj` (a small cone leaves thousands of floats
with one cosine); `acos` of the cosine alone does not.

## What `scene` cannot carry

`scene` comments these (`# dropped: ...`) and lists them on stderr: tangent and
bitangent values (the builder derives them again); a non-identity MTRX row 0 or non-finite rows (skinned
models' NaN rows, written as the identity); MTRX translations; PANM
`matrix_offset` and `bind_matrix_index`; the second-channel material fields
(`rgb_gen2`, `emissive_type2`, `glass_type2`, `reflect_color2`, always zero in
the corpus) and generator alpha bytes; a strip naming a material id the model
lacks; light pad bytes; occlusion `glow_scale`. Values build derives are not
carried: part `rel`, bounds, CMDL, face normal runs and plane
distances, tangents. Over the 958 JO models, `build(scene(x))` is the same model as `x`
(`opennova-3di compare`) for 956; the other two draw with a material id they
lack.
