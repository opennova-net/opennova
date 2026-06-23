# TODO

## General Mission

- [ ] Waypoint types needs to be an enum and then not a number in the UI
- [ ] Zones are awkward to create
- [ ] AI class and AI script: should be a selection, not a free input, if we can pull the options from a loadable resource (def, etc)
- [ ] "Weapon loadout" field semantics need an IDA grill (the editor panel itself is in)
- [ ] "Raw" group fields need an IDA grill and better editor integration
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration
- [ ] Still no sound?

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration
- [ ] Validate "reset after" / "pre mission" / "post mission" is implemented properly
- [ ] "ShowWaypoints" needs to be validated
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Editor-wide

- [ ] The editor needs "depth": these files reference each other, and those references should be links

## Player info (player.mnu / PLAYER_INFO)

The avatar lists, cascade, team filter, name, 3D preview, and ACCEPT seam are wired
(godot/game/player_info_menu_host.gd; grilled in docs/playerinfo/avatars-re.md
D-PLAYERINFO-7..12). Remaining:

- [ ] Loadout combos (PRIMARY/SECONDARY/ACCESSORY + *_AMMO* + STATIC_TOTAL_WEIGHT).
      A separate subsystem (D-PLAYERINFO-11): the runtime weapon table is built by
      `WeaponDef_LoadAll @ 0x54dd10` (weapon.def -> `g_weaponDefTable` dword_2540CE0,
      0xBF40 B) and consumed by `populate_weapon_slot_lists @ 0x560430` filtered by the
      class mask (`g_playerInfoClassMask`) + team mask (`g_playerInfoTeamMask`),
      slot-routed by entry+68; ammo via `populate_weapon_accessory_ammo_ui @ 0x55e8b0`,
      weight via `update_player_info_weight_and_weapon_icons @ 0x55f480`. `libs/def`
      `DefWeaponDef` does NOT yet capture slot/class/weight/team-mask, so this needs:
      (1) grill `WeaponDef_ParseProperty` for the weapon.def tokens that feed those
      fields, (2) extend `libs/def` (weapon.def + ammo.def) to parse them, (3) a
      `NovaWeaponDatabase`/`NovaAmmoDatabase` binding, (4) wire the 13 loadout combos +
      the weight budget label in the companion.
- [ ] Persist the chosen avatar/name to a player profile + render the chosen combo on
      the spawned soldier (D-PLAYERINFO-1, combo -> spawned-player model still open).
