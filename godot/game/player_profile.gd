class_name PlayerProfile
extends RefCounted

# The local player's persisted callsign — the game ClientAuth.NA identity every session
# leg rides. Self-identification on a join is NAME-MATCH (net-re §5.23 D.0), so two
# players sharing one callsign cannot coexist in a session (the joiner fails the join on
# the ambiguity, D-NET-169). The default is therefore uniquified per machine instead of a
# shared literal. Stored under user:// beside the NovaWorld client settings; a profile
# UI editing this value is a follow-up.

const CONFIG_PATH := "user://player_profile.cfg"
const SECTION := "player"
# The organic-spawn record's entity name is a Name[16] cstring (net-re §5.23) — keep the
# callsign inside what the wire echo can carry so the name-match sees an exact string.
# The cap's engine home is engine/base/gameprofile/game_type.h kMaxCallsignLength.
const MAX_CALLSIGN_LENGTH := NetProtocol.MAX_CALLSIGN_LENGTH


static func load_callsign() -> String:
	# The `--callsign` launch flag overrides HERE, at the single source, so every
	# consumer — the session controller's resolve AND the character-profile
	# "name" the spawn loadout carries onto the wire — sees the same callsign.
	# A controller-only override left the two-instance demo colliding on the
	# shared per-machine default (name-match self-ID, D-NET-169). The override
	# rides the same Name[16] wire echo as the profile value, so it gets the
	# same clamp — a longer callsign could never satisfy the name-match self-ID.
	var flag_override := LaunchFlags.callsign() \
			.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if not flag_override.is_empty():
		return flag_override
	var stored := String(ConfigStore.read(CONFIG_PATH, SECTION, "callsign", "")) 			.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if not stored.is_empty():
		return stored
	var generated := _default_callsign()
	ConfigStore.write(CONFIG_PATH, SECTION, "callsign", generated)
	return generated


static func save_callsign(callsign: String) -> void:
	callsign = callsign.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if callsign.is_empty():
		return
	ConfigStore.write(CONFIG_PATH, SECTION, "callsign", callsign)


# Retail keeps the five PLAYER_INFO/weapon records in weapon.sav beside the
# active game or expansion, not under user:// [orig: PlayerProfile_LoadAllFromDisk
# path build @0x54F68C-0x54F6B7]. OpenNova uses retail profile slot 0 as its
# active slot; the native reader/writer preserves the other four slots.
static func weapon_profile_path(root: ResourceRoot) -> String:
	if root == null:
		return ""
	var dir := String(root.get_root_dir())
	if dir.is_empty():
		return ""
	return dir.path_join(Simulation.weapon_profile_relpath(
			String(root.get_expansion())))


# Restore both side-specific character selections and expose the blue side as
# the initially active PLAYER_INFO page (SIDE_BLUE is authored CHECKED). Packed
# ids are resolved back through Avatars.def because their bit fields contain
# authored ids, not UI row indices [orig: EntitySlot_LookupAndPackEntry
# @0x57AD40, packed write @0x57AE47].
static func load_character_profile(root: ResourceRoot) -> Dictionary:
	var profile := {
		"name": load_callsign(),
		"team": 0,
		"side_profiles": [{}, {}],
	}
	var path := weapon_profile_path(root)
	if path.is_empty() or not FileAccess.file_exists(path):
		return profile
	var summary := Simulation.read_weapon_profile_summary(path)
	if summary == null or not summary.loaded:
		return profile
	var db := AvatarDatabase.new()
	if db.load_from_resource_root(root, "Avatars.def") != OK or not db.is_loaded():
		return profile
	var sides: Array[Dictionary] = [{}, {}]
	for side in 2:
		var raw: WeaponProfileSide = summary.blue if side == 0 else summary.red
		var packed := raw.avatar_packed
		var resolved := db.resolve_character_id(packed, side)
		if resolved == null:
			continue
		sides[side] = {
			"team": side,
			"nationality": resolved.nationality_index,
			"division": resolved.division_index,
			"combo": resolved.combo_index,
			"player_class": raw.player_class,
			"avatar_a": raw.avatar_a,
			"avatar_b": raw.avatar_b,
			"avatar_packed": packed,
		}
	profile["side_profiles"] = sides
	if not sides[0].is_empty():
		var name := String(profile.get("name", ""))
		profile = sides[0].duplicate(true)
		profile["name"] = name
		profile["side_profiles"] = sides
	return profile


static func save_character_profile(root: ResourceRoot, profile: Dictionary) -> int:
	var path := weapon_profile_path(root)
	if path.is_empty():
		return ERR_INVALID_PARAMETER
	return int(Simulation.save_weapon_profile_selection(path, profile))


# Per-machine stable suffix: with name-match self-ID, a shared default (the old literal
# "Player") cross-wired any two default-named clients in one session. The two-instance
# same-machine demo still overrides via --callsign.
static func _default_callsign() -> String:
	var machine := OS.get_unique_id()
	if machine.is_empty():
		machine = str(Time.get_ticks_usec())
	return "Player-%04X" % (machine.hash() & 0xFFFF)
