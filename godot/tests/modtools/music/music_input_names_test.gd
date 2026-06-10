extends GutTest

# MusInputNames: the section-scoped caller-input labels in the shared
# .music_profile.json sidecar. Display-only (the script keeps its l_N tokens);
# must coexist with MusVarNames' "vars" in the SAME file via read-merge-write.

const PROFILE := "user://music_inputnames_test_profile.json"


func before_each() -> void:
	_rm(PROFILE)


func after_all() -> void:
	_rm(PROFILE)


func test_set_and_read_back():
	assert_eq(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "Mission event"), OK)
	assert_eq(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 1, "Spare"), OK)
	var labels := MusInputNames.labels_for("gamescript", "Begin", PROFILE)
	assert_eq(labels, {0: "Mission event", 1: "Spare"}, "labels read back by 0-based index")
	assert_eq(MusInputNames.labels_for("gamescript", "Other", PROFILE), {},
		"another section sees nothing (inputs are per-state)")


func test_clearing_with_empty_label():
	MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "Mission event")
	assert_eq(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "  "), OK)
	assert_eq(MusInputNames.labels_for("gamescript", "Begin", PROFILE), {},
		"an empty label removes the entry (and the emptied section)")


func test_coexists_with_var_labels_in_one_profile():
	# MusVarNames owns "vars"; MusInputNames owns "section_inputs". Each write
	# path must preserve the other's key.
	assert_eq(MusVarNames.set_label(PROFILE, "gamescript", 5, "Pace"), OK)
	assert_eq(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "Mission event"), OK)
	assert_eq(MusVarNames.label_for("gamescript", 5, PROFILE), "Pace (Var05)",
		"the var label survived the input write")
	assert_eq(MusVarNames.set_label(PROFILE, "gamescript", 5, "Tempo"), OK)
	assert_eq(MusInputNames.labels_for("gamescript", "Begin", PROFILE), {0: "Mission event"},
		"the input label survived the var write")


func test_section_rename_moves_the_labels():
	MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "Mission event")
	assert_eq(MusInputNames.rename_section(PROFILE, "gamescript", "Begin", "Boot"), OK)
	assert_eq(MusInputNames.labels_for("gamescript", "Begin", PROFILE), {}, "old name is empty")
	assert_eq(MusInputNames.labels_for("gamescript", "Boot", PROFILE), {0: "Mission event"},
		"labels followed the rename")
	assert_eq(MusInputNames.rename_section(PROFILE, "gamescript", "Missing", "X"), OK,
		"renaming an unlabelled section is a quiet OK")


func test_other_scripts_profile_is_ignored_then_replaced():
	MusInputNames.set_input_label(PROFILE, "menuscript", "Main", 0, "Screen")
	assert_eq(MusInputNames.labels_for("gamescript", "Main", PROFILE), {},
		"a different script's profile reads as empty")
	# Writing under the new script replaces the stale profile outright
	# (mirrors MusVarNames.set_label).
	assert_eq(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 0, "Event"), OK)
	assert_eq(MusInputNames.labels_for("menuscript", "Main", PROFILE), {},
		"the stale script's labels are gone")
	assert_eq(MusInputNames.labels_for("gamescript", "Begin", PROFILE), {0: "Event"})


func test_input_index_bounds():
	assert_ne(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", -1, "X"), OK)
	assert_ne(MusInputNames.set_input_label(PROFILE, "gamescript", "Begin", 99, "X"), OK)
	assert_ne(MusInputNames.set_input_label(PROFILE, "gamescript", "", 0, "X"), OK)
	assert_ne(MusInputNames.set_input_label("", "gamescript", "Begin", 0, "X"), OK)


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
