# `art/on_person`: the player's third-person clips

The clips `on_person` (the third-person body in `art/on_player`) plays, made
on the same control rig as the first-person clips: each is retargeted onto
`KINE Operator`, fitted to the retail clip it stands in for, and baked onto
`on_person Rig` through its sockets. One Action holds both: its `KINE
Operator` slot is the clip to edit, its `on_person Rig` slot what exports.

**Status: local only.** Most sources come from packs whose terms are not
confirmed for a public repository, so only this README, the scripts and
`clips.json` are tracked. `work/` (the scene), `src/` (the source index and
any exported sources) and `export/` (the table and clips) are ignored, and
nothing lands in `assets/`.

## The clips

`clips.json` names, per clip, its source (a key of `src/source_index.json`),
the retail clip it stands in for, and how it is fitted. Every row of
retail's `US01.adm` that played that retail clip plays this one. A row no
entry fills is left out of the table, so it plays the reset clip (the
seats, emplaced guns, swim, ladder, roll, drag and parachute clips, the
emotes, and the knife, pistol, launcher, designator, binocular and mortar
holds, for now).

- A moving loop moves at retail's ground speed and heading (the game moves
  the body by the clip's step), played up to 35% faster or 20% slower to keep
  its feet planted (an entry may widen that). A source made in place is
  measured by its hips' pace over the planted foot.
- A one-shot keeps retail's length unless its entry says `"fit": "natural"`;
  its hips follow the source's, evened out to retail's whole travel, or the
  source's own (`"own_travel"`, the deaths).
- The hips stand at the source's height in KINE's measure, raised or lowered
  up to 8 cm to keep the lower foot where the source's stands.
- A loop's head stands at retail's height (its clip's mean `top`): the hips
  are raised or lowered up to 20 cm and the legs re-solved under them, each
  foot kept where it was, as far as the straightest leg reaches. The
  first-person eye is the head bone, so this keeps the eye where retail's
  stands in every stance; a holding pose (the weapon channel takes only its
  arms and head) and the prone clips are left as they are (`"eye": false`).
- `"mirror": true` plays the source left for right, so a left heading can
  be its right one's mirror.

| Rows | Source |
|---|---|
| Sprints (`run_2`, `run_3`) | KINEMATION Shared, `A_Locomotion_Stand_Sprint_Loop_IP` |
| Standing run forward, back, left, right | MocapOnline `W2_Run_F_Loop`, `W2_Jog_B/L/R_Loop` |
| Standing run, the four diagonals | Kubold Rifle Animset Pro strafe runs (the left ones the right ones mirrored) |
| Crouched, eight headings; prone; standing idle; jump loop; dive to prone | Kubold Rifle Animset Pro |
| Crouched idle, jump start | KINEMATION Shared |
| Rifle and SMG holds (hip and scoped), reload, second idle | Infima Games, Tactical FPS Animations (third-person) |
| Third idle | MocapOnline Rifle Basic 2.7A |
| Grenade hold and throw | Kubold Rifle Animset Pro |
| Deaths (every bullet, limb and grenade row, fire, pungi) | MocapOnline and Kubold deaths, by direction |

## The retarget

Per KINE bone, a source bone (or none) and a calibration that turns KINE's
rest onto the source's rest bone by bone: each bone aimed down its source
bone, the hands framed by their knuckles and the feet by their toes, a
bone's correction carried to its children. A frame's source turn from its
rest, applied to that calibrated KINE bone, is KINE's pose. A KINE bone the
source lacks between two it has (the UE4 skeletons' missing spine and neck
bones) takes the half-way turn; any other rides its parent. The forearm twist
bones take their share of the hand's roll. Sources: UE5 Manny (Infima) bone
for bone, UE4 mannequins (KINEMATION Shared, Kubold) and MocapOnline's
skeleton by name.

## Building it

Needs Blender 5.x, the OpenNova 3DI add-on (installed, or this checkout's
`tools/blender` with `ON3DI_CLI` naming an `opennova-3di`), and retail's loose
`US01.adm` and clips (the `OPENNOVA_JO_ASSETS` folder), read at build time
for each clip's speed, length, flags and triggers.

```text
blender -b --factory-startup --python art/on_person/index_sources.py -- kine=<KINEMATION>/Shared/Character/Animations/Generic kubold=<src>/kubold/EXPORT infima=<src>/infima/anims motus=<BLD_Rifle_Basic_27A2>/Animation
    # src/source_index.json: every source clip's length, speed and hips
blender art/on_player/on_player.blend -b --python art/on_person/build_clips.py -- <reference dir>
    # work/on_person.blend: every clips.json clip on KINE and on the person
blender art/on_person/work/on_person.blend -b --python art/on_person/build_clips.py -- <reference dir> run_f idle
    # rebuild some clips in the scene
blender art/on_person/work/on_person.blend -b --python art/on_person/export.py
    # export/: US01.adm, US01_rst.bad, US01_s<slot>.bad
```

The Infima sources come out of the Unreal project that holds the pack (the
kine worktree's `art/US01/ue_export.py`), the Kubold ones from the pack's
`SourceFiles.zip`.

To see it in game, mount a folder that hard-links the install's archives and
holds `assets/on_person.3di` as `US01.3di` with `export/`'s files beside it,
loose, and launch with `--loose --resource-dir <that folder>`.
