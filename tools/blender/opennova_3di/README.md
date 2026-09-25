# OpenNova 3DI (Blender)

Imports and exports NovaLogic `.3di` models for Joint Operations and newer.
Both directions run the bundled `opennova-3di`, which reads and writes every
model through OpenNova's own 3DI code
([ADR 0047](../../../docs/adr/0047-blender-3di-exporter.md)); the add-on lays
the `.o3d` scene text out in Blender and reads it back. Pre-1.0 and
experimental.

## Install

Download `opennova-blender-addon-windows-v<version>.zip` from
[releases](https://github.com/opennova-net/opennova/releases), or
`opennova-blender-addon-windows.zip` from a PR's **OpenNova CI builds** comment.
In Blender 4.2 or newer: Edit > Preferences > Get Extensions > Install from Disk.
Select the downloaded zip without unpacking it. Windows x64 only for now.

The **3DI executable** field automatically shows the bundled converter's path;
no manual setup is needed. Choosing another executable overrides it for that
scene. Clearing the field uses the bundled converter again.

To build locally, `scripts/package_blender_addon.sh [out.zip]` builds the CLI
and the installable zip (default `build/opennova_3di.zip`). PRs and releases
use the same packaging workflow and smoke-test the CLI extracted from the zip.
The CLI links the Visual C++ runtime statically, so no separate runtime
installation is needed. Its dedicated build directory is `build/blender-addon`.

## Importing

File > Import > NovaLogic 3DI (.3di), or Import .3di in the OpenNova sidebar.
Select one or more loose `.3di` files; their textures are looked up beside
them the way the game does (the stored name first, then the same name with
`.tga`, `.dds`, `.mdt`, `.pcx`, `.png`, `.jpg` or `.bmp`, any case, so a
`.tga` reference finds the `.dds` retail ships). Each model comes into the
current scene under a model root of its own, laid out by the naming
convention below, with LOD 1 and up hidden, and its output path named after
the imported file. Each model exports again as it stands. A file that cannot
be read is reported and skipped; the others still import. Blender cannot
open PCX or archive-compressed textures; those references stay on the
material with a warning. Imported texture entries have Write TGA off, so an
export never writes a `.tga` over the texture the game already uses.

Import creates an editable authoring scene, and export never relies on
anything import set up: collision volume planes, seam flags, tangents,
bounds, glass, emissive and the alpha pass are recomputed from the scene on
every export, by the rules the retired OED exporter used. Export rebuilds
bullet faces from the selected render LOD (face surfaces and flags come from
the materials, voted on import), regenerates skinned hit spheres and
normalizes skin weights, so retail face normals, zero-length or broken (NaN)
vertex normals and seam flags can change. Each collision volume imports as
one polygon per retail plane, so export reads the same planes back; a flat
ladder imports as the one polygon it is. The warnings list what the scene
cannot carry. Use `opennova-3di compare` to inspect a rebuilt model before
using it in game.

## Laying out a scene

Objects are named by the NovaLogic ASE/OED convention
([`docs/threedi/scene-naming-contract.md`](../../../docs/threedi/scene-naming-contract.md)).
The model faces Blender's front view (-Y) by default.

```
F16                   Empty: the model root (model name, output .3di, collision LOD)
F16_LOD0              Empty under the model root, custom property _lod_index = 0
  PN01                Empty: part 1 (its origin is the pivot)
    01 Mesh0          the part's render mesh
    _01 center        helper: the part pivot
    UPG01 ctrlx05     user point: type G, part 01, label ctrlx05 (+Z = facing)
    LP01              light owned by part 01 (a point or spot light, aimed down its -Z)
    CB01-colonly      collision volume on part 01 (a convex mesh)
    CB01a-colonly     the next CB volume on part 01
    VC01-colonly      a vehicle-contact volume
    OB01-occonly      an occluder in section 01 (OS open, OP window, OP02-04 portal)
    PN02              Empty: part 2, a child of part 1
      02 Mesh0
      _02 center
      ~01a attach     helper: part 2's parent is part 01
F16_LOD1              Empty, _lod_index = 1 (same names; Blender's .001 is ignored)
```

A part's `~PPx attach` helper names its parent part `PP` (`00` for none, the
part's own number for itself, both of which retail ships). In the collision LOD
the helpers are also the attach points the game stores for its sections, one
per helper; without any, export derives them from the part pivots. Export
warns about any object whose name it does not use.

Add Model (in the sidebar) makes a model root and its `_LOD0` root to start
from.

A skinned model (a person, first-person arms, the M1A1's hull) replaces the
`PN##` empties with one Armature under each LOD root whose bones are `BN01`,
`BN02`, ... (the bone head is the pivot, the bone parent the part parent). A
skinned mesh `## Mesh0` has an Armature modifier and `BN##` vertex groups (up
to three weights a vertex; the export reads the rest pose), and `##` names the
part its geometry belongs to. A person's or the arms' mesh is a mesh part
numbered after the bones (`ArmsG`: 37 bones, then `38 Mesh0`; parent 0, pivot
= the mesh object's origin), as the retail exporter wrote bones first and mesh
objects after them: that part holds the mesh bounds and bullet faces, and each
bone's section is bounded by every vertex it moves. A vehicle hull puts its
geometry on the bones themselves (`dM1A1`: the hull on `BN01`, each wheel on
its own bone in the collision LOD), and each bone's section takes its own
bullet faces. A bone's part animation (tracks, flags and the track frame its
tracks turn about) is in the Bone properties.

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

## Several models

A scene holds any number of models. Export Model writes the model of the
active object, and Export All Models writes each to its own output path. Every
model exports in its root's own frame, so moving, parenting or mounting a
model does not change what it exports. Two settings on a model root show
models together the way the game draws them. They are display only, and
export ignores them:

- **Bones follow**: a skinned model's bones follow another model's parts of
  the same index. The game draws a first-person gun and the player's arms
  with the gun's part matrices, so posing the gun's `PN##` parts poses the
  arms. Importing a `_1st` gun together with `ArmsG` pairs them.
- **Mount on** and **At user point**: the model sits on another model's user
  point, facing its direction, the way an ITEMS.DEF `addeweap` child does
  (the M1A1's turret `m1trret` on the hull `dM1A1`'s `ewep01`). Posing or
  moving the parent carries the child. An unknown point puts the child on the
  parent's root, as in game.

Materials are `Material_<index>_<SHADER>` (`Material_0_FF_ST_OP`,
`Material_1_FFP_GLASS`): the name carries the shader, and the material
panel's Shader field (any tag the engine knows, searchable) renames the
material. A material with no tag in its name gets the default for its
textures (`FF_ST_OP` one, `FF_MT_OP` two, `FFP_GLASS` none, which a mesh
without a material takes too). Glass shaders are glass, `*_LUM` shaders
emissive, blending shaders (glass among them) draw in the alpha pass, and a
bump shader (`VS_PHONGT`, `VS_DOT3DIFF`, ...) gets tangents derived from its
UVs. With no texture entries, the image texture wired to the Principled
BSDF's Base Color (else the first image texture node) is exported as a
32-bit TGA named after the image (its first 12 characters, then `.tga`; two
images may not share a file name). A material's texture list names every
slot instead: slot 1 diffuse, slot 2 the detail texture of an `FF_MT` shader
(drawn on the mesh's second UV map; the first is the one Blender renders
with), 3 and 4 normal maps. Write TGA writes `.tga` entries only.

## Panels

- **3D viewport sidebar > OpenNova**: Import and Add Model, the forward axis,
  then the active object's model: its name, output `.3di`, the collision LOD
  (whose meshes also become the bullet faces; 0 = the most detailed), Bones
  follow, Mount on, and Export Model. Export All Models writes every model.
- **Object properties** on a model root: the same model settings. On a LOD root: the LOD threshold (projected radius;
  0 = the coarsest) and type (`gnrc`, `bldg`, `door`, `veh0`). On a `PN##`
  part: part animation tracks (rotation about the part's up, side or forward
  axis, scale, or translation, driven by an engine register such as
  `HELO_ROTOR` or by a spin or wave), and an optional raw PANM flags word. On a
  user point, light or occlusion mesh: its export order.
- **Bone properties** on a `BN##` bone: its part animation, as on a `PN##`
  part, plus the track frame.
- **Light properties** on an `LP##` light: the colour generator (style, rate,
  phase or register, end colour), attenuation and the corona / terrain /
  object light switches. A spot light's cone points down the light's -Z, as
  Blender draws it, and an unrotated light points straight down.
- **Material properties**: shader (with what it implies), the bullet faces'
  surface type (metal 14, glass 15, ...) and flags (bullets pass, front only:
  a bullet from behind passes), alpha test, two-sided, the reflection colour,
  the texture list, and the RGB / alpha / UV generators and texture flipbook.
  A register-driven flipbook selects its register by name.

## Animations

A model's clips live on its rig, and the whole set writes at once:
**Export Animations** makes the `.adm` table the model root names (empty: one
named after the model, beside the `.blend`) and one `<clip>.bad` beside it for
every clip. **Export All Animations** does that for every model with a rig,
and skips, with a warning, a rig that carries no clip set of its own (the arms
beside a first-person gun). **File > Import > NovaLogic Animations** reads a
`.adm` (or a single `.bad`) back onto the active model's rig.

- **A clip is an Action.** Push each one onto its own NLA track; the track order
  is the set's order and the Action's name is the `.bad` file name. Export plays
  each Action through its strip's action slot (Blender 4.4 and newer), so an
  Action keyed on another rig exports its own motion, and it refuses a rig in
  NLA tweak mode. The clip's own settings live in the Dope Sheet sidebar's
  OpenNova panel: its rate (retail ships 30 everywhere), whether it loops,
  whether it carries per-bone translations (a bolt, a magazine, a rig that
  slides), the unwitnessed flag bit 3, and a frame count longer than the
  Action, whose extra frames hold its last pose.
- **Each clip exports on its own.** A channel a clip does not key is at rest:
  a bone it leaves alone keeps its rest pose, `!RM` stays at the origin and the
  trigger and capsule are 0, whatever the clip before it did. A bone that
  follows another model's parts (**Bones follow**) follows its own clip while
  the set exports.
- **The table** is the row list on the model root: a slot (`anim_reset`,
  `anim_walk_forward`, ...) and the clips that answer it. Several clips on one
  row are a ring the game rotates through, and it serves a row from its LAST
  entry back. The reset row's first clip is the bind every clip is measured
  against.
- **Root motion** is the bone `!RM`: key it along the path the body travels and
  the clip carries the step between each pair of frames. The body itself
  animates in place; the game moves the entity by those steps. Any bone named
  `!something` is not a part, so control bones live there too; a `BN##` bone
  under one takes the nearest `BN##` above it as its part parent.
- **Events** are the rig's keyed **Trigger** word: 1 and 2 place the left and
  right footstep, 4, 8 and 16 fire the ammo rows, and 0x20 upwards play the six
  foley sounds of the body's sound profile. `opennova-3di catalog` lists them.
  The word is 32 bits, so a word with the top bit set shows as a negative
  number (a version 0 clip's 0xffffffff is -1).
- **The rest pose is the bind.** A clip's channel is the bone's own rotation in
  the model's frame. The game binds every clip of a table to the reset clip: a
  bone deforms by `key * bind^-1`, the bind being the reset clip's first key.
  So the first table imported onto a rig that holds no clip turns each rest
  bone onto that key, and a clip then poses the rig exactly as the game draws
  it. The bone heads, lengths and weights do not move, so the model still
  exports the same model. A rig that already holds clips keeps its rest, since
  their Actions are keyed against it, and a single `.bad` carries no table, so
  it leaves the rest and the model's rows as they are. Bone names come from the
  clips (a `.3di` carries none), and their vertex groups are renamed with them;
  a lower-case `bn38 bone` becomes `BN38 bone`.
- **Import keys what the game plays.** Every clip is keyed on each frame of its
  length: a bone that holds a key over several frames gets the pose the game
  blends between its keys there, and a bone that stops keying early holds its
  last key. A re-export therefore keys every frame (`DVFLEE1E`, `DT1RST` and
  `stgr_RST` come back with more keys and the same poses), and keys past the
  clip's own length (`M60_1i`), which the game never plays, are left out. The
  import lists these in its warnings, and a failed import leaves the scene as
  it found it.

**A first-person weapon** animates its own parts rather than bones, so its clips
get an armature named `!Rig` whose bones mirror them; import builds it. Each
part then hangs from the LOD root and follows its bone through an `O3D follow`
constraint, which the rig's Rest Position turns off, and a `~PPx attach` helper
keeps the part hierarchy (a part that already has one keeps it; a new one sits
at the part's pivot). The gun's parts move in the viewport, and the model still
exports byte for byte as it did before the import. The arms follow the
weapon's parts by index (pick the arms model's **Bones follow**), so a
first-person set belongs to the weapon, and exporting it from the arms alone
writes only the channels the arms have bones for.

To reuse retail's own clips, match the retail rig: JO's people are 19 bones plus
a mesh part, a first-person weapon 40 parts, and a clip's channels pair with the
model's parts by index.

`opennova-3di anim info <file> --verbose` prints a table or a clip, and
`opennova-3di anim compare <a> <b>` tells whether two sets hold the same
animation.

## Collision volumes

A `-colonly` mesh is a convex volume: the game keeps the solid all its face
planes bound, so a concave or twisted mesh loses whatever sticks out. Export
warns with the volume's name and how far it reaches outside; split such a
mesh into convex pieces. A ladder (`CL`) faces the plane of its last face; a
flat ladder is a single polygon facing the way the ladder does.

Inspect any `.3di` (retail ones too) with `opennova-3di info <file> --verbose`;
`opennova-3di compare <a.3di> <b.3di>` tells whether two files hold the same
model.
