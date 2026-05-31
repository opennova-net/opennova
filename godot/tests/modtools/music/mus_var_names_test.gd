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
	# Per fixtures/mus/golden_jo_gamemus.mus.txt line 87 (`if (Var01 != 0)`)
	assert_eq(MusVarNames.label_for("gamescript", 1), "MissionActive (Var01)")


func test_gamescript_unknown_indices_fall_back():
	assert_eq(MusVarNames.label_for("gamescript", 0), "Var00")
	assert_eq(MusVarNames.label_for("gamescript", 2), "Var02")


func test_has_friendly_names_predicate():
	assert_true(MusVarNames.has_friendly_names("menuscript"))
	assert_true(MusVarNames.has_friendly_names("gamescript"))
	assert_false(MusVarNames.has_friendly_names("user_authored"))
	assert_false(MusVarNames.has_friendly_names(""))
