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

- One record per line, its fields separated by whitespace (space, tab, `\r`,
  `\v`, `\f`). `#` starts a comment at the start of a line or after
  whitespace (retail shader tags such as `VS_PHONGT#UV` hold a `#`). The first
  record is `o3d 1`; a UTF-8 byte order mark ahead of it is ignored.
- A name field (model, shader, texture, register, user point) is a bare token,
  or `"quoted"` when it holds whitespace or is empty (retail ships `"FLARE
  01"`, `"ground "`, an empty CTRL name). A quoted field runs to the next `"`,
  so a name cannot hold `"` or a line break: build refuses one, and `scene`
  writes such a retail name without those characters and says so. Model and
  user point names are at most 15 characters: GHDR and USRP give them a
  16-byte field, and 15 keeps the NUL the loader's C strings end on (no JO
  name is longer than 9).
- Numbers are what `strtod` reads, plus `nan`, `-nan`, `inf` and `-inf`, which
  `scene` writes for the NaNs and infinities retail ships (J_bsh1's vertex
  normals, ChmLFP1's occlusion planes); `nan` builds the quiet NaN retail
  stores (0x7FC00000, with the sign for `-nan`). Integer fields are whole
  numbers in decimal; flag words also take `0x` hex.
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
| `tangents` | 0 or 1 | vertices carry tangent/bitangent. Optional: a material whose shader reads the TANGENT semantic (VS_DOT3DIFF, VS_PHONGT, VS_SKBUMPDIFFT, ...) turns the layout on itself. The values are never carried: the builder derives them from the UVs by the OED rule (below) |
| `skinned` | 0/1 | GHDR mesh type 2 (before the first `lod`): parts are bones, strips carry bone tables, vertices weights |
| `uv1` | 0/1 | every `v` carries a second UV set (the detail stage of FF_MT shaders) after its first (before the first `lod`) |
| `register` | NAME | a CTRL register, declared in order; a name outside `threedi_ctrl_catalog.h` is a note (the loader reads LOD_FRAC for it) |
| `mtrx` | r00 .. r22 | a PANM rotation frame, MTRX row 1, 2, ... (row 0 is the identity build writes itself): a 3x3 rotation in mission axes, row-major, `p' = p R` |
| `material` | SHADER | opens a material (a shader tag of the engine's table, `opennova-3di catalog`; another tag is a note) |
| `texture` | name [slot type flags frame] | a texture on the open material (slot 1 diffuse, 2 detail, 3/4 normal). The name is the MTRL row's 16-byte field, which retail fills to the last byte with no NUL (`bo105blur.dds.tg`): at most 16 bytes of printable ASCII, a file name without a folder. The loader opens the name cut three characters past its first `.` and decodes a `.tga`, `.mdt` or `.pcx` file itself; any other name loads only as the `.dds` of its stem, which `build` notes ([orig: Texture_LoadByNameWithChannel @ 0x58B4E1]) |
| `texanim` | frames type time | the material's texture flipbook |
| `reflect` | r g b a | the glass reflection colour (0..255) |
| `matflags` / `alphatest` / `glass` / `emissive` | value | material flag byte, alpha-test threshold, glass, emissive type |
| `rgbgen` | style reg rate r g b r g b [phase] | RGB generator (styles above 112 read the declared register `reg`; colours 0..255) |
| `alphagen` / `ugen` / `vgen` | style reg rate start end [phase] | alpha / U / V generators |
| `lod` | threshold [type] | opens a render LOD: `threshold` is the projected radius in PIXELS at which it takes over, a whole number the loader stores shifted left 16 in the level's own slot ([orig: ThreediGp_LoadFromFile @ 0x5b5bdf..0x5b5be5]; tables descend to 0, the coarsest: Armry01 200, 60, 20, 0); type `gnrc`, `bldg`, `door`, `veh0`. A LOD may hold no parts |
| `part` | parent x y z [cx cy cz] | opens a part in the LOD: its parent (itself for the root, -1 for none, any part of the LOD) and pivot. A part that draws nothing may give the point its sphere sits on (radius 0): the exporter seeds such a part with one placeholder vertex, in the retail corpus its `_## center` helper's first mesh vertex, near the pivot (1,779 of the 2,411 such JO parts also carry it, on the 8.8 grid, as their section's only collision vertex); without it the sphere is at the origin (658 retail parts) |
| `strip` | material [alpha] | opens a triangle-list strip on the open part |
| `bones` | p0 p1 ... | a skinned strip's bone table (1 to 16 part indices, bytes; before its vertices) |
| `v` | x y z nx ny nz u v [u1 v1] [i0 i1 i2 i3 w0 w1 w2] | a strip vertex (at most 65535 per strip); `u1 v1` with `uv1 1`; skinned: four bone-table slots (bytes) and three weights, each a finite number from 0 to 1, together at most 1 (added in float, as the shader adds them: retail's four-decimal weights reach 1.0001, ArmGlovD, which builds). Slot `i3` takes the rest, 1 - (w0 + w1 + w2), as retail's vertex shader blends the fourth influence ([orig: _BaseInc.fx CalcSkinWorldPosAndNormal]), so a vertex with no weight rides `i3`'s bone wholly |
| `t` | a b c | a strip triangle, three distinct vertices (the loader drops one that repeats a corner) |
| `panm` | part parent [flags [matrix]] | a part-animation row in the open LOD, in part order: row i transforms part i, as all 3,250 JO tables do (the runtime reads the table by row); `flags` (a word, `0x` allowed) replaces the flags the tracks imply, `matrix` selects an `mtrx` frame |
| `track` | target style REG\|-\|param rate start end [axis] | a track on the last `panm`: target `rotx roty rotz scalex scaley scalez trans`. Styles above 0x70 name a declared register; the others may carry an integer phase param. Rotations in 1/16384 turn, others 8.8, all int16; only a `trans` track takes `axis`, 1 (x), 2 (y) or 3 (z; the default) |
| `userpoint` | name x y z dx dy dz part [type] | a USRP point (15 characters; part -1 = none; type 71 G / 83 S) |
| `light` | part x y z atten_start atten_end style rate phase\|reg r g b r g b flags [dx dy dz falloff] | a LGHT light owned by `part`: style and rate (units per second, 0 up to 256: WriteLGHT packs it times 256 into a u16, truncated) of its colour generator, `phase` (styles up to 0x70) or a declared register index (above), start and end colours 0..255, the flag byte (1 no corona, 2 no terrain light, 4 no object light, 8 spot). An omni light omits the axis: it keeps retail's default (straight down, no cone); a spot light gives its axis, the direction the light points in mission axes, and its cone half-angle in degrees (0 up to 256: its byte) |
| `occ` | type section connecting | opens an occlusion record: type 0 occluder, 1 open, 2 window, 3 portal, 4 (OH); `section` its parent section, `connecting` the section a window or portal leads to |
| `ov` | x y z | a vertex of the open `occ` (at most 128: 7-bit edge words) |
| `op` | nx ny nz d | a plane of the open `occ` (at most 32: the runtime's occlusion clip mask is a 32-bit word per record); without any, the OED rule picks them (below) |
| `of` | a b c [plane] | a face of the open `occ`; its plane index with explicit `op` planes, none without |
| `cobj` | parent [ox oy oz] | opens collision section i (pairs with part i of the collision LOD: one section per part, as WriteCOBJ walks it): its parent part and offset (the part pivot, as retail stores it) |
| `cv` | x y z | a collision vertex (\|x\|, \|y\|, \|z\| under 128: CVRT stores 8.8 in an int16), at most 32,768 per section: retail reads a bullet face's corners as signed 16-bit indices (docs/threedi/3di-gp-format-re.md §2.11) |
| `cf` | a b c [poly_type [flags [nx ny nz]]] | a bullet face (poly_type = impact material, a byte; the effect row is material + 4; flags a 32-bit word, decimal or `0x`: 1 both sides, 0x100 bullets pass, 0x800 front only: without flag 1 a projectile stops at the face only when it crosses it from the front (`engine/runtime/world/collision_query.cpp`, [orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0, the test @ 0x4e5139]; no JO face carries 0x800). The normal comes from the given (unquantized) corners; an explicit one is stored as given, for a face the 8.8 grid collapses or whose normal disagrees with its winding (`scene` writes retail's for both). The plane distance is `-(n . v0)` |
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
([orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe)]; it reproduces Armry01's OCCL records).

The OED volume rule, for a `cvmesh`, is the same plane rule over its triangles
(a triangle whose edge cross product is at most 0.0001 long takes plane 0),
with no plane limit: the retired port stopped a table at 32 planes, but the
JO corpus ships volumes of up to 61, which a capped table could not have
built, so build deliberately keeps every plane. The volume is the
solid all those planes bound, so a mesh must be convex: `build` notes one
whose vertices reach more than a centimetre outside it. A ladder (type 4)
swaps plane 0 with the plane its last triangle took: the runtime reads plane
0 as the ladder's facing. Seam flags: in section then volume order, each
triangle clears its plane's flag, then sets it when the triangle's box shrunk
by 0.01 lies inside another type-1 volume's box in any section; the last
triangle on a plane decides ([orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe)]).

The derived collision values follow OED's writer (5fc5b4f6a^
`engine/formats/oed/export_3di.cpp`), truncated as it truncates them: CVRT on
the 8.8 grid; CNRM Q14 with the dominant axis chosen on those integers (z,
then y, only when strictly largest); a section's offset (the part pivot);
its bounds over its points and, on a rigid model, its volume boxes and the
occlusion records it parents (OED keeps them in the same list: Armry01's
window sets section 1's min z), its midpoint the floor of the bounds' mean
(all 5,046 odd-sum axes of the JO corpus round down), its radius the farthest
point from that midpoint (a volume-only section's is 0). A section's points
are its source mesh: on a rigid model the render floats of its part in the
collision LOD (which the file does not name: the first LOD whose parts match
the sections in triangle and bullet-face counts, when the part's vertices on
the 8.8 grid are exactly the section's corners), else its stored corners; on
a skinned model with collision geometry, every LOD 0 vertex a weight binds to
it (a mesh part's section keeps the empty sentinels, US01 19 and ArmsG 37),
while a bone section is its `csphere` or the sentinels. The
CMDL box over every bullet face and LOD 0's triangles, its radii and height
(`radii[2]`) over the bullet faces alone (retail CNet01, with none, stores 0,
0 and -20000; OED's port folded LOD 0 into the radii too, which the corpus
does not); a bullet face's plane distance and box. The CMDL and the face
words come from the stored corners and normals, our rule: retail took them
from the authored corners (97.7% of the JO CFAC box words lie off the 8.8
grid the stored corners sit on), which the file keeps only for a rigid
section's own part, so deriving from what is stored lets `build(scene(x))`
re-mint a built model byte for byte.

The render words: GHDR's radius is the farthest render vertex from the origin,
truncated (932 of the 958 JO models; rounding gives 486); a part's sphere is
its vertex box's centre and the farthest vertex from it (5,239 of 5,933 rigid
parts and 244 of 256 attributable skinned ones; the box's half-diagonal gives
704 and none), over the vertices the part's triangles use (a strip `scene`
writes from a shared window carries only those: below); a part's `rel` is its
pivot less its parent's in float, the root's (-0, 0, 0) and a parentless
part's its own pivot (40,863 of 40,935 words); user points and section offsets truncate to 16.16. Tangents: each
triangle's dP/du and dP/dv from its UVs, summed over the triangles sharing a
vertex of the same part, position and normal, normalized (a triangle with
degenerate UVs reuses the previous one's).

## Validation

The build fails, naming the line, on an unknown record, a field that does not
parse (an optional one included: `cf 0 1 2 abc`, `lod abc`), a value its word
cannot hold (a byte field over 255, an int16 field, a negative light rate), a
token past the record's fields, an unclosed quote or a `"` inside a name, an
index outside its strip, collision object or occlusion record, a triangle that
repeats a vertex, a strip over 65535 vertices or indices, a texture name over
16 bytes, outside printable ASCII or naming a folder, a skinned weight that is
no finite number from 0 to 1 or weights summing past 1 (within 1e-4), more
than 255 parts, a part parent the LOD lacks, a PANM row out of part order, a
skinned strip without a bone table or naming a missing part, a track `axis`
other than 1..3 or on a track other than `trans`, a generator or light
register that is not declared, more than 8 `sitex` seats in any case (the seat
scan reads the prefix without case and stops at 8 [orig:
Entity_GetBoneSlotType @ 0x434ED0; the scan end @ 0x43A5AF]), a volume with
fewer than 4 planes, an occlusion record over 128 vertices or 32 planes, a
collision vertex 128 or more from the origin, a collision section over 32,768
vertices or 32,768 distinct bullet-face normals (a face names its normal by a
signed 16-bit index too), or a model the writer refuses (named with its chunk
and size when a chunk outgrows the 16,777,215 bytes a 3DI3 chunk's 24-bit
length says: ROOT holds the whole model, an RLOD one LOD); a whole-model error
names the file alone. Notes (not errors): a register outside the catalog, a
shader outside the engine's table, a texture that loads only as the `.dds` of
its stem, a volume mesh that is not convex, more than 16 user points (the
item-effect scan reads 16), weighted bone slots past their strip's table
(retail FSldr03 ships them), collision faces whose corners are collinear. The
minted bytes are read back before the file is written. The ctest fixtures are
`fixtures/threedi/o3d/spinner.o3d`, `skinned.o3d` and `building.o3d`.

A skinned model's strips are all owned by the root ROBJ while each part keeps
the bounds of the geometry authored on it (the retail layout; the builder
applies it). `scene` writes each strip back on the tightest part whose sphere
holds all its vertices, among the skinned mesh parts (the parts no bone table
names that carry bounds: FSldr03 part 19, ArmsG part 37), or, for a model
authored on its bones (dM1A1's hull), among every part that carries bounds.

Retail's exporter lets strips share one vertex window, across parts too (70
of the 958 JO models: Dblkhwk1's rotor strips of parts 3 and 4, Armry01's part
3 strip over the windows of parts 1 and 2). `scene` writes a strip whose window
another strip overlaps with only the vertices its own triangles use,
renumbered: the others belong to the strips it shares with, and the part
sphere retail stores spans the part's own triangles (Armry01's part 3 and
Dblkhwk1's part 4 match it exactly that way, and no sphere over the whole
window). A strip with a window of its own is written whole: a vertex no
triangle uses is the author's, and build writes it again.

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
lacks; light pad bytes; occlusion `slot_priority_scale` (the portal-slot priority weight). Values build derives are not
carried: part `rel`, bounds and spheres (but a part that draws nothing keeps
its centre), section bounds, CMDL, face normal runs and plane distances,
tangents.

Over the 2,413 `.3di` of the JOTAC archives (2026-09-27, test and scratch
files included), `scene` reads 2,409, and `build` takes 2,396 of those scenes;
the other thirteen carry what build refuses: Pinegr_L's 40,824-vertex
collision section, PANM rows out of part order (btr70, mk_1st), a part whose
parent its LOD lacks (mwr_1st, panc_1st), an occlusion record over 32 planes
(dmrk51), a track or generator naming no register (Dt801x, kat13), more than
8 seats (StrgateE), and faces naming vertices outside their section (mLpost02
to 05). For all 2,396, `build(scene(x))` is byte for byte
`build(scene(build(scene(x))))`, and `opennova-3di compare` calls 1,883 of
them the same model as `x` (1,859 with drift notes; a part sphere the rule
above does not give over a part's own vertices, 525 models, is drift: nothing
reads it beside the GHDR radius). The rest differ in words retail derived from
what the file does not keep: occlusion record centres (205 models, each stored
centre the mirror across y of its vertices' centre), section bounds (194),
part `rel` words (101, NaN ones included), GHDR radii over geometry the file
does not carry (56, the 30 of the `fxflsh` family among them), CMDL radii
(35), bullet faces (CB2048, carrier, kat04, mere), and six models that draw
with a material id they lack or name no LOD type.
