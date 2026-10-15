# `onsold1` sources: the person

`onsold1.blend` is the source of truth for OpenNova's person: the player's soldier
`onsold1`, the rebel rifleman `onenmy1`, the Avatars head and body `onsoldh` and
`onsoldb` (`onsold1` cut at the collar, drawing on `onsold1`'s textures), and the
clip set both rigs play. Its `bake_source` collection holds the dense parts the
textures are baked from (`onsold1_hi_*`, `onenmy1_hi_*`); the baked textures
under `textures/` are what export writes into `assets/` (`onsold1_diffuse.png`
and `onenmy1_diffuse.png`, 2048 a side, the diffuse with the specular mask in its
alpha, written as `onsold1_0` and `onenmy1_0`, whose `.dds` the import sources
`assets/art/onsold1__src.png` and `assets/art/onenmy1__src.png` make;
`onsold1_normal.png` and `onenmy1_normal.png`, 512 a side, written as
`onsold1_0n.mdt` and `onenmy1_0n.mdt`).

## The models

Both persons are KINEMATION's Tactical Shooter Pack "Operator" (its modular
parts), used under the maintainer's licence for the pack (2026-10-13; the
pistol's pack, `art/on_pist/SOURCES.md`), refitted and dressed as the original
game's jungle soldiers; the original's `US01` was looked at for proportions,
polygon and texture budgets, the drawing mode and its rest hand only. Each model
stands on the original's 19 person bones in the original's bind (the bone names,
hierarchy, rest and the bind each clip key is measured from; the rigs, their
user points and every clip are as they were), with the skinned mesh on a part of
its own after the bones, so the engine's person code and the original's clips
drive it. No geometry, texture or clip of the original is in this file.

The pack's parts stand on the UE5 Manny skeleton in Manny's rest. Each was moved
onto the person rig's rest by its own skinning: every Manny bone that answers one
of the rig's joints was given the affine map taking it to that joint (turned onto
the rig's bone direction, stretched along the bone to the rig's length and
widened across it by a girth factor, 1.04 to 1.12, the original's soldier being
broader and taller than the pack's), the bones between those (twist and
corrective bones) the map of the one above them, and the pack's own weights
blend the maps. The torso's height was mapped piecewise from the pack's thighs,
shoulders, neck and head to the rig's; the boots keep their soles on the ground.
The hands were closed into a grip with the index finger along it (the original's
rest hand holds a weapon so) and each turned about its wrist onto the original's
rest hand's shape (`US01`'s hand vertices, a shape fit only). The weights are
then the pack's merged onto the rig's 19 bones by name (`pelvis` and `spine_01`
on the hips, `spine_02` and `spine_03` on the lower spine, `spine_04` shared by
both spines, `spine_05` on the upper spine, both necks on the neck, each finger
on its hand), four at most a vertex.

Each model has three LODs (thresholds 100, 30 and 0), each part decimated to a
budget: LOD 0 from the pack's part (its loose pieces under 2.5 cm dropped, which
stay in the bake), LOD 1 and LOD 2 from LOD 0, or for the vest, the hip pouches,
the boots and the rebel's rifle a closed hull (the part solidified, voxel
remeshed at 1.2 cm and 2 cm, the rifle at 3.5 mm, 6 mm and 1 cm, then decimated),
which keeps those parts whole where a decimated shell folds into shards. Every
LOD has UV islands of its own in the one atlas (LOD 0 the pack's islands, scaled
to one texel density with the face at 2.2 times it, packed), and every LOD is
baked from the dense parts (selected to active, part by part): the colour, the
specular mask and the tangent-space normals. The face attribute `on_part` on each
LOD mesh names the part a face came from (`onsold1`: 0 head, 1 helmet, 2 shirt,
3 vest, 4 hip pouches, 5 trousers, 6 boots, 7 gloves; `onenmy1`: 0 head,
1 bandana, 2 shirt, 3 chest rig, 4 trousers, 5 boots, 6 hands, 7 rifle).

| Model | LOD 0 | LOD 1 | LOD 2 |
|---|---|---|---|
| `onsold1` | 8,598 triangles | 3,076 | 1,228 |
| `onsoldh` | 1,999 | 697 | 279 |
| `onsoldb` | 6,599 | 2,379 | 949 |
| `onenmy1` | 8,059 | 2,758 | 1,097 |

(The original's soldiers are 3,000 to 4,600 triangles at LOD 0, `US01` one LOD of
3,298, on five textures of 256 to 512 a side; its 2048 textures are few.)

The soldier `onsold1` wears the Operator's head, helmet, combat shirt, plate
carrier with its pouches, hip pouches and drop-leg holster, trousers, boots and
gloves: woodland fatigues and helmet cover, an olive vest and pouches, black
boots and olive gloves, as the first-person arms (`art/on_ar15/SOURCES.md`). The
head's bust below the collar is cut away; its scalp under the helmet's rim is
short dark hair. `onsoldh` is the head and the helmet, `onsoldb` everything
else, each LOD of each its LOD of `onsold1`.

The rebel `onenmy1` wears the same head (darker skin, black hair, stubble) under a
red bandana with its knot's two tails, a tiger-stripe shirt, olive trousers
(their belt; the holster straps cut away), brown boots and bare hands. His chest
rig is the plate carrier's front magazine pouches alone on webbing of our own: two
shoulder straps that cross on the back and a waist strap round it, ribbons laid on
the shirt's surface. The rifle in his right hand is the pack's AK-105 (its
cartridges dropped), along the barrel line today's rifle had and turned about it
so its pistol grip runs through the fist, which it was slid to; `MFlash01` moved
with it to its muzzle (part 16, about 11 cm from where it was).

To bake again: each `*_hi_<part>` in `bake_source` carries its bake materials
(`<model>_src_<part>_<material>`, fake users), which read the pack's textures
from the pack's folder (`.../KINEMATION/TacticalShooterPack/Meshes/`) and emit the
part's colour, or with their `spec_switch` value at 1 its specular mask, over the
pack's normal map. Separate a LOD mesh by `on_part`, give the pieces one material
with a 2048 image node, and bake each part's dense mesh onto its piece (selected
to active, extrusion 1.2 cm, ray 4 cm, without clearing): Emit for the colour
(sRGB), Emit with `spec_switch` at 1 for the mask (Non-Color), Normal for the
normals. The diffuse takes the mask as its alpha; the normals are box-filtered
to 512.

## The pack

The pack's files are not kept here: the `.blend` holds only what was made from
them.

| What | Pack file | Used for |
|---|---|---|
| The parts | `Meshes/Character/Operator/Modular/SK_Head_01a.FBX`, `SK_Helmet_01a.FBX`, `SK_Shirt_01a.FBX`, `SK_Vest_01a.FBX`, `SK_Hips_Pouches_01a.FBX`, `SK_Pants_01a.FBX`, `SK_Boot_01a.FBX`, `SK_Glove_01a.FBX`, `SK_Hand_01a.FBX` | The soldier's head, helmet, shirt, vest, hip pouches, trousers, boots and gloves; the rebel's head, shirt, chest-rig pouches (from the vest), trousers, boots and hands; their weights |
| Their textures | `Meshes/Character/Operator/Textures/TX_{Body,Eye,Mask,Shirt,Vest,Pouches,Pants_01a,Boot,Glove}_{BaseColor,Normal,OcclusionRoughnessMetallic}.TGA` | The skin and eyes as they are; elsewhere the folds, seams and stitching (the base colour's luminance) and the occlusion under our colours; the normal maps; the specular mask from the roughness and metalness |
| The rifle | `Meshes/Weapons/AK105/SKM_AK105.FBX`, `SKM_Mag_AK105.FBX` | The rebel's rifle |
| Its textures | `Meshes/Weapons/AK105/Textures/T_AK105_{Body,Rail}_{BaseColor,Normal,Roughness,Metallic}.TGA` | The rifle's colour, normals and specular mask |

## Third-party material (CC0)

| Asset | Source | Author | Licence | Used for |
|---|---|---|---|---|
| Universal Animation Library [Standard] (`Idle_Loop`, `Pistol_Idle_Loop`, `Pistol_Reload`, `Walk_Loop`, `Jog_Fwd_Loop`, `Sprint_Loop`, `Crouch_Idle_Loop`, `Jump_Start`, `Jump_Loop`, `Swim_Idle_Loop`, `Swim_Fwd_Loop`, `Death01`) | [OpenGameArt](https://opengameart.org/content/universal-animation-library) (also [quaternius.com](https://quaternius.com/packs/universalanimationlibrary.html)) | Quaternius | CC0 1.0 | Retargeted onto the person rig: the idles, the walk, every run and crouched run (the strafes and back runs are the jog under a turned lower body or played backwards), the sprint, the jump, the swim and the first death; the pistol hold `on_pist_hold` (`Pistol_Idle_Loop` stretched to 2 s) and the pistol reload `on_pist_rload` (`Pistol_Reload` at its own 1.67 s) |
| Universal Animation Library 2 [Standard] (`Hit_Knockback`, `Idle_Lantern_Loop`, `Sword_Regular_A`, `OverhandThrow`) | [OpenGameArt](https://opengameart.org/content/universal-animation-library-2) | Quaternius | CC0 1.0 | The second death (the grenade deaths); the knife hold's free arm, head and breath (`Idle_Lantern_Loop`, 2.5 s over 2 s); the knife attack `on_knife_atk` (`Sword_Regular_A`'s slash at its own speed, between two eases from and back to the knife hold); the grenade hold `on_gren_hold` (`OverhandThrow`'s raised arm, held) and the grenade throw `on_gren_throw` (`OverhandThrow` from that arm through the release and follow-through, easing back to the hold) |

The animation libraries are not in the repository: the clips in the `.blend` are
their retargeted, refitted bakes (CC0 permits it), and the libraries themselves
are downloaded again from the links above to rebuild a clip.

## Our own

Everything else is ours: the woodland and tiger-stripe camouflage (procedural
noise in each part's own space), the colours, the fit onto the original's rig,
the grip, the chest rig's straps and the bandana, the rifle hold every standing,
crouched and airborne clip keeps as fixed turns in the model's frame (the
original's weapon channel replaces the arms with them), the kneel, the prone
pose, the crawls, the prone rolls, the dive to prone, the forward death, the
reload and the rebel's firing loop (procedural), each loop's step, hip height and
head height fitted to the original slot it answers.

The hold clips the weapon channel plays (the `anim_knife`, `anim_pistol`,
`anim_grenade`, `anim_knife_attack`, `anim_grenade_attack`, `anim_binoculars`,
`anim_reload2` and `anim_idle_3` rows) key only the bones that channel takes
(both clavicles, upper arms, forearms and hands, the neck and the head) over a
torso left at rest, since the game takes those bones' turns in the world from the
hold and everything else from the body's own clip. The sources above are carried
onto the rig by the direction of each bone; the attacks and the grenade clips
keep their arms as the source's chest carries them, with a third of the chest's
own turn, so the source's lunge does not throw the arms about over a torso that
stays still. Ours, keyed by hand: the knife arm of the knife hold (the hand low
by the right hip), the binoculars (the left hand at the eyes, the right arm down
with the weapon, a slow breath) and the scoped hold `on_scope` (our rifle hold
`on_idle` raised to the cheek with the head bowed onto the stock, over 2 s).
What was taken from the original's own hold clips (`US01.adm`'s `IDL_PST`,
`IDL_KNF`, `IDL_GND`, `IDL_BNC`, `Dt1idlA3`) is measurement only, no clip: their
lengths and loop flags, where the hands sit against the head or the chest (the
knife hand, the grenade hand, both binocular hands, the scoped hold's hands), and
the right hand's turn from rest in the pistol, knife and grenade holds and the
left hand's at the binoculars, which each hold copies so the item the game hangs
from the hand at a fixed turn (the knife, a grenade, the pistol while it
reloads, the binoculars) points where the original's does.

The clips' sounds are their Actions' event markers, which Export Animations writes
as the events' trigger bits: `FOOT_LEFT` and `FOOT_RIGHT` where a foot plants
(the footsteps), `FOLEY_4` on the swim strokes and `FOLEY_5` on the crawls (the
sound profile's `SSAudio4` and `SSAudio5`). Every crawl marks the start of each
knee's stroke, two to a cycle: frames 2 and 17 of the backward crawl, 2 and 18
of the side crawls, as the original's crawls mark theirs, and frames 2 and 32 of
the forward crawl `on_crawl_f`. The original's own forward crawl (`Dt1PrF`)
carries no event and so crawls in silence; ours sounds like its other crawls.
