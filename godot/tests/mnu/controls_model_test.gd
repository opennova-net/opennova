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


# A key captured with Ctrl held alone records the Ctrl- combo; extra flags
# defeat the exact modifier compare [orig: KeyBinding_HandleKeyAssignment
# @ 0x55bb4f..0x55bb51; Input_QueueKeyEvent @ 0x760c10; the "Ctrl-" prefix
# KeyBinding_FormatBindingString @ 0x559a10].
func test_ctrl_combo_records_and_round_trips() -> void:
	var model := ControlsModel.new()
	var action: int = model.action_index_for_row(0)
	assert_true(model.assign_godot_key(action, KEY_Y, true), "Ctrl+Y assigns")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"Ctrl - Y or Up", "the Ctrl- prefix renders on the slot")
	assert_false(model.assign_godot_key(action, KEY_CTRL, true),
			"the Ctrl key itself never captures")
	var blob: Dictionary = model.save_blob()
	var other := ControlsModel.new()
	other.load_blob(blob)
	assert_eq(other.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"Ctrl - Y or Up", "the modifier word round-trips the blob")


# The gameplay sampler over live device state follows the keyboard
# dispatcher's two passes [orig: Input_ProcessKeyboardEvents @0x49d327..0x49d3ac
# (the modifier pass), @0x49d3ba..0x49d488 (the fallback, only when the modifier
# pass matched nothing)]: a bare 1 fires Knife, Ctrl+1 fires seat1 and never
# Knife.
func test_is_token_pressed_follows_the_two_passes() -> void:
	var model := ControlsModel.new()
	_press(KEY_1, true)
	assert_true(model.is_token_pressed("Knife"), "a bare 1 fires Knife")
	assert_false(model.is_token_pressed("seat1"), "a bare 1 never fires the Ctrl+1 seat row")
	assert_eq(model.pressed_key_for_token("Knife"), 0x31, "the firing VK reports")
	_press(KEY_CTRL, true)
	assert_true(model.is_token_pressed("seat1"), "Ctrl+1 fires seat1")
	assert_false(model.is_token_pressed("Knife"),
			"the modifier pass claims the key: Knife stays silent")
	assert_eq(model.pressed_key_for_token("Knife"), 0, "no VK fires the silenced row")
	_press(KEY_1, false)
	_press(KEY_CTRL, false)
	assert_false(model.is_token_pressed("seat1"), "released")
	assert_false(model.is_token_pressed("Knife"), "released")


# An open chat line owns the keyboard, not the mouse: a captured model fires no
# keyboard slot, while a held mouse button still fires its row [orig:
# Input_ProcessPlayerFrame @0x49d4c0 -- the mouse pass gated only on the
# playback file @0x49d4d5, the held keyboard pass behind !g_InputCaptureMode
# @0x49d509].
func test_a_captured_keyboard_leaves_the_mouse_rows_live() -> void:
	var model := ControlsModel.new()
	_press(KEY_W, true)
	assert_true(model.is_token_pressed("move_forward"), "W walks")
	model.set_keyboard_captured(true)
	assert_false(model.is_token_pressed("move_forward"), "the line owns W")
	assert_eq(model.pressed_key_for_token("move_forward"), 0, "no keyboard VK fires")
	_click(MOUSE_BUTTON_LEFT, true)
	assert_true(model.is_token_pressed("attack_1"), "the held left button still fires")
	_click(MOUSE_BUTTON_LEFT, false)
	model.set_keyboard_captured(false)
	assert_true(model.is_token_pressed("move_forward"), "the closed line hands W back")
	_press(KEY_W, false)


func _click(button: MouseButton, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = button
	event.pressed = pressed
	Input.parse_input_event(event)
	Input.flush_buffered_events()


# Physical key state through Input, flushed so the sampler sees it this frame.
func _press(keycode: Key, pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.pressed = pressed
	Input.parse_input_event(event)
	Input.flush_buffered_events()


# The witnessed capture button->mask map lives at the seam
# [orig: the mouse capture callback @ 0x55c78b..0x55c7d5].
func test_mouse_mask_seam_translation() -> void:
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_LEFT), 0x1)
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_RIGHT), 0x2)
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_MIDDLE), 0x10)
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_UP), 0x400)
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_DOWN), 0x800)
	assert_eq(ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_XBUTTON1), 0,
			"unmapped buttons translate to no mask")


# Every VK the catalog or the translation table can produce must round-trip
# VK -> Godot Key -> the same VK — a silent collision or typo in the 60-pair
# table would otherwise break load/display invisibly.
func test_vk_godot_key_round_trip_sweep() -> void:
	var model := ControlsModel.new()
	var seen_keys := {}
	var checked := 0
	# Sweep the whole byte VK space plus the keypad-Enter 269 remap.
	var vks := range(1, 256)
	vks.append(269)
	for vk in vks:
		var key: int = ControlsModel.godot_key_from_vk(vk)
		if key == 0:
			continue
		assert_false(seen_keys.has(key),
			"VK %d maps to Godot key %d already produced by VK %d" %
					[vk, key, int(seen_keys.get(key, -1))])
		seen_keys[key] = vk
		assert_eq(ControlsModel.vk_from_godot_key(key), vk,
				"VK %d survives the round trip" % vk)
		checked += 1
	assert_gt(checked, 70, "the sweep exercised the whole table")
	# Every catalog default key must be displayable through the translation.
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	for r in rows:
		var cells := r as PackedStringArray
		if cells[2] != "":
			assert_false(cells[2].contains("#"),
					"%s displays through the table (no raw #VK fallback)" % cells[1])


func test_static_mouse_and_joystick_defaults_survive_profile_round_trip() -> void:
	var model := ControlsModel.new()
	var blob := model.save_blob()
	assert_eq(blob["Prone"][4], 0x10)
	assert_eq(blob["ScopeZeroDec"][4], 0x800)
	assert_eq(blob["ScopeZeroDec"][6], 17)
	assert_eq(blob["ScopeZeroInc"][4], 0x400)
	assert_eq(blob["FreeLook"][4], 2)
	var restored := ControlsModel.new()
	restored.load_blob(blob)
	assert_eq(restored.save_blob(), blob)
	assert_eq(restored.mouse_event_token(MOUSE_BUTTON_WHEEL_UP), "cycleweaponP")
	_press(KEY_CTRL, true)
	assert_eq(restored.mouse_event_token(MOUSE_BUTTON_WHEEL_UP), "ScopeZeroInc")
	_press(KEY_CTRL, false)


# Every binding label resolves through the process-wide "Keys" table (retail's
# g_TextKeyHelp = keyhelp.bin, the engine's KeyHelp_GetStringWithFallback lookup
# behind controls::format_binding / key_name) that the shell installs through
# RtxtStringFile.install_key_strings when it registers Strings.TABLE_KEYHELP.
# The shipped table renders "Ctrl-" and " or "; with no table installed the
# binary's own literals render marker-stripped ("Ctrl - ", " or ").
func test_key_labels_resolve_through_the_installed_keys_table() -> void:
	var table := RtxtStringFile.new()
	var keys: int = table.add_section("Keys")
	table.add_entry("Ctrl-", "Ctrl-", keys, Vector2i.ZERO)
	table.add_entry("OR", " or ", keys, Vector2i.ZERO)
	table.add_entry("UP", "Up Arrow", keys, Vector2i.ZERO)
	var model := ControlsModel.new()
	var action: int = model.action_index_for_row(0)
	assert_true(model.assign_godot_key(action, KEY_Y, true), "Ctrl+Y assigns")
	RtxtStringFile.clear_key_strings()
	assert_false(RtxtStringFile.has_key_strings(), "no table before the install")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"Ctrl - Y or Up", "no table: the marker-stripped literals render")
	table.install_key_strings()
	assert_true(RtxtStringFile.has_key_strings(), "the table is installed process-wide")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"Ctrl-Y or Up Arrow", "the installed entries replace the prefix, separator and key name")
	RtxtStringFile.clear_key_strings()
	assert_false(RtxtStringFile.has_key_strings(), "clear forgets the table")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"Ctrl - Y or Up", "the literals are back once the table is cleared")


# The Strings registry is the shell's install point: registering keyhelp under
# Strings.TABLE_KEYHELP installs it, unregistering (or clear()) forgets it.
func test_strings_keyhelp_registration_installs_the_keys_table() -> void:
	var table := RtxtStringFile.new()
	var keys: int = table.add_section("Keys")
	table.add_entry("OR", " / ", keys, Vector2i.ZERO)
	RtxtStringFile.clear_key_strings()
	Strings.register_table(Strings.TABLE_KEYHELP, table)
	assert_true(RtxtStringFile.has_key_strings(), "registering keyhelp installs the table")
	var model := ControlsModel.new()
	var action: int = model.action_index_for_row(0)
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"W / Up", "the registered table's separator renders")
	Strings.register_table(Strings.TABLE_KEYHELP, null)
	assert_false(RtxtStringFile.has_key_strings(), "unregistering forgets it")
	Strings.register_table(Strings.TABLE_KEYHELP, table)
	Strings.clear()
	assert_false(RtxtStringFile.has_key_strings(), "clear() forgets it with the registry")
	assert_eq(model.control_text(action, ControlsModel.DEVICE_KEYBOARD),
			"W or Up", "the literal separator is back")
