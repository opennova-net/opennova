# ADR 0047: A Blender .3di add-on over the engine's reader and writer

- **Status**: accepted (2026-09-23; extended to animations 2026-09-24)
- **Owners**: the 3DI format library (`engine/formats/threedi`), the animation
  formats (`engine/formats/bad`, `engine/formats/adm`), `apps/threedi_cli`,
  `tools/blender/opennova_3di`
- **Supersedes/updates**: supersedes [ADR 0038](0038-native-runtime-assets-glb-editor.md),
  which stays as the record of its hard cut (the retired Python, Qt and DCC
  surfaces, the flat C ABI with `opennova_shared`, the `abi_exports_check.py`
  gate). Its standing decisions are restated in decision 10; its ban on a
  Blender add-on, on Blender custom properties and on an import route is lifted
  for this add-on, and its retirement of the `.bad`/`.adm` writers is reversed
  (decisions 11 to 13). [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)
  and [ADR 0027](0027-3di3-only-no-model-ir.md) stand unchanged and are what this record
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

A rig is inert without clips. The animation pair `.bad` (one skeletal clip) and
`.adm` (the table binding anim slots to clip rings) was read-only under decision
10 below, which this record now lifts for the same route: the same one encoder,
the same text transport, the same add-on.

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
   `build` reads it into the construction API strictly (every field parsed and
   range-checked against the word it fills, no trailing tokens, no unclosed
   quote), validates what retail imposes (u16 indices, name lengths, int16
   tracks, the 8-seat scan in any case, declared registers, volume plane counts,
   occlusion vertex and plane limits, the 8.8 collision range, PANM rows in part
   order), mints the model, reads the bytes back, and writes the file whole or
   not at all. `scene` is build's exact inverse (`build(scene(x))` re-mints a
   builder-made model byte for byte; NaN and infinity are spelled so the text
   reads back) and
   names the file each texture resolves to beside the model, by the runtime's
   own candidate order (`engine/base/resource_index/texture_candidates.h`,
   shared with the Godot resolver). `info` prints what any `.3di` holds,
   `compare` tells whether two files hold the same model (anything the runtime
   reads, beyond storage noise, is a difference; heuristic derived values and
   moves within tolerance print as `drift:` lines with counts, which `--strict`
   also fails), and `catalog` prints
   the engine's CTRL register and generator-style tables (with the shader tags
   and their capability words, decision 8, and the anim slot keys and event
   trigger bits, decision 12, one kind per line). The CLI owns both
   winding conversions: render triangles flip (retail winds counter-clockwise
   in model axes, the mirror of mission); collision and occlusion faces keep
   the scene's counter-clockwise-about-the-normal order, which is retail's
   (Dtruck2 905 of 906 bullet faces, Armry01 all of them and its OCCL faces).
3. **The Blender add-on** (`tools/blender/opennova_3di`, a Blender 4.2+
   extension) reads the scene by the NovaLogic ASE/OED object-naming convention
   (`classify_name`, [orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe)], as the retired
   importer/exporter used it: `_lod_index` LOD roots, `PN##`, `## Mesh<n>`,
   `_## center`, `~PPx attach`, `UP<c>## <label>`, `LP##` lights,
   `<code>##[a..]-colonly`, `OB/OS/OP/OH##[-MM]-occonly`, a `BN##` armature
   for a skinned model; the table in
   `docs/threedi/scene-naming-contract.md`) and writes `.o3d`. Import runs
   `opennova-3di scene` and lays the `.o3d` out by the same convention, each
   model under a model root Empty in the current scene (its model name, output
   path and collision LOD), textures from the files `scene` resolved; an
   imported model exports again. A scene holds any number of models, and each
   exports in its model root's own frame. Blender's `.001` duplicate suffixes are
   stripped before classification; two objects with one identity inside a LOD
   are an error. What a name cannot carry (LOD thresholds and types, PANM
   tracks, a material's shader, generators and the texture rows its nodes
   cannot give, light generators, export order, the bullet-face surface and
   flags, the collision LOD) is a visible add-on property; a rotated `PN##` is
   a PANM rotation frame (an MTRX row). A material is read the way Blender
   draws it: the image feeding its Principled BSDF's Base Color is the diffuse
   texture (on the second UV map, the detail texture), a tangent-space Normal
   Map node's image the `.mdt` normal map, written with the game's green (its
   tangent frame, dP/du and dP/dv on D3D UVs, runs down the texture, Blender's
   up), Backface Culling off two-sided, a Greater Than (Less Than: inverted)
   Math node on Alpha the alpha test, Render Method Blended the alpha pass and
   Emission a glow; an image-less material draws its colour from a swatch
   texture, never additive glass. Its Shader property names any tag of the
   engine's shader table (`opennova-3di catalog`); one left empty takes OED's
   default for its texture count among the rows that draw those settings.
   Export never
   relies on anything import set up: every value the engine derives (volume
   planes, seam flags, tangents, section and model bounds, glass and emissive,
   the alpha pass) is recomputed from the authored scene on every export by the
   rules below. Import never stashes source data to make a round trip
   reproduce it: what the scene form cannot express is reported and dropped.
   A part that draws nothing takes its sphere's centre from its `_## center`
   helper's first mesh vertex (OED's placeholder, which in the collision LOD is
   also that section's lone collision vertex); a volume whose code the OED
   names do not list is type 0 (import names it `CX`), as 1,333 volumes of the
   first-person weapons are; the export writes only what the strict reader
   takes, naming the object otherwise, and prints every number as the exact
   double it holds. The add-on writes 32-bit uncompressed TGA textures itself
   (an industry format, not a 3DI concern), float images sRGB-encoded, named
   from the model in at most 15 bytes as retail packs them, and only once the
   model is built; an image loaded unchanged from a texture file the game
   reads is that file, copied as it stands.
4. **Collision follows the OED rules** (ModSuperOed, as the retired
   `engine/formats/oed` port carried them, 5fc5b4f6a^). A volume is the solid
   its authored faces bound [orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe)]: its vertex
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
   subobjects), at the part's pivot, the COBJ offset retail carries
   (Dtruck2's wheels, Dblkhwk1's rotors); its bounds cover its source mesh
   (the part's render floats), its volume boxes and the occlusion records it
   parents, its midpoint is the floor of the bounds' mean and its radius the
   farthest point from it (a volume-only section's is 0). The CXLT table is
   WriteCXLT's: the collision LOD's attach points, the `~PPx attach` helpers
   (in the add-on, any helper in that LOD writes a whole table, a part without
   one contributing its pivot); with none, build derives one row per non-root
   section at its offset, our rule and the retail row count in 917 of the 958
   JO models. The CMDL box envelops the collision faces and LOD 0's triangles,
   its radii and height (`radii[2]`) the collision faces' alone, as the retail
   corpus stores them. A skinned model follows the retail person layout: one
   section per bone at its pivot, bounded by every LOD 0 vertex the bone moves,
   the bullet faces on the mesh part's section, whose own bounds stay at the
   empty sentinels (a derived sphere there would be a phantom shootable bone).
   The derived values are truncated as OED's writer truncated them (GHDR,
   user points and section offsets included). The CMDL and the bullet-face
   words come from the stored corners and normals, our rule: retail took them
   from the authored corners, which the file keeps only for a rigid section's
   own part, and deriving from what is stored lets `build(scene(x))` stay exact.
   The render words follow the same writer: GHDR's radius is the farthest
   render vertex, truncated; a part's sphere is its vertex box's centre and the
   farthest vertex from it; a part that draws nothing keeps the centre its
   `_## center` helper gives it, with radius 0. The full rule set is in the
   `.o3d` record.
5. **Skinned models follow the retail corpus.** The parts are the armature's
   bones (hierarchy and pivots, which retail animations pair with by index),
   then any mesh parts, as the retail exporter wrote bones then mesh objects
   (FSldr03: 19 bones + part 19; ArmsG: 37 bones + part 37). A skinned mesh
   `## Mesh<n>` names the part its geometry is authored on: a bone, or a mesh
   part after the bones (parent 0, pivot = the mesh object's origin). A model
   without a mesh part (dM1A1's hull) authors its geometry on the bones, and
   import restores that per part for the collision LOD, whose triangles are
   exactly the sections' bullet faces (dM1A1: LOD 1, 875 hull faces and 40 per
   wheel). A bone carries its part's PANM tracks, flags and track frame
   (dM1A1's turret ring and wheels). Vertices store the bind pose with up to
   three weights; a vertex whose stored weights are all zero keeps them.
   Strips split so no bone table exceeds 16 parts, and every strip is owned by
   the root ROBJ while each part keeps the bounds of its own geometry (all 30
   surveyed JO mesh_type-2 models, e.g. FSldr03). The builder applies that
   layout.
6. **Lights and occlusion follow the retired OED exporter.** A light is `LP##`
   (`##` the owning part); an omni light keeps retail's default axis (straight
   down, no cone) and a spot light's axis is its local -Z (where Blender draws
   the cone) and its cone the light's spot size,
   with `view_proj` built as the retired exporter built it (it reproduces
   Armry01's omni-light records to within one ulp in two entries, the NaN
   columns included). An occlusion mesh's planes
   follow the OED rule (the six bounding planes, then deduplicated face planes,
   at most 32), which reproduces Armry01's OCCL records plane for plane; a
   record's centre sums its vertices in double and divides once, as OED did.
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
9. **Assemblies are display only.** Models the game draws together share a
   scene, and the add-on reproduces how the game combines them without
   writing any of it. The bones of a skinned model follow another model's
   parts of the same index, as retail draws first-person arms with the gun's
   part matrices [orig: Player_RenderFirstPersonViewModel @ 0x4ded60, its
   Entity_BuildBoneWorldMatrices call @ 0x4df028]. Importing both together
   pairs them. A model mounts on another model's user point, as an ITEMS.DEF
   `addeweap` child does: the name is matched whole and trimmed, without case,
   and a missing name places the child at the parent's root. The child takes
   the point's look-at frame as retail builds it, the matrix's rows being the
   child's axes (a level point faces the child along its direction; a pitched
   one tips it the other way) [orig: Bone_BuildAttachmentMatrix @ 0x56C630;
   Math_BuildDirectionLookAtMatrix @ 0x612C90]. Both the arms' drive and a mount
   bind with the rigs at rest. Export reads skinned meshes in their rest pose
   with their Armature modifiers off, and every model in its own root's frame.
10. **Standing from ADR 0038.** No other Python product code, Qt importer, or
   Python test suite (the stdlib `scripts/lint`, `scripts/ida`, `scripts/net`,
   `scripts/mcp`, `scripts/ci`, `scripts/parity` and `tools/net` scripts and
   the `scripts/oracles` witness regenerators, which drive the pinned retail
   executable under Unicorn, remain); no native
   ASE/TDP/OED modules; Godot `ObjectData` loads immutable 3DI documents. A
   GLB/GLTF <-> 3DI editor seam remains future work and uses the same naming
   contract. ADM and BAD are no longer read-only: decisions 11 to 13 give them
   the same authoring route models have, under the same rules (one encoder,
   from scratch per ADR 0003, nothing stashed on import).

11. **One clip encoder.** `engine/formats/bad/bad_build.{h,cpp}` is the only
   code that builds a clip: it assembles a `BadBuildClip` into the `BadFile`
   `bad_write.cpp` serializes, and `adm_write.cpp` emits the table's canonical
   row shape (parse-equality, the ADR 0021 writer-policy shape). Every frame
   conversion and every derivation lives there, and the mission <-> clip frame
   map reads `threedi_build`'s own permutation rather than minting a second
   owner of it. The seam derives the bone table's bind, the bone positions
   (through the set's reset bind, as 30,358 of 32,011 retail bones store them),
   the child and parent addresses and the translation pad row; none of these
   is asked of an author. The author supplies frame_count + 1 keys (or a
   duration per key), events (each with its velocity, trigger word, and
   `bottom` and `top`, the hips' and the head's height above the ground,
   which the seam carries as given: a clip poses the body about its hips, so
   the ground is not in it [orig: AnimMap_UpdateEntity @ 0x40b5f0, the
   out-transform @ 0x40b82f..0x40b8a3]) and, for a translated
   bone, translation rows, and the seam counts them: retail's reader lerps
   translation row trunc(frame_count * t) with the next one [orig: sub_4102D0
   @ 0x4102d0 via BoneAnim_TransformBones @ 0x410360], so row frame_count is
   read and the writer repeats it once more as the pad row retail files carry.

12. **`opennova-3di anim`** (`apps/threedi_cli`, one translation unit per
   command as the model commands are) speaks the `.o3a` clip-set text
   (`docs/anim/o3a-scene-format.md`): one file is one rig's table and every
   clip it names. `anim build` mints the `.adm` and every `.bad` in memory,
   reads each back through its parser, and writes nothing unless all of them
   succeed; a clip name or row variant must be a bare file stem, so nothing is
   written outside the table's directory. `anim scene` is build's exact inverse
   (`build(scene(x))` re-mints a builder-made set byte for byte); `anim info`
   prints a set; `anim compare` says whether two sets are the same animation,
   reading keys and the bind as rotations and reporting NaN and absent clips as
   differences. `catalog` also prints the engine's anim slot keys and event
   trigger bits. A row's slot is its key past any first five characters,
   without case, as retail reads it [orig: AnimMap_ParseConfigLine @ 0x40cb60;
   AnimMap_FindSlotByName @ 0x40cfa0], and a table must hold an `anim_reset`
   row: retail faults loading one without it [orig: AnimMap_LoadAdmFile, the
   unchecked slot-0 read @ 0x40ce11], so `anim build` refuses it.

13. **A rig's rest pose is its bind.** A channel key IS the bone's rotation in
   the model's frame, and the bind it is measured against is the reset clip's
   first key (the `anim_reset` row's last variant), which the runtime carries
   as the SKELETON's rest and poses with the key, so what a bone deforms by is
   `key * bind^-1` [orig: AnimMap_RegisterEntity @0x40bb60 pins the bind;
   AnimChannel_ComputeBoneMatrices @0x410da0 reads it; the loaders build the
   rest from it]. The add-on therefore poses a bone with the key itself, and
   the first table imported onto a rig without clips turns each rest bone onto
   the reset clip's key; a later table replaces the rows (and any clip of the
   same name) but keeps the rest, a lone `.bad` leaves both alone, and a table
   without an `anim_reset` row aligns nothing (retail cannot load one, so
   `anim build` refuses it). Heads, lengths and weights do not move, so the model still
   exports the same model, and a clip shows the pose the game draws. Import
   keys frames 0..frame_count as the runtime evaluates them (its duration walk
   and slerp). A clip is an Action on the rig's NLA tracks, exported through its
   own strip and action slot over the rig's rest, with any drive muted, so a
   clip keys only what it animates; the table is the rows on the model root;
   the rig stands on a `Root` bone (any case, no part) at the ground, the hips
   `BN01` below it and a head (the model root's `head_bone`, else the one bone
   whose name ends in `head`), the shape of a Godot humanoid. Each event is
   measured from the pose: `bottom` the hips' height above Root, `top` the
   head's (the bottom when the rig has no head), the step the hips' move to
   the next frame with the change in bottom as its vertical; a loop's last two
   events repeat event 0 and a one-shot's stand still, as every retail clip's
   do. A rig without Root stands on Blender's Z = 0 (our convention: the
   first-person sets never step). Import keys the hips and, for a set that
   travels, Root, so a clip plays with its feet planted, and raises a model at
   the world origin so the ground is Z = 0; every export reads a model with
   its root at the origin, so where it stands does not change its bytes. The
   event bits are keyed on the rig. A bone named `!...` is no part either,
   which lets a rig hold control bones. A rigid model's parts hang
   from its LOD root and follow their animation bone through an `O3D follow`
   Child Of constraint that is muted at Rest Position, so the model exports
   byte for byte as before its clips were imported.

14. **Weapon timing is authored, then evaluated by the runtime FSM.** An
   animation-table row may explicitly name a weapon action role. Action-local
   Shot, Eject, Active End and Ready markers supply its phase timing; firing
   cadence has one selected source, a Ready marker or target RPM. Neither
   imported provenance nor an existing weapon definition is an input.
   `opennova-3di weapon timing` converts this authoring input to explicit
   ACTION delay fields and measures the result with the engine's existing
   `weapon_fsm_bake` / `weapon_fsm_tick`; no Python FSM or alternate gameplay
   behavior is introduced. The command library consequently links the runtime
   group. Its text output is an ACTION-block snippet for the existing
   `weapon.def` parser, written beside the BAD/ADM set as
   `<table>_weapon_actions.txt`. It does not invent ammunition, damage or other
   weapon settings. A preview is disposable; export recompiles from current
   Actions and markers. Invalid timing is rejected before animation files are
   written. Frame-to-tick authoring policy, marker semantics and the runtime's
   reload/switch limitations are documented in the add-on README. Native tests
   check actual cadence and parser acceptance; a Blender test authors the
   geometry, skin, Actions and markers from scratch and exports them without
   importing any assets.

## Consequences

- A modder installs one zip; a model reaches retail through the same writer
  every fixture is minted by (ADR 0003), and a retail model opens in Blender
  through the same reader the runtime uses.
- The CLI is a second front end for any DCC: another exporter writes `.o3d` or
  `.o3a`, another importer reads what `scene` and `anim scene` write.
- The animation formats' record (`docs/anim/adm-bad-format-re.md`) carries the
  corpus witnesses the seam derives from, and the `.o3a` grammar is
  `docs/anim/o3a-scene-format.md`.
- Known animation gaps, each reported rather than carried: a first-person
  set's `top` is its `bottom` in 137 of 201 registrations, but 64 carry a
  higher top by a rule nothing has witnessed (1.6578 in 21 reset clips), and
  an FP rig has no head bone to give it; `flags` bit 3 (73 retail
  clips) is carried and unread; import keys every frame, so a bone that keys
  sparsely or with durations (DVFLEE1E, DT1RST, stgr_RST) re-exports with a key
  per frame and the same poses, keys past a clip's length (M60_1i) are dropped,
  and a version 0 clip re-exports as version 1.
- Known model gaps, each reported rather than carried: retail's own tool is not
  witnessed, so its seam flags and tangent values match the OED rules only
  where that tool agreed with ModSuperOed; CTRL registers nothing references;
  an empty CXLT table (11 retail models: no attach helper can say "none");
  the centres of skinned bones that draw nothing (dM1A1, DT801); MTRX frames
  that are not rotations (Frag_1st and Stch_1st rows 37 to 39 come back
  orthonormal); the first-person weapons' collision meshes, which the file
  does not keep; placements that land one 16.16 step off through Blender's
  float composition (drift);
  non-`BB` volume flags (Armry02's `CB` volumes with flag 1); zero-length
  vertex normals (Blender cannot hold them); a retail volume plane that bounds
  no face of 0.5 cm2 or more, and a stored box looser than the solid its
  planes cut (Armry02: one), are rebuilt from the faces; volume order within
  a section follows the names (OED kept its scene order, which a Blender
  scene lacks); retail bullet faces whose corners collapse on the 8.8 grid
  are dropped (a mesh cannot hold them); flat `CB`/`CP` volumes (38 in the
  corpus) import as nothing, with a note, since no single polygon rebuilds
  their planes (flat ladders do import, as their one polygon). Words retail
  derived from what the file does not keep rebuild differently: part spheres
  no subset of the stored geometry gives (61 models, Armry01's part 3 among
  them), GHDR radii over geometry the file lacks (25: the `fxflsh` family and
  the first-person weapons' own collision LOD), three skinned vehicles
  authored on their bones (dM1A1, DT801, Ftruck1X), NaN `rel` words (Dmil261x,
  Excavatr), and occlusion centres taken before LOD recentering (Armry01, 7
  of 8); the CMDL and bullet-face words, derived from the stored corners (our
  rule), sit up to a few millimetres from retail's.
- `base/resource_index/texture_candidates.cpp` joins `citation_allowlist_engine`
  (our loose-folder resolver policy, moved down from the Godot resolver so the
  CLI shares it; the `texture_candidates` ctest pins its order). `threedi_build.cpp` and `bad_build.cpp` carry the `[orig:]`
  cites of the rules they port, so they need no entry.

## Verification

- Model ctests: `threedi_o3d_cli`, `threedi_o3d_skinned` and `threedi_o3d_building`
  mint the three fixture scenes (`fixtures/threedi/o3d`) and check the axis
  conversion, both windings, the register-driven PANM row and its MTRX frame,
  materials and the UV1 detail stage, user points, lights, occlusion planes by
  the OED rule, collision faces, volumes, section offsets and the skinned
  layout; `threedi_cli_roundtrip_{spinner,skinned,building}` require
  build -> scene -> build to re-mint each byte for byte. `threedi_o3d_build`
  pins the builder's derived rules and `threedi_o3d_unicode` runs the CLI in
  folders with accented and Japanese names. `threedi_o3d_commands` changes one
  field at a time and requires `compare` to call it different (or drift, for
  derived values within storage noise), and requires `build` to refuse every
  scene the strict reader rejects. The gated `threedi_o3d_retail_roundtrip`
  (OPENNOVA_JO_ASSETS) requires Armry01, Dblkhwk1, US01, ArmsG and Mp5b_1st
  to come back the same model, with drift only in the builder-derived
  categories. The synthetic model set (`fixtures/threedi/synth`) is minted
  through `threedi_build`, and `minimal_3di_gen` reproduces every file.
- Over the 958 JO models (scene -> build -> compare): `build(scene(x))` is
  byte for byte `build(scene(build(scene(x))))` for all of them, their CXLT
  tables come back exactly, and `compare` calls 903 the same model (888 with
  drift notes). The 55 that differ carry words retail derived from data the
  file does not keep (Consequences): part spheres the rule does not give over
  a part's own vertices (41), GHDR radii (25), the three skinned vehicles
  authored on bones, NaN `rel` words, and two models that draw with a
  material id they lack.
- Animation ctests: `bad_roundtrip`, `bad_parse` (a translated retail clip's
  rows), `bad_build` (the frame maps, the bind and positions through the reset
  bind, the events' bottom and top written as stated, the refusals, the
  canonical table),
  `adm_write`, `adm_variants`, `anim_sample` (the translation row lerp, the
  bind's translation gate, retail's reset-clip choice), `anim_o3a_commands`
  (the clip-set round trip byte-identical, what `anim compare` calls the same
  animation, the sets `anim build` refuses, among them an event without its
  bottom and top and a `capsule` record) and the gated
  `anim_o3a_retail_roundtrip`. Over the corpus, `build(scene(x))` is the same
  animation as `x` for all 477 `.bad` clips and 81 of the 82 `.adm` tables
  (ESTAND02 names a clip the corpus does not ship; `anim compare` reports it).
- The gated `anim_o3a_runtime_playback` loads US01.ADM, mp5_1st.adm and
  357_1st.adm and their rebuilds through the runtime's own loader over the
  models' bone tables (US01.3di, Mp5b_1st.3di, 357_1st.3di), as the game does,
  and compares every evaluated pose: 1,665, 81 and 81 poses, worst 2.41e-6
  degrees.
- Through Blender 5.1 (headless, the integrated CLI): of 91 retail models,
  import then export gives 23 the same model and none that fails to export;
  the differences left are the gaps above (Armry01 2 lines, Dtruck2 7,
  dM1A1 49 among them; 357_1st now 0). US01 with US01.ADM, CIndo01 with
  Cindo01.adm, Mp5b_1st with mp5_1st.adm, 357_1st with 357_1st.adm and M60_1st
  with m60_1st.adm export the model byte for byte as before their clips were
  imported, and their clip sets compare the same animation as the shipped ones
  (worst 2.3e-4 degrees, at the bind); a model and its clips round-trip
  through a folder named with accented and Japanese characters.
- Assemblies: 357_1st, ArmsG, dM1A1 and m1trret imported as one batch share a
  scene; ArmsG pairs with 357_1st (and with Mp5b_1st after its clips are
  imported), posed arms still export their rest pose, and m1trret mounted on
  dM1A1's `ewep01` follows the moved hull and exports the same model.
- A model exported by the packaged add-on loaded, rendered and flew in retail
  Joint Operations (2026-09-23, an F-16 on `cpln`); a skinned soldier on the
  retail person rig rendered and animated in retail with `anim_def US01`, and
  a first-person MP5 (`Mp5b_1st`, the 40-part view-model rig) with skinned
  arms (`ArmsG`) played the stock MP5 clips (2026-09-23).
