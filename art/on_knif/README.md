# `art/on_knif`: the base game's first-person combat knife

`on_knif.blend` is the first-person knife, the view model of `WPN_KNIFE` and `WPN_KNIFE2` (the
engine's fixed names for the blue and red sides' knife): `on_knif Rig` (56 parts: `BN01 Pelvis`,
`on_ar15 Rig`'s 54 arm bones in the same order, then `BN56 Knife`, a child of `BN17 hand_r`), the
knife's mesh `56 Mesh` on its bone, `on_arms Skin` for the previews, the `Hip view` camera
(weapon.def `pos`) and the six `on_knif_anim_*` Actions the clip table names. Export Model
and Export Animations on `on_knif` write every `assets/on_knif*` file (never Export Model on
`on_arms`: `art/on_ar15/on_ar15.blend` stays the arms' source). The scene `on_knif bake` holds the
mesh with the bake materials the textures come from. Materials and textures: `SOURCES.md`.

## The arms and the grip

The rig's first 55 bones are `on_ar15 Rig`'s, unchanged and in their order (the scene was made from
`art/on_pist/on_pist.blend` with the pistol's bones taken out), so the arms the AR-15 draws draw with
the knife as they are. The knife exports non-skinned, one rigid part. `BN56 Knife` hangs from the
right hand in a hammer grip, its rest the hand's rest times the grip. The grip is the pistol's: the
right hand as `on_pist_anim_wpn_idle` holds the WK-11 (the KINEMATION pack's hand on a real grip),
the knife's handle laid where the pistol's grip runs, the spine against the palm along the backstrap
(raked as the grip is, 22 degrees), the edge toward the fingers, the guard just above the index
finger and the pommel under the little finger. The fingers are the pistol hand's, the index curled as
the middle finger is (it lies on the trigger guard on the pistol), each closed until the glove meets
the handle (the deformed skin within 2 mm of it all round). It keeps its rest in every clip; the hand
carries it.

## The camera

The `Hip view` camera is the original's knife eye, `pos 4 -5 -186` (`M9k_1st`'s): the rig stands as
the soldier, as the original's does (`art/on_ar15/README.md`, The rig stands as the soldier), so the
original's eye over the pelvis is the same place over ours. `BN01 Pelvis` stays at rest in every clip
(`art/on_ar15/README.md`, The pack's camera). A knife has no sights, so there is no `Aim view` and no
`tpos`.

## The clips

Hand-keyed in Blender, with no motion capture, and framed as the original's knife is framed: each key
places the knife before the eye (where its handle stands, how far its tip leans right and away, its
turn about the blade), a two-bone arm solve puts the hand on it (the shoulder fixed but for a little
reach in the clavicle across the body, the elbow toward a pole down and out, the forearm's turn shared
with its two twist bones, a half and a quarter), and Blender's bezier curves carry the bones between
the keys. The left arm stays at rest, below the view. Frames are counted as the game counts them, the
last key's time; every clip is at 30 fps.

| Action | What it is | Frames |
|---|---|---|
| `on_knif_anim_reset` | the rest pose | 1 |
| `on_knif_anim_wpn_idle` | the hold, as the original's `m9k_1i`: the knife upright low on the right (its handle 23 degrees right of the view's centre, the tip 6 degrees above it), the flat to the eye and the edge turned 20 degrees toward it, so its grind catches the light; a breath (at most 5 mm and 1 degree, a 2 s loop) | 60, cyclic |
| `on_knif_anim_wpn_fire_backhand` | the backhand: across to the left with the wrist turning the edge to the right (frames 2, 3), cut back across to the right (5 to 8), carried through low on the right (10), back to the hold (15) | 15 |
| `on_knif_anim_wpn_fire` | the forehand, the original's `m9k_1f` cut: cocked back to the right (2), swept across to the left with the edge leading (4 to 8), carried through low on the left (10), back to the hold (15) | 15 |
| `on_knif_anim_wpn_switchto` | the draw: up from below the view on the right, the blade turning upright, a little past the hold (10) and settling into it | 21 |
| `on_knif_anim_wpn_switchfrom` | the holster: tipped right and down out of view | 12 |

- **The slash plays in the recoil, as the original's knife plays it.** The original's `WPN_KNIFE`
  fires on `anim_wpn_idle` with no delays and runs its `recoil` on `anim_wpn_fire` with `delaystart
  auto`, so the round (`AMMO_ON_KNIFE`, an instant kill zone) lands as the button goes down and the
  slash then plays out before the next. The table follows `M9k_1st.adm`, the slash on the
  `anim_wpn_fire` row, 15 frames as the original's `m9k_1f`. Its weapon.def rows: fire `anim
  anim_wpn_idle`, `delaystart 0`, `delayend 0`; recoil `anim anim_wpn_fire`, `delaystart auto` (the
  clip's 0.5 s, 32 ticks as the editor bakes it).
- **The slashes take turns.** The fire row holds two clips, the backhand (`on_knif_f`) and the
  forehand (`on_knif_f2`): a row's clips are a ring the game serves from its last clip back, one step
  each play, so the first slash is the forehand and the next the backhand. Both are 15 frames, so the
  recoil's `auto` bakes the same delay whichever is up.
- **The draw is timed by its clip**, as the AR-15's and the pistol's: the row names `wpn_std_idle`,
  which runs on `delaystart` (44 ticks, the clip's 0.7 s), and the Action carries `ON:Active End` and
  `ON:Ready` on its last frame. The original's knife draws on a one-frame pose (`m9k_1d`) through
  `wpn_std_switchto`'s timer instead. The holster runs inside the switch timer of
  `wpn_std_switchfrom`.

## Changing a clip

The clips' keys are in the Actions (the bones keyed on the key frames, bezier between them). To
change one, pose the rig in Blender and key it, keep the Action's frame range, Clip rate (30) and
markers, then Export Animations. The editor's first-person preview of `on_knif.adm` and its weapon
range (`weapon.def`, WPN_KNIFE, Fire) show it as the game draws it.
