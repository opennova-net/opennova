class_name PlayerProfile
extends RefCounted

# The process's player profile (player.sav and weapon.sav in memory): the native
# PlayerProfiles' process store, its file places, its callsign rule and its
# character half are the engine's (runtime/profile/player_profiles.h,
# runtime/inmatch/join_character_profile.h; docs/playerinfo/player-sav-re.md).


## The process's profile; its first use loads it from the working directory.
static func store() -> PlayerProfiles:
	return PlayerProfiles.process()


## The menu's start on a mount: a fresh record's defaults from the mount, both files read again.
static func load_for(root: ResourceRoot) -> int:
	return int(PlayerProfiles.load_process_for(root))


## A save point: both files written where the last load read them.
static func save() -> int:
	return int(PlayerProfiles.save_process())


static func load_callsign() -> String:
	return PlayerProfiles.process_callsign()


## The profile's character half as the PLAYER_INFO profile Dictionary the menu screens and
## the spawn loadout still speak: the shown side's fields beside the name and both sides.
static func load_character_profile(root: ResourceRoot) -> Dictionary:
	var profile := PlayerProfiles.process_character_profile(root)
	var sides: Array[Dictionary] = [_side(profile, 0), _side(profile, 1)]
	var shown: Dictionary = sides[profile.team].duplicate()
	shown.merge({"name": profile.name, "side_profiles": sides}, true)
	return shown


static func _side(profile: CharacterProfile, side: int) -> Dictionary:
	return {
		"team": profile.side_team(side),
		"nationality": profile.side_nationality(side),
		"division": profile.side_division(side),
		"combo": profile.side_combo(side),
		"player_class": profile.side_player_class(side),
		"avatar_a": profile.side_avatar_a(side),
		"avatar_b": profile.side_avatar_b(side),
		"avatar_packed": profile.side_avatar_packed(side),
	}


## PLAYER_INFO's ACCEPT into the current record, in memory (the next save point writes it).
static func accept_player_info(profile: Dictionary) -> int:
	return int(store().accept_player_info(CharacterSelection.from_player_info_snapshot(profile)))
