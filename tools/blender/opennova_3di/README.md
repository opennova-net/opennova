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
In Blender 5.0 or newer: Edit > Preferences > Get Extensions > Install from Disk.
Select the downloaded zip without unpacking it. Windows x64 only for now.

The add-on runs the `opennova-3di` bundled with it; no setup is needed. To run
another build, name it in the add-on's preferences (Edit > Preferences >
Add-ons > OpenNova 3DI); clearing the field runs the bundled one again.

To build locally, `scripts/package_blender_addon.sh [out.zip] [version]` builds
the CLI and the installable zip (default `build/opennova_3di.zip`). A version
(a release tag such as `v0.0.10`, the leading `v` dropped) is stamped into the
zipped `blender_manifest.toml`, which Blender reports as the add-on's version;
without one the zip keeps the tracked manifest's version. PRs and releases use
the same packaging workflow and smoke-test the CLI extracted from the zip.
The CLI links the Visual C++ runtime statically, so no separate runtime
installation is needed. Its dedicated build directory is `build/blender-addon`.

## Importing

File > Import > NovaLogic 3DI (.3di), or Import .3di in the OpenNova sidebar.
Select one or more loose `.3di` files; their textures are looked up beside
them the way the game does (the stored name first, then the same name with
`.tga`, `.dds`, `.mdt`, `.pcx`, `.png`, `.jpg` or `.bmp`, any case, so a
`.tga` reference finds the `.dds` retail ships). Each model comes into the
current scene under a model root of its own, laid out in the scene shape
below, with LOD 1 and up hidden, and its output path named after the
imported file. A static model's parts come in as `PN##` empties, a skinned
model's as the bones of one armature. A first-person gun imported together
with its arms (the arms' bones the gun's first parts, as `ArmsG`'s are for
every stock gun) comes in as a rig, and the arms deform with it. Each model
exports again as it stands. A file that cannot be read is reported and
skipped; the others still import. Blender cannot open PCX or
archive-compressed textures; those references stay on the material with a
warning. An imported texture is the file it was read from: export names it the
way the game finds it and copies it beside the `.3di` rather than writing a
new one, and never replaces a different file of that name.

Import creates an editable authoring scene, and export never relies on
anything import set up: collision volume planes, seam flags, tangents,
bounds, glass, emissive and the alpha pass are recomputed from the scene on
every export, by the rules the retired OED exporter used. Export rebuilds
bullet faces from the selected render LOD (face surfaces and flags come from
the materials, voted on import; a face that disagrees with its material's
vote keeps its own on its polygon, on a rigid model), so retail face normals, zero-length or
broken (NaN) vertex normals and seam flags can change. A skinned model's hit
spheres and their bounds come back as helpers export writes again. Each
collision volume imports as one polygon per retail plane, so export reads the
same planes back; a flat ladder imports as the one polygon it is. The
warnings list what the scene cannot carry. Use `opennova-3di compare` to
inspect a rebuilt model before using it in game.

## Laying out a scene

A model is a model root (an Empty) holding one LOD root per level of detail
(an Empty with the custom property `_lod_index`), and a LOD's parts are
either `PN##` empties or the bones of the model's one armature
([`docs/threedi/scene-naming-contract.md`](../../../docs/threedi/scene-naming-contract.md)).
Everything else belongs to the part it sits on: a mesh of any name under a
`PN##` or parented to a bone is that part's geometry, and so are the user
points, lights and helpers there. The model faces its root's -Y (Blender's
front view); turn the model root to face it another way.

A static model (a prop, a building, a vehicle whose parts turn by their
tracks):

```
F16                   Empty: the model root (model name, output .3di, collision LOD)
F16_LOD0              Empty under the model root, custom property _lod_index = 0
  PN01                Empty: part 01, the root, at the model origin (its origin is the pivot)
    Fuselage          a mesh of any name: part 01's geometry
    UPG ctrlx05       user point: type G, label ctrlx05, on part 01 (+Z = facing)
    LP                light on part 01 (a point or spot light, aimed down its -Z)
    CB-colonly        collision volume in part 01's section (a convex mesh)
    CBa-colonly       the next CB volume there
    CX-colonly        a type 0 volume (any other C, D, L or V code)
    OB-occonly        an occluder in part 01's section (OS open, OP window, OP-04 portal)
    PN02              Empty: part 02, a child of part 01
      Rotor           its geometry
    PN03              Empty: part 03, which draws nothing
      _center         a mesh: its first vertex seeds the part's bounds
F16_LOD1              Empty, _lod_index = 1 (its own PN## empties; Blender's .001 is ignored)
```

An animated or skinned model (a first-person gun, arms, a person) holds one
armature under its LOD 0 root. Its bones named `BN##` (a label may follow,
`BN16 L Hand`) are the parts: the bone head is the pivot, the nearest `BN##`
bone above it the parent. Other bones are no part: a `Root` on the ground
(see Animations) or the control bones a rigger adds (Deform off). Build the
bones in Edit Mode with any names and press **Number Parts**: it numbers the
bones that deform in hierarchy order, parents first, keeping each name as the
label, and renames their channels in the model's clips (after Symmetrize or a
duplicate, press it again). Parent a rigid mesh to its bone (Ctrl+P > Bone);
give a skinned mesh an Armature modifier on the rig and vertex groups
(Ctrl+P > With Automatic Weights). A first-person gun and its arms share the
gun's armature:

```
AK_1st                model root
AK_1st_LOD0           LOD root
  AK_1st Rig          the armature: BN01..BN37 the arm bones, BN38 Body, BN39 Magazine, ...
    Receiver          a mesh parented to bone BN38 Body: part 38's geometry
    UPS MFLASH01      a user point parented to BN38 Body
  Arms                model root, under the gun's root with no offset
    Arms_LOD0         LOD root
      Arms Mesh       the arms mesh: an Armature modifier on AK_1st Rig, vertex groups BN01..
      _05 hit         a helper of the arms: under the LOD root, naming its part
```

The arms model's parts are the rig's bones up to the highest one its weights
use, since the game draws the arms with the gun's part matrices. Arms built
with an armature of their own join a gun's rig with **Deform with Rig of**.
The arms are the player's own (Avatars.def, per character): a gun whose parts
01 to 37 are not the stock arms' rig draws every character's arms wrong, and
export says so. A first-person gun has at most 64 parts (the game's bone
arrays), and its arms no more than the gun. **Add Animation Rig** turns a
static model's `PN##` parts into such an armature, everything on a part
parented to its bone, so clips can animate it.

A skinned mesh deforms on up to four bones a vertex (Weights > Limit Total);
export writes them dominant first, the way the game blends them, and refuses
a vertex with more, one with no weight, and a weight on a bone that deforms
but is no part (Root, a control bone). A mesh parented to a bone of a skinned
model is skinned wholly on that bone. A skinned model keeps its skinned
geometry on its root part, or with **Mesh part** on (the retail layout of
US01 and ArmsG) on a part of its own after the bones, whose pivot is the
mesh's origin (on the ground under the hips) and whose collision section
holds the bullet faces. Each bone's collision section is its hit sphere,
which export makes around the vertices the bone moves; a `_## hit` Empty
drawn as a sphere on the bone (its size the radius) or a `_## bounds` box
Empty sets the sphere or its box instead, and a bone whose **Hit sphere** is
off (Bone properties) has none. Import makes these Empties only where the
file's sphere or box is not the one export would make, so a model exported
from Blender comes back without them.

Helpers take their part from where they sit, and the `##` in their names is
optional: `UPG05 grip` on part 05 is fine, `UPG04 grip` there is refused.
`UPx00` is a user point on no part. In a LOD whose bones another LOD holds (a
skinned model's later LODs, arms on a gun's rig) a helper sits under the LOD
root and its name must give its part (`_05 hit`). Other helpers:

- `_center` (a mesh): on a part that draws nothing (a first-person gun's arm
  parts often do), its first vertex is the point the part's bounds sit on,
  and in the collision LOD the section's one collision vertex, as the retail
  tool seeded such parts. Import makes one for every such part.
- `_attach` (an Empty): in the collision LOD, an attach point the game stores
  (CXLT; a palm's broken pieces pivot on them). With any in that LOD, export
  writes an attach point for every section after the root (every section on
  a skinned model), at the section's `_attach` or, where it has none, at its
  pivot; without any, the builder puts each at its section's pivot. A retail
  table that is not one per part (none at all in Chair03X, a row for the root
  too in the first-person guns) imports with the model's **Attach points** set
  to the attach helpers: then each `_attach` is a row, in export order.
- `_sphere` (an Empty, drawn as a sphere): on an occlusion mesh, the record's
  sphere when it is not the one the mesh's vertices give (its origin the
  centre, its display size times its scale the radius). Import makes one only
  for the 206 retail models that store each occlusion centre mirrored across
  the model's y.

The root part's pivot is the model origin. A part numbered before its parent
exports with a note (the game reads that parent before posing it; Number
Parts avoids it). A part's **Parent** (in its closed "Stored as" panel) keeps
what a hierarchy cannot say, a part that names itself or no part as its
parent, as some retail models store; only import sets it. Its **Track
parent** (the same panel) is the parent its PANM row names, apart from the
part's own: a model with part tracks has a PANM table, and with one the game
re-places every part without a track about that row parent's pose, keeping
only the part's turn, so a clip's travel of a part (a first-person gun's
magazine leaving it, its bolt sliding) is lost unless the row names the part
itself (Itself). Export warns about any object it does not use.

**Add Model** (in the sidebar) makes a model root and its `_LOD0` root at the
origin. **Add Part from Selection** puts the selected objects on a new `PN##`
at the active object's origin, under the part it sits on (a LOD's first part
is `PN01` at the model origin). **Add LOD** adds the model's next LOD root;
give each LOD but the last a threshold, falling from LOD 0.

Turn off **Generate bullet faces** on models such as first-person arms that
do not need triangle collision. Render geometry, skin weights, bone bounds
and authored collision volumes are still exported.

Strips split when the 65,535-index limit (and, skinned, the 16-bone palette)
is reached; large meshes need no manual splitting.

To reuse retail animations, match the retail rig, since animations pair with
parts by index: JO's people share one rig of 19 bones (plus the mesh part);
the first-person arms (`ArmsG`) are 37 arm bones (plus the mesh part), the
first 37 parts of every stock first-person gun.

A rotated `PN##` empty is a rotation frame: its animation tracks turn about
the empty's own axes (Dblkhwk1's tail rotor is canted this way). A bone keeps
its track frame in its Bone properties.

## Several models

A scene holds any number of models. Export Model writes the model of the
active object to its output path (empty: the model root's name beside the
`.blend`, at most 15 characters, as a game archive holds it), File > Export >
NovaLogic 3DI asks for the file first, and Export All Models writes each to
its own output path, refusing two that would write one file. Every model
exports in its root's own frame, so moving, parenting or mounting a model
does not change what it exports.

Arms deforming with a first-person gun's rig stand where the gun stands:
their root sits under the gun's root with no offset, so posing the gun poses
the arms, and export refuses arms that stand elsewhere. Their parts take the
gun's pivots and track frames.

**Mount on** and **At user point** (display only, export ignores them): the
model sits on another model's user point, facing its direction, the way an
ITEMS.DEF `addeweap` child does (the M1A1's turret `m1trret` on the hull
`dM1A1`'s `ewep01`). Posing or moving the parent carries the child. An unknown
point puts the child on the parent's root, as in game.

## Materials

A material exports the way Blender 5.0 or newer draws it, through its
Principled BSDF:

- **Base Color**: the image whose colour reaches Base Color is the diffuse
  texture, through reroutes, node groups, Mix nodes and other colour nodes
  (export names those, since the game draws the image as it is). An image read
  on the mesh's second UV map (a UV Map node naming it) is the detail texture,
  which the game multiplies in at twice its value (the `FF_MT` shaders). A
  roughness, normal or mask image is never taken for the diffuse texture. With
  no image, the Base Color (or an RGB node's colour) is written as an 8 by 8
  swatch texture, and a mesh without a material draws in Blender's default
  grey.
- **Normal**: a Normal Map node in tangent space reading an image is the
  normal map, an `.mdt` in slot 3. Wired straight, the image holds Blender's
  normals (green up, as Blender bakes them), and export inverts its green for
  the game, whose tangent frame runs down the texture. Read through a green
  flip (Separate Color, 1 minus Green, Combine Color) it is a green-down file,
  written as it is. The node's strength, a Bump node and object or world space
  do not reach the game.
- **Backface Culling** off is two-sided, for drawing and, unless the material's
  Both sides setting says otherwise, for the bullet faces.
- **Alpha**: a Math node, Greater Than (Less Than for the inverted test)
  against a constant threshold, on the Principled Alpha is the alpha test at
  that threshold, Blender's own alpha clip.
- **Render Method** Blended draws the strips in the alpha pass.
- **Emission** on asks for a glowing shader.

The material panel's **Shader** names the engine shader (any tag in its
table, searchable). Left empty, export takes OED's rule, the first shader of
the model's kind drawing that many textures (`FF_ST_OP`, `FF_MT_OP`,
`VS_SKBASIC`), among the shaders that draw the settings above: Blended picks a
blending one (`FF_ST_AB`), Emission a `*_LUM` one, a U or V generator a `#UV`
one (only those move UVs), a normal map a bump one (`VS_DOT3DIFF`,
`VS_SKBUMPDIFFT`), and export says what no shader of the kind draws. A shader
named in the panel keeps OED's rule: `FFP_GLASS` draws no texture. Glass
shaders are glass, `*_LUM` shaders emissive, blending shaders draw in the alpha
pass, and a bump shader gets tangents derived from the render UV map, so its
meshes need one with area. **Export order** is the material's index in the
model (import sets it; -1 sorts a material after the ordered ones, by first
use). A material in a mesh's slots that no face draws with exports only with
an Export order: 208 JO models keep such materials, and import puts them in
the slots of the model's first mesh. The name is free.

An image loaded unchanged from a texture file the game reads (`.tga`, `.dds`,
`.mdt`, `.pcx`) is that file: its row names it the way the game finds it
(retail's `x.dds.tga` finds `x.dds`), and export copies it beside the `.3di`
unless another file already has its name there. Any other image (a PNG, a
painted, packed or generated image, a bake) is written as a 32-bit TGA named
after the model and the material, `<model>_<material index>.tga`, with `d`
added for a detail texture and `n.mdt` for a normal map: ASCII, one dot and
at most 15 characters, the names retail packs. Float images (16-bit PNG and
TIFF, EXR, float bakes) are sRGB-encoded as their 8-bit files would be;
non-colour data keeps its values. UDIM, image sequence and movie images are
refused, and a textured mesh needs a UV map. Every name, image and pixel is
checked before anything is written, the textures are written only once
`opennova-3di` has built the model, and the models of one Export All cannot
write two images under one name. Nor can two models writing into one folder
whose names cut to one stem (`gunmodel_a` and `gunmodel_b` both give
`gunmodel_0.tga`): export refuses each, naming the other, until one has
another Model name or folder. **Write textures** off writes none.

The texture list carries only what the nodes cannot say: flipbook frames, row
flags (1 a flipbook frame, 2 the render-state override), an object-space or
height-map normal texture, a file Blender cannot open. A slot it lists is taken
from it, not from the nodes. A row's file name is printable ASCII, at most 16
characters, without a folder; **Write** writes its image under that name,
which must then be `<stem>.tga` or `<stem>.mdt` in at most 15 characters. A
row may also name no file, as 63 rows of the JO models do (`M24_1st`'s lens
keeps an empty slot 2 row): it exports as it is, with a warning when its
shader samples that slot. A material holds at most 24 rows.

Import lays a material out the same way: a slot's lone plain row becomes its
image node (a tangent-space shader's `.mdt` behind a green flip into a Normal
Map node), the other rows stay in the list, and the flags become Backface
Culling, the Math node, the render method and Emission.

## Panels

- **3D viewport sidebar > OpenNova**: Import .3di, Import Animations and Add
  Model, Write textures, then the active object's model: its name, output
  `.3di`, the collision LOD (whose meshes also become the bullet faces; 0 = the
  most detailed), Generate bullet faces, Mesh part (a skinned model), Mount on,
  Add LOD, Add Part from Selection or Add Animation Rig (a static model),
  Number Parts (a model with its own rig) and Deform with Rig of (a skinned
  one), Export Model, and its Animations box. Export All Models writes every
  model, and Export All Animations every rig's clip set.
- **Dope Sheet sidebar > Action > OpenNova 3DI** on a rig playing an Action: its
  rows, Manual Frame Range, Cyclic and Clip rate, its event trigger markers
  and Add Event Trigger, Assign Weapon Action and the timing markers of the
  actions it answers, and a closed Raw section with flag bit 3.
- **Object properties** on a model root: the same model settings. On a LOD
  root: its index, the LOD threshold (projected radius in pixels; the last
  LOD's 0) and type (`gnrc`, `bldg`, `door`, `veh0`). On a `PN##` part: part
  animation tracks (rotation about the part's up, side or forward axis, scale,
  or translation, driven by an engine register such as `HELO_ROTOR` or by a
  spin or wave), and in a closed "Stored as" panel its stored Parent, Track parent and a
  PANM flags word to write instead of the one the tracks imply (import never
  sets it: a stored word the tracks do not imply is reported and dropped). On
  any other object of a model: the part it sits on, and on a
  user point, light, occlusion mesh or attach helper its export order.
- **Bone properties** on a `BN##` bone: its part number, Hit sphere (a skinned
  model's) and part animation, as on a `PN##` part, plus the track frame, and
  in a closed Clip bind panel the bind frame its clip keys are measured from,
  when an imported table left one.
- **Light properties** on an `LP` light: the colour generator (style, rate,
  phase or register, end colour), attenuation and the corona / terrain /
  object light switches, the other flag bits in a closed panel. A spot light's
  cone points down the light's -Z, as Blender draws it, and an unrotated light
  points straight down.
- **Material properties**: the shader (with what it implies) and export
  order, what Blender's settings give (two-sided, the alpha test, the alpha
  pass, the glow), the bullet faces' surface picked by name (Metal, Glass,
  Wood: the effects row a round that hits them plays, the engine's own table
  through `opennova-3di catalog`; Water, Glass, Cloth, Foliage and Flesh let
  rounds go on through at a cost of their energy) and flags (both sides,
  bullets pass, front only: a round from behind passes), set once for every
  face the material makes; a model whose faces carry their own (an imported
  retail model's that disagree with their material's) says so there, "Mixed: 3
  of its 120 faces keep their own (3 Glass)", with **Make all** and **Select
  them** (the faces keep theirs on their polygons' `o3d_own_surface` and
  `o3d_own_face_flags` attributes, each the value plus one, 0 the material's,
  so a polygon Blender makes itself, a new face or a mesh joined in, takes the
  material's; export writes them as they are), the other
  flag bits, the reflection colour, the texture rows the
  nodes cannot give, and the RGB / alpha / UV generators and texture
  flipbook. A flipbook with frames on anim type 1 reads a register, which it
  selects by name; any other keeps its frame time.

## Animations

A model's clips pose its rig: the `BN##` bones of the one Armature under its
LOD 0 root. **Add Animation Rig** turns a static model's `PN##` parts into
one (a bone at each pivot, everything on a part hung from its bone). A
first-person gun and its arms share the gun's rig, so the gun's clips pose
both and the arms have none of their own. **Export Animations** writes the
`.adm` table the model root names (empty: one named after the model, beside
the `.blend`) and every clip beside it; **Export All Animations** does that
for every model with a rig of its own. **File > Import > NovaLogic
Animations** reads a `.adm` (or a single `.bad`) onto the active model's rig.

- **The table is the model root's rows.** A row is an anim slot and the
  clips that answer it: `anim_reset`, `anim_wpn_fire`, `anim_walk_forward`,
  one of the engine's 252 slots (the Slot field searches them). The game
  drops a row naming any other slot, so export refuses it, and one slot takes
  one row. Several clips on a row are a ring the game plays from its last
  clip back, one step at every play and every loop. A row names its slot by
  what follows its key's first five characters, so `ANIM_RESET` is the reset
  row too.
- **A clip is an Action.** The Actions the rows name are the clip set, with
  no NLA track needed; an Action on no row is not exported (export lists
  them). Each plays through its slot for the rig, the one it was keyed on,
  so one Action keyed on two rigs exports each rig's own motion. A clip's
  file is named after the table and its slot: `<table>_<code>.bad`, the code
  `rst` for the reset, `i`, `ei`, `f`, `rc`, `r`, `e`, `swt`, `swf`, `swr`,
  `su` and `sd` for the weapon slots (idle, empty idle, fire, recoil, reload,
  empty, draw, holster, fire mode, aim in, aim out) and `s<slot number>` for
  the others. A row's second clip adds `2` (`tfa_akm_i2`), after an
  underscore when the code ends in a digit (`tfa_s1_2`). A retail archive
  holds a name of at most 15 characters, so keep the table's name short: the
  table `tfa_akm.adm` leaves room for `tfa_akm_swt.bad`, and export refuses a
  longer name, saying which.
- **A clip's settings are its Action's.** Its frames are the Action's Manual
  Frame Range (without one, its keyed range), and it loops when the Action
  is Cyclic; the Dope Sheet sidebar's OpenNova panel shows both, and its Clip
  rate (retail ships 30 everywhere). Blender plays every clip at the scene's
  rate, so export notes a clip whose rate differs. A loop needs a rate under
  62 times its frames, or the game steps past its end within a tick. A bone
  may turn and move, but not scale (a scaled bone is refused). A clip carries
  translations when a bone moves off its rest offset from its parent (a
  bolt, a magazine), and the reset clip then carries them too, since the
  game moves a bone only when both do. The panel's closed Raw section holds
  the unwitnessed flag bit 3.
- **The reset starts at rest.** The reset row's last clip is the bind every
  clip is measured against: a bone deforms by `key * bind^-1`, the bind being
  that clip's first key. A reset clip must therefore start at the rest pose
  (export names a bone more than 0.01 degree off), and a clip then poses the
  rig in Blender exactly as the game draws it. A table whose reset row names
  no clip, or that has none, gets `<table>_rst`, one looping interval of the
  rest pose at 30 fps. Bones may point any way: a bone keeps the frame an
  imported table measured its keys from as its **bind frame** (below), and
  export measures its keys from that frame again, so they come back as the
  table stored them.
- **Each clip exports on its own.** A channel a clip does not key is at
  rest, whatever the clip before it did. **Edit Clip** (the button beside
  each clip in the rows) plays a clip the same way, through its slot, with
  the timeline's preview range on its frames; the scene's rate and the NLA
  stay as they are.
- **A body stands on Root.** A person's rig has a `Root` bone (any case) on
  the ground as its top bone, the hips (`BN01`, the model origin) below it,
  and a head. Move the body over the ground with Root and bob it with the
  hips. Export measures every frame from the pose: the bottom is the hips'
  height above Root, the top the head's, and the step is how far the hips
  move to the next frame, which is how far the game moves the body. The head
  is the **Head** field in the Animations panel, or else the one bone whose
  name ends in `head` (`BN15 Head`); with no head, the top is the bottom. As
  in every retail clip, a loop's last two events repeat its first and a
  one-shot's stand still. Root is no part, and neither is any other bone not
  named `BN##`, but **Number Parts** numbers every bone that deforms except
  Root: turn Deform off on a control bone.
- **A first-person rig has no Root.** Its clips pose the model where it
  stands: `BN01` moves by its own translation, and every event stands still
  at the hips' rest height above Blender's ground (Z = 0), which is what
  retail's first-person clips carry. The game never reads a first-person
  clip's events.
- **Event triggers are markers.** A marker on the Action named after an
  event bit sets that bit on its frame: `FOOT_LEFT` and `FOOT_RIGHT` place
  the footsteps, `FIRE_PRIMARY`, `FIRE_SECONDARY` and `FIRE_MARKER3` fire the
  ammo rows, and `FOLEY_1` to `FOLEY_6` play the six foley sounds of the
  body's sound profile (`opennova-3di catalog` lists them). **Add Event
  Trigger** places one at the playhead. Export notes a marker it does not
  know and refuses one past the clip's frames. First-person clips need none.
- **Import keys what the game plays.** Every clip becomes an Action keyed for
  the rig on each frame of its length, with its Manual Frame Range, Cyclic
  for a loop, its rate, and a marker per trigger bit. A bone that holds a key
  over several frames gets the pose the game blends between its keys there,
  and a bone that stops keying early holds its last key, so a re-export keys
  every frame (`DVFLEE1E`, `DT1RST` and `stgr_RST` come back with more keys
  and the same poses); keys past the clip's own length (`M60_1i`), which the
  game never plays, are left out. The table's rows merge into the model's by
  slot, and the model's other rows stay. A clip a row already names under the
  name of one being imported (a set imported again) is replaced everywhere it
  is used. The scene takes the reset clip's rate. The warnings list what the
  scene cannot carry, and a failed import leaves the scene as it found it (a
  static model it turned into a rig keeps its rig).
- **Import onto a static model** turns its `PN##` parts into the rig first,
  as Add Animation Rig does. Every rig the add-on builds points each bone
  toward its first child (else on from its parent), and import never turns a
  rest bone. Where the table's bind points a bone another way (retail's
  point each arm and finger bone back toward its parent), the bone keeps that
  bind as its **bind frame** (Bone properties, the closed Clip bind panel) and
  each clip is keyed as the turn it makes from it: the clips pose the rig as
  the game draws them, and a re-export writes the keys and bone tables the
  table had. Nothing hung from a bone moves, so the model still exports the
  same model. The bind frames are the last imported table's; a lone `.bad`
  and a table without a reset row leave them. Bone names come from the clips
  (a `.3di` carries none), and their vertex groups are renamed with them; a
  lower-case `bn38 bone` becomes `BN38 bone`.
- **Import stands the rig on the ground.** When a set moves or bobs the body,
  each frame keys the hips at its bottom and a `Root` bone (made at the ground
  under the hips when the rig has none) along the steps, so a planted foot
  stays put. A set that stands still, as every first-person set does, needs
  no Root. A model at the world origin is raised so the ground is Blender's
  Z = 0, and arms on its rig rise with it, so a gun and its arms overlay a
  body at the hips. This is display only: every export reads a model as if it
  stood at the origin. A clip whose stored top is more than 3 cm from the
  head's height is named in the warnings, and so is a first-person clip whose
  top stands above its bottom (a third of retail's do, by a rule nothing has
  shown); a re-export writes the rig's own measure.

To reuse retail's own clips, match the retail rig: JO's people are 19 bones
plus a mesh part, the first 37 parts of a first-person gun are the stock arms'
bones, and a clip's channels pair with the model's parts by index.

`opennova-3di anim info <file> --verbose` prints a table or a clip, and
`opennova-3di anim compare <a> <b>` tells whether two sets hold the same
animation.

## Weapon timing

A first-person gun's clips are played by one or more `weapon.def` entries
(the AKM's `WPN_AK47AUTO` and `WPN_AK47` share one clip set), and each entry's
ACTION blocks time them. The add-on writes the keys those blocks need from the
clips' markers, measured by the engine's own weapon state machine; nothing
reads an existing weapon definition or imported timing.

1. In the model root's **Weapon** box, add an entry per `weapon.def` entry
   that plays these clips: its name, its fire mode (as its FLAGS say) and the
   rate it should fire at. A model with no entry exports its clips and no
   weapon edits.
2. Put each action's clip on its row: idle on `anim_wpn_idle`, fire on
   `anim_wpn_fire`, and so on (empty idle's row is `anim_wpn_empty_idle`).
   **Assign Weapon Action** (Dope Sheet sidebar) makes the Action the rig
   plays the clip of an action's row, adding the row and the action's timing
   markers. It leaves the clip's loop as it is: retail's long idles loop by
   the clip's own flag. Overheated is not offered: it has no clip of its own
   and nothing in the game ever enters it.
3. Place the timing markers in the Action Editor with **Show Pose Markers**
   on, or with the panel's frame fields and **At Playhead** buttons. They are
   the Action's own markers, counted from its first frame at its Clip rate;
   scene markers do not time anything.
4. Optionally point **Hip view** and **Aim view** at cameras placed at the
   eye: the entries' `POS` and `TPOS`. The game looks along the model's
   forward from the eye, so only a camera's place is used.
5. **Preview Game Timing** shows, per entry, the rate the game's whole ticks
   give against its target, and per action its DELAYSTART and DELAYEND and
   how much of its clip the view shows before the next action replaces it,
   warning where a one-shot clip shows less than half. The preview is kept
   until Blender closes, never in the scene, and says when the timing has
   changed since.
6. **Export Animations** writes `<table>_weapon_edits.txt` beside the table:
   per entry only the keys to set (ANIM, DELAYSTART, DELAYEND, POS, TPOS),
   never FUNCTION, sounds, effects or flags. A table without a fire row still
   writes its clips, and no edits. **Merge into weapon.def** sets those keys
   in a copy of a `weapon.def` you pick (`opennova-3di weapon merge`), written
   beside it as `<name>_merged.def` unless you name another file; every other
   byte stays as it was. `docs/anim/weapon-timing-format.md` describes both
   files.

| Marker | Times |
| --- | --- |
| `ON:Shot` | Fire: the shot pose; none means the first frame. Fire's recovery is each entry's target rate, so fire takes no Ready. |
| `ON:Eject` | Recoil: the casing and refire decision; none means the first frame. |
| `ON:Active End` | Reload, empty, fire mode, aim in and aim out: the end of the action's active phase; none means Ready, else the clip's end. |
| `ON:Ready` | The end of the recovery after that boundary, and an idle's window before the game plays it again; none means the clip's end. |

Draw and holster take no marker: the game paces them on its switch timer.
Every clip on one action's row must time alike, in seconds: the game plays a
ring's clips in turn, so any of them may be the one playing. A Ready marker
may lie past the clip's last frame to hold a recovery; the other markers name
a pose the clip holds. Reload refills the magazine when it starts; its markers
time the animation and the action, not the magazine.

## Tests

`tests/blender/*_test.py` author their scenes from scratch and run headless,
against a given `opennova-3di` or against the installed extension:

```text
blender -b --factory-startup --python-exit-code 1 --python tests/blender/anim_test.py -- <opennova-3di.exe>
blender -b --factory-startup --python-exit-code 1 --python tests/blender/weapon_test.py -- --installed
```

## Collision volumes

A `-colonly` mesh is a convex volume: the game keeps the solid all its face
planes bound, so a concave or twisted mesh loses whatever sticks out. Export
warns with the volume's name and how far it reaches outside; split such a
mesh into convex pieces. A ladder (`CL`) faces the plane of its last face; a
flat ladder is a single polygon facing the way the ladder does.

Inspect any `.3di` (retail ones too) with `opennova-3di info <file> --verbose`;
`opennova-3di compare <a.3di> <b.3di>` tells whether two files hold the same
model.
