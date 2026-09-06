# ADR 0046: Godot authors native models

- **Status**: accepted (2026-09-06)
- **Owners**: the Godot layer (the `opennova_model` editor plugin, `godot/src/model/`),
  the 3DI format library
- **Supersedes/updates**: implements [ADR 0038](0038-native-runtime-assets-glb-editor.md)
  decision 4 (the reserved scene <-> 3DI seam) and amends its decision 6
  (LODs, collision, user points and lights now have an explicit scene form);
  amends [ADR 0044](0044-godot-authors-native-worlds.md) decisions 1 and 3 for
  MODELS only (a model's scene is its source); amends [ADR 0030](0030-formats-placement-criterion.md)'s
  industry-format clause (a write-only TGA encoder lives in `engine/formats/tga`).
  [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md) and
  [ADR 0027](0027-3di3-first-class.md) stand unchanged and are what this record
  builds on.

## Context

`assets/` is the game's source tree and the concrete required-resources
manifest (`assets/README.md`). It carries 213 retail bring-up files under the
`TEMPORARY` banner of its allowlist: six skinned models, their textures, the
infantry and rifle clips and three definition tables, committed 2026-08-31 so
the minimal set runs from a fresh checkout while our own writers caught up.
ADR 0038 retired the whole DCC pipeline and left one path to a `.3di`: a
hand-written C++ recipe (`tests/fixtures/minimal_3di_builder.h`) through the
parity writer. The house shipped that way; a rig, a rifle or anything an
artist shapes cannot.

ADR 0044 made Godot the home for world authoring on the rule that native
documents stay authoritative and Godot scenes only describe composition. That
rule fits worlds, whose documents are edited in place and whose scenes are
transient projections. It does not fit models: a model has no editable
document short of the file itself, and the thing an artist edits (a mesh, a
rig, a material) is exactly what a scene holds. The question this record
settles is what the source of a model is, and how Nova-specific meaning
travels with it without a second content model or importer-private state.

## Decision

1. For models, the Godot scene under `godot/authoring/<name>/` is the source.
   The exported `assets/<name>.3di` and the `.tga` textures it names are built
   artifacts, byte-guarded by re-export (`godot/tests/model_export_guard_test.gd`,
   the Godot analogue of the `minimal_3di_gen` ctest byte guard). `assets/`
   stays flat and stays the artifact tree.
2. Meshes enter Godot through Godot's stock importers (glTF, FBX, OBJ, .blend).
   No DCC add-on, converter or exchange schema lives in this repository. Nova
   semantics travel two ways only: the scene naming contract
   (`docs/threedi/scene-naming-contract.md`: part, bone, mesh, LOD, section,
   volume, user-point and light names) and one typed authoring record beside
   the scene (`ModelAuthoringManifest`: the GHDR words, the CTRL registers,
   one `ModelMaterialSpec` per material keyed by the surface material's name,
   the per-LOD words and PANM rows, the texture sources and the bone-row count
   a rig must carry). Importer-private metadata and custom properties carry
   nothing (ADR 0038 decision 5 stands).
3. A `.3di` opened in the editor is a transient node projection
   (`ModelSceneProjector`), never a Godot import of the format and never a
   Godot Resource; the format reads and writes itself (`ModelDocument`). A
   construct the scene form does not spell (occlusion records, indexed strips,
   a second matrix, an OVRT table) is refused by name, never dropped or carried
   through opaquely (ADR 0003).
4. The export runs through the engine's construction API
   (`engine/formats/threedi/threedi_build`, the synth builder moved
   engine-side) into the parity writer. That API is a construction seam, not an
   intermediate representation: nothing at runtime walks a build model
   (ADR 0027 stands).
5. A rig that retail clips animate keeps the bone-row order of its reset clip
   (`assets/DT1RST.BAD`: `BN01 Hips` .. `BN19 L Foot`; `rAKM_RST.bad`: the
   46-row gun rig the arms share) until our own clips exist; clips pair with
   model rows by index. The manifest's `expected_bone_rows` pins it.
6. Provenance: a projection of retail bytes may be inspected in the editor but
   is never saved under `godot/authoring/` or exported; every source there is
   original work (`assets/README.md`'s asset policy).

## Consequences

- The plugin lives in `godot/addons/opennova_model/` (Open model, Export
  .3di, Verify, Export all); the workflow in `godot/tools/model_export.gd`
  with the headless `--export-models [--verify] [name...]` command; the staged
  file transaction the world plugin wrote is shared as
  `godot/tools/file_transaction.gd`.
- Frames have one owner: the engine's `threedi_build.h` carries the mission,
  model and presentation axis maps and their inverses; the projector and the
  exporter both spell them with `Vector3` only.
- Two facts the byte round trip forced and the bindings record: an
  `ArrayMesh` octahedral-packs normals and tangents, so a projection keeps the
  file's exact values in RGB float custom channels (`CUSTOM0` normal,
  `CUSTOM1` tangent, `CUSTOM2` bitangent) the exporter prefers when present;
  and a recipe's double pivots leave ROBJ `rel` values the float `abs` cannot
  re-derive, so the builder takes a scene node's exact local origin as the
  `rel` where it differs as a float, keeping the derived signed-zero
  convention otherwise.
- Godot's scene importer takes the contract's `-colonly` suffix as its own
  hint: the mesh becomes a `StaticBody3D` named by the stem with a collision
  shape under it. The exporter accepts that converted form (a box from the
  shape) beside the typed `ModelBoundingVolume3D` and the plain named mesh.
- Textures are authored as PNG sources and exported as truecolor TGA
  (`engine/formats/tga`, write-only; decode stays with Godot's `Image`).
  Models name `.tga`; retail loads loose `.tga` directly, so no `.dds` or
  `.MDT` output exists.
- The authoring tree is excluded from the game export preset; its binaries
  ride Git LFS (`.gitattributes`, the Godot tests job's pull, `fixture_lint`'s
  size and LFS walks). The plugin is shipping GDScript counted by every lint
  scope beside `opennova_world`.
- The first authored model is the crate (`godot/authoring/crate/`: a Blender
  box through the glTF importer; `items.def 108002`, placed once in
  `mnml.bms`), then the body and head on the 19-row rig and the arms and rifle
  on the 46-row rig; the clips need the `.bad`/`.adm`
  writers of the next one, so `US01.ADM`, `E_STAND.adm` and the 156 clips stay
  under the banner until then.

## Verification

- `threedi_roundtrip`, `minimal_3di_gen` (byte-green through the builder
  move), `threedi_scene_names`, `tga_write` and `minimal_model_validate` (the
  authored models: runtime-safe collision, tags the authored `.fx` set serves,
  allowlisted textures, the tangent vertex-format rule) in ctest.
- `godot/tests/model_scene_projector_test.gd` pins the projection of the
  synthetic set; `model_scene_roundtrip_test.gd` proves every in-scope
  `fixtures/threedi/synth` model projects and exports back to the same bytes,
  with the armory family refused by name for its occlusion records;
  `model_export_guard_test.gd` re-exports every manifest under
  `godot/authoring/` and byte-compares the tracked artifacts.
- The retail A/B of `assets/README.md` stays the acceptance for every
  replacement: the file loads in `Jointops.exe /w /d /FRISK` and draws.
