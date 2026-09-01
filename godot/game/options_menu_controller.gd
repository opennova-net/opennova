class_name OptionsMenuController
extends RefCounted

## Binds the shared PlayerOptions model to either retail options surface:
## options.mnu from the front end or game.mnu's inline OPTIONS_WRAPPER. It also
## owns the one keyboard/mouse remap interaction, keeping MenuShell focused on
## cross-document navigation and game launch policy.

const MenuOptionScrollPolicy := preload("res://game/menu_option_scroll_policy.gd")
const RetailVideoQualityPolicy := preload("res://game/retail_video_quality_policy.gd")

const CONTROL_TABLE := "CONTROL_MAPPING"
const CROSSHAIR_STYLE_CONTROL := "XHAIR_APPEARANCE"
const KEYBOARD_CONTROL := "KEYBOARD"
const MOUSE_CONTROL := "MOUSE"
const JOYSTICK_CONTROL := "JOYSTICK"

var _driver: MenuDriver
var _options: PlayerOptions
var _remap_table_id := -1
var _remap_row := -1
var _remap_action := -1
var _control_device := ControlsModel.DEVICE_KEYBOARD
# Presence gates, refreshed per document. The controller answers its named
# controls only on an options surface — a document that authors the control
# table (options.mnu) or any of the engine's witnessed Options sliders
# (game.mnu's OPTIONS_WRAPPER). Widgets that happen to share those names on
# any other document — jo_sp.mnu's start ACCEPT, a DIFFICULTY on a host
# screen — stay the shell's.
var _has_control_table := false
var _is_options_surface := false
# The options state at surface entry. Retail stages edits as live previews and
# the in-game dialog's Cancel re-seeds the screen from the saved settings while
# Accept commits them (docs/mnu/menu-re.md "The in-game options dialog"), so
# OPT_CANCEL rolls the shared model back to this snapshot.
var _entry_state: PlayerOptions.State


func setup(driver: MenuDriver, options: PlayerOptions) -> void:
	_driver = driver
	_options = options
	_driver.screen_changed.connect(_on_screen_changed)
	_driver.widget_value_changed.connect(_on_widget_value_changed)
	_driver.widget_activated.connect(_on_widget_activated)
	_driver.list_activated.connect(_on_list_activated)


## Rebuild every options-owned widget after MenuDriver opens a document. All
## helpers are presence-gated, so non-options menu files are a cheap no-op.
func prepare_document() -> void:
	_end_remap(false)
	var table_id := _find_control_table()
	_has_control_table = table_id >= 0
	_is_options_surface = _has_control_table or _authors_options_scroll()
	if not _is_options_surface:
		return
	MenuOptionScrollPolicy.apply(_driver)
	RetailVideoQualityPolicy.apply(_driver)
	if _options != null:
		_entry_state = _options.current()
	_seed_player_options()
	_lock_unsupported_controls()
	if _has_control_table:
		_control_device = ControlsModel.DEVICE_KEYBOARD
		_fill_control_mapping(table_id, _control_device)
		_set_checked(KEYBOARD_CONTROL, true)
		_set_checked(MOUSE_CONTROL, false)


## Capture gets first refusal over shell input. False means the ordinary menu
## pump should continue processing the event.
func consume_input(event: InputEvent) -> bool:
	if _remap_action < 0:
		return false
	if event is InputEventKey:
		return _consume_remap_key(event as InputEventKey)
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.pressed and _control_device == ControlsModel.DEVICE_MOUSE:
			_consume_remap_mouse(button.button_index)
			return true
	return false


func _seed_player_options() -> void:
	if _options == null:
		return
	var state := _options.current()
	_seed_scroll("SOUNDFXVOLUME", state.sound_fx_volume)
	_seed_scroll("DIALOGVOLUME", state.dialog_volume)
	_seed_scroll("MUSICVOLUME", state.music_volume)
	_seed_scroll("MOUSE_SENSITIVITY", state.mouse_sensitivity)
	_set_checked("INVERT_MOUSE", state.invert_mouse)
	var style_id := _driver.widget_id(CROSSHAIR_STYLE_CONTROL)
	if style_id >= 0 and _driver.widget_kind_of(style_id) == MnuDocument.TYPE_SPINLIST:
		_driver.select_row(style_id, state.crosshair_style, false)
	# The colour spinlist seeds BY VALUE (the engine's select-by-value seed
	# through the driver): the persisted RGB against the authored item values.
	var color_id := _driver.widget_id("XHAIR_COLOR")
	if color_id >= 0 and _driver.widget_kind_of(color_id) == MnuDocument.TYPE_SPINLIST:
		_driver.select_row_by_value(color_id, str(state.crosshair_color), false)
	_set_checked("XHAIR_SPREAD", state.crosshair_spread)


func _seed_scroll(control_name: String, value: int) -> void:
	var id := _driver.widget_id(control_name)
	if id < 0 or _driver.widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		return
	var scroll := _driver.get_widget_scroll_range(id)
	if scroll != null:
		_driver.set_widget_scroll_range(id, scroll.minimum, scroll.maximum,
				scroll.page, value)


# The not-yet-serviced controls and the checked state their rows show are the
# engine's tables (options_policy.h, re-exported by MenuFrame; D-MNU-21).
func _lock_unsupported_controls() -> void:
	for forced: Dictionary in MenuFrame.options_forced_checks():
		_set_checked(String(forced["control"]), bool(forced["checked"]))
	for control_name: String in MenuFrame.options_unsupported_controls():
		var id := _driver.widget_id(control_name)
		if id >= 0:
			_driver.set_widget_disabled(id, true)


# True when the document authors any of the engine's witnessed Options
# sliders — game.mnu's inline OPTIONS_WRAPPER has no control table.
func _authors_options_scroll() -> bool:
	for range: Dictionary in MenuFrame.options_scroll_ranges():
		var id := _driver.widget_id(String(range["control"]))
		if id >= 0 and _driver.widget_kind_of(id) == MnuDocument.TYPE_SCROLL:
			return true
	return false


func _set_checked(control_name: String, checked: bool) -> void:
	var id := _driver.widget_id(control_name)
	if id >= 0:
		_driver.set_widget_checked(id, checked)


func _on_screen_changed(_screen_name: String) -> void:
	# Screen/document switches invalidate an armed table selection.
	_end_remap(true)


func _on_widget_value_changed(widget_name: String, kind: String,
		index: int, _value: String) -> void:
	if _options == null or not _is_options_surface:
		return
	var state := _options.current()
	match widget_name.to_upper():
		"SOUNDFXVOLUME":
			if kind != "scroll": return
			state.sound_fx_volume = index
		"DIALOGVOLUME":
			if kind != "scroll": return
			state.dialog_volume = index
		"MUSICVOLUME":
			if kind != "scroll": return
			state.music_volume = index
		"MOUSE_SENSITIVITY":
			if kind != "scroll": return
			state.mouse_sensitivity = index
		"XHAIR_APPEARANCE":
			if kind != "spinlist": return
			state.crosshair_style = index
		"XHAIR_COLOR":
			# Retail persists the selected item's `value=` attribute (the
			# decimal RGB the shipped rows author), not the row index or the
			# display text.
			if kind != "spinlist": return
			var color_id := _driver.widget_id("XHAIR_COLOR")
			if color_id < 0: return
			state.crosshair_color = int(_driver.item_value(color_id, index))
		_:
			return
	_options.update(state)


func _on_widget_activated(id: int, widget_name: String) -> void:
	if not _is_options_surface:
		return
	match widget_name.to_upper():
		"INVERT_MOUSE":
			if _options == null:
				return
			var state := _options.current()
			state.invert_mouse = _driver.is_widget_checked(id)
			_options.update(state)
		"XHAIR_SPREAD":
			if _options == null:
				return
			var state := _options.current()
			state.crosshair_spread = _driver.is_widget_checked(id)
			_options.update(state)
		"KEYBOARD":
			if _has_control_table:
				_switch_control_device(ControlsModel.DEVICE_KEYBOARD)
		"MOUSE":
			if _has_control_table:
				_switch_control_device(ControlsModel.DEVICE_MOUSE)
		"JOYSTICK":
			# The device page is served (the table shows the D-CTRL-1 blank
			# column); only arming a joystick capture is refused.
			if _has_control_table:
				_switch_control_device(ControlsModel.DEVICE_JOYSTICK)
		"DEFAULTS":
			# The table gate keeps a same-named widget on any other document
			# from wiping the persisted bindings.
			if _has_control_table:
				_restore_control_defaults()
		"CLEAR_KEY":
			if _has_control_table:
				_clear_selected_binding()
		"ACCEPT":
			# options.mnu authors an actionless Accept beside the control
			# table. pop_screen emits the driver's quit seam, which MenuShell
			# resolves through its file stack. On documents without the table
			# (jo_sp.mnu's start control) the name is the shell's, not ours.
			if _has_control_table:
				_driver.pop_screen()
		"OPT_ACCEPT":
			# The in-game dialog's Accept commits the staged edits: the live
			# state becomes the new baseline for a later Cancel.
			if _options != null:
				_entry_state = _options.current()
			_show_ingame_main_wrapper()
		"OPT_CANCEL":
			# Cancel reverts to the surface-entry snapshot — retail re-seeds
			# the whole screen from the saved settings and rolls the live
			# preview back (docs/mnu/menu-re.md "The in-game options dialog").
			if _options != null and _entry_state != null:
				_options.update(_entry_state.copy())
				_seed_player_options()
			_show_ingame_main_wrapper()


func _show_ingame_main_wrapper() -> void:
	var main_id := _driver.widget_id("MAIN_WRAPPER")
	var options_id := _driver.widget_id("OPTIONS_WRAPPER")
	if main_id >= 0 and options_id >= 0:
		_driver.set_widget_shown(main_id, true)
		_driver.set_widget_shown(options_id, false)


func _on_list_activated(id: int, row: int) -> void:
	if _is_control_table(_driver.widget_name_of(id)):
		_arm_remap(id, row)


# The KEYBOARD/MOUSE/JOYSTICK device radios re-fill the table for the picked
# device (docs/mnu/menu-re.md — UI_SelectControlsInputDevice, registered per
# device by the OPTIONS scene).
func _switch_control_device(device: int) -> void:
	_end_remap(false)
	_control_device = device
	var table_id := _find_control_table()
	if table_id >= 0:
		_fill_control_mapping(table_id, device)


# DEFAULTS restores the catalog bindings and re-fills the table
# (docs/mnu/menu-re.md — the OPTIONS DEFAULTS callback).
func _restore_control_defaults() -> void:
	_end_remap(false)
	ControlsBindings.model().restore_defaults()
	ControlsBindings.persist()
	var table_id := _find_control_table()
	if table_id >= 0:
		_fill_control_mapping(table_id, _control_device)


# CLEAR_KEY empties the selected row's binding for the active device
# (docs/mnu/menu-re.md — the OPTIONS CLEAR_KEY callback).
func _clear_selected_binding() -> void:
	_end_remap(false)
	var table_id := _find_control_table()
	if table_id < 0:
		return
	var selected := _driver.table_selected_rows(table_id)
	var row := selected[0] if selected.size() > 0 else -1
	var action := ControlsBindings.model().action_index_for_row(row)
	if action >= 0:
		ControlsBindings.model().clear_binding(action, _control_device)
		ControlsBindings.persist()
		_fill_control_mapping(table_id, _control_device)
		_driver.table_select_row(table_id, row)


# The table fill mirrors retail's populate/display pair; the armed row keeps
# its cell blank while a capture is live (docs/mnu/menu-re.md —
# UI_PopulateControlMappingList / update_control_mapping_display).
func _fill_control_mapping(table_id: int, device: int, blank_row := -1) -> void:
	_driver.table_clear_rows(table_id)
	var rows := ControlsBindings.model().get_rows(device)
	for i: int in rows.size():
		var cells: PackedStringArray = rows[i]
		if i == blank_row:
			cells[2] = ""
		_driver.table_add_row(table_id, cells)


# Arming a row: pump state set, row stored, its cell cleared, focus taken
# (docs/mnu/menu-re.md — UI_ControlsRemapArmHandler; the joystick refusal is
# D-CTRL-1).
func _arm_remap(table_id: int, row: int) -> void:
	if _control_device == ControlsModel.DEVICE_JOYSTICK:
		return
	var action := ControlsBindings.model().action_index_for_row(row)
	if action < 0:
		return
	_remap_table_id = table_id
	_remap_row = row
	_remap_action = action
	_fill_control_mapping(table_id, _control_device, row)
	_driver.table_select_row(table_id, row)


# The capture pump's Esc/assign split; the assignment goes through the
# bindings model (docs/mnu/menu-re.md — the capture pump and its callback).
func _consume_remap_key(event: InputEventKey) -> bool:
	if not event.pressed:
		return true
	if event.physical_keycode == KEY_ESCAPE:
		_end_remap(true)
		return true
	if ControlsBindings.model().assign_godot_key(_remap_action,
			event.physical_keycode, event.ctrl_pressed, event.shift_pressed,
			event.echo):
		ControlsBindings.persist()
		_end_remap(true)
	return true


func _consume_remap_mouse(button_index: int) -> void:
	var mask := ControlsModel.mouse_mask_from_godot_button(button_index)
	if mask != 0:
		ControlsBindings.model().assign_mouse_mask(_remap_action, mask)
		ControlsBindings.persist()
	_end_remap(true)


func _end_remap(refill: bool) -> void:
	if _remap_action < 0:
		return
	var table_id := _remap_table_id
	var row := _remap_row
	_remap_table_id = -1
	_remap_row = -1
	_remap_action = -1
	if refill and table_id >= 0 \
			and _driver.widget_kind_of(table_id) == MnuDocument.TYPE_TABLE:
		_fill_control_mapping(table_id, _control_device)
		_driver.table_select_row(table_id, row)


func _find_control_table() -> int:
	var id := _driver.widget_id(CONTROL_TABLE)
	if id >= 0 and _driver.widget_kind_of(id) == MnuDocument.TYPE_TABLE:
		return id
	return -1


func _is_control_table(widget_name: String) -> bool:
	return widget_name.nocasecmp_to(CONTROL_TABLE) == 0
