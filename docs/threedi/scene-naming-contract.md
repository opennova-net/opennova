# Scene naming contract

The NovaLogic ASE/OED object-naming convention: the scene-object names that
carry a model's 3DI roles. OED classified them by `classify_name`
([orig: ConvertToInternal @ 0x4268B3], ported in the retired
`engine/formats/oed/convert_internal.cpp`); the Blender exporter
(`tools/blender/opennova_3di`, [ADR 0047](../adr/0047-blender-3di-exporter.md))
reads them today, and a future GLB/GLTF <-> 3DI seam keeps them.

Names are ASCII. Numeric identities are two digits and 1-based in the name
(`01` is the first), 0-based inside; `00` parses to -1 (a user point with no
part). A leading `!` makes an object ignored. Blender's own `.001`
duplicate suffixes are stripped before classification (object names are
unique per `.blend`, so LOD1's `PN01` is `PN01.001`); two objects that
classify to the same identity inside one LOD are an error.

| Scene element | Name form | Meaning |
| --- | --- | --- |
| LOD root | any name, custom property `_lod_index` | render LOD `_lod_index` (0 = primary) |
| Part | `PN##` (Empty) | 3DI subobject `##`; its origin is the pivot |
| Part mesh | `## Mesh<n>` | mesh `<n>` of part `##` (sits under its `PN##`) |
| Part center | `_## center` | part `##`'s transform center (pivot) |
| Attachment | `~PPx attach` | sits under a child part: its parent is part `PP`; `x` (a, b, ...) tells siblings apart |
| User point | `UP<c>## <label>` | USRP point: type letter `c` (`G` 71 gameplay, `S` 83 effect), part `##` (`00` = none), label = the USRP name (no label: `Noname`); faces along its local +Z |
| Light | `LP##` | light `##` (a light object; not a `-colonly` mesh) |
| Bone | `BN##` (Armature bone) | part `##` of a skinned model: the head is the pivot, the parent bone the part parent; `BN##` vertex groups carry the weights |
| Material | `Material_<i>_<SHADER>` | export order `i`, shader tag `SHADER` |

## Collision volumes

`<TYPE>##[<dup>]-colonly`, on the primary LOD. `##` is the **owning part**
(`classify_name` stores it as the object index; the volume joins that part's
collision section). `<dup>` is a lowercase suffix for the 2nd and later volumes
of one type on one part: 2nd `a`, 3rd `b`, ... (the Blackhawk's 35 hull volumes
are all `CB01...`). The volume is the convex hull of the mesh's vertices.

| Code | Type | Code | Type | Code | Type |
| --- | --- | --- | --- | --- | --- |
| `CB` | 1 | `BB` | 8 (blink box) | `LP` | 14 |
| `CS` | 2 | `CD` | 9 | `DH` | 16 |
| `CC` | 3 | `CT` | 10 | `DM` | 17 |
| `CL` | 4 | `CM` | 11 | `DL` | 18 |
| `CV` | 5 | `VK` | 12 | `CP` | 19 |
| `CA` | 6 | `CF` | 13 | | |
| `VC` | 7 | | | | |

`BB` takes flag letters before `##`; each clears a bit of `0x3E`: `V` 0x2,
`S` 0x4, `W` 0x8, `L` 0x10, `O` 0x20 (for example `BBVSO03`). Type 14 shares the
`LP` prefix with lights; the `-colonly` suffix tells them apart. Runtime
meanings: docs/world/world-wac-ai-re.md §15.

## Occlusion volumes

`<PFX>##[-<MM>]-occonly`: `OB` 20, `OS` 21, `OP` 22 (reads a connecting
subobject after `-`: `OP01-02-occonly`), `OH` 23. The Blender exporter refuses
them by name until they have a scene form.

## Bullet faces

The collision faces bullets hit come from one render LOD's part meshes, chosen
by the OED `.3dp` `poly_collision_lod` setting (default 0, the most detailed);
each face's surface type comes from its material.
