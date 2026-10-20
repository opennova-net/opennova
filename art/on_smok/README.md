# `art/on_smok`: the base game's first-person smoke grenade

`on_smok.blend` is the first-person smoke grenade, the view model of `WPN_GRENADESM` (the engine's
fixed name): `on_frag Rig`'s 58 bones, the same rests (`BN56 Body` at the frag's grip, `BN57 Spoon`
and `BN58 Pin` at the frag's fuze), with a canister in place of the frag's body, and the frag's
`Hip view` (`pos -33 8 -177`). Export Model on `on_smok` writes `assets/on_smok.3di` and its textures.

It plays the frag's clips: `WPN_GRENADESM` names `animadm on_frag` and `gfx1 on_smok`, so the idle,
the throw with its pin pull, the draw and the holster are `art/on_frag/README.md`'s. A clip pairs
with a model by part index, and the canister's fuze, spoon hinge and pin sit where the frag's do (the
canister is gripped a third of the way down, its base 8.8 cm under the grip's centre), so the left
hand finds the ring and the pin leaves with it; when the frag's grip moves, the three rests here
move to match it. The scene keeps no clips of its own (the editor's first-person preview of
`on_frag.adm` with the weapon WPN_GRENADESM shows the canister on them). The throw releases at the
frag's frame 38, so the smoke's fire row keeps the frag's `delaystart 80` (the original's smoke
throws on its own clip at `delaystart 65`).

Materials and textures: `SOURCES.md`.
