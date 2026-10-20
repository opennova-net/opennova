# `art/on_frag`: the base game's first-person fragmentation grenade

`on_frag.blend` is the first-person frag grenade, the view model of `WPN_GRENADEHE` (the engine's
fixed name): `on_frag Rig` (58 parts: `BN01 Pelvis`, `on_ar15 Rig`'s 54 arm bones in the same
order, then `BN56 Body` on `BN17 hand_r`, and `BN57 Spoon` and `BN58 Pin` under the body), the three
part meshes, `on_arms Skin` for the previews, the `Hip view` camera (weapon.def `pos`) and the seven
`on_frag_anim_*` Actions the clip table names. Export Model and Export Animations on `on_frag` write
every `assets/on_frag*` file (never Export Model on `on_arms`). The scene `on_frag bake` holds the
mesh the textures are baked from. The smoke grenade (`art/on_smok/`) is the same rig with a canister
on it, and plays this table. Materials and textures: `SOURCES.md`.

## The arms and the grip

The rig's first 55 bones are `on_ar15 Rig`'s, as `art/on_knif/README.md` says. `BN56 Body` hangs from
the right hand in a ball grip, measured off the hand: the grenade's centre straight out of the palm's
skin (the palm's normal across the knuckle line, 55% of the way from the wrist to the middle
knuckle), one radius and 2 mm out and 1.2 cm toward the fingers; the fuze toward the web of the thumb
and index; the spoon under the fingers, which hold the lever down; the pin's ring toward the wrist,
so with the hand upright the ring faces the eye and the left hand. Each finger closes from an open
hand until the glove meets the body (the deformed skin within 2 mm of it), the index lying just under
the fuze. It keeps its rest in every clip; the hand carries it.

## The camera

The `Hip view` camera is the original's grenade eye, `pos -33 8 -177` (`WPN_GRENADEHE`'s, and its
smoke's and flashbang's): the rig stands as the soldier, as the original's does
(`art/on_ar15/README.md`, The rig stands as the soldier).

## The clips

Hand-keyed in Blender, with no motion capture, and framed as the original's frag is framed (its
`frag_1i` and `frag_1f`, measured from the same eye). Each key places the right hand by the
grenade: where its centre stands before the eye, where its fuze and ring point; the hand follows
the grip, a two-bone arm solve puts it there (the elbow toward a pole, the clavicle lifted for the
windup, the forearm's turn shared with its twist bones) and Blender's bezier curves carry the bones
between the keys. The hold and the pull were chosen for the wrist: of the attitudes that face the ring
where it is wanted, the one whose hand lines up best with the forearm (the hold within 5 degrees of
the hand's rest line). Every clip is at 30 fps.

| Action | Rows | What it is | Frames |
|---|---|---|---|
| `on_frag_anim_reset` | `anim_reset` | the rest pose | 1 |
| `on_frag_anim_wpn_idle` | `anim_wpn_idle` | the hold, as the original's: the grenade upright low on the right (18 degrees right of the view's centre and below it), the hand upright behind it, the fuze up, the ring toward the eye; a breath | 60, cyclic |
| `on_frag_anim_wpn_fire` | `anim_wpn_fire` | the throw, the original's: the grenade brought in toward the middle (4), the left hand up from below, its index through the ring (6) and pulling the pin out to the left (9 to 15), dropping away with it (18); the grenade up past the right of the head (21 to 29), back over the top (32 to 35) and released in front at frame 38, the hand following through down out of view (40, 42); the next grenade up from below into the hold (46 to 54) | 54 |
| `on_frag_anim_wpn_hold` | `anim_wpn_recoil`, `anim_wpn_reload` | the hold, one frame: the next grenade is already in the hand | 1, cyclic |
| `on_frag_anim_wpn_empty_idle` | `anim_wpn_empty_idle` | the arms at rest below the view | 1, cyclic |
| `on_frag_anim_wpn_switchto` | `anim_wpn_switchto` | the draw: the grenade up from below the view, a little past the hold (11) and settling into it | 16 |
| `on_frag_anim_wpn_switchfrom` | `anim_wpn_switchfrom` | the holster: down out of view | 12 |

- **The throw is the original's.** `frag_1f` pulls the pin as the throw starts, winds up past the
  head, lets go in front and brings the next grenade up into the hold, all in its 54 frames, so the
  recoil and reload rows play a one-frame pose (`frag_1D`, `frag_1d`). Ours does the same, the left
  hand taking the pin; `BN58 Pin` rides the left index from the hook (frame 6) to frame 43, keyed on
  every frame, and stands in the next grenade's fuze from frame 44. A last grenade is never seen to
  come back: the game hides the view model once none is left (`NoClipsNoDraw`).
- **The release is the original's.** `WPN_GRENADEHE`'s fire row is `delaystart 80`, `delayend auto`
  (the original's `frag_1f` is 54 frames, as ours): the round leaves 80 ticks into the throw. The
  Action's `ON:Shot` marker on frame 38 gives `round(38 / 30 x 62) + 1 = 80`, so the grenade is in
  front of the eye on the tick the round launches. A press only starts the throw's charge; the throw
  plays from the release (`flags PowerThrow`).
- **The draw is timed by its clip**, as the AR-15's: `wpn_std_idle` on `delaystart` 34, `ON:Active
  End` and `ON:Ready` on its last frame (the original's draws on its one-frame pose through
  `wpn_std_switchto`'s timer).
- **The sounds follow the clips.** The throw's set (`GF_ONGNADE_THROW`, `onfrgthr.wav`) carries the
  pin's scrape and jingle at 0.30 s, as the left hand pulls it, and the arm's swing and the spoon at
  1.28 s and 1.33 s, the release; the draw's set (`GF_ONGNADE_ST`) plays the pouch (`onmedpsf.wav`),
  and the reload plays none, as the original's.

## Changing a clip

As the knife's (`art/on_knif/README.md`).
