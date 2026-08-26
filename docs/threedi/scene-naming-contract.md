# Scene naming contract

This document preserves the format-neutral names a future GLB/GLTF editor can
use when converting ordinary scene data to and from 3DI. It is a naming
contract, not an importer, exporter, Blender schema, or license to depend on
custom properties.

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
| Bone | `BN##` | Stable bone identity used by joints and weights |

Collision and occlusion nodes retain their established two-letter type prefix,
numeric identity, and the `-colonly` or `-oconly` role suffix. Their complete
field mapping is intentionally deferred until the GLB editor is designed;
names alone must not be treated as a lossless encoding for flags, planes, or
connected-part data.

Material names may remain descriptive, but a future converter may not assume a
name alone losslessly carries shader codes, texture casing, control registers,
or animated-texture state. UV sets use `UVMap` for the primary channel and
`UVMap_Lightmap` for the lightmap channel where those names are available.

Hierarchy, transforms, meshes, materials, skinning, weights, UVs, lights, and
animation travel as standard scene/GLTF data. Coordinate conversion has one
owner per direction and happens exactly once. Nova-specific semantics that
standard GLTF cannot represent require a future, explicit editor decision;
they must never be recovered from importer-private metadata or custom
properties.
