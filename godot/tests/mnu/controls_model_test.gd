extends GutTest

# The Options -> Controls (CONTROL_MAPPING) catalog model: byte-exact JO defaults,
# class grouping, and the player-facing visibility filter. [orig:
# UI_PopulateControlMappingList @ 0x55c0c0; aAbsoluteTurnLe @ 0x8159cb]


func test_controls_model_keyboard_defaults() -> void:
	var model := ControlsModel.new()
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	assert_gt(rows.size(), 40, "keyboard catalog populated")
	var found_forward := false
	for r in rows:
		var cells := r as PackedStringArray
		assert_eq(cells.size(), 3, "row is [class, action, control]")
		if cells[1] == "Forward":
			found_forward = true
			assert_eq(cells[0], "Movement", "Forward is in the Movement class")
			assert_eq(cells[2], "W or Up", "Forward default keyboard binding")
		# Admin/internal classes are hidden from the player-facing remap table.
		assert_ne(cells[0], "Server", "admin class hidden")
		assert_ne(cells[0], "Cheat", "cheat class hidden")
	assert_true(found_forward, "Forward row present in the keyboard rows")


# The live remap operations behind the Controls table [orig:
# KeyBinding_HandleKeyAssignment @ 0x55bb20; CLEAR_KEY @ 0x55bfd0;
# DEFAULTS @ 0x55bd90; mouse capture @ 0x55c780].
func test_remap_assign_clear_defaults_and_blob() -> void:
	var model := ControlsModel.new()
	var action: int = model.action_index_for_row(0)
	assert_gte(action, 0, "row 0 maps to a catalog action")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"W or Up", "row 0 is Forward with its default binding")

	# Both slots full: the assigned key replaces the primary.
	assert_true(model.assign_godot_key(action, KEY_G, false), "assign G")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"G or Up", "assignment replaces the primary slot")

	# Mouse capture stores the witnessed mask; clear empties per device.
	model.assign_mouse_mask(action, 0x2)
	assert_eq(model.control_text(action, ControlsModel.DEVICE_MOUSE),
			"Right", "mouse mask formats")
	model.clear_binding(action, ControlsModel.DEVICE_MOUSE)
	assert_eq(model.control_text(action, ControlsModel.DEVICE_MOUSE),
			"", "mouse clear")
	model.clear_binding(action, ControlsModel.DEVICE_KEYBOARD)
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"", "keyboard clear empties both slots")

	# The blob round-trips edits into a fresh model; DEFAULTS restores.
	model.assign_godot_key(action, KEY_F5, false)
	var blob: Dictionary = model.save_blob()
	var other := ControlsModel.new()
	other.load_blob(blob)
	assert_eq(other.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"F5", "blob round-trips the edit")
	other.restore_defaults()
	assert_eq(other.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"W or Up", "DEFAULTS restores the catalog binding")

	# The gameplay lookup follows the live records.
	var keys: PackedInt32Array = model.godot_keys_for_token("move_forward")
	assert_eq(keys.size(), 1, "one live key after the edits")
	assert_eq(keys[0], int(KEY_F5), "the lookup yields the Godot keycode")
