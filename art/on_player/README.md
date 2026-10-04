# `art/on_player`: the player's first-person arms and control rig

`on_player.blend` holds what every first-person gun scene shares. A gun scene
links its `Player` collection and overrides it (`art/on_ar15/` does).

| Object | What it is |
|---|---|
| `KINE Operator` | The control rig: the KINEMATION Tactical Shooter Pack's Operator skeleton (UE5 Manny's), as the pack's FBX imports it (armature space in cm, object scale 0.01). Every gun's clips are keyed on it. |
| `KINE Eye` | The pack's first-person camera place, on the head bone (FBX head local (-2, -7.7, 3.5) cm). |
| `on_arms` | The model, exported to `assets/on_arms.3di`. Its root follows `KINE Eye` less the model-space eye (0, -0.13, 0.70) m, so a clip on the control rig poses the arms in view space. |
| `on_arms Rig` | The stock first-person arms' 37 bones: retail ArmsG's parents and pivots (`assembly.STOCK_ARMS`), in its bind (a T-pose, palms up, fingers straight). Each bone copies its control bone's world place and turn. The forearm takes its turn from `lowerarm_twist_02`, which splits the forearm's twist between the elbow and the wrist. |
| `on_arms Skin` | The Operator's gloves and the arms of its shirt, posed into that bind and weighted on the 37 bones (the clavicle, upper-arm twists and correctives onto the upper arm, the forearm twists onto the forearm, the metacarpals onto the hand). |

Because the rig is the stock 37, the game draws these arms correctly on every
stock first-person gun, and a stock character's arms correctly on every gun
built on this rig (ADR 0047; the add-on's `stock_arms_fit` note stays quiet).

The model-space eye puts a gun's `weapon.def` `pos` near retail's rifles'
(`-33.28 0 -179.2` against the M4's `-26.55 23.25 -165`).

Source: KINEMATION, Tactical Shooter Pack ("Tactical FPS Animations" 4.0.1,
Unity Asset Store), the Operator meshes and textures (downscaled to 1024).
The scene was built once by a throwaway script; edit it in Blender from here.
