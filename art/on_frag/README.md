# `art/on_frag`: the base game's first-person fragmentation grenade

`on_frag.blend` is the first-person frag grenade, the view model of `WPN_GRENADEHE` (the engine's
fixed name): `on_frag Rig` (58 parts: `BN01 Weapon origin`, `on_ar15 Rig`'s 54 arm bones in the same
order, then `BN56 Body` on `BN17 hand_r`, and `BN57 Spoon` and `BN58 Pin` under the body), the three
part meshes, `on_arms Skin` for the previews, the `Hip view` camera (weapon.def `pos`) and the six
`on_frag_anim_*` Actions the clip table names. Export Model and Export Animations on `on_frag` write
every `assets/on_frag*` file (never Export Model on `on_arms`). The scene `on_frag bake` holds the
mesh the textures are baked from. The smoke grenade (`art/on_smok/`) is the same rig with a canister
on it, and plays this table. Materials and textures: `SOURCES.md`.

## The arms and the grip

The rig's first 55 bones are `on_ar15 Rig`'s, as `art/on_knif/README.md` says. `BN56 Body` hangs from
the right hand at the grip: the grenade's centre out of the palm (the palm's normal from the rest
fingers, 3.5 cm out, level with the knuckles, 1 cm toward the little finger), the fuze toward the
thumb and index, the spoon against the palm. The pin's ring stands on the grenade's left, where the
left hand's index hooks it.

## The clips

Hand-keyed, as the knife's (`art/on_knif/README.md`, The clips), at 30 fps; the right fist is index
55/65/40 degrees, middle 60/68/40, ring 65/68/40, little 70/68/40, thumb 25/45/35.

| Action | Rows | What it is | Frames |
|---|---|---|---|
| `on_frag_anim_reset` | `anim_reset` | the rest pose | 1 |
| `on_frag_anim_wpn_idle` | `anim_wpn_idle` | the grenade held palm up on the right, the fuze up; the pulled pin in the left hand, at rest below the view; a breathing sway | 60, cyclic |
| `on_frag_anim_wpn_fire` | `anim_wpn_fire` | the throw: up and back past the head (frames 9, 21), held, forward over the top (34), released at frame 38 as the fingers open, the grenade carried off along the throw for three frames then put away, the arm following through down and out of view (45, 54) | 54 |
| `on_frag_anim_wpn_empty_idle` | `anim_wpn_empty_idle`, `anim_wpn_recoil` | the empty hand: the arms at rest below the view, the grenade put away | 1, cyclic |
| `on_frag_anim_wpn_switchto` | `anim_wpn_switchto`, `anim_wpn_reload` | the draw with the pin pulled: the grenade up from below (12), the left hand coming in and hooking the ring (20), pulling the pin out to the left (27), dropping away below the view with it (38), the grenade settling into the hold (42) | 42 |
| `on_frag_anim_wpn_switchfrom` | `anim_wpn_switchfrom` | the holster: down out of view | 12 |

- **The release is the original's.** `WPN_GRENADEHE`'s fire row is `delaystart 80`, `delayend auto`
  (its `frag_1f` 54 frames, the same length as ours): the round leaves 80 ticks into the throw. The
  Action's `ON:Shot` marker on frame 38 gives `round(38 / 30 x 62) + 1 = 80`, so the grenade leaves the
  hand on the tick the round launches.
- **The reload is the draw.** The original's reload, recoil, empty and switch rows play one-frame
  poses (`frag_1d`); its next grenade appears in the hand. Here the reload row replays the draw, so
  the next grenade comes up and has its pin pulled (`delayend auto`, its 42 frames), and the recoil
  plays the empty hand. The draw is timed by its clip, as the AR-15's: `wpn_std_idle` on `delaystart`
  88, `ON:Active End` and `ON:Ready` on its last frame.
- **The pin goes with the left hand.** `BN58 Pin` is keyed in the fuze until the hook (frame 20), then
  with the left hand, at the offset it was hooked at, in every clip after (the idle, the throw and the
  holster key it there too), so it never shows again once pulled.

## Changing a clip

As the knife's (`art/on_knif/README.md`).
