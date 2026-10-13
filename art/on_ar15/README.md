# `art/on_ar15`: the base game's first-person carbine

`on_ar15.blend` is the first-person carbine and its arms: `on_ar15 Rig` (64 parts: `BN01 Weapon
origin`, the arms' 54 bones named after the UE5 Manny bones they come from, the gun's nine), the gun's
part meshes on its bones, `on_arms Skin` deformed by the same rig, the `Hip view` and `Aim view`
cameras (weapon.def `pos` and `tpos`) and the eight `on_ar15_anim_*` Actions the clip table names.
Export Model on `on_ar15` and `on_arms` and Export Animations on `on_ar15` write every `assets/on_ar15*`
and `assets/on_arms*` file. Materials and textures: `SOURCES.md`.

## Where the clips come from

The gun, the arms' rig and the clips are Infima Games' Tactical FPS Animations (TFA) assault rifle.
Each clip is the pack's first-person clip of the Manny skeleton, its arm and gun bones taken in the
skeleton's root frame (the rig's origin is 1.07 m above that root):

| Action | TFA clip | Frames at 30 fps |
|---|---|---|
| `on_ar15_anim_wpn_idle` | `A_TFA_FP_AR_Idle_Loop_Standing` | 60, cyclic |
| `on_ar15_anim_wpn_fire` | `A_TFA_FP_AR_Fire_Auto` | 44 |
| `on_ar15_anim_wpn_reload` | `A_TFA_FP_AR_Reload` (the tactical reload), cut at frame 79 | 79 |
| `on_ar15_anim_wpn_empty` | `A_TFA_FP_AR_Fire_Empty` | 34 |
| `on_ar15_anim_wpn_switchrank` | `A_TFA_FP_AR_FireModeSwitch` | 28 |
| `on_ar15_anim_wpn_switchfrom` | `A_TFA_FP_AR_Holster` | 12 |
| `on_ar15_anim_wpn_switchto` | `A_TFA_FP_AR_Holster` played backwards | 12 |

- **The reload ends where the pack unlocks.** The pack's montage lets the player act again at 2.632 s
  (`AN_UnlockActions`), with the gun back in hand, and blends the rest out. The game blends nothing,
  and the reload refuses fire until its `delaystart` runs out, so the clip is cut at frame 79 (2.633 s)
  and frames 74 to 79 ease every bone but the two magazines into the pose the pack's clip settles on.
  The reload's weapon.def row is `delaystart 164` (the add-on's timing, the clip's last frame).
- **The draw is the holster backwards.** The game paces a switch on its own timer, about 0.5 s
  (`WeaponAction_SwitchTo`, ±30 a tick from −900), and plays each clip at its own rate. The pack's
  draw (`A_TFA_FP_AR_Equip`, 2.33 s, the charging handle racked) does not fit it; the holster
  (0.4 s, the gun lowered out of view) does, and played backwards it starts where the holster ends.

## The pack's camera

The pack's first-person camera rides the Manny `head` bone, which its clips turn (up to 0.6° in the
idle, 1.3° in the fire, 2.1° in the holster; never moved). The game draws the viewmodel at the camera
and moves its camera by nothing a clip carries, so the head's turn is baked into `BN01 Weapon origin`
as its inverse: at each frame `BN01` is `X H0 H(t)^-1 X^-1` in the rig's space, where `H(t)` is the
head in the clip's skeleton, `H0` the head on the clip's first frame and `X` the map from that
skeleton's root frame into the rig's. The arms and gun then stand against the view as they do against
the pack's moving camera. Every clip starts and ends with `BN01` at rest, since a change of action
snaps to the next clip's first frame.

The reload keeps `BN01` at rest: the pack's tactical first-person reload, the only source of its head
track, is not among the files the clips were rebuilt from.

## Re-baking a clip

The pack's clips are not in the repository. To change one: put the TFA clip on a Manny armature in a
scratch scene, sample its arm and gun bones in the skeleton's root frame onto `on_ar15 Rig` (`X`
above: Godot/glTF Y-up to Blender Z-up, origin lowered 1.07 m), sample its `head` and key `BN01` as
above, set the Action's frame range and Clip rate (30), then Export Animations.
