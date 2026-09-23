# ADR 0047: A Blender .3di exporter over the engine's writer

- **Status**: accepted (2026-09-23)
- **Owners**: the 3DI format library (`engine/formats/threedi`), `apps/threedi_cli`,
  `tools/blender/opennova_3di`
- **Supersedes/updates**: replaces ADR 0038 (deleted with this record). Its standing
  decisions are restated below; its ban on a Blender add-on and on Blender custom
  properties is lifted for this exporter. [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)
  and [ADR 0027](0027-3di3-first-class.md) stand unchanged and are what this record
  builds on.

## Context

ADR 0038 (2026-08-26) retired every object-authoring route: the Qt importer,
the Python FFI packages, the Blender add-ons and the native ASE/3DP/OED
pipeline. It left one path to a `.3di`, a hand-written C++ recipe through the
parity writer, and reserved a future GLB/GLTF editor seam. Modders and our own
content work need to author a model (a vehicle, a prop) in a DCC and ship it to
retail, and the retired pipeline's lesson stands: a second encoder or a second
language's copy of the format drifts.

## Decision

1. **One encoder.** `engine/formats/threedi/threedi_build.{h,cpp}` (the
   construction API: a `ThreediBuildModel` assembled into the contiguous
   `Threedi3di3` the parity writer serializes, `threedi_3di3_write_memory`) is
   the only code that builds a model. The frame conversions (mission <-> model
   axes) and every quantization live there.
2. **`opennova-3di`** (`apps/threedi_cli`) reads the `.o3d` scene text
   ([`docs/threedi/o3d-scene-format.md`](../threedi/o3d-scene-format.md)) into
   that API, validates what retail imposes (u16 indices, name lengths, the
   8-seat scan, CTRL register names, volume plane counts), mints the model, and
   reads the bytes back before writing them. `info` prints what any `.3di`
   holds. It owns the one winding flip (model axes mirror mission axes).
3. **The Blender add-on** (`tools/blender/opennova_3di`, a Blender 4.2+
   extension) reads the scene by the NovaLogic ASE/OED object-naming convention
   (`classify_name`, [orig: ConvertToInternal @ 0x4268B3], as the retired
   importer/exporter used it: `_lod_index` LOD roots, `PN##`, `## Mesh<n>`,
   `_## center`, `~PPx attach`, `UP<c>## <label>`, `<code>##[a..]-colonly`,
   `Material_<i>_<SHADER>`, a `BN##` armature for a skinned model; the table in
   `docs/threedi/scene-naming-contract.md`)
   and writes `.o3d`. Blender's `.001` duplicate suffixes are stripped before
   classification, as the retired exporter did; two objects with one identity
   inside a LOD are an error. What a name cannot carry (LOD thresholds, PANM
   tracks, material flags and generators, the bullet-face surface type, the
   collision LOD) is an add-on property. It writes 32-bit uncompressed TGA
   textures itself (an industry format, not a 3DI concern).
4. **Collision follows retail.** Volumes are convex hulls laid out as the retail
   corpus lays a BVOL out (the six AABB planes first, then the hull planes,
   outward normals, seams flagged 1); the bullet faces are a render LOD's
   meshes, chosen by the OED `.3dp` `poly_collision_lod` setting (default 0, the
   most detailed), never a separate LOD. A skinned model follows the retail
   person layout: one section per bone at its pivot with a hit sphere around the
   vertices it dominates, the bullet faces on the mesh part's section.
5. **Skinned models follow the retail corpus.** The parts are the armature's
   bones (hierarchy and pivots, which retail animations pair with by index),
   then one part per skinned mesh (`01 Mesh<n>`; parent 0, pivot = the mesh
   object's origin) as the retail exporter wrote bones then mesh objects
   (FSldr03: 19 bones + part 19; ArmsG: 37 bones + part 37);
   vertices store the bind pose with up to three weights; strips split so no
   bone table exceeds 16 parts; and every strip is owned by the root ROBJ while
   each part keeps the bounds of its own geometry (all 30 surveyed JO
   mesh_type-2 models, e.g. FSldr03). The builder applies that layout.
6. **Distribution.** `scripts/package_blender_addon.sh` builds the CLI and zips
   it into the add-on's `bin/`. The add-on is the repository's one Python
   product module; it links nothing native and ships no FFI.
7. **Standing from ADR 0038.** No other Python product code, Qt importer, or
   Python test suite (the stdlib `scripts/lint`, `scripts/ida`, `scripts/net`,
   `scripts/mcp`, `scripts/ci` and `tools/net` scripts remain); no native
   ASE/TDP/OED modules; ADM and BAD stay read-only runtime formats; Godot
   `ObjectData` loads immutable 3DI documents. A GLB/GLTF <-> 3DI editor seam
   remains future work and uses the same naming contract.

## Consequences

- A modder installs one zip; a model reaches retail through the same writer
  every fixture is minted by (ADR 0003).
- The CLI is a second front end for any DCC: another exporter writes `.o3d`.
- Occlusion records and lights have no scene form in this exporter yet; the
  add-on refuses `-occonly` names by name.
- `formats/threedi/threedi_build.cpp` joins `citation_allowlist_engine`
  (construction code with no retail counterpart; the writer it drives is cited).

## Verification

- ctests `threedi_cli_build` + `threedi_o3d_cli`: a fixture scene minted by the
  CLI reads back with the axis conversion, retail winding, the register-driven
  PANM row, a material generator, user points, collision faces and a convex
  volume's plane run and seam flag; `threedi_o3d_skinned` mints a skinned
  fixture and checks the root-owned strips, per-part bounds, bone tables,
  weights and bone spheres.
- A model exported by the packaged add-on from a clean headless Blender is
  byte-identical to one exported from an interactive session, and loaded,
  rendered and flew in retail Joint Operations (2026-09-23, an F-16 on `cpln`);
  a skinned soldier on the retail person rig rendered and animated in retail
  with `anim_def US01`, and a first-person MP5 (`Mp5b_1st`, the 40-part
  view-model rig) with skinned arms (`ArmsG`) played the stock MP5 clips
  (2026-09-23).
