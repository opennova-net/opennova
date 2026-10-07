extends GutTest

# ControlsBindings: the game's live binding rows, which the player profile's
# binding table reaches at each session start and at the in-game options Accept
# (ControlsModel.apply_profile, engine profile::apply_controls; the Options
# screen edits its own copy, docs/playerinfo/player-sav-re.md "The controls
# words").


func before_each() -> void:
	# The display labels below are the no-table fallback ("Ctrl - "): a shell
	# booted earlier in the run may have installed keyhelp.bin as the engine's
	# process-wide "Keys" table (which renders "Ctrl-"), so pin the fallback.
	RtxtStringFile.clear_key_strings()


func _fresh_profiles() -> PlayerProfiles:
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	return profiles


# The controls apply [orig: sub_563620 @0x563620 -> sub_562E60 @0x562e60]: the
# record's table onto the live rows, the ENABLE_JOYSTICK word into the gate. A
# fresh record carries the catalog defaults and the joystick off.
func test_the_profile_table_and_joystick_word_reach_the_live_rows() -> void:
	var model := ControlsModel.new()
	var profiles := _fresh_profiles()
	model.apply_profile(profiles)
	assert_false(model.is_joystick_enabled(), "a fresh record leaves the joystick off")
	assert_true(model.display_text_for_token("seat1").begins_with("Ctrl - "),
			"the fresh table keeps seat1's Ctrl chord")
	# A live edit the profile's table does not hold is put back by the apply.
	var blob := model.save_blob()
	var seat1: PackedInt32Array = blob["seat1"]
	seat1[2] = 0
	blob["seat1"] = seat1
	model.load_blob(blob)
	assert_false(model.display_text_for_token("seat1").begins_with("Ctrl"), "the live edit lands")
	assert_true(profiles.set_word("joystick_enabled", 1))
	model.apply_profile(profiles)
	assert_true(model.display_text_for_token("seat1").begins_with("Ctrl - "),
			"the apply restores the table's binding")
	assert_true(model.is_joystick_enabled(), "the ENABLE_JOYSTICK word opens the gate")
	model.apply_profile(null)
	assert_true(model.is_joystick_enabled(), "no profile changes nothing")


# The by-code binding read (the HUDLS key label's record 200 + category): the
# row the start-up re-lay puts at record `code` formats exactly like its token,
# and a code no row carries is the zero record's empty string (engine controls
# action_for_code carries the witness).
func test_display_text_by_action_code() -> void:
	var model := ControlsModel.new()
	assert_eq(model.display_text_for_action_code(201), model.display_text_for_token("Knife"),
			"record 201 is the Knife row's binding")
	assert_eq(model.display_text_for_action_code(211), model.display_text_for_token("magazine"),
			"record 211 is the magazine row's binding")
	assert_ne(model.display_text_for_action_code(201), "", "the Knife row is bound by default")
	assert_eq(model.display_text_for_action_code(200), "", "no row dispatches 200")


# The live records' revision moves on every binding write and never on a read:
# the HUD formats the HUDLS key labels again only when it moved (engine
# controls BindingSet::revision).
func test_binding_revision_moves_on_writes_only() -> void:
	var model := ControlsModel.new()
	var r := model.get_binding_revision()
	model.display_text_for_action_code(201)
	model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	assert_eq(model.get_binding_revision(), r, "reads leave the revision")
	var bound := -1
	for i in model.get_rows(ControlsModel.DEVICE_KEYBOARD).size():
		var action := model.action_index_for_row(i)
		if action >= 0 and model.control_text(action, ControlsModel.DEVICE_KEYBOARD) != "":
			bound = action
			break
	assert_gte(bound, 0, "a bound row exists")
	model.clear_binding(bound, ControlsModel.DEVICE_KEYBOARD)
	assert_ne(model.get_binding_revision(), r, "a clear moves it")
	r = model.get_binding_revision()
	assert_true(model.assign_godot_key(bound, KEY_K))
	assert_ne(model.get_binding_revision(), r, "an assignment moves it")
	r = model.get_binding_revision()
	model.apply_profile(_fresh_profiles())
	assert_ne(model.get_binding_revision(), r, "the profile's apply moves it")
	r = model.get_binding_revision()
	model.restore_defaults()
	assert_ne(model.get_binding_revision(), r, "the defaults move it")
