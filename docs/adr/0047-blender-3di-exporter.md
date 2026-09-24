# ADR 0047: A Blender .3di add-on over the engine's reader and writer

- **Status**: accepted (2026-09-23)
- **Owners**: the 3DI format library (`engine/formats/threedi`), `apps/threedi_cli`,
  `tools/blender/opennova_3di`
- **Supersedes/updates**: replaces ADR 0038 (deleted with this record). Its standing
  decisions are restated below; its ban on a Blender add-on, on Blender custom
  properties and on an import route is lifted for this add-on. [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)
  and [ADR 0027](0027-3di3-first-class.md) stand unchanged and are what this record
  builds on.

## Context

ADR 0038 (2026-08-26) retired every object-authoring route: the Qt importer,
the Python FFI packages, the Blender add-ons and the native ASE/3DP/OED
pipeline. It left one path to a `.3di`, a hand-written C++ recipe through the
parity writer, and reserved a future GLB/GLTF editor seam. Modders and our own
content work need to author a model (a vehicle, a prop, a building) in a DCC
and ship it to retail, and to open a retail model to learn its layout or build
on it; the retired pipeline's lesson stands: a second encoder, or a second
language's copy of the format, drifts.

## Decision

1. **One encoder, one decoder.** `engine/formats/threedi/threedi_build.{h,cpp}`
   (the construction API: a `ThreediBuildModel` assembled into the contiguous
   `Threedi3di3` the parity writer serializes, `threedi_3di3_write_memory`) is
   the only code that builds a model; `threedi_3di3_read` is the only code that
   reads one. The frame conversions (mission <-> model axes) and every
   quantization live there.
2. **`opennova-3di`** (`apps/threedi_cli`, one translation unit per command)
   speaks the `.o3d` scene text
   ([`docs/threedi/o3d-scene-format.md`](../threedi/o3d-scene-format.md)).
   `build` reads it into the construction API, validates what retail imposes
   (u16 indices, name lengths, int16 tracks, the 8-seat scan, declared
   registers, volume plane counts, occlusion vertex and plane limits), mints the
   model and reads the bytes back before writing them. `scene` is build's exact
   inverse (`build(scene(x))` re-mints a builder-made model byte for byte) and
   names the file each texture resolves to beside the model, by the runtime's
   own candidate order (`engine/base/resource_index/texture_candidates.h`,
   shared with the Godot resolver). `info` prints what any `.3di` holds,
   `compare` tells whether two files hold the same model, and `catalog` prints
   the engine's CTRL register and generator-style tables. The CLI owns both
   winding conversions: render triangles flip (retail winds counter-clockwise
   in model axes, the mirror of mission); collision and occlusion faces keep
   the scene's counter-clockwise-about-the-normal order, which is retail's
   (Dtruck2 905 of 906 bullet faces, Armry01 all of them and its OCCL faces).
3. **The Blender add-on** (`tools/blender/opennova_3di`, a Blender 4.2+
   extension) reads the scene by the NovaLogic ASE/OED object-naming convention
   (`classify_name`, [orig: ConvertToInternal @ 0x4268B3], as the retired
   importer/exporter used it: `_lod_index` LOD roots, `PN##`, `## Mesh<n>`,
   `_## center`, `~PPx attach`, `UP<c>## <label>`, `LP##` lights,
   `<code>##[a..]-colonly`, `OB/OS/OP/OH##[-MM]-occonly`,
   `Material_<i>_<SHADER>`, a `BN##` armature for a skinned model; the table in
   `docs/threedi/scene-naming-contract.md`) and writes `.o3d`. Import runs
   `opennova-3di scene` and lays the `.o3d` out by the same convention, one
   Blender scene per model, textures from the files `scene` resolved; an
   imported model exports again. Blender's `.001` duplicate suffixes are
   stripped before classification; two objects with one identity inside a LOD
   are an error. What a name cannot carry (LOD thresholds and types, PANM
   tracks, material flags, textures and generators, light generators, export
   order, the bullet-face surface and flags, the collision LOD) is a visible
   add-on property; a rotated `PN##` is a PANM rotation frame (an MTRX row).
   The material name carries the shader tag, any of the engine's shader table
   (`opennova-3di catalog`); the add-on's shader field edits the name, and a
   name without one takes OED's default for its texture count. Export never
   relies on anything import set up: every value the engine derives (volume
   planes, seam flags, tangents, section and model bounds, glass and emissive,
   the alpha pass) is recomputed from the authored scene on every export by the
   rules below. Import never stashes source data to make a round trip
   reproduce it: what the scene form cannot express is reported and dropped.
   The add-on writes 32-bit uncompressed TGA textures itself (an industry
   format, not a 3DI concern).
4. **Collision follows the OED rules** (ModSuperOed, as the retired
   `engine/formats/oed` port carried them, 5fc5b4f6a^). A volume is the solid
   its authored faces bound [orig: ConvertToInternal @ 0x4268B3]: its vertex
   box's six planes, then each triangle's plane unless one matches it (0.005
   per normal axis, 0.03 distance, the last match wins); a ladder (`CL`) faces
   the plane of its last triangle (plane 0 after OED's swap). A volume mesh
   must therefore be convex; `build` names any whose vertices lie more than a
   centimetre outside that solid. Seam flags follow OED's overlap rule: a
   triangle whose box, shrunk by 0.01, lies inside another solid (`CB`)
   volume's box flags its plane. The bullet faces are a render LOD's meshes,
   chosen by the OED `.3dp` `poly_collision_lod` setting (default 0, the most
   detailed), never a separate LOD, wound as retail stores them, with normals
   taken from the unquantized corners (retail keeps faces the 8.8 grid
   collapses) and plane distances `-(n . v0)`, the plane the runtime tests.
   There is one section per part of that LOD (WriteCOBJ walks its
   subobjects), at the part's pivot, the COBJ offset and CXLT translation
   retail carries (Dtruck2's wheels, Dblkhwk1's rotors); its bounds cover its
   vertices and volumes and its radius the farthest vertex (a volume-only
   section's is 0). The CMDL box envelops the collision faces and LOD 0's
   triangles, its radii and height (`radii[2]`) the collision faces' alone,
   as the retail corpus stores them. A skinned model follows the retail
   person layout: one section per bone at its pivot, bounded by every LOD 0
   vertex the bone moves, the bullet faces on the mesh part's section. The
   derived collision values are truncated as OED's writer truncated them,
   from the stored (quantized) positions so `build(scene(x))` stays exact.
5. **Skinned models follow the retail corpus.** The parts are the armature's
   bones (hierarchy and pivots, which retail animations pair with by index),
   then one part per skinned mesh (`01 Mesh<n>`; parent 0, pivot = the mesh
   object's origin) as the retail exporter wrote bones then mesh objects
   (FSldr03: 19 bones + part 19; ArmsG: 37 bones + part 37);
   vertices store the bind pose with up to three weights; strips split so no
   bone table exceeds 16 parts; and every strip is owned by the root ROBJ while
   each part keeps the bounds of its own geometry (all 30 surveyed JO
   mesh_type-2 models, e.g. FSldr03). The builder applies that layout.
6. **Lights and occlusion follow the retired OED exporter.** A light is `LP##`
   (`##` the owning part); an omni light keeps retail's default axis (straight
   down, no cone) and a spot light's axis and cone come from the light object,
   with `view_proj` built as the retired exporter built it (it reproduces
   Armry01's omni-light records to within one ulp in two entries, the NaN
   columns included). An occlusion mesh's planes
   follow the OED rule (the six bounding planes, then deduplicated face planes,
   at most 32), which reproduces Armry01's OCCL records plane for plane.
7. **Materials and vertices follow the OED rules too.** A blending shader's
   strips draw in the alpha pass (FFP_GLASS among them); a glass shader
   reflects 128 grey unless another colour is set and is glass; a `*_LUM`
   shader is emissive 2 (every material of the 958 JO models agrees); a
   shader reading the TANGENT semantic lays out tangents, which the builder
   derives from the UVs (OED's per-face basis, summed over the triangles
   sharing a vertex).
8. **Distribution.** `scripts/package_blender_addon.sh` builds the CLI and zips
   it into the add-on's `bin/`. The add-on is the repository's one Python
   product module; it links nothing native and ships no FFI, and it keeps no
   copy of engine tables (registers, style names and shader tags with their
   capability words come from `catalog`).
9. **Standing from ADR 0038.** No other Python product code, Qt importer, or
   Python test suite (the stdlib `scripts/lint`, `scripts/ida`, `scripts/net`,
   `scripts/mcp`, `scripts/ci` and `tools/net` scripts remain); no native
   ASE/TDP/OED modules; ADM and BAD stay read-only runtime formats; Godot
   `ObjectData` loads immutable 3DI documents. A GLB/GLTF <-> 3DI editor seam
   remains future work and uses the same naming contract.

## Consequences

- A modder installs one zip; a model reaches retail through the same writer
  every fixture is minted by (ADR 0003), and a retail model opens in Blender
  through the same reader the runtime uses.
- The CLI is a second front end for any DCC: another exporter writes `.o3d`,
  another importer reads what `scene` writes.
- Known gaps, each reported rather than carried: retail's own tool is not
  witnessed, so its seam flags and tangent values match the OED rules only
  where that tool agreed with ModSuperOed; CTRL registers nothing references;
  non-`BB` volume flags (Armry02's `CB` volumes with flag 1); zero-length
  vertex normals (Blender cannot hold them); a retail volume plane that bounds
  no face of 0.5 cm2 or more, and a stored box looser than the solid its
  planes cut (Armry02: one), are rebuilt from the faces; volume order within
  a section follows the names (OED kept its scene order, which a Blender
  scene lacks); retail bullet faces whose corners collapse on the 8.8 grid
  are dropped (a mesh cannot hold them).
- `formats/threedi/threedi_build.cpp` and
  `base/resource_index/texture_candidates.cpp` join `citation_allowlist_engine`
  (construction code with no retail counterpart; our loose-folder resolver
  policy).

## Verification

- ctests `threedi_cli_build`/`threedi_o3d_cli`, `threedi_cli_build_skinned`/
  `threedi_o3d_skinned` and `threedi_cli_build_building`/`threedi_o3d_building`
  mint the three fixture scenes and check the axis conversion, both windings,
  the register-driven PANM row and its MTRX frame, materials and the UV1
  detail stage, user points, lights, occlusion planes by the OED rule,
  collision faces, volumes and section offsets, and the skinned layout.
  `threedi_cli_roundtrip_{spinner,skinned,building}` require
  build -> scene -> build to re-mint each fixture byte for byte.
  `threedi_o3d_retail_roundtrip` (OPENNOVA_JO_ASSETS) runs Armry01, Dblkhwk1,
  US01, ArmsG and Mp5b_1st through scene -> build -> compare.
- Over the 958 JO models, `build(scene(x))` is the same model as `x`
  (`compare`) for 956; the other two draw with a material id they lack.
- Through Blender 5.1: retail Armry01 imports and exports as the same model
  under the original aggregate comparator (bldg LODs, detail textures, three
  lights, eight occlusion records, blink boxes, the FLICKER generator, bullet
  faces from LOD 1). The 2026-09-24 review adds per-corner render, skin,
  collision and occlusion comparisons. Armry01 retains zero-normal and
  collision differences; US01 and ArmsG retain normalized-weight and collision
  differences because their bullet faces and hit spheres are rebuilt.
- The 2026-09-24 validation ported the OED collision, material and tangent
  rules above. `threedi_o3d_commands` pins the bullet-face plane distance,
  a ladder's facing, box volumes' six planes, the seam rule, a volume-only
  section's zero radius, the CMDL of a model without sections and derived
  tangents. `compare` now also checks plane distances, dominant axes, ladder
  facing and the CMDL; `build(scene(x))` stays the same model for 956 of 958.
  Through Blender, Dtruck2's and Armry01's collision and every Dblkhwk1 volume
  come back the same (the hull rule left 8 Dblkhwk1 volumes off); Armry02
  keeps its seven flag-1 `CB` volumes and one loose box as reported gaps.
  The F-16 tested in retail carried every bullet face wound backwards and
  mirrored through the origin; its re-export fixes both and names five
  non-convex volumes.
- Review regressions in `threedi_o3d_commands` reject changed UV mappings,
  weights, bone assignments, collision and occlusion faces, undeclared track
  and flipbook registers, and overflowing PANM and collision indices. Reordered
  bone tables, register tables and triangle corners remain equivalent.
- A model exported by the packaged add-on loaded, rendered and flew in retail
  Joint Operations (2026-09-23, an F-16 on `cpln`); a skinned soldier on the
  retail person rig rendered and animated in retail with `anim_def US01`, and
  a first-person MP5 (`Mp5b_1st`, the 40-part view-model rig) with skinned
  arms (`ArmsG`) played the stock MP5 clips (2026-09-23).
