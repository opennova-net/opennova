# `onarmry1` sources

`onarmry1.blend` is the source of truth: the base game's armory (`items.def` "Armory", 100021), laid out as
the original's small armory `Armry03` ("Armory Version #3": a decoration, `attrib: Landable Armory`).
Its scene `onarmry1` is the game model: two of `oncrate1` side by side, turned so their rope handles face
front and back, with the third-person AR-15 (`onar15_3`) and pistol (`onpist_3`) laid on their sides on
top. Three LODs of one part (thresholds 64, 19 and 0): LOD 0 the crate's LOD 0 with `onar15_3`'s LOD 1 and
`onpist_3`'s LOD 0, LOD 1 the crate's box with their next LODs, LOD 2 the crate's box with their lowest
LODs, the collision and bullet faces. `CB-colonly` is the crates, solid; `CA-colonly` is the armory volume
(type 6), the ground in front where a soldier stands (2.2 m out from the crates, 2.8 m across, 2.2 m high,
as `Armry03`'s reaches 2 m out): standing in it, the use key opens the WEAPON screen (`weapon.mnu`).
`UPG Armory` is the armory label's point above the crates (`Armry03`'s `Armory` user point, type G).

The materials are the crate's and the two weapons', and each names the files those models already write
(`oncrate1_0.tga`, `oncrate1_0n.mdt`, `on_ar15_{0,1,2,3}_c.tga`, `on_ar15_0n.mdt`, `on_pist_{0,1}_c.tga`,
`on_pist_0n.mdt`), so the armory writes no texture of its own and the game loads each once.

The original game's `Armry03` was looked at for its layout, its volumes, its user point and its item
row only. No third-party material is used beyond what `oncrate1`, `onar15_3` and `onpist_3` use (the
weapons' Infima and KINEMATION exceptions: `art/on_ar15/README.md`, `art/on_pist/SOURCES.md`).
