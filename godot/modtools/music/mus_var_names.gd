class_name MusVarNames
extends RefCounted

# Per-script Var00..Var15 friendly-name lookup.
#
# Each .bin is its own program, so we can't infer var roles from compiled
# bytecode in general. For NovaLogic's well-known shipping scripts
# (gamescript and menuscript, shipped as separate GAMEMUS.BIN / MENUMUS.BIN
# SCR0 files) the var roles are pinned by the HOST side in Jointops.exe: the
# game writes each slot via AudioVM_SetVariable @ 0x671FA0, so the (index ->
# meaning) map is read straight off the call sites (see the per-script comments
# below). This is corroborated by on-godot-oscarmike's hand-annotated
# .scratch/menumus_commented.mus and our golden decompile (the script side).
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
	#        9=options, 10=player, 11=SP, 0=error). BINARY-CONFIRMED: the host
	#        writes Var02 from the active menu's MUSICVAR field (the .mnu
	#        <MUSICVAR> attribute, UIScene+0x14) every screen event.
	#        Witnessed: Jointops.exe!UI_DispatchScreenEvent @ 0x54E6A0 (store at
	#        0x54EFF4) -> AudioVM_SetVariable(2, ...) @ 0x671FA0.
	# Var14: "Intro played" flag (0=not yet, 1=played); guards JOMENU601
	# Source: on-godot-oscarmike .scratch/menumus_commented.mus lines 77-87
	"menuscript": {
		0: "Entry",
		2: "MenuScreen",
		14: "IntroPlayed",
	},
	# gamescript (gamemus.bin). The in-game host drives these via
	# AudioVM_SetVariable(idx, val) @ Jointops.exe!0x671FA0 (g_audiovm_globals[idx]=val):
	#   - seeded once at mission start: Jointops.exe!Game_StartMission @ 0x524360
	#     (Var01=dword_A762E0, Var07=100, Var02..06/08..12=0)
	#   - re-driven per-frame from the LOCAL PLAYER's state:
	#     Jointops.exe!Entity_UpdateInfantryPhysics_Continuation2 @ 0x4B434F
	#     (gated on g_local_player_entity).
	# Friendly names below are the high/medium-confidence identifications.
	# Lower-confidence per-frame slots (left raw): Var03/Var04 = orientation/state
	# args (@0x4B62E4/0x4B62F0), Var06 = a state bool (@0x4B62C9). Var09/11/12 are
	# seeded 0 and never re-driven (meaning unknown).
	"gamescript": {
		# Var01: seeded from dword_A762E0 at mission start; gates Testmission
		#        audio via `if (Var01 != 0)`. Witnessed @ 0x5255BF.
		1: "MissionActive",
		# Var02: local-player view pitch (outPitch). Witnessed @ 0x4B62D8.
		2: "ViewPitch",
		# Var05: local-player movement speed magnitude (sqrt(sumsq)>>16 +1; 0 when
		#        stationary). Witnessed @ 0x4B62A9.
		5: "Speed",
		# Var07: local-player health % (init 100; cur*100/max via
		#        Entity_GetMaxHealthWithDifficulty). Witnessed @ 0x4B6324 / seed @ 0x5255F0.
		7: "HealthPct",
		# Var08: game/round state (dword_24C1970; shared with scoreboard/HUD/
		#        camera/net-client-msgs). Witnessed @ 0x4B6335.
		8: "GameState",
		# Var10: local-player team. Witnessed @ 0x4B62FC.
		10: "Team",
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
