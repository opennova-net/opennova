# `art/on_ar15`: OpenNova's first-person carbine

`on_ar15.blend` is the KINEMATION Tactical Shooter Pack's TR15 (an AR-15
pattern carbine with a holographic sight and a vertical grip) as a first-person
gun. It exports `assets/on_ar15.3di`, `on_ar15.adm` and the `on_ar15_*` clips
and textures. It replaces the earlier `on_ar15`, whose arms were not the stock
rig.

## The scene

- `Player` (linked from `../on_player/on_player.blend`, overridden): the
  control rig `KINE Operator`, `KINE Eye` and the arms. The arms deform with
  this gun's rig, under its root, for display; they export from `art/on_player`.
- `KINE TR15` and `KINE TR15 Mag`: the pack's gun and magazine rigs on the
  control rig's `ik_hand_gun`, with the sight and grip on their sockets.
- `TR15_*` Actions: the pack's clips, keyed on the control rigs (the
  character and weapon clip of a name share one Action). `TR15_Gen_*` are the
  pack's general clips, kept for later use.
- `on_ar15` (the model): `on_ar15 Rig` and the part meshes on its bones, the
  user points `bcasing`, `bullet` and `MFLASH01` on `BN38 Body`, and the
  `Hip view` and `Aim view` cameras. The hip eye frames the gun low and right
  as retail's rifles stand at the game's 80 degree view (`pos -12.8 4 -195`;
  the pack's own camera place, framed for a much wider view, shows it too big
  and high); the aim eye is 15 cm behind the sight's window, the pack's eye
  relief (`tpos -52.245 25.365 -179.092`). The root follows `KINE Eye` like
  the arms' does.

## The rig

| Parts | Bones |
|---|---|
| BN01-BN37 | The stock first-person arms (the player rig's rest) |
| BN38 Body | The gun |
| BN39-BN46 | Bolt, Charger, Magazine, Trigger, Safety, Cartridge, Dustcover, MagRelease |
| BN47 Follower, BN48-BN50 Round1-3 | The magazine's follower and its top three rounds |

## The clips

The `on_ar15_*` Actions are what the rows name. They are baked keys on
`on_ar15 Rig`: each bone's world place and turn, taken from the control rig
playing a pack clip, frame by frame.

| Slot | From |
|---|---|
| idle | `TR15_Idle_Grip` (10.7 s, the loop with a regrip), 30 fps |
| fire, recoil | the idle pose, the bolt cycling (`TR15_W_Fire`) and a kick about the stock, 60 fps: fire is its first 3 frames, recoil the rest |
| reload | `TR15_Reload_Tactical`, 30 fps |
| empty | the idle pose with the trigger pulled |
| emptyidle | the idle pose with the bolt held back (`TR15_W_Fire_Out`'s last frame) |
| switchto, switchfrom | `TR15_Draw`, `TR15_Holster`, at a rate that plays each in half a second (the game paces them on its switch timer) |
| switchrank | the idle pose with a short roll of the gun |

To rework a clip: edit the `TR15_*` Action on the control rigs, turn on the
Copy Location and Copy Rotation constraints of `on_ar15 Rig`'s bones (they
are muted so the baked keys play), bake the rig into the `on_ar15_*` Action
(Pose > Animation > Bake Action, Visual Keying), and turn the constraints off
again.

The weapon entries `WPN_M4AUTO` and `WPN_M4` are timed at 536 rounds a
minute, the 7-tick shot of retail's M4 (fire 3 ticks, recoil 2).

## Mag feeding

The rounds and the follower carry part tracks on OpenNova's control
registers: `WPN_ROUND_1` to `WPN_ROUND_3` scale a spent top round to nothing,
and `WPN_SPENT` slides the follower 16 cm up the magazine as it empties. The
retail executable reads those names as `LOD_FRAC`, which nothing writes, so
there every track stays at its start: a full magazine.

Source: KINEMATION, Tactical Shooter Pack ("Tactical FPS Animations" 4.0.1,
Unity Asset Store), the TR15 meshes, textures (downscaled to 1024) and clips.
The scene was built once by a throwaway script; edit it in Blender from here.
