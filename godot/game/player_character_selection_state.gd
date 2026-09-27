class_name PlayerCharacterSelectionState
extends RefCounted

# The in-process PLAYER_INFO character memory for OpenNova's active profile
# slot: retail's per-(slot, team) selection globals (g_CharSelClass /
# g_CharSelNationality / g_CharSelDivision / g_CharSelCombo @0x2551130.., strides
# 67596 per slot and 32774 per team) collapsed to slot 0 with both team sides
# kept. It hides authored-id packing, the per-side retail default, and ACCEPT's
# shared-class rule behind the menu-facing interface.
#
# The profile Dictionary shape (the ACCEPT snapshot, PlayerProfile's load, and
# MainGame's set_local_player_profile all speak it):
#   name, team (the side shown/committed), player_class (5..9), voice,
#   nationality/division/combo (the active side's tree indices),
#   side_profiles: [blue, red] -- each Selection.to_dict() or {} when unknown,
#   plus the loadout keys the companion appends.

class Selection extends RefCounted:
	var team := -1
	var nationality := -1
	var division := -1
	var combo := -1
	var player_class := 8
	var avatar_a := 0
	var avatar_b := 0
	var avatar_packed := 0


	static func from_dict(source: Dictionary) -> Selection:
		if source.is_empty():
			return null
		var selection := Selection.new()
		selection.team = int(source.get("team", -1))
		selection.nationality = int(source.get("nationality", -1))
		selection.division = int(source.get("division", -1))
		selection.combo = int(source.get("combo", -1))
		selection.player_class = int(source.get("player_class", 8))
		selection.avatar_a = int(source.get("avatar_a", 0))
		selection.avatar_b = int(source.get("avatar_b", 0))
		selection.avatar_packed = int(source.get("avatar_packed", 0))
		return selection


	func duplicate_selection() -> Selection:
		return from_dict(to_dict())


	func to_dict() -> Dictionary:
		return {
			"team": team,
			"nationality": nationality,
			"division": division,
			"combo": combo,
			"player_class": player_class,
			"avatar_a": avatar_a,
			"avatar_b": avatar_b,
			"avatar_packed": avatar_packed,
		}


var _db: AvatarDatabase = null
var _persisted_profile: Dictionary = {}
var _side_profiles: Array[Selection] = [null, null]


func set_persisted_profile(profile: Dictionary) -> void:
	_persisted_profile = profile.duplicate(true)
	_side_profiles = [null, null]
	var saved_sides: Array = profile.get("side_profiles", [])
	for side in mini(saved_sides.size(), 2):
		if saved_sides[side] is Dictionary:
			_side_profiles[side] = Selection.from_dict(saved_sides[side])


# Bind the mounted Avatars.def and validate both remembered sides against it.
func set_database(db: AvatarDatabase) -> void:
	_db = db
	for side in 2:
		_side_profiles[side] = _normalize_side(side, _side_profiles[side])


func initial_team(authored_team: int) -> int:
	return clampi(int(_persisted_profile.get("team", authored_team)), 0, 1)


func persisted_name() -> String:
	return String(_persisted_profile.get("name", ""))


func side_selection(side: int) -> Selection:
	if side < 0 or side >= _side_profiles.size():
		return null
	return (_side_profiles[side].duplicate_selection()
			if _side_profiles[side] != null else null)


func player_class(side: int) -> int:
	if side >= 0 and side < _side_profiles.size() \
			and _side_profiles[side] != null:
		return _side_profiles[side].player_class
	return int(_persisted_profile.get("player_class", -1))


# Build one side's canonical selection. avatar_a/avatar_b are authored
# nationality/division ids, not visible rows; the packed word also carries the
# authored combo id and side bit [orig: EntitySlot_LookupAndPackEntry
# @0x57AD40; packed store @0x57AE47].
func make_selection(team: int, nat_index: int, div_index: int,
		combo_index: int, selected_class: int = 8) -> Selection:
	if _db == null or nat_index < 0 or nat_index >= _db.get_nationality_count():
		return null
	var nat := _db.get_nationality(nat_index)
	if nat == null or nat.alignment != team:
		return null
	if div_index < 0 or div_index >= _db.get_division_count(nat_index):
		return null
	if combo_index < 0 or combo_index >= _db.get_combo_count(nat_index, div_index):
		return null
	var div := _db.get_division(nat_index, div_index)
	var combo := _db.get_combo(nat_index, div_index, combo_index)
	var selection := Selection.new()
	selection.team = team
	selection.nationality = nat_index
	selection.division = div_index
	selection.combo = combo_index
	selection.player_class = selected_class
	selection.avatar_a = nat.id
	selection.avatar_b = div.id
	selection.avatar_packed = NetProtocol.pack_character_id(
			selection.avatar_a, selection.avatar_b, combo.id, team)
	return selection


func remember(selection: Selection) -> void:
	if selection == null:
		return
	var side := selection.team
	if side == 0 or side == 1:
		_side_profiles[side] = selection.duplicate_selection()


# Produce the character half of ACCEPT. The selected class is stamped across
# both side snapshots while avatar bytes stay per-side, matching retail's two
# side-block class loop followed by its selected-side avatar stores
# [orig: PlayerInfo_SaveFromDialog @0x55EE3F..0x55EF38].
func snapshot(team: int, nat_index: int, div_index: int, combo_index: int,
		selected_class: int, player_name: String, voice_row: int) -> Dictionary:
	var current := make_selection(
			team, nat_index, div_index, combo_index, selected_class)
	if current != null:
		remember(current)
	var profile: Dictionary = current.to_dict() if current != null else {
		"team": team,
		"nationality": nat_index,
		"division": div_index,
		"combo": combo_index,
	}
	profile["name"] = player_name
	profile["voice"] = voice_row
	profile["player_class"] = selected_class
	var sides: Array[Dictionary] = [{}, {}]
	for side in 2:
		if _side_profiles[side] != null:
			var saved := _side_profiles[side].duplicate_selection()
			saved.player_class = selected_class
			sides[side] = saved.to_dict()
	profile["side_profiles"] = sides
	return profile


# Retail's per-side default: the first Avatars.def combo of the side's alignment
# [orig: EntitySlot_LookupAndPackEntry @0x57AD40 -- PlayerProfile_InitDefaults
# @0x54BB40 seeds it, and PlayerSession_InitFromProfile @0x50ca80 reallocates a
# saved id the registry no longer resolves to it].
func _first_selection(side: int, selected_class: int) -> Selection:
	if _db == null:
		return null
	var resolved := _db.resolve_character_id(_db.first_character_id(side), side)
	if resolved == null:
		return null
	return make_selection(side, resolved.nationality_index, resolved.division_index,
			resolved.combo_index, selected_class)


# A saved side that still resolves against the mounted Avatars.def is kept as
# saved; a stale or absent one takes the retail per-side default.
func _normalize_side(side: int, saved: Selection) -> Selection:
	var selected_class := (saved.player_class if saved != null
			else int(_persisted_profile.get("player_class", 8)))
	if selected_class < 5 or selected_class > 9:
		selected_class = 8
	var selection := make_selection(side,
			saved.nationality if saved != null else -1,
			saved.division if saved != null else -1,
			saved.combo if saved != null else -1, selected_class)
	if selection != null:
		return selection
	return _first_selection(side, selected_class)
