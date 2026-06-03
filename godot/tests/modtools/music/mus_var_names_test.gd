extends GutTest

const MusVarNames = preload("res://modtools/music/mus_var_names.gd")


func test_unknown_script_falls_back_to_raw_var():
	assert_eq(MusVarNames.label_for("", 0), "Var00")
	assert_eq(MusVarNames.label_for("user_authored_thing", 5), "Var05")
	assert_eq(MusVarNames.label_for("", 15), "Var15")


func test_menuscript_known_vars_get_friendly_names():
	# Per on-godot-oscarmike .scratch/menumus_commented.mus
	assert_eq(MusVarNames.label_for("menuscript", 0), "Entry (Var00)")
	assert_eq(MusVarNames.label_for("menuscript", 2), "MenuScreen (Var02)")
	assert_eq(MusVarNames.label_for("menuscript", 14), "IntroPlayed (Var14)")


func test_menuscript_unknown_indices_fall_back():
	# Indices not in the known table render the raw VarXX form even when
	# the script itself has a known table.
	assert_eq(MusVarNames.label_for("menuscript", 1), "Var01")
	assert_eq(MusVarNames.label_for("menuscript", 5), "Var05")
	assert_eq(MusVarNames.label_for("menuscript", 13), "Var13")


func test_gamescript_known_vars_get_friendly_names():
	# Binary-grounded by the Jointops.exe grill: AudioVM_SetVariable @ 0x671FA0
	# callers Game_StartMission @ 0x524360 and
	# Entity_UpdateInfantryPhysics_Continuation2 @ 0x4B434F.
	assert_eq(MusVarNames.label_for("gamescript", 1), "MissionActive (Var01)")
	assert_eq(MusVarNames.label_for("gamescript", 2), "ViewPitch (Var02)")
	assert_eq(MusVarNames.label_for("gamescript", 5), "Speed (Var05)")
	assert_eq(MusVarNames.label_for("gamescript", 7), "HealthPct (Var07)")
	assert_eq(MusVarNames.label_for("gamescript", 8), "GameState (Var08)")
	assert_eq(MusVarNames.label_for("gamescript", 10), "Team (Var10)")


func test_gamescript_unknown_indices_fall_back():
	# Slots left raw (lower-confidence or seeded-0-and-never-driven).
	assert_eq(MusVarNames.label_for("gamescript", 0), "Var00")
	assert_eq(MusVarNames.label_for("gamescript", 3), "Var03")
	assert_eq(MusVarNames.label_for("gamescript", 9), "Var09")


func test_has_friendly_names_predicate():
	assert_true(MusVarNames.has_friendly_names("menuscript"))
	assert_true(MusVarNames.has_friendly_names("gamescript"))
	assert_false(MusVarNames.has_friendly_names("user_authored"))
	assert_false(MusVarNames.has_friendly_names(""))
