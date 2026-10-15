# `art/on_pist`: the base game's first-person pistol

`on_pist.blend` is the first-person pistol, the view model of `WPN_colt45` (our WK-11, named by the sidearm id the engine's default kits name): `on_pist Rig` (63 parts:
`BN01 Weapon origin`, `on_ar15 Rig`'s 54 arm bones in the same order, then the gun's eight, `BN56 Body`,
`BN57 Slide`, `BN58 Trigger`, `BN59 Hammer`, `BN60 Magazine`, `BN61 Cartridge` (under the magazine),
`BN62 Safety` and `BN63 Slide_Stop`), the gun's part meshes `56 Mesh` .. `63 Mesh` on their bones, the
`Hip view` and `Aim view` cameras (weapon.def `pos` and `tpos`) and the seven `on_pist_anim_*` Actions
the clip table names. Export Model and Export Animations on `on_pist` write every `assets/on_pist*` file
but the textures, which the editor imports (`assets/art/on_pist_{0,1}_c.png`, below). Materials and
textures: `SOURCES.md`.

## The arms

The game draws `on_arms.3di` with the gun's rig part for part: part *i* of the arms is bone *i* of the
rig. The rig's first 55 bones are `on_ar15 Rig`'s, unchanged and in their order, so the arms the AR-15
draws draw with the pistol as they are. `on_arms Skin` is kept in the scene for the previews;
`art/on_ar15/on_ar15.blend` stays the arms' source (an Export Model of `on_arms` here writes the same
model). The gun exports non-skinned, one rigid part per bone.

## Where the gun and the clips come from

The gun and the clips are KINEMATION's Tactical Shooter Pack WK-11 Viper (a 2011-pattern 9 mm pistol,
22.5 cm long): the mesh `SKM_WK-11_Viper`, split into its eight moving parts, and the pack's first-person
clips of the Manny skeleton (`A_FP_WK-11_Viper_*`) with the gun's own clips (`A_W_WK-11_Viper_*`).

- **The skeleton.** The pack's Manny is the UE5 Manny the AR-15's arms come from, and its bind pose is
  `on_pist Rig`'s rest: fitted on the arm bones, the map from the skeleton's root frame into the rig's is
  the AR-15's (the 1.07 m drop, no turn), within 0.01 mm. The pack's character clips are animation-only
  ASCII FBX, which Blender does not read and which carry no bind pose; Godot 4.6 reads them (its
  `FBXDocument`, then written as glTF), and the bind pose came from the one binary character FBX in the
  pack (`A_FP_MKR9_Idle`), the same skeleton.
- **Bone for bone.** Each arm bone follows its Manny bone (its rest offset from that bone kept), in the
  skeleton's root frame, sampled at 30 fps.
- **Where the gun sits.** As the pack's `TacticalWeaponBone` places it: the right hand's transform times
  the inverse of `ik_hand_r`'s, times `ik_hand_gun`'s, then turned by the pack's `weaponRotationOffset`
  for the Viper (90° about X, `TacAnimSettings_WK-11_Viper`). The gun's parts then follow the gun clip.
- **The clips** (frames counted as the game counts them, the last key's time):

| Action | KINEMATION clips | Frames at 30 fps |
|---|---|---|
| `on_pist_anim_wpn_idle` | `A_FP_WK-11_Viper_Idle_Pose` (one pose), under a breathing sway of our own (0.3° at most, a 2 s loop) | 60, cyclic |
| `on_pist_anim_wpn_fire` | `A_FP_WK-11_Viper_Fire` and `A_W_WK-11_Viper_Fire`, frames 0 to 8 (the kick and the slide's cycle, back at frame 1), the last 4 eased into the idle | 8 |
| `on_pist_anim_wpn_reload` | `A_FP_WK-11_Viper_Reload_Tactical` and `A_W_WK-11_Viper_Reload_Tac`, frames 0 to 88, the last 5 eased into the idle | 88 |
| `on_pist_anim_wpn_empty` | the idle pose, the trigger pulled and let go as `A_W_WK-11_Viper_Fire` pulls it (the pack has no first-person dry fire) | 20 |
| `on_pist_anim_wpn_switchto` | `A_FP_WK-11_Viper_Quick_Draw`, the last 3 frames eased into the idle | 25 |
| `on_pist_anim_wpn_switchfrom` | `A_FP_WK-11_Viper_Quick_Holster`, played in 14 frames for its 20 | 14 |
| `on_pist_anim_reset` | the rest pose | 1 |

- **The reload ends where the gun is back in hand.** The pack's tactical reload runs 3.47 s and settles
  after 2.93 s. The game blends nothing, and the reload refuses fire until its `delaystart` runs out, so
  the clip is cut at frame 88 (2.93 s), where the new magazine is home and the gun is back in the idle
  hold. The Action carries `ON:Active End` and `ON:Ready` on its last frame: the reload's weapon.def row
  is `delaystart 183`. The pack's empty reload (with the slide release) is not used: it starts with the
  slide locked back, which the game never shows (the gun idles slide forward, empty or not), and a
  clip must start where the one before it ends.
- **The draw is the pack's quick draw, timed by its clip**, as the AR-15's is: the row names
  `wpn_std_idle`, which runs on `delaystart` (53 ticks, the clip's 0.83 s) and then hands the slot to
  idle, and the Action carries `ON:Active End` and `ON:Ready` on its last frame. The holster is the
  pack's quick holster played faster (0.47 s), inside the switch timer of `wpn_std_switchfrom`, which
  swaps the next weapon in; it ends with the gun out of view, where the draw starts.

## The camera

The game's first-person camera moves by nothing a weapon clip carries (`art/on_ar15/README.md`, The
pack's camera), so `BN01 Weapon origin` stays at rest in every clip and the pack's head turn is left out.
The `Hip view` eye stands where the pack's camera is in the idle pose, moved by the opposite of the
pack's `weaponOffset` for the Viper (the gun 1.5 cm across and 3 cm up from the head, in the character's
root frame): the pack moves the gun and the hands with it, the game moves the eye, and the picture is
the same. The add-on writes it as `pos` (-4.332 -3.84 -115.511).

## Aiming

As the AR-15: `flags sighted` and `flags nocardswitch` keep the gun drawn while the scope key eases the
view from the `Hip view` eye to the `Aim view` eye (`pos` to `tpos`), and the view stays at 80°.

The sights are the Viper's fixed pair: the front post and the rear blade, its notch's top 3.2 mm above
the post's tip. The sight line runs from the post's tip to the top of the rear blade at the notch's
middle; in the idle pose it points 0.26° left of the view and 1.65° below it, with the gun canted 0.07°.
The `Aim view` camera stands on that line 17 cm behind the rear sight, looking down it with the gun's
top up. The add-on writes its place (`tpos` -32.708 16.642 -102.353); its turn is `tpos`'s rotation, set
by hand: yaw 359.74, pitch 1.654, roll 359.935 (a negative angle reads as 0 in the game, so a yaw of
-0.26 is written 359.74). The game turns the whole view model about the eye by it, so the post
stands in the notch at the middle of the screen.

## Re-baking a clip

The pack's files are not in the repository. To change a clip: convert the pack's `A_FP_WK-11_Viper_*`
and `A_W_WK-11_Viper_*` FBX to glTF with Godot (headless, `FBXDocument` then `GLTFDocument`), import
them into a scratch scene at 60 fps, sample the Manny's arm bones and `ik_hand_r`, `ik_hand_gun` and the
gun's bones at 30 fps, put each arm bone on its Manny bone and the gun on `ik_hand_gun` as above, leave
`BN01` at rest, ease the clip's ends into the idle's first pose, set the Action's frame range, Clip rate
(30) and markers, then Export Animations.
