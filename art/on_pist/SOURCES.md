# `on_pist` sources

## The pack

The pistol's mesh, its textures' source and its clips are from KINEMATION's Tactical Shooter Pack (the
WK-11 Viper, `README.md`), used under the maintainer's licence for the pack (2026-10-13). This is an
exception to the base game's CC0 rule, as the AR-15's Infima Games pack is (`art/on_ar15/README.md`).
The pack's files (its meshes, textures, clips and sounds) are not kept here: the `.blend` holds only
what was made from them, and the files below name what each was made from.

| What | Pack file | Used for |
|---|---|---|
| The gun | `Meshes/Weapons/WK-11_Viper/SKM_WK-11_Viper.FBX` | The eight part meshes (11,210 triangles), on the gun's bones as the pack's gun clips move them |
| Its textures | `Meshes/Weapons/WK-11_Viper/Textures/T_WK-11_Viper_{Body,Magazine}_{BaseColor,Emission}.TGA` | `on_pist_0_c.tga` (the body) and `on_pist_1_c.tga` (the magazine), below |
| The clips | `Animations/WK-11_Viper/Character/A_FP_WK-11_Viper_*.FBX`, `Animations/WK-11_Viper/Weapon/A_W_WK-11_Viper_*.FBX` | The seven Actions (`README.md`, The clips) |
| The bind pose | `Animations/MKR9/Character/A_FP_MKR9_Idle.FBX` | The Manny skeleton's bind pose, which the animation-only clips lack |
| The gun's placement | `Animations/WK-11_Viper/TacAnimSettings_WK-11_Viper.asset`, `Scripts/Animation/TacticalShooterPlayerJob.cs` | `weaponRotationOffset` and `weaponOffset`, and how the pack applies them |

## The pistol's materials

The pistol's two materials `on_pist_0` (the body, the slide and the small parts) and `on_pist_1` (the
magazine and its cartridge) draw with `VS_PHONGT`, as the AR-15's and the original's first-person
`M9_1st` do: a Phong highlight whose strength is the diffuse texture's alpha, through a tangent-space
normal map. The normal map is flat (`textures/on_pist_n.png`, 64 a side, both materials share it;
export writes it as `on_pist_0n.mdt`).

The diffuses `textures/on_pist_{0,1}_c.tga` are graded for the original's lighting, which draws
`2 x texel x light`, as `on_ar15`'s are: each is the pack's base colour with its emission added
(the sights' dots), box-filtered from 2048 to 1024 a side, then halved, so a texel holds half the
colour it shows lit; the alpha holds the specular mask, 1.3 times the texel's luminance. The editor
imports them (as `assets/art/on_pist_{0,1}_c.png`, the same images) into the `on_pist_{0,1}_c.dds`
the game's model loader opens before the `.tga` the model names. With the tangent frames a vertex is
64 bytes, and the LOD's 12,248 vertices sit well under the game's 32,768.

The arms are `on_ar15`'s (`art/on_ar15/SOURCES.md`).
