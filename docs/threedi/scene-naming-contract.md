# Scene naming contract

This document is the format-neutral naming contract the Godot model tool
(ADR 0046: `ModelSceneProjector` / `ModelSceneExporter`, `godot/src/model/`)
reads and writes when converting ordinary scene data to and from 3DI. It is a
naming contract, not an importer, exporter, Blender schema, or license to
depend on custom properties; the words a name cannot carry live in the typed
`ModelAuthoringManifest` beside the scene.

Names are ASCII and case-stable. Numeric identities are zero-padded to two
digits. A converter must reject ambiguous DCC deduplication suffixes such as
`.001`; it must not silently reinterpret them as part identities.

| Scene element | Name form | Meaning |
| --- | --- | --- |
| Part | `PN##` | Stable 1-based 3DI subobject identity |
| Part mesh | `## Mesh<n>` | Mesh `<n>` belonging to part `##` |
| Part center | `_NN center` | Transform center for part `NN` |
| Attachment | `~NNx attach` | Named attachment `x` on part `NN` |
| User point | `UPcNN <label>` | User-point type `c`, part `NN`, optional label |
| Light | `LP##` | Stable light identity |
| Bone | `BN##` | Stable bone identity used by joints and weights (a label may follow a space: `BN01 Hips`; retail's own reset clips name their rows this way, `assets/DT1RST.BAD`) |
| LOD | `LOD#` | The RLOD container (0-based, the file's order); its threshold, type tag and PANM rows ride the manifest's `ModelLodSpec` |
| Collision section | `CO##` | One COBJ, paired with part `##`; `CO## faces` is its collision-face mesh (surfaces named `pt<poly>_mf<flags>`, faces wound clockwise seen from outside, the retail winding) |

Collision and occlusion nodes retain their established two-letter type prefix,
numeric identity, and the `-colonly` or `-oconly` role suffix (the tables
below, from the retired `pyopennova/scene_naming.py`). A bounding volume's
flags, bounds and planes travel as the typed `ModelBoundingVolume3D` node the
projector writes; a plain mesh named per the contract exports its AABB as an
axis box. Godot's scene importer takes `-colonly` as its own hint (the mesh
becomes a `StaticBody3D` named by the stem, `CB01`, holding a collision
shape); the exporter accepts that converted form as the same volume. Occlusion
records (`-oconly`) have no scene form yet and are refused by name. Names alone
are never a lossless encoding for flags, planes, or connected-part data.

### Collision volumes

`<TYPE><NN>[<dup>]-colonly`. `NN` is the 1-based zero-padded volume index (a
negative source index displays as `01`). `<dup>` is a base-26 lowercase suffix
for the 2nd and later occurrences of an otherwise identical name: 2nd `a`,
3rd `b`, 27th `z`, 28th `aa`. Unknown numeric types fall back to `CX`.

| Code | Type | Code | Type | Code | Type |
| --- | --- | --- | --- | --- | --- |
| 1 | `CB` | 8 | `BB` (blink box) | 14 | `LP` |
| 2 | `CS` | 9 | `CD` | 15 | (unassigned: `CX`) |
| 3 | `CC` | 10 | `CT` | 16 | `DH` |
| 4 | `CL` | 11 | `CM` | 17 | `DM` |
| 5 | `CV` | 12 | `VK` | 18 | `DL` |
| 6 | `CA` | 13 | `CF` | 19 | `CP` |
| 7 | `VC` | | | | |

`BB` appends the enabled-flag letters before `NN`, taken from `~flags & 0x3E`
(a cleared bit means enabled): bit 1 `V`, bit 2 `S`, bit 3 `W`, bit 4 `L`,
bit 5 `O`, in that order (for example `BBVSO03`; no letters when every bit is
set). Type 14 shares the `LP` prefix with lights; the `-colonly` suffix is what
distinguishes the two.

### Occlusion volumes

`<PFX><NN>[-<MM>]-oconly`: type 0 `OB`, 1 `OS`, 2 `OP`, 3 `OP` (type 3 shares
the `OP` prefix); unknown types fall back to `OX`. `NN` is the parent
subobject + 1 (negative displays as `01`). Types 2 and 3 with a connecting
subobject of 0 or more append `-MM` (that subobject + 1). Occlusion names take
no duplicate suffix.

Material names may remain descriptive, but a future converter may not assume a
name alone losslessly carries shader codes, texture casing, control registers,
or animated-texture state. UV sets use `UVMap` for the primary channel and
`UVMap_Lightmap` for the lightmap channel where those names are available.

Hierarchy, transforms, meshes, materials, skinning, weights, UVs, lights, and
animation travel as standard scene/GLTF data. Static parts nest by parent with
the pivot as the node position and meshes local to their part; skinned parts
are the bones of one `Skeleton3D` with translation-only rests, their meshes
under the skeleton named by the part that owns their strips. User points
(`UPcNN <label>`, the direction being the node's -Z axis) and lights (`LP##`)
sit under `UserPoints` and `Lights` in file order. An `ArrayMesh` packs normals
and tangents octahedrally, so a projection keeps the file's exact values in
RGB float custom channels (`CUSTOM0` normal, `CUSTOM1` tangent, `CUSTOM2`
bitangent) the exporter prefers when present. Coordinate conversion has one
owner per direction (`engine/formats/threedi/threedi_build.h`) and happens
exactly once. Nova-specific semantics that
standard GLTF cannot represent require a future, explicit editor decision;
they must never be recovered from importer-private metadata or custom
properties.
