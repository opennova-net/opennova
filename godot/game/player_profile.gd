class_name NovaPlayerProfile
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
const MAX_CALLSIGN_LENGTH := 15


static func load_callsign() -> String:
	var stored := String(NovaConfigStore.read(CONFIG_PATH, SECTION, "callsign", "")) 			.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if not stored.is_empty():
		return stored
	var generated := _default_callsign()
	NovaConfigStore.write(CONFIG_PATH, SECTION, "callsign", generated)
	return generated


static func save_callsign(callsign: String) -> void:
	callsign = callsign.strip_edges().left(MAX_CALLSIGN_LENGTH)
	if callsign.is_empty():
		return
	NovaConfigStore.write(CONFIG_PATH, SECTION, "callsign", callsign)


# Per-machine stable suffix: with name-match self-ID, a shared default (the old literal
# "Player") cross-wired any two default-named clients in one session. The two-instance
# same-machine demo still overrides via NW_LAN_NAME.
static func _default_callsign() -> String:
	var machine := OS.get_unique_id()
	if machine.is_empty():
		machine = str(Time.get_ticks_usec())
	return "Player-%04X" % (machine.hash() & 0xFFFF)
