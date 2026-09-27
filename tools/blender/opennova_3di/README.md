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
the materials, voted on import), so retail face normals, zero-length or
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
      _center         (a part that draws nothing: its first vertex seeds its bounds)
F16_LOD1              Empty, _lod_index = 1 (its own PN## empties; Blender's .001 is ignored)
```

An animated or skinned model (a first-person gun, arms, a person) holds one
armature under its LOD 0 root. Its bones named `BN##` (a label may follow,
`BN16 L Hand`) are the parts: the bone head is the pivot, the nearest `BN##`
bone above it the parent. Other bones are no part: a `Root` on the ground
(see Animations) or the control bones a rigger adds. Build the bones in Edit
Mode with any names and press **Number Parts**: it numbers the bones that
deform in hierarchy order, parents first, keeping each name as the label
(after Symmetrize or a duplicate, press it again). Parent a rigid mesh to its
bone (Ctrl+P > Bone); give a skinned mesh an Armature modifier on the rig and
vertex groups (Ctrl+P > With Automatic Weights). A first-person gun and its
arms share the gun's armature:

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
holds the bullet faces. Each bone's collision section is its hit sphere: a
`_## hit` Empty drawn as a sphere on the bone (its size the radius) and a
`_## bounds` box Empty beside it give the sphere and its box; without any,
export makes a sphere for every bone around the vertices it moves.

Helpers take their part from where they sit, and the `##` in their names is
optional: `UPG05 grip` on part 05 is fine, `UPG04 grip` there is refused.
`UPx00` is a user point on no part. In a LOD whose bones another LOD holds (a
skinned model's later LODs, arms on a gun's rig) a helper sits under the LOD
root and its name must give its part (`_05 hit`). Other helpers:

- `_center` (a mesh): on a part that draws nothing (a first-person gun's arm
  parts often do), its first vertex is the point the part's bounds sit on,
  and in the collision LOD the section's one collision vertex, as the retail
  tool seeded such parts. Import makes one for every such part.
- `_attach` (an Empty): in the collision LOD, the attach point the game stores
  for its part's section (CXLT; a palm's broken pieces pivot on them). With
  any in that LOD, export writes an attach point for every section after the
  root (every section on a skinned model), at the section's `_attach` or, where
  it has none, at its pivot; without any, the builder puts each at its
  section's pivot, so the few retail models that store none (Chair03X) come
  back with them, and import says so.

The root part's pivot is the model origin. A part numbered before its parent
exports with a note (the game reads that parent before posing it; Number
Parts avoids it). A part's **Parent** (in its closed "Stored as" panel) keeps
what a hierarchy cannot say, a part that names itself or no part as its
parent, as some retail models store; only import sets it. Export warns about
any object it does not use.

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
- **Backface Culling** off is two-sided, for drawing and for the bullet faces.
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
use). The name is free.

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
write two images under one name. **Write textures** off writes none.

The texture list carries only what the nodes cannot say: flipbook frames, row
flags (1 a flipbook frame, 2 the render-state override), an object-space or
height-map normal texture, a file Blender cannot open. A slot it lists is taken
from it, not from the nodes. A row's file name is printable ASCII, at most 16
characters, without a folder; **Write** writes its image under that name,
which must then be `<stem>.tga` or `<stem>.mdt` in at most 15 characters. A
material holds at most 24 rows.

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
  Number Parts and Deform with Rig of (a model with its own rig), Export Model,
  and its Animations box. Export All Models writes every model, and Export All
  Animations every rig's clip set.
- **Object properties** on a model root: the same model settings. On a LOD
  root: its index, the LOD threshold (projected radius in pixels; the last
  LOD's 0) and type (`gnrc`, `bldg`, `door`, `veh0`). On a `PN##` part: part
  animation tracks (rotation about the part's up, side or forward axis, scale,
  or translation, driven by an engine register such as `HELO_ROTOR` or by a
  spin or wave), and in a closed "Stored as" panel its raw PANM flags word and
  stored Parent. On any other object of a model: the part it sits on, and on a
  user point, light or occlusion mesh its export order.
- **Bone properties** on a `BN##` bone: its part number and part animation, as
  on a `PN##` part, plus the track frame.
- **Light properties** on an `LP` light: the colour generator (style, rate,
  phase or register, end colour), attenuation and the corona / terrain /
  object light switches, the other flag bits in a closed panel. A spot light's
  cone points down the light's -Z, as Blender draws it, and an unrotated light
  points straight down.
- **Material properties**: the shader (with what it implies) and export
  order, what Blender's settings give (two-sided, the alpha test, the alpha
  pass, the glow), the bullet faces' surface type (metal 14, glass 15, ...)
  and flags (bullets pass, front only: a bullet from behind passes), the other
  flag bits, the reflection colour, the texture rows the nodes cannot give, and
  the RGB / alpha / UV generators and texture flipbook. A register-driven
  flipbook selects its register by name.

## Animations

A model's clips live on its rig, and the whole set writes at once:
**Export Animations** makes the `.adm` table the model root names (empty: one
named after the model, beside the `.blend`) and one `<clip>.bad` beside it for
every clip. **Export All Animations** does that for every model with a rig,
and skips, with a warning, a rig that carries no clip set of its own (the arms
beside a first-person gun). **File > Import > NovaLogic Animations** reads a
`.adm` (or a single `.bad`) back onto the active model's rig.

- **A clip is an Action.** Push each one onto its own NLA track; the track order
  is the set's order and the Action's name is the `.bad` file name. Set the
  model's **Clip file prefix** (for example `rifle_`) to keep standard Action
  names while giving its exported clips unique filenames. ADM references use
  that prefix too; engine row names stay unchanged. Export plays
  each Action through its strip's action slot (Blender 4.4 and newer), so an
  Action keyed on another rig exports its own motion, and it refuses a rig in
  NLA tweak mode. The clip's own settings live in the Dope Sheet sidebar's
  OpenNova panel: its rate (retail ships 30 everywhere), whether it loops,
  whether it carries per-bone translations (a bolt, a magazine, a rig that
  slides), the unwitnessed flag bit 3, and a frame count longer than the
  Action, whose extra frames hold its last pose.
- **Each clip exports on its own.** A channel a clip does not key is at rest:
  a bone it leaves alone keeps its rest pose (`Root` and the hips too) and the
  trigger is 0, whatever the clip before it did. A bone that follows another
  model's parts (**Bones follow**) follows its own clip while the set exports.
- **The table** is the row list on the model root: a slot (`anim_reset`,
  `anim_walk_forward`, ...) and the clips that answer it. Several clips on one
  row are a ring the game rotates through, and it serves a row from its LAST
  entry back. The reset row (`anim_reset`) names the bind every clip is
  measured against: its last clip, since each clip on that row replaces the one
  before it (every retail table holds one, and the game cannot load a table
  without one, so export refuses it). A row names its slot by what follows the
  key's first five characters, so `ANIM_RESET` is the reset row too.
- **The rig** is the usual humanoid one: a `Root` bone (any case) on the
  ground as the top bone, the hips (`BN01`, the model origin) below it, and a
  head. Move the body over the ground with Root and bob it with the hips.
  Export measures every frame from the pose: the bottom is the hips' height
  above Root, the top the head's, and the step is how far the hips move to the
  next frame, which is how far the game moves the body. The head is the
  **Head** field in the Animations panel, or else the one bone whose name ends
  in `head` (`BN15 Head`); with no head, the top is the bottom. A rig without
  Root (a first-person rig) stands on Blender's ground, Z = 0, so its model
  sits with the hips at the game's height (1.07 m). As in every retail clip, a
  loop's last two events repeat its first and a one-shot's stand still. Root
  is no part, and neither is a bone named `!something`, where control bones
  go; a `BN##` bone under either takes the nearest `BN##` above it as its part
  parent.
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
  their Actions are keyed against it; a table without a reset row names no bind
  (and does not export) and leaves the rest as it is; and a single `.bad` carries no table, so it
  leaves the rest and the model's rows as they are. Bone names come from the
  clips (a `.3di` carries none), and their vertex groups are renamed with them;
  a lower-case `bn38 bone` becomes `BN38 bone`.
- **Import keys what the game plays.** Every clip is keyed on each frame of its
  length: a bone that holds a key over several frames gets the pose the game
  blends between its keys there, and a bone that stops keying early holds its
  last key. A re-export therefore keys every frame (`DVFLEE1E`, `DT1RST` and
  `stgr_RST` come back with more keys and the same poses), and keys past the
  clip's own length (`M60_1i`), which the game never plays, are left out. The
  import lists these in its warnings, and a failed import leaves the scene as
  it found it. A clip the rig already holds under the name of one being
  imported (a set imported again) is replaced in its place in the set, and the
  table's rows follow the new one.
- **Import stands the rig on the ground.** Each frame keys the hips at its
  bottom and, when the set moves the body, a `Root` bone (made at the ground
  under the hips when the rig has none) along the steps, so a planted foot
  stays put. A model at the world origin is raised so the ground is Blender's
  Z = 0, and the arms following a first-person gun rise with it, so the gun
  and its arms overlay a body at the hips. This is display only: every export
  reads a model as if its root stood at the origin. A clip whose stored top is
  more than 3 cm from the head's height is named in the warnings, and so is a
  first-person clip whose top stands above its bottom (a third of retail's do,
  by a rule nothing has shown); a re-export writes the rig's own measure.

**A first-person gun** is animated by its own rig, the bones its parts are
(Add Animation Rig makes one from `PN##` parts), and the arms deforming with
that rig move with it, so a first-person set belongs to the gun.

To reuse retail's own clips, match the retail rig: JO's people are 19 bones plus
a mesh part, a first-person weapon 40 parts, and a clip's channels pair with the
model's parts by index.

`opennova-3di anim info <file> --verbose` prints a table or a clip, and
`opennova-3di anim compare <a> <b>` tells whether two sets hold the same
animation.

## Weapon action timing

Weapon timing works on a new rig and newly keyed Actions. Imported models,
clips, source frame metadata and existing weapon definitions are not required.

1. Create the model root, LOD 0 and BN## armature as above. Key a bind-pose
   Action spanning at least two frames. In the Action Editor sidebar, choose
   **Assign Weapon Action > Bind / Reset**.
2. Key the other Actions and assign **Fire**, **Idle**, **Reload**, etc.
   Overheated is not offered: it runs the idle handler, has no `wpn_` anim
   state of its own, and no shipped weapon.def authors it.
   Assignment keeps the Action on an NLA track, binds its animation-table row,
   and adds local timing markers. It clears the clip's **Loop** flag: retail's
   weapon clips are one-shots, its idle holds too, and the idle action replays
   its clip on each Ready window.
   Existing rows can instead be assigned a **Weapon action** role on the model.
3. Edit the named markers in the Action Editor with **Show Pose Markers** on,
   or use their frame fields and **At Playhead** buttons. These are Action-local
   markers; scene timeline markers and NLA placement do not define timing.
4. Choose the weapon's **Fire mode**, then **Ready marker** or **Target RPM**
   as the single cadence source. **Preview Game Timing** displays requested
   and achievable RPM and the generated delays. The Text Editor's
   `<model> - Weapon Timing` text contains the full native FSM event trace.
5. **Export Animations** writes the BAD/ADM set and, when **Export weapon
   actions** is enabled, `<table>_weapon_actions.txt` alongside it. Merge those
   ACTION blocks into the weapon definition and apply the stated Auto/Burst
   flags while preserving its other flags. Sound/effect references can be
   authored on each weapon row. Ammo, damage and inventory settings remain
   part of the weapon definition.

| Marker | Authoring meaning |
| --- | --- |
| `ON:Shot` | Fire clip's shot pose; absent means the first frame/immediate shot. |
| `ON:Eject` | Recoil clip's casing/decision pose; absent means immediate. |
| `ON:Active End` | Other actions' active-phase boundary; absent uses Ready or clip end. |
| `ON:Ready` | On Fire, Shot-to-Ready requests the full firing cycle, including recoil and transition ticks. On other actions, the gap after the active boundary authors recovery. Idle uses it as its repeat window. |

Target RPM ignores Fire's Ready marker; it never fights a second timing
source. A Ready marker may lie past the keyed clip to author a held recovery;
Shot, Eject and Active End must identify a pose inside the exported clip.
Timing uses the clip's exported integer rate and its own first frame, including
negative or nonzero starts. Moving an NLA strip changes neither. Non-idle
variants of one weapon row must agree on phase times in seconds: the runtime
has only one delay pair per ACTION. Idle variants use the first served clip's
length for their repeat window.

The CLI's `weapon timing` command compiles current markers and measures the
result with `weapon_fsm_tick`. Active pose boundaries account for the clip's
62-step playback clock and the counter-zero tick that does not advance it.
Recovery and requested firing periods use 62.5 logic ticks per second. The
solver adjusts Fire's recovery delay to the nearest whole-tick requested
period and includes the runtime's extra transitions: for immediate automatic
fire with zero recoil, delayend 3 gives a five-tick cycle (750 RPM), not a
three-tick cycle. Semi-auto measures fresh presses at the first eligible Idle
tick; burst reports the within-burst rate. The preview assumes ammo is
available and no environmental/heat gate blocks fire. A changed timing input
marks the preview stale; export always compiles again from the authored scene.

The engine's existing constraints remain visible: reload refills ammo on
entry; its markers control animation/action windows, not a new magazine-in
gameplay event. Draw/holster use the fixed switch timer and do not accept timing
markers. Export uses a zero start delay and a two-tick refreshed end counter
to enter that timer and keep the animation advancing; inspect the native trace
for its actual length. A draw/holster clip may therefore be cut short by the
runtime. BAD trigger bits
are body events and do not replace first-person weapon action timing. A recoil
window that cannot sustain the requested firing mode is rejected by the native
preview rather than exported with an invented behavior.

To run the asset-free authoring regression:

```text
blender --background --factory-startup --python-exit-code 1 --python tests/anim/blender_weapon_authoring.py -- <opennova-3di.exe> <output-directory>
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
