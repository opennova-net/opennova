# OpenNova 3DI Exporter (Blender)

Exports a Blender scene to a NovaLogic `.3di` model for Joint Operations and
newer. The add-on writes the `.o3d` scene text and runs the bundled
`opennova-3di`, which builds the model through OpenNova's own 3DI writer
([ADR 0047](../../../docs/adr/0047-blender-3di-exporter.md)). Pre-1.0 and
experimental.

## Install

`scripts/package_blender_addon.sh [out.zip]` builds the CLI and the zip
(default `build/opennova_3di.zip`). In Blender 4.2 or newer: Edit >
Preferences > Get Extensions > Install from Disk. Windows only for now.

## Laying out a scene

Objects are named by the NovaLogic ASE/OED convention
([`docs/threedi/scene-naming-contract.md`](../../../docs/threedi/scene-naming-contract.md)).
The model faces Blender's front view (-Y) by default.

```
F16_LOD0              Empty, custom property _lod_index = 0
  PN01                Empty: part 1 (its origin is the pivot)
    01 Mesh0          the part's render mesh
    _01 center        helper: the part pivot
    UPG01 ctrlx05     user point: type G, part 01, label ctrlx05 (+Z = facing)
    CB01-colonly      collision volume on part 01 (its convex hull)
    CB01a-colonly     the next CB volume on part 01
    VC01-colonly      a vehicle-contact volume
    PN02              Empty: part 2, a child of part 1
      02 Mesh0
      _02 center
      ~01a attach     helper: part 2's parent is part 01
F16_LOD1              Empty, _lod_index = 1 (same names; Blender's .001 is ignored)
```

A skinned model (a person) replaces the `PN##` empties with one Armature under
each LOD root whose bones are `BN01`, `BN02`, ... (the bone head is the pivot,
the bone parent the part parent). Its `## Mesh<n>` meshes carry an Armature
modifier and `BN##` vertex groups (up to three weights a vertex; the export
reads the rest pose). To reuse retail animations, match the retail rig: JO's
people all share one 20-bone rig (`opennova-3di info US01.3di` prints its
pivots), and animations pair with bones by index. Each bone gets a hit sphere
around the vertices it dominates; the bullet faces go on the mesh's part.

Materials are `Material_<index>_<SHADER>` (`Material_0_FF_ST_OP`,
`Material_1_FFP_GLASS`); the first image texture node is exported as a 32-bit
TGA (file names at most 15 characters).

## Panels

- **3D viewport sidebar > OpenNova**: model name, output `.3di`, forward axis,
  the collision LOD (whose meshes also become the bullet faces; 0 = the most
  detailed), and Export.
- **Object properties** on a LOD root: the LOD threshold (projected radius;
  0 = the coarsest). On a `PN##` part: part animation tracks (rotation, scale
  or translation driven by an engine register such as `HELO_ROTOR`, or a
  constant spin / wave).
- **Material properties**: shader, alpha test, two-sided, texture name
  override, the collision surface type of its faces (metal 14, glass 15, ...),
  and the RGB / alpha / UV generators.

Inspect any `.3di` (retail ones too) with `opennova-3di info <file> --verbose`.
