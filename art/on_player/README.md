# `art/on_player`: the player's arms, body and control rig

`on_player.blend` holds the player: the control rig every clip is keyed on,
the first-person arms and the third-person body. A gun scene links its
`Player` collection and overrides it (`art/on_ar15/` does).

| Object | What it is |
|---|---|
| `KINE Operator` | The control rig: the KINEMATION Tactical Shooter Pack's Operator skeleton (UE5 Manny's), as the pack's FBX imports it (armature space in cm, object scale 0.01). Every gun's clips are keyed on it. |
| `KINE Eye` | The pack's first-person camera place, on the head bone (FBX head local (-2, -7.7, 3.5) cm). |
| `on_arms` | The model, exported to `assets/on_arms.3di`. Its root follows `KINE Eye` less the model-space eye (0, -0.13, 0.70) m, so a clip on the control rig poses the arms in view space. |
| `on_arms Rig` | The stock first-person arms' 37 bones: retail ArmsG's parents and pivots (`assembly.STOCK_ARMS`), in its bind (a T-pose, palms up, fingers straight). Each bone copies its control bone's world place and turn. The forearm takes its turn from `lowerarm_twist_02`, which splits the forearm's twist between the elbow and the wrist. |
| `on_arms Skin` | The Operator's gloves and the arms of its shirt, posed into that bind and weighted on the 37 bones (the clavicle, upper-arm twists and correctives onto the upper arm, the forearm twists onto the forearm, the metacarpals onto the hand). |
| `on_person` | The third-person body, exported to `assets/on_person.3di`: the whole Operator (its pieces joined, decimated to a drawn LOD and a lighter LOD for the bullet faces) on retail's 19 person bones in retail's order and bind, then 36 of ours (30 finger bones, 2 toes, 4 forearm twists), scaled to retail's hip height so the eye (the head bone) stands where retail's does. Its arms keep the Operator's lengths. |
| `PS ...` bones on `KINE Operator` | The person's sockets ("Person sockets" collection, hidden): one rigid child of each Manny bone, standing at the person bone's rest when KINE is in the person's bind. Each person bone copies its socket's place and turn in armature space (the person rig's armature space is KINE's, its object carrying the scale), so a clip keyed on KINE poses the body exactly. The constraints are muted: a clip scene bakes them, as a gun scene does. |

Because the rig is the stock 37, the game draws these arms correctly on every
stock first-person gun, and a stock character's arms correctly on every gun
built on this rig (ADR 0047; the add-on's `stock_arms_fit` note stays quiet).

That model-space eye is where the pack's camera sits, so the arms in this
scene stand where the pack shows them. In game the eye is each gun's own
`Hip view` camera (its `weapon.def` `pos`), framed for the game's 80 degree
view rather than the pack's wider one.

The person's rig is read from retail's `US01` when it is built (its 19
bones' order, pivots on the Operator's joints, retail's bind frames), as the
kine worktree's `art/US01/build_model.py` does, so retail's person clips play
on it. Its own clip set is not here: the clips made so far come from packs
whose terms are not cleared (`art/US01`), and they stay local until a clip
scene keyed on `KINE Operator` can ship.

Source: KINEMATION, Tactical Shooter Pack ("Tactical FPS Animations" 4.0.1,
Unity Asset Store), the Operator meshes and textures (downscaled to 1024).
The scene was built once by a throwaway script; edit it in Blender from here.
