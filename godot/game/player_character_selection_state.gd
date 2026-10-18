class_name PlayerCharacterSelectionState
extends RefCounted

# The in-process PLAYER_INFO character memory for OpenNova's active profile
# slot. The state and its rules (retail's per-(slot, team) selection globals
# collapsed to slot 0 with both team sides kept, the authored-id packing, the
# per-side retail default, ACCEPT's shared-class rule) are the engine's
# (runtime/inmatch/player_info_character.h carries the witnesses, bound as
# PlayerInfoCharacterState); this keeps the profile Dictionary's marshalling.
#
# The profile Dictionary shape (the ACCEPT snapshot, PlayerProfile's load, and
# MainGame's set_local_player_profile all speak it):
#   name, team (the side shown/committed), player_class (5..9),
#   nationality/division/combo (the active side's tree indices),
#   side_profiles: [blue, red] -- each a side selection's Dictionary or {} when
#   unknown, plus the loadout keys the companion appends.

var _state := PlayerInfoCharacterState.new()


# One side's selection from its profile Dictionary; null for {}.
static func _selection_from_dict(source: Dictionary) -> PlayerInfoSideSelection:
	if source.is_empty():
		return null
	var selection := PlayerInfoSideSelection.new()
	selection.team = int(source.get("team", -1))
	selection.nationality = int(source.get("nationality", -1))
	selection.division = int(source.get("division", -1))
	selection.combo = int(source.get("combo", -1))
	selection.player_class = int(source.get("player_class", 8))
	selection.avatar_a = int(source.get("avatar_a", 0))
	selection.avatar_b = int(source.get("avatar_b", 0))
	selection.avatar_packed = int(source.get("avatar_packed", 0))
	return selection


static func _selection_to_dict(selection: PlayerInfoSideSelection) -> Dictionary:
	return {
		"team": selection.team,
		"nationality": selection.nationality,
		"division": selection.division,
		"combo": selection.combo,
		"player_class": selection.player_class,
		"avatar_a": selection.avatar_a,
		"avatar_b": selection.avatar_b,
		"avatar_packed": selection.avatar_packed,
	}


func set_persisted_profile(profile: Dictionary) -> void:
	var sides: Array[PlayerInfoSideSelection] = [null, null]
	var saved_sides: Array = profile.get("side_profiles", [])
	for side in mini(saved_sides.size(), 2):
		if saved_sides[side] is Dictionary:
			sides[side] = _selection_from_dict(saved_sides[side])
	_state.set_persisted(profile.has("team"), int(profile.get("team", 0)),
			profile.has("player_class"), int(profile.get("player_class", 0)), sides[0], sides[1])


# Bind the mounted Avatars.def and validate both remembered sides against it.
func set_database(db: AvatarDatabase) -> void:
	_state.set_database(db)


func initial_team(authored_team: int) -> int:
	return _state.initial_team(authored_team)


func side_selection(side: int) -> PlayerInfoSideSelection:
	return _state.side_selection(side)


func player_class(side: int) -> int:
	return _state.player_class(side)


# Produce the character half of ACCEPT (the engine's snapshot: the shown side
# remembered, the selected class stamped across both sides) as the profile
# Dictionary. A shown side that does not resolve carries its raw indices.
func snapshot(team: int, nat_index: int, div_index: int, combo_index: int,
		selected_class: int, player_name: String) -> Dictionary:
	var snap := _state.snapshot(team, nat_index, div_index, combo_index, selected_class)
	var current: PlayerInfoSideSelection = snap[0]
	var profile: Dictionary = _selection_to_dict(current) if current != null else {
		"team": team,
		"nationality": nat_index,
		"division": div_index,
		"combo": combo_index,
	}
	profile["name"] = player_name
	profile["player_class"] = selected_class
	var sides: Array[Dictionary] = [{}, {}]
	for side in 2:
		var saved: PlayerInfoSideSelection = snap[1 + side]
		if saved != null:
			sides[side] = _selection_to_dict(saved)
	profile["side_profiles"] = sides
	return profile
