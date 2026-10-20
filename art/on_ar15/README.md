# `art/on_ar15`: the base game's first-person carbine

`on_ar15.blend` is the first-person carbine and its arms: `on_ar15 Rig` (64 parts: `BN01 Pelvis`,
the arms' 54 bones named after the UE5 Manny bones they come from, the gun's nine), the gun's
part meshes on its bones, `on_arms Skin` deformed by the same rig, the `Hip view` and `Aim view`
cameras (weapon.def `pos` and `tpos`) and the eight `on_ar15_anim_*` Actions the clip table names.
Export Model on `on_ar15` and `on_arms` and Export Animations on `on_ar15` write every `assets/on_ar15*`
and `assets/on_arms*` file. Materials and textures: `SOURCES.md`.

## The rig stands as the soldier

The original's first-person rig is the soldier's own skeleton cut down to the arms: `M9k_1st`,
`ak47_1st` and the rest are T-posed character skeletons with `BN01 Pelvis` at the model origin and the
upper arms 0.587 m above it, and every first-person clip stands that skeleton on the ground, its
`bottom` the pelvis's height (1.069 m for the stock arms' clips; docs/net/novaworld-net-re.md §5.40,
the add-on README's Animations). So weapon.def `pos` and `tpos` are the eye measured from the soldier's
pelvis (the original's knife `pos 4 -5 -186`: the eye 0.727 m over it).

Every first-person rig here is built the same way on our soldier (`art/onjo1/person/onsold1.blend`):
`BN01 Pelvis` at the model origin, the upper arms 0.5934 m over it, where `onsoldb.3di` has them, and
each scene's model root standing at the soldier's rest hip height, 1.0185 m (`onp1_rst`'s `bottom`),
so every clip carries the soldier's hip height as the original's carry theirs. The eye cameras ride
with the arms, so each `pos` and `tpos` is the eye over the pelvis (the AR-15's 0.794 m, the pistol's
0.679 m). Nothing in the game reads a first-person clip's events, and the picture is the same either
way; this keeps the files what the original's are.

## Where the clips come from

The gun, the arms' rig and the clips are Infima Games' Tactical FPS Animations (TFA) assault rifle.
Each clip is the pack's first-person clip of the Manny skeleton, its arm and gun bones taken in the
skeleton's root frame, which stands 0.8424 m under `BN01 Pelvis` (the Manny arms sit lower on the
pelvis than our soldier's; the rig carried them 1.07 m over that root before it stood as the soldier):

| Action | TFA clip | Frames at 30 fps |
|---|---|---|
| `on_ar15_anim_wpn_idle` | `A_TFA_FP_AR_Idle_Loop_Standing` | 60, cyclic |
| `on_ar15_anim_wpn_fire` | `A_TFA_FP_AR_Fire_Auto` | 44 |
| `on_ar15_anim_wpn_reload` | `A_TFA_FP_AR_Reload` (the tactical reload), cut at frame 79 | 79 |
| `on_ar15_anim_wpn_empty` | `A_TFA_FP_AR_Fire_Empty` | 34 |
| `on_ar15_anim_wpn_switchrank` | `A_TFA_FP_AR_FireModeSwitch` | 28 |
| `on_ar15_anim_wpn_switchfrom` | `A_TFA_FP_AR_Holster` | 12 |
| `on_ar15_anim_wpn_switchto` | `A_TFA_FP_AR_Equip_Quick` | 47 |

- **The reload ends where the pack unlocks.** The pack's montage lets the player act again at 2.632 s
  (`AN_UnlockActions`), with the gun back in hand, and blends the rest out. The game blends nothing,
  and the reload refuses fire until its `delaystart` runs out, so the clip is cut at frame 79 (2.633 s)
  and frames 74 to 79 ease every bone but the two magazines into the pose the pack's clip settles on.
  The reload's weapon.def row is `delaystart 164` (the add-on's timing, the clip's last frame).
- **The draw is the pack's quick draw, timed by its clip.** The game's draw handler paces itself on
  a timer, about half a second for any weapon (`WeaponAction_SwitchTo @0x5431d0`: −900 by +30 a tick),
  and ignores the row's delays; an action runs the handler its `FUNCTION` names, so the row names
  `wpn_std_idle`, which runs on `DELAYSTART` (98 ticks, the clip's 1.57 s) and then hands the slot to
  idle, and the whole draw plays at its own speed (REVX02's AT4 draws the same way through
  `wpn_std_reload`, which also refills the magazine). Its Action carries `ON:Active End` and
  `ON:Ready` on its last frame, so the add-on's timing writes those keys (and the `FUNCTION`). It is
  baked from the pack's clip as the others are, every arm bone from its Manny bone and the grip from
  `ik_hand_gun` (where the pack attaches the gun), its last 4 frames eased into the idle's first pose.
  The holster is the pack's, 0.4 s, inside the switch timer that swaps the next weapon in; the draw
  starts with the gun out of view, where the holster leaves it.

## The pack's camera

The pack's first-person camera rides the Manny `head` bone, which its clips turn (up to 0.6° in the
idle, 1.3° in the fire, 2.1° in the holster and the reload, 2.7° in the draw). The game's first-person
camera moves by nothing a weapon clip carries: the engine samples a clip's per-frame events (velocity,
`bottom`, `top`) for a soldier's body only (`AnimChannel_InterpolateKeyframe @0x40B230`, reached only
through `AnimMap_UpdateDualChannels @0x40B8C0` from the infantry body and AI updates), never for the
first-person channel. So the camera stays still, and `BN01 Pelvis` stays at rest in every clip.
Baking the head's turn into `BN01` as its inverse was tried and dropped: it moves the arms and gun
against a still world, which does not read as the pack's moving camera.

## Aiming

The weapon.def entry is `flags sighted` and `flags nocardswitch`: the scope key eases the view from the
`Hip view` eye to the `Aim view` eye (`pos` to `tpos`, 15 steps), and the gun stays drawn instead of the
2D sights card. The view stays at 80°: the game narrows it by the slot's zoom (`scope_max_mag 2`) only
while the sights card is up, which `nocardswitch` never raises.

The sights are the pack's flip-up pair, both raised: the rear (its peep 10.7 mm across) and the front
(`SM_TFA_AR_Sight_Front`, added on the handguard's front-sight socket, `SOCKET_Sight_Front_Default`, in
the rear sight's material). In the idle pose the gun is canted about 9° and the sight line (the peep's
centre to the post's tip, parallel to the bore) points along the view to within 0.05°. The `Aim view`
camera stands on that line 13.95 cm behind the peep, looking down it with the gun's top up. The add-on
writes only its place (`tpos` 0.791 11.839 -193.877); its turn is `tpos`'s rotation, set by hand: yaw
0.018, pitch 0, roll 8.973. The game turns the whole view model about the eye by it, so the cant comes
out and the post's tip stands in the peep's centre at the middle of the screen.

## Re-baking a clip

The pack's clips are not in the repository. To change one: export the TFA clip from the pack's Unreal
project as FBX and import it into a scratch scene, sample its arm and gun bones onto `on_ar15 Rig` (the
map from the skeleton's root frame into the rig's: fitted on the arm bones, a turn and the 0.8424 m drop
from the skeleton's root to `BN01 Pelvis`), leave `BN01` at rest, set the Action's frame range and
Clip rate (30), then Export Animations.
