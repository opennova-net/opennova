# Object workspace

Author and edit NovaLogic 3D object projects: materials, part animations, lights,
and draw-distance variants, with a live 3D preview. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

The Object workspace opens an object project (`.3dp`) and previews the model under
fixed editor lighting. The mode rail switches between five workflows:

- **Preview**: view the object under fixed editor lighting.
- **Materials**: edit each material's shader tag, textures, and alpha.
- **Part Anims**: inspect and edit the model's PANM part-animation entries.
- **Lights**: inspect and edit the object's light colors, falloff, and flags.
- **LODs**: bind an ASE scene to each level of detail and edit the per-LOD
  threshold, attributes, and the project's collision LOD.

The model geometry itself is authored in a DCC tool (Blender or 3ds Max) and
brought in as ASE. The Environment popup is available here too, so you can preview
the object under different lighting.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.3di` | [`libs/threedi`](../../../libs/threedi) | the compiled NovaLogic model: geometry, materials, part anims, collision, occlusion |
| `.3dp` | [`libs/tdp`](../../../libs/tdp) | the object *project*: material defs, LOD settings, part-anim metadata for round-trip editing |
| `.ase` | [`libs/ase`](../../../libs/ase) | the per-LOD mesh source bound in the LODs workflow |

The `.3di` export session itself is driven by [`libs/oed`](../../../libs/oed).

## Authoring geometry in a DCC: naming conventions

Object meshes are authored in Blender or 3ds Max and exchanged as ASE. Each
object's 3DI role is encoded in its **scene-object name**: the importer assigns
these names when it brings a model in
([`apps/importer/scene_builder/`](../../../apps/importer/scene_builder/)), and
the exporter classifies objects by the same names on the way out
([`blender/ase_exporter.py`](../../../blender/ase_exporter.py)), so a round trip is
lossless. Keep names exactly as below. Indices are two-digit and 1-based.

| Role | Blender object | Name | Example |
|---|---|---|---|
| Part / subobject | Empty | `PN` + 2-digit index | `PN01`, `PN02` |
| Render mesh | Mesh under its part | 2-digit part index + ` Mesh` + n | `01 Mesh0`, `02 Mesh1` |
| Collision volume | Mesh | `<code>` + index + `-colonly` | `CB01-colonly` |
| Occlusion volume | Mesh | `<prefix>` + index + `-occonly` | `OB01-occonly`, `OP01-02-occonly` |
| User point | Empty | `USR` + index, or `UP<c>` + index | `USR01`, `UPm01 muzzle` |
| Light | Empty | `LP` + index | `LP01`, `LP02` |
| Bone | Armature bone | `BN` + index | `BN01`, `BN02` |
| Material | material slot | `Material_<index>_<shader tag>` | `Material_0_FF_ST` |

Notes (all from the importer/exporter above):

- **Collision** `<code>` is one of
  `CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP`.
  Repeated volumes of the same kind get a letter suffix (`CB01`, `CB01a`, ...).
  `BB` (a blink box) appends enabled-flag letters (`V S W L O`), e.g. `BBVS01`.
- **Occlusion** `<prefix>` is `OB`, `OS`, or `OP`. Portal types add the connecting
  subobject index (`OP01-02`).
- **User points**: `USR` for an untyped point, or `UP<c>` where `<c>` is a single
  type character. A label, when present, follows the name (`UPm01 muzzle`).
- **LODs** are grouped by a `_lod_index` integer custom property on each LOD-root
  Empty. On export, LOD 0 writes the main `.ase` and each extra LOD writes
  `<stem>_lod<N>.ase`.
- The exporter also reads `opennova_shader`, `opennova_zero_axis`, and
  `opennova_bone_index`, strips Blender `.001` duplicate suffixes, caps texture
  filenames at 15 characters, and splits materials into Multi/Sub-Object slots of
  64.

A material's shader tag selects a NovaLogic shader; OpenNova keeps a canonical
shader-tag table rather than parsing `.fx` files. The full ASE export options live
in the DCC plugins; see the top-level [README](../../../README.md).

## How it is built

A multi-workflow workspace. The adapter
[`object_workspace.gd`](object_workspace.gd) declares the six workflows and
manages the preview viewport.

| File | Role |
|---|---|
| [`object_workspace.gd`](object_workspace.gd) | workspace adapter: workflow rows, open / save / export, export-chunk mask |
| `object_editor.gd` | document state: open / save the `.3dp` project and export the `.3di` |
| `object_preview.gd` | 3D preview viewport: camera, animation playback, grid and axis guides |
| `ui/inspectors/` | one inspector per workflow (`preview`, `anims`, `materials`, `part_anims`, `lights`, `lods`) |
| `ui/object_forms.gd`, `ui/generator_style_catalog.gd` | object-only form builders (ctrl-reg rows) and the generator-style picker; the shared forms library is `framework/inspector_forms.gd` |

## Related

- Editor framework: [`../README.md`](../README.md); contract in
  [`../framework/editor_workspace.gd`](../framework/editor_workspace.gd).
- DCC export pipeline and project overview: [top-level README](../../../README.md).
