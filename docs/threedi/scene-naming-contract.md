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
numeric identity, and the `-colonly` or `-oconly` role suffix (the tables
below, from the retired `pyopennova/scene_naming.py`). Their complete field
mapping is intentionally deferred until the GLB editor is designed; names alone
must not be treated as a lossless encoding for flags, planes, or connected-part
data.

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
animation travel as standard scene/GLTF data. Coordinate conversion has one
owner per direction and happens exactly once. Nova-specific semantics that
standard GLTF cannot represent require a future, explicit editor decision;
they must never be recovered from importer-private metadata or custom
properties.
