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


func test_live_mode_persists_a_card_rename():
	# The workspace handler: input_renamed -> sidecar write beside the pair ->
	# the divider/sentences re-read it on the next populate.
	var doc = preload("res://modtools/music/music_editor_document.gd").new()
	_copy("res://../fixtures/sbf/jo_gamemus.sbf", "user://music_inputnames_pair.sbf")
	_copy("res://../fixtures/mus/jo_gamemus.bin", "user://music_inputnames_pair.bin")
	doc.open_pair("user://music_inputnames_pair.sbf")
	var lm: Control = preload("res://modtools/music/ui/live_mode.tscn").instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm._on_input_renamed("Begin", 0, "Outcome")
	var profile_path: String = doc.get_var_profile_path()
	assert_ne(profile_path, "", "the pair carries a profile sidecar path")
	var sname := String(doc.mus_script.get_default_script_name())
	assert_eq(MusInputNames.labels_for(sname, "Begin", profile_path), {0: "Outcome"},
		"the rename landed in the sidecar")
	# And the drilled-in Begin reads it in the engine-events divider.
	lm._drill_into("Begin")
	await get_tree().process_frame
	var divider: Control = lm._program_view.dispatch_divider()
	assert_not_null(divider)
	var explain := ""
	for c in divider.get_child(0).get_children():
		if c is Label:
			explain += (c as Label).text + "\n"
	assert_string_contains(explain, "Outcome", "the divider names the engine-handed value")
	_rm(profile_path)
	_rm("user://music_inputnames_pair.sbf")
	_rm("user://music_inputnames_pair.bin")


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
