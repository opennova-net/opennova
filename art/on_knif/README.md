# `art/on_knif`: the base game's first-person combat knife

`on_knif.blend` is the first-person knife, the view model of `WPN_KNIFE` and `WPN_KNIFE2` (the
engine's fixed names for the blue and red sides' knife): `on_knif Rig` (56 parts: `BN01 Weapon
origin`, `on_ar15 Rig`'s 54 arm bones in the same order, then `BN56 Knife`, a child of `BN17
hand_r`), the knife's mesh `56 Mesh` on its bone, `on_arms Skin` for the previews, the `Hip view`
camera (weapon.def `pos`) and the five `on_knif_anim_*` Actions the clip table names. Export Model
and Export Animations on `on_knif` write every `assets/on_knif*` file (never Export Model on
`on_arms`: `art/on_ar15/on_ar15.blend` stays the arms' source). The scene `on_knif bake` holds the
mesh with the bake materials the textures come from. Materials and textures: `SOURCES.md`.

## The arms

The rig's first 55 bones are `on_ar15 Rig`'s, unchanged and in their order (the scene was made from
`art/on_pist/on_pist.blend` with the pistol's bones taken out), so the arms the AR-15 draws draw with
the knife as they are. The knife exports non-skinned, one rigid part. `BN56 Knife` hangs from the
right hand, its rest the hand's rest times the grip: the handle's axis along the knuckle line from the
pinky to the index (the blade out of the thumb's side), its centre in the loops the curled fingers
make, the spine toward the palm. It keeps its rest in every clip; the hand carries it.

## The clips

Hand-keyed in Blender, with no motion capture: each frame poses the knife's frame and a two-bone arm
solve (the shoulder fixed, the elbow toward a pole down and out to the right, the forearm's turn shared
with its two twist bones, a half and a quarter) puts the hand on it; the fingers hold one fist (index
60/76/42 degrees on its three joints, middle 66/80/42, ring 72/80/42, little 76/80/42, the thumb
45/40/35 over them). The left arm stays at rest, below the view. Frames are counted as the game counts
them, the last key's time; every clip is at 30 fps.

| Action | What it is | Frames |
|---|---|---|
| `on_knif_anim_reset` | the rest pose | 1 |
| `on_knif_anim_wpn_idle` | the knife low on the right, the point forward, up and left, the edge down; a breathing sway (at most 3.5 mm and 0.8 degrees, a 2 s loop) | 60, cyclic |
| `on_knif_anim_wpn_fire` | the slash: wound back up to the right (frame 3), swept across to the left and down with the edge leading (5, 7), carried through (9), back to the hold (15) | 15 |
| `on_knif_anim_wpn_switchto` | the draw: up from below the view on the right, the blade turning up, settling into the hold | 21 |
| `on_knif_anim_wpn_switchfrom` | the holster: down out of view | 12 |

- **The slash plays in the recoil, as the original's knife plays it.** The original's `WPN_KNIFE`
  fires on `anim_wpn_idle` with no delays and runs its `recoil` on `anim_wpn_fire` with `delaystart
  auto`, so the round (`AMMO_KNIFE01`, an instant kill zone) lands as the button goes down and the
  slash then plays out before the next. The table follows `M9k_1st.adm`: the slash is the
  `anim_wpn_fire` clip, 15 frames as the original's `m9k_1f`. Its weapon.def rows: fire `anim
  anim_wpn_idle`, `delaystart 0`, `delayend 0`; recoil `anim anim_wpn_fire`, `delaystart auto` (the
  clip's 0.5 s, 32 ticks as the editor bakes it).
- **The draw is timed by its clip**, as the AR-15's and the pistol's: the row names `wpn_std_idle`,
  which runs on `delaystart` (44 ticks, the clip's 0.7 s), and the Action carries `ON:Active End` and
  `ON:Ready` on its last frame. The original's knife draws on a one-frame pose (`m9k_1d`) through
  `wpn_std_switchto`'s timer instead. The holster runs inside the switch timer of
  `wpn_std_switchfrom`.

## The camera

As the AR-15's and the pistol's (`art/on_ar15/README.md`, The pack's camera): `BN01 Weapon origin`
stays at rest in every clip. The `Hip view` camera is the pistol's eye (`pos -4.332 -3.84 -115.511`);
a knife has no sights, so there is no `Aim view` and no `tpos`.

## Changing a clip

The clips were keyed by a throwaway script from the poses above (the knife's frame at each key frame,
eased between them, the arm solved and every bone keyed on every frame). To change one, pose the rig
in Blender, key it, keep the Action's frame range, Clip rate (30) and markers, then Export
Animations.
