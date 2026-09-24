# OpenNova 3DI (Blender)

Imports and exports NovaLogic `.3di` models for Joint Operations and newer.
Both directions run the bundled `opennova-3di`, which reads and writes every
model through OpenNova's own 3DI code
([ADR 0047](../../../docs/adr/0047-blender-3di-exporter.md)); the add-on lays
the `.o3d` scene text out in Blender and reads it back. Pre-1.0 and
experimental.

## Install

`scripts/package_blender_addon.sh [out.zip]` builds the CLI and the zip
(default `build/opennova_3di.zip`). In Blender 4.2 or newer: Edit >
Preferences > Get Extensions > Install from Disk. Windows only for now.

## Importing

File > Import > NovaLogic 3DI (.3di), or Import .3di in the OpenNova sidebar.
Select one or more loose `.3di` files; their textures are looked up beside
them the way the game does (the stored name first, then the same name with
`.tga`, `.dds`, `.mdt`, `.pcx`, `.png`, `.jpg` or `.bmp`, any case, so a
`.tga` reference finds the `.dds` retail ships). Each model opens in a scene of
its own, laid out by the naming convention below, with LOD 1 and up hidden.
The scene exports again as it stands. Blender cannot open PCX or
archive-compressed textures; those references stay on the material with a
warning. Imported texture entries have Write TGA off, so an export never
writes a `.tga` over the texture the game already uses.

Import creates an editable authoring scene. Export rebuilds bullet faces from
the selected render LOD, regenerates skinned hit spheres and normalizes skin
weights. Retail face normals, per-face flags, zero-length vertex normals and
seam flags can therefore change. Armry01, US01 and ArmsG import and export,
but their rebuilt collision records can differ. Use
`opennova-3di compare` to inspect a rebuilt model before using it in game.

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
    LP01              light owned by part 01 (a point or spot light)
    CB01-colonly      collision volume on part 01 (its convex hull)
    CB01a-colonly     the next CB volume on part 01
    VC01-colonly      a vehicle-contact volume
    OB01-occonly      an occluder in section 01 (OS open, OP window, OP02-04 portal)
    PN02              Empty: part 2, a child of part 1
      02 Mesh0
      _02 center
      ~01a attach     helper: part 2's parent is part 01
F16_LOD1              Empty, _lod_index = 1 (same names; Blender's .001 is ignored)
```

A skinned model (a person, first-person arms) replaces the `PN##` empties with
one Armature under each LOD root whose bones are `BN01`, `BN02`, ... (the bone
head is the pivot, the bone parent the part parent). The skinned mesh is
`01 Mesh0` (the root owns skinned strips), with an Armature modifier and
`BN##` vertex groups (up to three weights a vertex; the export reads the rest
pose). The exporter appends each skinned mesh as its own part after the bones
(parent 0, pivot = the mesh object's origin), as the retail exporter wrote
bones first and mesh objects after them: that part holds the mesh bounds and
bullet faces. Each bone gets a hit sphere around the vertices it dominates.

To reuse retail animations, match the retail rig, since animations pair with
parts by index: JO's people share one rig of 19 bones plus the mesh part
(`opennova-3di info US01.3di` prints its pivots, or import it); the
first-person arms (`ArmsG`) are 37 arm bones plus the mesh part.

A first-person weapon (`gfx1` in weapon.def, e.g. `Mp5b_1st`) is a rigid
model whose part table IS the view-model rig: parts 01-37 (`PN01`-`PN37`,
empties with no mesh) reproduce the arm bones exactly, and the weapon's own
parts follow from `PN38` (the gun body under the right hand, `PN06`), as many
as the weapon needs. The weapon's `.adm` clips drive those parts by index and
the separate arms model (the player's Avatars.def `arms` graphic) is skinned
over the shared arm bones. Its points (`UPS38 MFLASH01`, `UPS38 bullet`,
`UPS38 bcasing`) sit on the gun body.

A rotated `PN##` empty is a rotation frame: its animation tracks turn about
the empty's own axes (Dblkhwk1's tail rotor is canted this way).

Materials are `Material_<index>_<SHADER>` (`Material_0_FF_ST_OP`,
`Material_1_FFP_GLASS`). With no texture entries, the first image texture node
is exported as a 32-bit TGA (file names at most 16 characters). A material's
texture list names every slot instead: slot 1 diffuse, slot 2 the detail
texture of an `FF_MT` shader (drawn on the mesh's second UV map), 3 and 4
normal maps.

## Panels

- **3D viewport sidebar > OpenNova**: Import, then the model name, output
  `.3di`, forward axis, the collision LOD (whose meshes also become the bullet
  faces; 0 = the most detailed), and Export.
- **Object properties** on a LOD root: the LOD threshold (projected radius;
  0 = the coarsest) and type (`gnrc`, `bldg`, `door`, `veh0`). On a `PN##`
  part: part animation tracks (rotation about the part's up, side or forward
  axis, scale, or translation, driven by an engine register such as
  `HELO_ROTOR` or by a spin or wave), and an optional raw PANM flags word. On a
  user point, light or occlusion mesh: its export order.
- **Light properties** on an `LP##` light: the colour generator (style, rate,
  phase or register, end colour), attenuation and the corona / terrain /
  object light switches.
- **Material properties**: shader, alpha test, two-sided, glass, emissive, the
  texture list, the collision surface type of its faces (metal 14, glass 15,
  ...), and the RGB / alpha / UV generators and texture flipbook. A
  register-driven flipbook selects its register by name.

Inspect any `.3di` (retail ones too) with `opennova-3di info <file> --verbose`;
`opennova-3di compare <a.3di> <b.3di>` tells whether two files hold the same
model.
