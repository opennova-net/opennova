# `onsold1` sources: the person

`onsold1.blend` is the source of truth for OpenNova's person: the player's soldier
`onsold1`, the rebel rifleman `onenmy1`, the Avatars head and body `onsoldh` and
`onsoldb` (`onsold1` cut at the collar, drawing on `onsold1`'s textures), and the
clip set both rigs play. Its `bake_source` collection holds the dense meshes the
textures are baked from (`onsold1_hi`, `onenmy1_hi`); the baked textures under
`textures/` are what export writes into `assets/` (`onsold1_0.tga` and
`onenmy1_0.tga`, the diffuse with the specular mask in its alpha;
`onsold1_0n.mdt` and `onenmy1_0n.mdt`, the normal maps).

The models are our own, made from scratch in the look of the original game's
jungle soldiers: fatigues, a load-bearing vest or a chest rig, a helmet or a
boonie hat, boots, gloves, face paint, and the rebel's rifle, all modelled as
signed-distance shapes and baked. The original's `US01` was looked at for size,
proportions and its rig only. Each model stands on the original's 19 person
bones in the original's bind (the bone names, hierarchy, rest and the bind each
clip key is measured from), with the skinned mesh on a part of its own after
the bones, so the engine's person code and the original's clips drive it. No
geometry, texture or clip of the original is in this file.

Each model has three LODs (thresholds 100, 30 and 0). LOD 2 of `onsold1`,
`onsoldb` and `onsoldh` is its LOD 1 collapse-decimated at rest to 2,000, 1,000
and 1,000 triangles, keeping LOD 1's vertex groups, UVs and materials; their
first LOD 2s had folded into shards, drawn from about 30 m.

## Third-party material (CC0 only)

| Asset | Source | Author | Licence | Used for |
|---|---|---|---|---|
| Universal Animation Library [Standard] (`Idle_Loop`, `Pistol_Idle_Loop`, `Pistol_Reload`, `Walk_Loop`, `Jog_Fwd_Loop`, `Sprint_Loop`, `Crouch_Idle_Loop`, `Jump_Start`, `Jump_Loop`, `Swim_Idle_Loop`, `Swim_Fwd_Loop`, `Death01`) | [OpenGameArt](https://opengameart.org/content/universal-animation-library) (also [quaternius.com](https://quaternius.com/packs/universalanimationlibrary.html)) | Quaternius | CC0 1.0 | Retargeted onto the person rig: the idles, the walk, every run and crouched run (the strafes and back runs are the jog under a turned lower body or played backwards), the sprint, the jump, the swim and the first death; the pistol hold `on_pist_hold` (`Pistol_Idle_Loop` stretched to 2 s) and the pistol reload `on_pist_rload` (`Pistol_Reload` at its own 1.67 s) |
| Universal Animation Library 2 [Standard] (`Hit_Knockback`, `Idle_Lantern_Loop`, `Sword_Regular_A`, `OverhandThrow`) | [OpenGameArt](https://opengameart.org/content/universal-animation-library-2) | Quaternius | CC0 1.0 | The second death (the grenade deaths); the knife hold's free arm, head and breath (`Idle_Lantern_Loop`, 2.5 s over 2 s); the knife attack `on_knife_atk` (`Sword_Regular_A`'s slash at its own speed, between two eases from and back to the knife hold); the grenade hold `on_gren_hold` (`OverhandThrow`'s raised arm, held) and the grenade throw `on_gren_throw` (`OverhandThrow` from that arm through the release and follow-through, easing back to the hold) |
| `Fabric077` (1K JPG: Color, Displacement) | [ambientCG](https://ambientcg.com/view?id=Fabric077) | ambientCG (Lennart Demes) | CC0 1.0 | The fatigues' twill and the helmet cover |
| `Fabric048` (1K JPG: Color, Displacement) | [ambientCG](https://ambientcg.com/view?id=Fabric048) | ambientCG (Lennart Demes) | CC0 1.0 | The vest, pouches and webbing nylon |
| `Leather026` (1K JPG: Color, Displacement) | [ambientCG](https://ambientcg.com/view?id=Leather026) | ambientCG (Lennart Demes) | CC0 1.0 | Boots and gloves |

The ambientCG maps sit under `art/onjo1/materials/ambientcg/` as downloaded
(2026-10-05). The animation libraries are not in the repository: the clips in
the `.blend` are their retargeted, refitted bakes (CC0 permits it), and the
libraries themselves are downloaded again from the links above to rebuild a clip.

## Our own

Everything else is ours: the woodland and tiger-stripe camouflage and the face
paint (procedural noise), the colours, the rifle hold every standing, crouched
and airborne clip keeps as fixed turns in the model's frame (the original's
weapon channel replaces the arms with them), the kneel, the prone pose, the
crawls, the prone rolls, the dive to prone, the forward death, the reload and
the rebel's firing loop (procedural), each loop's step, hip height and head
height fitted to the original slot it answers.

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
