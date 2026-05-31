class_name MusVarNames
extends RefCounted

# Per-script Var00..Var15 friendly-name lookup.
#
# Each .bin is its own program, so we can't infer var roles from compiled
# bytecode in general. For NovaLogic's well-known shipping scripts
# (gamescript and menuscript, both shipped under gamemus.bin / menumus.bin)
# the var roles are documented in on-godot-oscarmike's hand-annotated
# .scratch/menumus_commented.mus and inferable from our golden decompile.
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
	#        9=options, 10=player, 11=SP, 0=error)
	# Var14: "Intro played" flag (0=not yet, 1=played); guards JOMENU601
	# Source: on-godot-oscarmike .scratch/menumus_commented.mus lines 77-87
	"menuscript": {
		0: "Entry",
		2: "MenuScreen",
		14: "IntroPlayed",
	},
	# gamescript (gamemus.bin):
	# Var01: gates Testmission audio via `if (Var01 != 0)` in the golden
	#        decompile. Best-guess label "MissionActive" pending a fully
	#        annotated reference.
	# Source: fixtures/mus/golden_jo_gamemus.mus.txt line 87
	"gamescript": {
		1: "MissionActive",
	},
}


# Returns "Friendly (VarXX)" when a friendly name is known, else "VarXX".
# script_name: the MU01 chunk's name (e.g. "menuscript"). Empty string for
# unknown scripts falls through to the raw VarXX form.
static func label_for(script_name: String, var_index: int) -> String:
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
