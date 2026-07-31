class_name MusVarNames
extends RefCounted

# Per-script Var00..Var15 friendly-name lookup.
#
# Each .bin is its own program, so we can't infer var roles from compiled
# bytecode in general. For NovaLogic's well-known shipping scripts
# (gamescript and menuscript, shipped as separate GAMEMUS.BIN / MENUMUS.BIN
# SCR0 files) the var roles are pinned by the ENGINE side in Jointops.exe: the
# game writes each slot via AudioVM_SetVariable @ 0x671FA0, so the (index ->
# meaning) map is read straight off the call sites (see the per-script comments
# below). This is corroborated by a hand-annotated menumus decompile from the
# original RE pass and our golden decompile (the script side).
# For any other script (user-authored, third-party) we fall back to the
# raw VarXX label.
#
# When new scripts get analyzed and their var conventions identified, add
# them to KNOWN. Lookup is by the MU01 chunk's script name, which is
# stable across game versions (BHD vs JO vs DFX2 all use the same
# internal "gamescript" / "menuscript" names).

const KNOWN := {
	# menuscript (menumus.bin):
	# Var00: documented as "Entry point selector (1-3 all go to Main)"
	# Var02: the MUSICVAR; current menu screen ID (1=main, 5/6=MP, 8=host,
	#        9=options, 10=player, 11=SP, 0=error). BINARY-CONFIRMED: the engine
	#        writes Var02 from the active menu's MUSICVAR field (the .mnu
	#        <MUSICVAR> attribute, UIScene+0x14) every screen event.
	#        Witnessed: Jointops.exe!UI_DispatchScreenEvent @ 0x54E6A0 (store at
	#        0x54EFF4) -> AudioVM_SetVariable(2, ...) @ 0x671FA0.
	# Var14: "Intro played" flag (0=not yet, 1=played); guards JOMENU601
	# Source: the hand-annotated menumus decompile from the original RE pass (lines 77-87)
	"menuscript": {
		0: "Entry",
		2: "MenuScreen",
		14: "IntroPlayed",
	},
	# gamescript (gamemus.bin). The in-game shell drives these via
	# AudioVM_SetVariable(idx, val) @ Jointops.exe!0x671FA0 (g_audiovm_globals[idx]=val):
	#   - seeded once at mission start: Jointops.exe!Game_StartMission @ 0x524360
	#     (Var01=dword_A762E0, Var07=100, Var02..06/08..12=0)
	#   - re-driven per-frame from the LOCAL PLAYER's state:
	#     Jointops.exe!Entity_UpdateInfantryPlayerBody @ 0x4B40E0, gated
	#     entity == g_local_player_entity (@ 0x4B6234).
	# Full witness map: docs/audio/mus-sbf-re.md §Game music driving (2026-07-09).
	# Lower-confidence per-frame slots (left raw): Var03/Var04 = orientation/state
	# args (@0x4B62E4/0x4B62F0). Var09/11/12 are seeded 0 and never re-driven.
	"gamescript": {
		# Var01: seeded from dword_A762E0 at mission start (@ 0x5255BF). That
		#        global is NEVER WRITTEN in retail JO, so Var01 is always 0 and
		#        gamemus always runs its Multiplayerstart P0 loop; the script's
		#        Missionnull/Missionwin/Missionlose branches are dead content.
		1: "MissionActive",
		# Var02: local-player view pitch (outPitch). Witnessed @ 0x4B62D8.
		2: "ViewPitch",
		# Var05: distance to the nearest threat, whole units +1 (0 = none).
		#        Threat = Entity_FindNearestThreat @ 0x4B0990 within
		#        min(fog_dist/2, 40u); dist folded @ 0x4B6261-0x4B62A9.
		5: "ThreatDistance",
		# Var06: that threat's current target is the local player (bool).
		#        Witnessed @ 0x4B62B3-0x4B62C9.
		6: "ThreatTargetsMe",
		# Var07: local-player health % (init 100; cur*100/max via
		#        Entity_GetMaxHealthWithDifficulty). Witnessed @ 0x4B6324 / seed @ 0x5255F0.
		7: "HealthPct",
		# Var08: the match's scoring game TYPE (g_scoreGameType @ 0x24C1970),
		#        not a dynamic round state. Witnessed @ 0x4B6335.
		8: "GameType",
		# Var10: local-player team. Witnessed @ 0x4B62FC.
		10: "Team",
	},
}


# Optional per-var control metadata. Where present it lets the var inspector
# render a friendlier control than a full-range int32 SpinBox; absent entries
# fall back to that SpinBox, so this table is purely additive and a script
# can have a friendly name (in KNOWN) without a control hint here.
#   kind "slider" -> HSlider + readout, integer range [min,max]
#   kind "bool"   -> CheckBox (0/1)
#   kind "enum"   -> OptionButton; "options" maps the stored int value -> label
#   kind "int" / no entry -> SpinBox (full int32 range unless min/max given)
# These are a UI convenience layered over the binary-grounded KNOWN map; the
# ranges/labels mirror the engine-side semantics documented in KNOWN above.
const META := {
	"gamescript": {
		1: {"kind": "bool"},                          # MissionActive: gate flag
		5: {"kind": "int", "min": 0, "max": 64},      # ThreatDistance: units (0=none; search caps ~40u)
		6: {"kind": "bool"},                          # ThreatTargetsMe: 0/1
		7: {"kind": "slider", "min": 0, "max": 100},  # HealthPct: 0..100
		8: {"kind": "int", "min": 0, "max": 255},     # GameType: scoring-mode id
		10: {"kind": "int", "min": 0, "max": 32},     # Team: small index
	},
	"menuscript": {
		0: {"kind": "int", "min": 1, "max": 3},       # Entry: 1-3 all reach Main
		# MenuScreen (the MUSICVAR); IDs per the KNOWN comment above.
		2: {"kind": "enum", "options": {
				0: "Error", 1: "Main", 5: "MP", 6: "MP2",
				8: "Host", 9: "Options", 10: "Player", 11: "SP"}},
		14: {"kind": "bool"},                          # IntroPlayed: 0/1
	},
}


# Returns "Friendly (VarXX)" when a friendly name is known, else "VarXX".
# script_name: the MU01 chunk's name (e.g. "menuscript"). Empty string for
# unknown scripts falls through to the raw VarXX form. profile_path is an
# optional editor sidecar for user-authored scripts whose VarXX roles are
# outside the built-in gamescript/menuscript tables.
static func label_for(script_name: String, var_index: int, profile_path: String = "") -> String:
	var profile_vars := _profile_vars(script_name, profile_path)
	if profile_vars.has(var_index):
		var entry: Dictionary = profile_vars[var_index]
		var label := String(entry.get("label", ""))
		if label != "":
			return "%s (Var%02d)" % [label, var_index]
	if KNOWN.has(script_name):
		var per_script: Dictionary = KNOWN[script_name]
		if per_script.has(var_index):
			return "%s (Var%02d)" % [per_script[var_index], var_index]
	return "Var%02d" % var_index


# Returns true if any var in the given script has a friendly name registered.
# Used by the editor to decide whether to widen the var-inspector label
# column for legibility.
static func has_friendly_names(script_name: String) -> bool:
	return KNOWN.has(script_name) and not (KNOWN[script_name] as Dictionary).is_empty()


# Returns the control descriptor for (script, var_index), or {} when none is
# registered (the caller then renders a plain full-range int32 SpinBox).
static func meta_for(script_name: String, var_index: int, profile_path: String = "") -> Dictionary:
	var profile_vars := _profile_vars(script_name, profile_path)
	if profile_vars.has(var_index):
		var entry: Dictionary = (profile_vars[var_index] as Dictionary).duplicate(true)
		entry.erase("label")
		return entry
	if META.has(script_name):
		var per_script: Dictionary = META[script_name]
		if per_script.has(var_index):
			return per_script[var_index]
	return {}


# Sorted list of the var indices that have a friendly name for this script, so
# the inspector can render the handful that matter first. Empty for unknown
# scripts (user-authored), which then render every slot in raw order.
static func known_indices(script_name: String, profile_path: String = "") -> Array:
	var profile_vars := _profile_vars(script_name, profile_path)
	if not profile_vars.is_empty():
		var profile_keys: Array = profile_vars.keys()
		profile_keys.sort()
		return profile_keys
	if not KNOWN.has(script_name):
		return []
	var keys: Array = (KNOWN[script_name] as Dictionary).keys()
	keys.sort()
	return keys


# Write (or clear, with an empty label) one var's friendly name in the editor
# sidecar profile, read-merge-write so other vars' labels and control hints
# survive. Display-only: serialization always uses the VarXX tokens, so a
# rename can never change what compiles. A profile carrying a DIFFERENT
# script's name is replaced outright (mirrors _profile_vars ignoring it).
static func set_label(profile_path: String, script_name: String, var_index: int, label: String) -> int:
	if profile_path == "" or var_index < 0 or var_index > 16:
		return ERR_INVALID_PARAMETER
	var profile := {}
	if FileAccess.file_exists(profile_path):
		var f := FileAccess.open(profile_path, FileAccess.READ)
		if f != null:
			var parsed = JSON.parse_string(f.get_as_text())
			if parsed is Dictionary:
				profile = parsed
	var profile_script := String(profile.get("script_name", ""))
	if profile_script != "" and profile_script != script_name:
		profile = {}
	profile["script_name"] = script_name
	var vars: Dictionary = profile.get("vars", {}) if profile.get("vars", null) is Dictionary else {}
	var key := str(var_index)
	var entry: Dictionary = vars.get(key, {}) if vars.get(key, null) is Dictionary else {}
	var clean := label.strip_edges()
	if clean == "":
		entry.erase("label")
	else:
		entry["label"] = clean
	if entry.is_empty():
		vars.erase(key)
	else:
		vars[key] = entry
	profile["vars"] = vars
	var out := FileAccess.open(profile_path, FileAccess.WRITE)
	if out == null:
		return FileAccess.get_open_error()
	out.store_string(JSON.stringify(profile, "  "))
	out.close()
	return OK


static func _profile_vars(script_name: String, profile_path: String) -> Dictionary:
	if profile_path == "" or not FileAccess.file_exists(profile_path):
		return {}
	var f := FileAccess.open(profile_path, FileAccess.READ)
	if f == null:
		return {}
	var parsed = JSON.parse_string(f.get_as_text())
	if not (parsed is Dictionary):
		return {}
	var profile: Dictionary = parsed
	var profile_script := String(profile.get("script_name", ""))
	if profile_script != "" and profile_script != script_name:
		return {}
	if not (profile.get("vars", null) is Dictionary):
		return {}
	var raw_vars: Dictionary = profile["vars"]
	var out := {}
	for raw_key in raw_vars.keys():
		var key_str := String(raw_key)
		if not key_str.is_valid_int():
			continue
		var idx := int(key_str)
		if idx < 0 or idx > 16:
			continue
		if raw_vars[raw_key] is Dictionary:
			out[idx] = (raw_vars[raw_key] as Dictionary).duplicate(true)
	return out
