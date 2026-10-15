# `on_ar15` sources

## The carbine's materials

The carbine's four materials `on_ar15_0` .. `on_ar15_3` draw with `VS_PHONGT`, as the original's
first-person `M4_1st`, `M16_1st` and most of its first-person guns do: a Phong highlight whose
strength is the diffuse texture's alpha, through a tangent-space normal map. The normal map is flat
(`textures/on_ar15_n.png`, 64 a side, all four materials share it; export writes it as
`on_ar15_0n.mdt`). The diffuses `textures/on_ar15_{0,1,2,3}_c.tga` are graded for the original's
lighting, which draws `2 x texel x light`: each texel holds half the colour it shows lit, and the
alpha holds the specular mask, 1.3 times the texel's luminance (the original's guns' masks follow
their luminance at 1.2 to 1.6). The tangent frames make a vertex 64 bytes, so the LOD fits the
game's 2 MiB vertex pool buffer only under 32,768 vertices: the magazine and the spare magazine
(`59 Mesh`, `64 Mesh`) are halved (Decimate, collapse 0.5) to 2,960 faces each, which keeps the
whole carbine at 30,365 vertices.

## The arms

`on_arms` (the first-person arms in `on_ar15.blend`, the `Avatars.def` combo's
arms part) is KINEMATION's Tactical Shooter Pack "Operator" in first person: its
first-person sleeves and its gloves, used under the maintainer's licence for the
pack (2026-10-13; the pistol's pack, `art/on_pist/SOURCES.md`). The pack's files
are not kept here: the `.blend` holds only what was made from them.

The pack's character stands on the UE5 Manny skeleton in Manny's rest pose, and
`on_ar15 Rig`'s 54 arm bones are Manny's at that rest, 1.07 m lower (every bone
head within 0.001 mm), so the sleeves and gloves sit on the rig as the pack built
them, moved down 1.07 m; nothing is refitted, and the hands grip every weapon
whose first 55 parts are this rig's as the pack's own hands do. Their weights are
the pack's, each Manny bone's weight on the rig's bone of its name (the
`BNxx <name>` label); a bone the rig lacks (a corrective, a wrist helper) gives its
weight to its nearest ancestor that the rig has, and the torso's (the sleeves'
tops, which the first-person rig carries on no spine) to the clavicle on the
vertex's side, so a sleeve's top follows its shoulder. Four weights a vertex at
most. 8,191 vertices, 15,076 triangles: the sleeves 1,556, the gloves 13,520.

The textures are the pack's recoloured in the player soldier `onsold1`'s look
(`art/onjo1/person/SOURCES.md`): woodland camouflage sleeves and olive gloves.
`on_arms_src` (in the `bake_source` collection, hidden) is the arms' mesh with the
pack's UV map (`DiffuseUV`) beside ours (`UVMap`: the sleeves' islands packed to
fill their texture, the gloves' kept as the pack lays them, both hands on one
glove); its materials `on_arms_0_src` (the sleeves) and `on_arms_1_src` (the
gloves) read the pack's textures through `DiffuseUV`. Each emits its colour: the
sleeves the woodland pattern (four colours from three noise fields in the
mesh's own space, our own), the gloves one olive, each times the pack's base
colour's luminance over its mean (the folds, seams, stitching and knuckle pads)
and the pack's occlusion; and each carries the pack's normal map. To bake again:
bake Emit (into a 1024 sRGB image) and Normal (tangent space, into a 1024
Non-Color image) from `on_arms_src` with `UVMap` active, through each material's
`bake_target` node; save the colours as `textures/on_arms_{0,1}_c.tga` (alpha
opaque) and the normals, box-filtered to 512 with their green inverted (the
materials read a green-down file through their green flip), as
`textures/on_arms_{0,1}_n.tga`; then Export Model on `on_arms`, which writes the
diffuses as `on_arms_{0,1}_c.dds` (DXT1) and the normals as
`on_arms_{0,1}n.mdt`. The pack's textures are read from the pack's folder
(`.../KINEMATION/TacticalShooterPack/Meshes/Character/Operator/Textures/`).

`on_pist.blend` holds a copy of `on_arms Skin` for its own preview and export;
it is this mesh, on that file's own `on_arms` materials.

| What | Pack file | Used for |
|---|---|---|
| The sleeves | `Meshes/Character/Operator/Modular/SK_Shirt_01a_FPP.FBX` | `on_arms Skin`'s sleeves (material `on_arms_0`), their weights |
| The gloves | `Meshes/Character/Operator/Modular/SK_Glove_01a.FBX` | `on_arms Skin`'s hands (material `on_arms_1`), their weights |
| Their textures | `Meshes/Character/Operator/Textures/TX_Shirt_{BaseColor,Normal,OcclusionRoughnessMetallic}.TGA`, `TX_Glove_{BaseColor,Normal,OcclusionRoughnessMetallic}.TGA` | The folds, seams and occlusion under the recolour, and the normal maps |
