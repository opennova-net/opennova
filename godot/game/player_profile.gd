class_name PlayerProfile
extends RefCounted

# The player profile: retail's player.sav and weapon.sav records, held in memory
# for the process as the original holds them (the native PlayerProfiles over the
# engine's runtime/profile/player_profiles.h; witness record
# docs/playerinfo/player-sav-re.md). The menu's start on a mount loads it, the
# screens edit it in memory, and the original's save points write both files:
# player.sav in the directory the game runs in (LaunchFlags.working_dir, which a
# source run passes and the editor's Play makes its run directory, never the
# mounted resource root, a build no game may write: ADR 0046 S13 A8), weapon.sav
# under the active expansion's directory there when one is mounted.
#
# The local player's callsign is the current record's name: the game ClientAuth.NA
# identity every session leg rides. Self-identification on a join is NAME-MATCH
# (net-re §5.23 D.0), so two players sharing one name cannot coexist in a session
# (D-NET-169); the `--callsign` launch flag names a second instance on one machine.

# The organic-spawn record's entity name is a Name[16] cstring (net-re §5.23) — keep the
# callsign inside what the wire echo can carry so the name-match sees an exact string.
# The cap's engine home is engine/base/gameprofile/game_type.h kMaxCallsignLength.
const MAX_CALLSIGN_LENGTH := NetProtocol.MAX_CALLSIGN_LENGTH

static var _store: PlayerProfiles = null
static var _expansion := ""


## The process's profile. Before a menu has loaded it for a mount, the first use
## loads it from the working directory with a fresh record's defaults.
static func store() -> PlayerProfiles:
	if _store == null:
		_store = PlayerProfiles.new()
		_store.load(LaunchFlags.working_dir(), "")
	return _store


## Load both files again for `root`, as the menu's start loads them: a fresh
## record's defaults from the mount's character table and its menu tables (the
## Strings registry's, which the menu registered first), weapon.sav from the
## mount's expansion. Returns the error a present file gave on reading.
static func load_for(root: ResourceRoot) -> int:
	if _store == null:
		_store = PlayerProfiles.new()
	var avatars: AvatarDatabase = null
	if root != null:
		var db := AvatarDatabase.new()
		if db.load_from_resource_root(root, "Avatars.def") == OK and db.is_loaded():
			avatars = db
	_store.set_defaults(avatars, Strings.get_override_table(),
			Strings.get_table(Strings.TABLE_GAMEUI), 0)
	_expansion = String(root.get_expansion()) if root != null else ""
	return int(_store.load(LaunchFlags.working_dir(), _expansion))


## Write both files where the last load read them.
static func save() -> int:
	var dir := String(LaunchFlags.working_dir())
	if dir.is_empty():
		return ERR_UNCONFIGURED
	var error := int(store().save(dir, _expansion))
	if error != OK:
		push_warning("PlayerProfile: could not save the player profile in %s (error %d)"
				% [dir, error])
	return error


static func load_callsign() -> String:
	# The `--callsign` launch flag overrides HERE, at the single source, so every
	# consumer — the session controller's resolve AND the character-profile
	# "name" the spawn loadout carries onto the wire — sees the same callsign.
	# The override rides the same Name[16] wire echo as the profile's name, so it
	# gets the same clamp.
	var flag_override := LaunchFlags.callsign() \
			.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if not flag_override.is_empty():
		return flag_override
	return String(store().get_player_name()).left(MAX_CALLSIGN_LENGTH)


# Restore both side-specific character selections and expose the blue side as
# the initially active PLAYER_INFO page (SIDE_BLUE is authored CHECKED). Packed
# ids are resolved back through Avatars.def because their bit fields contain
# authored ids, not UI row indices (engine: inmatch/character_registry.h).
static func load_character_profile(root: ResourceRoot) -> Dictionary:
	var profile := {
		"name": load_callsign(),
		"team": 0,
		"side_profiles": [{}, {}],
	}
	var summary := store().character_summary()
	if summary == null or not summary.loaded:
		return profile
	var db := AvatarDatabase.new()
	if root == null or db.load_from_resource_root(root, "Avatars.def") != OK or not db.is_loaded():
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


## PLAYER_INFO's ACCEPT into the current record, in memory as the original's
## dialog writes it (the next save point writes the files): the PLAYERNAME text
## through the name rule, the character snapshot into the weapon record.
static func accept_player_info(profile: Dictionary) -> int:
	if profile.has("name"):
		store().commit_name(String(profile.get("name", "")))
	return int(store().apply_character_selection(profile))
