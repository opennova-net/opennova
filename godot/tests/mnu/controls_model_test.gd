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
