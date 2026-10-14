# `art/on_medp`: the base game's first-person medical pouch

`on_medp.blend` is the first-person medical pouch, the view model of `WPN_MEDPACK` (the engine's
fixed name, the medic's kit): `on_medp Rig` (56 parts: `BN01 Weapon origin`, `on_ar15 Rig`'s 54 arm
bones in the same order, then `BN56 Kit` on `BN17 hand_r`), the pouch's mesh, `on_arms Skin` for the
previews, the `Hip view` camera (weapon.def `pos`) and the six `on_medp_anim_*` Actions the clip table
names. Export Model and Export Animations on `on_medp` write every `assets/on_medp*` file (never
Export Model on `on_arms`). The scene `on_medp bake` holds the mesh the textures are baked from.
Materials and textures: `SOURCES.md`.

The original's medpack (`Medp_1st`, `medp_1st.adm`) is a small bag on the satchel charge's clips: a
held pose for the idle and every other slot, and a 30-frame use. This is the same simple kit: the
pouch lies on the right palm (`BN56 Kit` 4 cm out of the palm, its lid and patch up, its length across
the palm), the fingers curled under its edge (index 35/55/35 degrees, middle 38/58/35, ring 42/58/35,
little 46/58/35, thumb 20/15/10).

## The clips

Hand-keyed, as the knife's (`art/on_knif/README.md`, The clips), at 30 fps.

| Action | Row | What it is | Frames |
|---|---|---|---|
| `on_medp_anim_reset` | `anim_reset` | the rest pose | 1 |
| `on_medp_anim_wpn_idle` | `anim_wpn_idle` | the pouch on the palm, low on the right, its patch up; a breathing sway | 60, cyclic |
| `on_medp_anim_wpn_fire` | `anim_wpn_fire` | the use: the pouch pushed forward and down to the wounded man (10), pressed (14), held (19), brought back (30), the length of the original's use | 30 |
| `on_medp_anim_wpn_recoil` | `anim_wpn_recoil` | the idle's pose held, the original's one-frame recoil pose | 1, cyclic |
| `on_medp_anim_wpn_switchto` | `anim_wpn_switchto` | the draw: up from below the view on the right into the hold | 18 |
| `on_medp_anim_wpn_switchfrom` | `anim_wpn_switchfrom` | the holster: down out of view | 12 |

- **The use is the original's.** `WPN_MEDPACK`'s fire row is `delayend auto` with its `soundsetend`:
  the round (`medpack`, an instant kill zone of `rounds_kz_Medic`) heals as the button goes down and
  the clip plays out before the next; the recoil after it plays the held pose.
- **The draw is timed by its clip**, as the AR-15's (`wpn_std_idle` on `delaystart` 38, `ON:Active End`
  and `ON:Ready` on its last frame); the original draws on its held pose.
