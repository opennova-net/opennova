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
	var sides: Array[Dictionary] = [_side(profile.blue), _side(profile.red)]
	var shown: Dictionary = sides[profile.team].duplicate()
	shown.merge({"name": profile.name, "side_profiles": sides}, true)
	return shown


static func _side(side: PlayerInfoSideSelection) -> Dictionary:
	return {
		"team": side.team,
		"nationality": side.nationality,
		"division": side.division,
		"combo": side.combo,
		"player_class": side.player_class,
		"avatar_a": side.avatar_a,
		"avatar_b": side.avatar_b,
		"avatar_packed": side.avatar_packed,
	}


## PLAYER_INFO's ACCEPT into the current record, in memory (the next save point writes it).
static func accept_player_info(profile: Dictionary) -> int:
	return int(store().accept_player_info(CharacterSelection.from_player_info_snapshot(profile)))
