# Scene naming contract

How a Blender scene holds a NovaLogic model: the parts, what sits on each, and
the names that carry a 3DI role. The names follow the NovaLogic ASE/OED
object-naming convention; OED classified them by `classify_name`
([orig: ConvertToInternal @ 0x4268B3 (ModSuperOed.exe)], ported in the retired
`engine/formats/oed/convert_internal.cpp`). The Blender add-on
(`tools/blender/opennova_3di`, [ADR 0047](../adr/0047-blender-3di-exporter.md))
reads a scene by this contract to export and lays a model out by it to import;
`rig.py` is its one reading of the parts.

Names are ASCII. A part number is two digits and 1-based in a name (`01` is
the first), 0-based inside. A leading `!` makes an object ignored. Blender's
own `.001` duplicate suffixes are stripped before a name is read (object names
are unique per `.blend`, so LOD 1's `PN01` is `PN01.001`); two parts of one
number inside one LOD are an error. Export names every other object it leaves
out (an Empty that only groups objects is left alone), and user points,
lights, collision volumes, occlusion meshes and hit spheres outside the
primary LOD. A mesh Blender does not evaluate (it or its collection is
disabled in viewports, or its collection is excluded) exports as its base
mesh, so export refuses one that carries modifiers or shape keys, and a mesh
making instances it has not realized (Geometry Nodes) is refused.

## Parts

A model root holds its LOD roots, and a LOD's parts come from one of two
places:

- **`PN##` empties**: a static model (a prop, a building, a vehicle whose parts
  turn by their PANM tracks). The empty's origin is the pivot and the nearest
  `PN##` above it the parent part; a part with none above it hangs from part
  01, as retail stores a top part's parent (and the root's, itself). Add Part
  from Selection makes one at the active object's origin and puts the
  selection on it.
- **The rig**: an animated or skinned model (a first-person gun, arms, a
  person). One Armature under LOD 0's root; its bones named `BN##` (a label may
  follow, `BN16 L Hand`) are the parts: `##` the part number, the bone head
  the pivot, the nearest `BN##` bone above it the parent (none: part 01). Any
  other bone is no part: the `Root` on the ground (see Animations), a control
  bone. Number Parts numbers the part bones in hierarchy order, parents first,
  keeping each bone's label (every deforming bone but `Root` becomes a part),
  and Blender renames their vertex groups with them; Number Parts renames
  their channels in every clip the rig plays (the Actions the model's rows
  name and any keyed for the rig). Add Animation Rig turns a static model's
  `PN##` empties into such a rig in
  place. Every LOD of the model reads its parts from that rig: a later LOD
  holds meshes deforming with it, or `PN##` empties of its own on a rigid
  model.

A first-person gun and its arms share the gun's rig. The arms model holds no
armature: its meshes deform with the gun's rig through their Armature
modifier, its root stands under the gun's root, and its parts are the rig's
bones 01 to K, K the highest bone its weights use, since retail draws the arms
with the gun's part matrices, paired by index [orig:
Player_RenderFirstPersonViewModel @ 0x4DED60, the arms submit @ 0x4DF088].
Every first-person bone buffer holds 64 matrices, so a gun (a rig an arms
model shares, or one with a clip table) has at most 64 parts and its arms no
more parts than the gun [the part-count test @ 0x4DEF8B]. Deform with Rig of
turns arms with a rig of their own into that shape; importing a gun and its
arms together does it too. A gun whose parts 01 to 37 are not the stock arms'
rig (ArmsG's, which every character's arms share) is noted: the character's
own arms draw with those parts.

A skinned model keeps its skinned geometry on the root part, or on a part of
its own after the bones when its **Mesh part** setting is on: that part's
pivot is its skinned mesh's origin (parent 01), and its collision section
holds the bullet faces (the retail layout of 90 of the 207 skinned models,
US01 and ArmsG among them; 116, Delta04 and ArmGlovD among them, keep it on
the root, and H50_1st.3dp on a bone its strips name).

The root part's pivot is the model origin (no retail model moves it; the
engine poses every part from it). A part whose parent is numbered after it
is noted: the engine reads that parent before posing it. A part's **Parent**
setting stores what a hierarchy cannot: a part that names itself as its
parent (Eturret's turret) or none (Excavatr's arm, Chair03X's pieces), 46
retail models in all; only import sets it.

An object belongs to the part it sits on: the part bone it hangs from (a bone
that is no part holds none), part 01 of the rig its Armature modifier deforms
it with (a skinned mesh; the mesh part when the model keeps one), else the
nearest `PN##` at or above it. The `##` in a helper's name is optional, and an
error when it names another part. A LOD whose bones another LOD holds (a later
LOD of a skinned model, arms on a gun's rig) has nothing of its own to hang a
helper from: there a helper sits under the LOD root and names its part.

| Scene element | Form | Meaning |
| --- | --- | --- |
| Model root | any name (the Empty above the LOD roots) | one model, one `.3di`: its model name, output path (empty: `//<model root name>.3di`, at most 15 ASCII characters, as a game archive's entry holds it) and collision LOD are properties. A scene holds any number of models; each exports in its root's own frame, facing its -Y, so placing, turning or mounting a model does not change it |
| LOD root | any name, custom property `_lod_index` | render LOD `_lod_index` (0 = primary); its threshold (the projected radius in pixels above which it draws; the last LOD's 0) and RMDL type (`gnrc`, `bldg`, `door`, `veh0`) are properties. A root with no parts is an empty LOD (retail ships them) |
| Part | `PN##` (Empty) | 3DI subobject `##`; its origin is the pivot. A rotated `PN##` is a PANM rotation frame (an MTRX row): its tracks turn about the empty's axes (Dblkhwk1's canted tail rotor). A mirrored one (a negative scale) is a reflection frame, whose tracks turn the other way; retail stores five (dtaxi1, PKM_1st, ...) |
| Bone | `BN##` (a bone of the rig) | part `##`: the head is the pivot, and the bone may point any way (the add-on lays each toward its first child). Its PANM tracks, flags and track frame (the MTRX row, a 3x3 turn of the rig's axes, mirrored or not) are bone properties, and so is Hit sphere (a skinned model's, below); a part has one track per target. Its bind frame (a 3x3 turn of the rig's axes) is the turn its clip keys are measured from, where an imported table's bind is not its rest turn (see Animations) |
| Geometry | any mesh | the render geometry of the part it sits on; the UV map Blender renders with is the base UV0, the first other one the detail stage's UV1. With an Armature modifier on the rig it is skinned (on the root part or the mesh part); in a skinned model a mesh hung from a bone is skinned wholly on that bone |
| Skin weights | vertex groups named after the rig's bones | each vertex on at most four part bones (Weights > Limit Total), normalized as Blender's Armature modifier blends them and written dominant first: three stored weights, the fourth bone taking the remainder, as the game's skinning shader reads it [orig: _BaseInc.fx CalcSkinWorldPosAndNormal; ThreediGp_ConvertVerticesToGPUFormat @ 0x5B4C90]. A vertex with no weight of 1e-4 or more, a weight on a deforming bone that is no part (Root, a control bone) and one on a part bone whose Deform is off are refused, naming the vertex and the bone |
| Part centre | `_## center` (mesh) | on a rigid part that draws nothing, its first vertex is the point the part's bounds sit on (radius 0) and, in the collision LOD, its section's one collision vertex: OED seeded such a part with that vertex (5fc5b4f6a^ `engine/formats/oed/convert_internal.cpp`, the placeholder injection), and 1,779 of the 2,411 such retail parts carry it as their section's only collision vertex. Import makes one for every such part; a skinned model's (dM1A1, DT801) are not carried |
| Attach point | `_## attach` (Empty) | in the collision LOD, a CXLT row [OED's WriteCXLT wrote the collision LOD's attach points, a row per `~` attach helper in name order, 5fc5b4f6a^ `engine/formats/oed/convert_internal.cpp` and `export_3di.cpp`]; the runtime reads the rows by index (a palm's broken pieces pivot on rows 0 and 1). The model's **Attach points** setting says how they make the table. One per part (the default): when that LOD holds any, export writes one row per section the retail corpus gives one (every section after the root on a rigid model, every section on a skinned one), at the part's attach point or at its pivot (our rule), and with none the builder puts each at its section's pivot; import makes one where a row is not its part's pivot (46 retail models store section offsets that are not their parts' pivots). The attach helpers: a row per `_attach`, in export order, several on a part if need be, none an empty table; only import sets it, for the 156 retail tables one per part cannot say (37 empty, Chair03X among them; 46 with a row for the root too, M24_1st's in its helpers' name order, which is its parts' parent order; 73 of other counts, dM1A1's 33 rows for 25 sections) |
| Hit sphere | `_## hit` (Empty, drawn as a sphere) | on a bone of a skinned model: the bone section's hit sphere, its origin the centre, its display size times its scale the radius, which the runtime's person raycast reads [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]. `_## bounds` (an Empty drawn as a box, square with the model's axes) is the section's bounds box. A bone without them gets the ones the LOD 0 vertices it moves give: the box around them and the sphere about its middle reaching the farthest (OED's WriteCOBJ rule, 5fc5b4f6a^ `engine/formats/oed/export_3di.cpp`). A bone whose **Hit sphere** property is off, or that moves no vertex and has no `_## hit`, stores none, and no shot finds it. Import makes the helpers, hidden, only where the file's sphere or box is not the derived one on its 16.16 grid, and turns Hit sphere off where a section stores none although its bone moves vertices: a model this export wrote imports with none of them |
| User point | `UP<c>## <label>` (Empty) | USRP point on its part (`00`: on none, -1): type letter `c` (`G` 71 gameplay, `S` 83 effect), label = the USRP name (no label: `Noname`); faces along its local +Z. Its export-order property keeps the USRP order (seats and effect points are scanned in it); points without one follow in label order (our own rule: retail's exporter kept its scene order), so `sitex01`, `sitex02` keep seat order whatever part they sit on |
| Light | `LP##[a..]` (a light object) | a LGHT light owned by its part (`LP00` is part 01, as `classify_name` clamped it); a point light is omni, a spot light a cone about its local -Z, the way Blender draws it. An unrotated light points straight down, retail's omni default. Its colour is the start colour; the generator, attenuation and flags are properties |
| Material | any name (a Blender material) | read as Blender 5.0 or newer draws it, through its Principled BSDF: the image feeding Base Color (through reroutes, groups, Mix and colour nodes) on the render UV map is the diffuse texture, one on the second UV map the detail texture; a tangent-space Normal Map node's image the normal map, an `.mdt` written with the game's green (its tangent frame runs down the texture; an image read through a Separate Color, 1 - Green, Combine Color flip is written as it is); Backface Culling off is two-sided (bullet faces too, unless its **Both sides** setting says otherwise); a Greater Than (Less Than: inverted) Math node on Alpha with a constant threshold the alpha test; Render Method Blended the alpha pass; Emission a glow. Without an image, its Base Color is a swatch texture (a mesh without a material: Blender's default grey). Its Shader property is the shader tag (any in the engine's shader table); empty, OED's default for its texture maps (`FF_ST_OP` one, `FF_MT_OP` two, `VS_SKBASIC` on a skinned model) among the rows that draw those settings. Its Export order property is its index (import sets it); a material in a mesh's slots that no face draws with exports only with one (import puts the materials a retail model keeps unused in its first mesh's slots). An image loaded unchanged from a `.tga`, `.dds`, `.mdt` or `.pcx` file is that file, named as the game finds it and copied beside the model; another image is written as `<model>_<index>[d|n].tga|.mdt`, at most 15 characters, and a model is refused when another model writing into its folder has a name that cuts to the same stem. Its texture list holds only what the nodes cannot say (flipbook frames, row flags, an object-space or height-map normal texture, a file Blender cannot open), and a slot it lists is taken from it. Glass, emissive and the alpha pass follow the shader |

## Collision volumes

`<TYPE>[##][<dup>]-colonly` meshes, on the primary LOD: the volume joins the
collision section of the part it sits on (`##`, which `classify_name` stored
as the object index, is optional). `<dup>` is a lowercase suffix for the 2nd
and later volumes of one type on one part: 2nd `a`, 3rd `b`, ..., `z`, then
`aa` (the Blackhawk's 35 hull volumes are all `CB01...`); a section's volumes
export in that order, code first, then by name. The volume is the solid its faces bound, by the OED rule
(docs/threedi/o3d-scene-format.md): each face's plane, so the mesh must be
convex (export names one that is not). A ladder (`CL`) faces the plane of its
last face (Blender's Sort Mesh Elements can put a chosen face last). A flat
ladder is one polygon facing the way the ladder faces: 94 of the 102 retail
ladders have no thickness, and import lays each out that way.

| Code | Type | Code | Type | Code | Type |
| --- | --- | --- | --- | --- | --- |
| `CB` | 1 | `BB` | 8 (blink box) | `LP` | 14 |
| `CS` | 2 | `CD` | 9 | `DH` | 16 |
| `CC` | 3 | `CT` | 10 | `DM` | 17 |
| `CL` | 4 | `CM` | 11 | `DL` | 18 |
| `CV` | 5 | `VK` | 12 | `CP` | 19 |
| `CA` | 6 | `CF` | 13 | | |
| `VC` | 7 | | | | |

Any other code whose first letter is `C`, `D`, `L` or `V` is type 0:
`classify_name` leaves the type of a code it does not list at 0. 36 of the 48
retail first-person weapons (and IJava03) carry a type 0 box per section;
import names them `CX`.

`BB` takes flag letters before `##`; each clears a bit of `0x3E`: `V` 0x2,
`S` 0x4, `W` 0x8, `L` 0x10, `O` 0x20 (for example `BBVSO03`; Armry01's light
fixtures carry `BBL02`, `BBVSL03`). Type 14 shares the `LP` prefix with
lights; the `-colonly` suffix tells them apart. Runtime meanings:
docs/world/world-wac-ai-re.md §15. No name carries the flags of a non-`BB`
volume (Armry02 ships `CB` volumes with flags 1).

## Occlusion

`<PFX>[##][<dup>][-<MM>]-occonly` meshes on the primary LOD, in the section of
the part they sit on (`##` optional); the OCCL record type follows the prefix as the
retired exporter mapped it: `OB` 0 (occluder), `OS` 1 (open), `OP##` 2 (a
window to the exterior), `OP##-MM` 3 (a portal to section `MM`), `OH` 4 (no
witnessed runtime meaning). Faces wind counter-clockwise about the outward
normal; the planes follow the OED rule (docs/threedi/o3d-scene-format.md).
Armry01 lays out as `OS01`, `OB01`..`OB01c`, `OP02`, `OP02-04`, `OP04-03`.
An export-order property keeps the record order.

A record's sphere, which the runtime tests before building the record's
occluder planes, is its mesh's vertex centre and farthest vertex. A `_sphere`
Empty on the mesh (drawn as a sphere: its origin the centre, its display size
times its scale the radius) stores another: import makes one, hidden, only
where the file's sphere is not the derived one, for the 206 retail models that
store each centre mirrored across the model's y (Crdrblk2, DRGVLA).

## Assemblies

Models the game draws together can share a scene. The add-on shows how the
game combines them, but the assembly settings never reach a `.3di`:

- **Arms on a gun's rig** are not a display setting: they are the scene shape
  (see Parts), so posing the gun's bones poses the arms the way retail draws
  them [orig: Player_RenderFirstPersonViewModel @ 0x4ded60, its
  Entity_BuildBoneWorldMatrices call @ 0x4df028]. They take the gun's pivots
  and track frames as their own (retail's own arms sit within 0.5 mm of
  357_1st's parts and 1.2 cm of REVVY's AKM_1st's, a pair retail draws).
- **Mount on** + **user point** (on any model): the model sits on another
  model's user point, as an ITEMS.DEF `addeweap`/`addeweapC <userpoint>` child
  (the M1A1's turret on the hull's `ewep01`) sits on its parent. The names are
  matched whole and trimmed (retail labels carry trailing blanks), without
  regard to case, and the first match wins. A missing name leaves the child
  on the parent's root. The child takes the point's look-at frame as retail
  builds it: the direction read mirrored against the position, and the
  look-at matrix's rows as the child's axes, so a level point faces the child
  along it and a pitched one tips it the other way [orig:
  Bone_BuildAttachmentMatrix @ 0x56C630; Math_BuildDirectionLookAtMatrix @
  0x612C90].
- A mount binds with every rig of the two models at rest, so the pose a clip
  holds when it is set is not baked in.

## Bullet faces

The collision faces bullets hit come from one render LOD's part meshes, chosen
by the OED `.3dp` `poly_collision_lod` setting (default 0, the most detailed;
Armry01's are LOD 1's), and that LOD's parts are the collision sections (one
each). Each face's surface type and flags come from its material: "both
sides" (1) follows the material being two-sided unless its **Both sides**
setting says Yes or No, which import sets where a material's faces disagree
with its drawing (154 retail models: Baricd02's two-sided wire stores its
faces one-sided, each in both windings); "bullets pass" (0x100, retail's
rotor blades) and "front only" (0x800: without flag 1 a bullet stops only when it crosses
the face from the front [orig: Physics_RaycastAgainstBoneCollision @
0x4e4cb0, the test @ 0x4e5139]) are material settings. Import gives each
material the surface and flags most of its faces carry and names how many
faces lose that vote. A skinned model's skinned geometry gives the bullet
faces of the section of the part it is authored on: its mesh part (the retail
person layout of US01 and ArmsG) or its root (Delta04, ArmGlovD); a mesh hung
from a bone gives that bone's section its faces. The three retail vehicles
authored on their bones with strips mixing bones (dM1A1, DT801, Ftruck1X) come
back authored on one part, their per-part bounds and faces not kept.

Turning off a model's **Generate bullet faces** property (on by default)
keeps its render triangles out of the bullet faces, for a model such as
first-person arms that needs none; its collision volumes and the rest of its
collision LOD still export.

## Animations

A rig's clips are a set of their own (`docs/anim/o3a-scene-format.md`), laid out
on the model the rig belongs to:

- **A clip is an Action** a row of the table names, keyed for the rig (no NLA
  track is involved): its Manual Frame Range is its length (else its keyed
  range), Cyclic its loop, its Clip rate its fps and its Flag 8 the
  unwitnessed flag bit 3, and it carries translations when a bone moves off
  its rest offset from its parent (the reset too, whenever a clip does). Its
  `.bad` is named after the table and the slot it answers,
  `<table>_<code>[n]`, within the 15 characters an archive entry holds;
  import replaces a clip a row already names under an imported clip's name.
- **The table** is the rows on the model root: an `anim_<name>` key and its clip
  ring, in the order the `.adm` stores. The engine serves a row from its LAST
  variant back [orig: AnimMap_RegisterBoneNode @ 0x40C2D0], and the reset row
  (`anim_reset`, the last one) names in its last variant the clip whose first
  key is the bind, each reset variant replacing the one before [orig:
  AnimMap_FindSlotByName @ 0x40cfa0; AnimMap_RegisterEntity @ 0x40bb60]. A
  row names its slot by its key past the first five characters, without case
  (`ANIM_RESET` and `xxxx_reset` are the reset row too). The game cannot load a
  table without a reset row [orig: AnimMap_LoadAdmFile @ 0x40cc40, the read of
  slot 0's head @ 0x40ce11], so export writes one when the rows name none: a
  reset row naming no Action, or a table without one, gets `<table>_rst`, one
  looping interval of the rest pose at 30 fps (the shape of retail's resets).
  Only `anim build` refuses a table without a reset row.
- **The rig** is a humanoid's: `Root` (any case) on the ground as the top
  bone, the hips `BN01` (the model origin) below it, and the head, the bone the
  model root's `head_bone` names or, unnamed, the one bone whose name ends in
  `head` (`BN15 Head`). Each frame's event is measured from the pose, never
  keyed: the bottom is the hips' height above Root, the top the head's (the
  bottom when the rig has no head), and the velocity the hips' step to the next
  frame across the ground, up by the change in bottom, in the clip frame (x
  lateral, z forward). The runtime moves the entity by that step, stands its
  origin `bottom` above the ground and reads the top as the capsule's [orig:
  AnimMap_UpdateEntity @ 0x40b5f0]; that retail's exporter measured the hips
  and the head this way is read off the corpus (the top within 1 cm of the
  head's height in 91% of 185,661 person frames). The last two events repeat
  one, as in all 477 retail clips: a loop's are its event 0, a one-shot's stand
  still at frame_count - 1's bottom and top. A rig without Root stands on
  Blender's ground plane, world Z = 0 (our convention; the ground is in no
  clip), as a first-person rig does with its model placed at the hips' height
  (1.07). The hips never carry a translation row (0 of the 202 translated
  retail clips moves bone 0). Any bone not named `BN##` is no part either, so a
  rig may also hold the control bones an author rigs with.
- **Import stands the rig on the ground:** the hips keyed at each frame's
  bottom and, when the set moves the body, a `Root` made at the ground under
  the hips and keyed along the summed steps, so a planted foot stays put; a set
  that never moves (every first-person set) makes none. A model root at the
  world origin rises so the ground is Z = 0, with the models whose bones follow
  it (display only: every export reads a model as if its root stood at the
  origin). A stored top more than 3 cm from the head's height (from the bottom
  on a rig without a head: 64 of the 204 retail first-person clip
  registrations carry a higher top by an unwitnessed rule) is reported.
- **The event bits** are Action markers named after them, each setting its
  bit on its frame's event: `FOOT_LEFT` and `FOOT_RIGHT` (1 and 2) the
  footsteps, `FIRE_PRIMARY`, `FIRE_SECONDARY` and `FIRE_MARKER3` (4, 8 and 16)
  the ammo rows, `FOLEY_1` to `FOLEY_6` (0x20 to 0x400) the six foley sounds
  (`opennova-3di catalog` prints them).
- **A key is a turn from the bind.** A channel is the bone's own rotation in
  the model's frame, measured against the reset clip's first key, the bind,
  which the runtime carries as the skeleton's rest: a bone deforms by
  `key * bind^-1` [orig: AnimChannel_ComputeBoneMatrices @ 0x410da0;
  AnimMap_RegisterEntity @ 0x40bb60], so the reset starts at rest. A bone's
  bind is its rest turn, or the **bind frame** it keeps (a 3x3 turn of the
  rig's axes), and export writes each key as the bone's turn from its rest
  carried onto that bind, `key = pose * rest^-1 * bind`: a clip poses the rig
  in Blender as the game draws it, whichever way its bones point. Every rig
  the add-on builds points each bone toward its first child part more than a
  millimetre away (else on along its parent's direction, else up), and import
  never turns a rest bone: a table whose bind points a bone elsewhere
  (retail's point a bone's Y axis back toward its parent) gives the bone that
  bind as its bind frame, only where it turns more than 1e-4 degree from the
  rest, and poses each frame as `pose = key * bind^-1 * rest`. A re-export
  writes the keys and bone tables the table had, and the model is unchanged.
  The bind frames are the last imported table's; a lone `.bad` and a table
  without a reset row name no bind and leave them.
- **Bone names live in the clip.** A model's part table carries none, so a rig
  imported from a `.3di` alone names its bones `BN##`; a clip labels them
  (`BN16 L Hand`), and their vertex groups follow.

**A first-person gun's clips** animate its own rig, the bones its parts
are (they pair with the parts by the same index rule), and so the arms that
share it. The model export reads every rig at rest, so the model is its
authored layout whatever a clip is doing. A clip set is as wide as the rig it
was authored on: a first-person set belongs to the gun's rig (`mp5_1st.adm`
carries 40 channels; `ArmsG` is 37 bones and a mesh part).
