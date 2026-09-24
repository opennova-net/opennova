# Scene naming contract

The NovaLogic ASE/OED object-naming convention: the scene-object names that
carry a model's 3DI roles. OED classified them by `classify_name`
([orig: ConvertToInternal @ 0x4268B3], ported in the retired
`engine/formats/oed/convert_internal.cpp`); the Blender add-on
(`tools/blender/opennova_3di`, [ADR 0047](../adr/0047-blender-3di-exporter.md))
reads them to export and lays a model out by them to import, and a future
GLB/GLTF <-> 3DI seam keeps them.

Names are ASCII. Numeric identities are two digits and 1-based in the name
(`01` is the first), 0-based inside; `00` parses to -1 (a user point with no
part). A leading `!` makes an object ignored. Blender's own `.001`
duplicate suffixes are stripped before classification (object names are
unique per `.blend`, so LOD1's `PN01` is `PN01.001`); two objects that
classify to the same identity inside one LOD are an error.

| Scene element | Name form | Meaning |
| --- | --- | --- |
| Model root | any name (the Empty above the LOD roots) | one model, one `.3di`: its model name, output path and collision LOD are properties. A scene holds any number of models (a first-person gun and its arms, a hull and its turret); each exports in its root's own frame, so placing or mounting a model does not change it |
| LOD root | any name, custom property `_lod_index` | render LOD `_lod_index` (0 = primary); its threshold and RMDL type (`gnrc`, `bldg`, `door`, `veh0`) are properties. A root with no parts is an empty LOD (retail ships them) |
| Part | `PN##` (Empty) | 3DI subobject `##`; its origin is the pivot. A rotated `PN##` is a PANM rotation frame (an MTRX row): its tracks turn about the empty's axes (Dblkhwk1's canted tail rotor) |
| Part mesh | `## Mesh<n>` | mesh `<n>` of part `##` (sits under its `PN##`); a second UV map is the detail stage's UV1 |
| Part center | `_## center` | part `##`'s transform center (pivot) |
| Attachment | `~PPx attach` | sits under a child part: its parent is part `PP`; `x` (a, b, ...) tells siblings apart |
| User point | `UP<c>## <label>` | USRP point: type letter `c` (`G` 71 gameplay, `S` 83 effect), part `##` (`00` = none), label = the USRP name (no label: `Noname`); faces along its local +Z. Its export-order property keeps the USRP order (seats and effect points are scanned in it) |
| Light | `LP##[a..]` (a light object) | a LGHT light owned by part `##` (`01` the root, as `classify_name` parsed it); a point light is omni, a spot light a cone about its local +Z. Its colour is the start colour; the generator, attenuation and flags are properties |
| Bone | `BN##` (Armature bone) | part `##` of a skinned model: the head is the pivot, the parent bone the part parent; `BN##` vertex groups carry the weights (a weight-0 membership keeps a vertex whose weights are all zero, as dM1A1's LOD 3 stores them). Its PANM tracks, flags and track frame (the MTRX row: a rotation of the model's axes) are bone properties |
| Skinned mesh | `## Mesh<n>` (under the Armature) | geometry authored on part `##`. Every skinned strip is stored on the root, and each part keeps the bounds of what is authored on it. `##` is a bone (dM1A1's hull: `01 Mesh0` on `BN01`, with each wheel's own geometry on its bone in the collision LOD) or a mesh part numbered after the bones: parent 0, pivot = the mesh origin (ArmsG: 37 bones, then `38 Mesh0`) |
| Material | `Material_<i>_<SHADER>` | export order `i`, shader tag `SHADER` (any tag in the engine's shader table; the add-on's shader field renames the material). Without a tag, OED's default for the material's texture maps: `FF_ST_OP` for one, `FF_MT_OP` for two, `FFP_GLASS` for none (`VS_SKBASIC`, `VS_SKGLASS` on a skinned model). Glass, emissive and the alpha pass follow the shader |

## Collision volumes

`<TYPE>##[<dup>]-colonly`, on the primary LOD. `##` is the **owning part**
(`classify_name` stores it as the object index; the volume joins that part's
collision section). `<dup>` is a lowercase suffix for the 2nd and later volumes
of one type on one part: 2nd `a`, 3rd `b`, ..., `z`, then `aa` (the Blackhawk's
35 hull volumes are all `CB01...`); a section's volumes export in that order,
code first. The volume is the solid its faces bound, by the OED rule
(docs/threedi/o3d-scene-format.md): each face's plane, so the mesh must be
convex (export names one that is not). A ladder (`CL`) faces the plane of its
last face (Blender's Sort Mesh Elements can put a chosen face last).

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
`S` 0x4, `W` 0x8, `L` 0x10, `O` 0x20 (for example `BBVSO03`; Armry01's light
fixtures carry `BBL02`, `BBVSL03`). Type 14 shares the `LP` prefix with
lights; the `-colonly` suffix tells them apart. Runtime meanings:
docs/world/world-wac-ai-re.md §15. No name carries the flags of a non-`BB`
volume (Armry02 ships `CB` volumes with flags 1).

## Occlusion

`<PFX>##[<dup>][-<MM>]-occonly` meshes on the primary LOD, `##` the record's
parent section (1-based); the OCCL record type follows the prefix as the
retired exporter mapped it: `OB` 0 (occluder), `OS` 1 (open), `OP##` 2 (a
window to the exterior), `OP##-MM` 3 (a portal to section `MM`), `OH` 4 (no
witnessed runtime meaning). Faces wind counter-clockwise about the outward
normal; the planes follow the OED rule (docs/threedi/o3d-scene-format.md).
Armry01 lays out as `OS01`, `OB01`..`OB01c`, `OP02`, `OP02-04`, `OP04-03`.
An export-order property keeps the record order.

## Assemblies

Models the game draws together can share a scene. The add-on shows how the
game combines them, but the assembly settings never reach a `.3di`:

- **Bones follow** (on a skinned model): its `BN##` bones follow another
  model's `PN##` parts of the same index. Retail draws a first-person gun and
  the player's skinned arms with one array of bone matrices built from the
  gun's parts [orig: Player_RenderFirstPersonViewModel @ 0x4ded60;
  Entity_BuildBoneWorldMatrices @ 0x4df028]. Importing a gun and its arms
  together pairs them.
- **Mount on** + **user point** (on any model): the model sits on another
  model's user point, as an ITEMS.DEF `addeweap`/`addeweapC <userpoint>` child
  (the M1A1's turret on the hull's `ewep01`) sits on its parent. The name is
  matched whole, without regard to case, and the first match wins. A missing
  name leaves the child on the parent's root. The child faces the user
  point's direction [orig: build_bone_attachment_matrix @ 0x56C630;
  build_direction_look_at_matrix @ 0x612C90].

## Bullet faces

The collision faces bullets hit come from one render LOD's part meshes, chosen
by the OED `.3dp` `poly_collision_lod` setting (default 0, the most detailed;
Armry01's are LOD 1's), and that LOD's parts are the collision sections (one
each). Each face's surface type and flags come from its material: "both
sides" (1) follows Two sided; "bullets pass" (0x100, retail's rotor blades)
and "hit from behind" (0x800) are material settings. A skinned model with a
mesh part carries its bullet faces on that part's section (the retail person
layout). One without a mesh part carries them on each part's section, from the
geometry authored on each bone (dM1A1: LOD 1, 875 hull faces and 40 per
wheel).
