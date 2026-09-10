# OpenNova models in Godot

Enable **OpenNova Model** in Project Settings > Plugins (enabled in this
project). It is the model half of the Godot authoring tools (ADR 0046): a
native `.3di` opens as an ordinary scene, and an authoring scene exports back
to `.3di` through the engine's parity writer. The scene is the source; the
`.3di` and the `.tga` textures it names are built artifacts that live flat in
`assets/` and are byte-guarded by `res://tests/model_export_guard_test.gd`.

## The authoring tree

```
godot/authoring/<name>/<name>.tscn   the scene (Godot-authored, or imported through Godot's stock importers)
godot/authoring/<name>/<name>.tres   the ModelAuthoringManifest
godot/authoring/<name>/*.png         texture sources (LFS)
godot/authoring/<name>/*.glb         mesh sources when a DCC made them (LFS)
```

The manifest carries every word the scene cannot spell: the model name and
GHDR words, the CTRL registers, one `ModelMaterialSpec` row per material
(keyed by the surface material's resource name: shader tag, flags, generators
and texture rows, with every quantized value in its integer form), the per-LOD
words and PANM rows, the texture sources (a PNG and the `.tga` it becomes),
and, for a rig, the bone-row count the file must carry. Nothing rides
importer-private metadata or custom properties (ADR 0038 decision 5).

## The scene form

Node names follow `docs/threedi/scene-naming-contract.md`; identity lives
only in the two-digit numbers, and a DCC dedup suffix (`.001`) is refused:

- `LOD0`, `LOD1`, ... under the root; each holds the parts.
- Static parts: `PN01`, `PN02`, ... nested by parent part, the node position
  being the pivot relative to its parent; meshes are `MeshInstance3D`
  children named `01 Mesh0` (part, then mesh ordinal), one surface per
  strip, vertices local to the part.
- Skinned parts: one `Skeleton3D` with bones `BN01` .. (a label may follow:
  `BN01 Hips`), rests translation-only; meshes named `## Mesh<n>` under the
  skeleton with a `Skin`; a mesh's number is the part that owns its strips.
- `Collision/CO01` .. (`ModelCollisionSection3D`): the section offset is the
  node position; `CO01 faces` holds the collision faces (surfaces named
  `pt<poly>_mf<flags>`); `ModelBoundingVolume3D` children named
  `CB01-colonly` .. carry type, flags, bounds and planes (`set_box` fills an
  axis box); a plain `MeshInstance3D` named per the contract exports its
  AABB as a box.
- `UserPoints/UPG01 label` (`ModelUserPoint3D`, or any Node3D named per the
  contract, whose -Z axis is the direction).
- `Lights/LP01` (`ModelLight3D`).

Every surface material's resource name must match a manifest row. Vertex
normals and tangents live octahedrally packed in an `ArrayMesh`; a projected
scene keeps the file's exact values in RGB float custom channels
(`CUSTOM0` normal, `CUSTOM1` tangent, `CUSTOM2` bitangent) which the exporter
prefers when present. Frames: the presentation frame is Godot's; the exporter
and projector share the engine's axis maps (`threedi_build.h`).

## Workflow

- **Project > Tools > OpenNova: Open model...** projects a `.3di` into
  `godot/authoring/<name>/` (scene, manifest, PNG copies of its textures) and
  opens it. A construct with no scene form yet (occlusion records, indexed
  strips, a second matrix, ...) is refused by name, never dropped.
  Only our own models become sources: a projection of retail bytes may be
  inspected but never saved into the authoring tree or exported
  (`assets/README.md`).
- The viewport toolbar's **Export .3di** writes the scene's artifacts into the
  manifest's `output_directory` (default `res://../assets`) through one staged
  transaction; **Verify** re-exports in memory and reports what differs.
- **OpenNova: Export all authored clips** projects every `ClipSetSource`
  under the authoring tree (its scene's Animations through `ClipProjector`)
  into its `.bad` files and `.adm`, installed the same way; headless:
  `godot --headless --path godot --script res://tools/export_clips.gd -- --export-clips [--verify] [folder/file...]`.
- **OpenNova: Export all authored models** rebuilds every manifest's
  artifacts. Headless:
  `godot --headless --path godot --script res://tools/export_models.gd -- --export-models [--verify] [name...]`.
- A mesh from Blender or another DCC enters through Godot's stock importers
  (glTF/FBX/OBJ/.blend) into the authoring scene; name its nodes and bones per
  the contract, add the manifest rows, export.

Rigs animated by retail clips (the body, the arms and rifle) must keep the
retail bone-row order of their reset clip until our own clips exist
(`assets/README.md`, "Bring-up"); `expected_bone_rows` on the manifest pins it.

## Validation

`res://tests/model_scene_projector_test.gd` pins the projection of the
synthetic set; `res://tests/model_scene_roundtrip_test.gd` proves every
in-scope synthetic model projects and exports back to the same bytes;
`res://tests/model_export_guard_test.gd` re-exports every manifest under
`godot/authoring/` and byte-compares the tracked artifacts in `assets/`.
