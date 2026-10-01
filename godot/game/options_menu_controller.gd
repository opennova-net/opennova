class_name OptionsMenuController
extends RefCounted

## Binds the shared PlayerOptions model to either retail options surface:
## options.mnu from the front end or game.mnu's inline OPTIONS_WRAPPER. It also
## owns the one keyboard/mouse remap interaction, keeping MenuShell focused on
## cross-document navigation and game launch policy.

const MenuOptionScrollPolicy := preload("res://game/menu_option_scroll_policy.gd")
const RetailVideoQualityPolicy := preload("res://game/retail_video_quality_policy.gd")

const CROSSHAIR_STYLE_CONTROL := "XHAIR_APPEARANCE"

var _driver: MenuDriver
var _options: PlayerOptions
# The native options controller requests commit/revert; this is the store's
# detached value passed back to its owner for persistence and device preview.
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
	_driver.prepare_options(ControlsBindings.model())
	if not _driver.is_options_surface():
		return
	MenuOptionScrollPolicy.apply(_driver)
	RetailVideoQualityPolicy.apply(_driver)
	if _options != null:
		_entry_state = _options.current()
	_seed_player_options()
	_lock_unsupported_controls()


## The native capture returns input ownership and persistence requests.
func consume_input(event: InputEvent) -> bool:
	var effects := _driver.consume_options_input(ControlsBindings.model(), event)
	if effects & MenuDriver.OPTIONS_PERSIST_BINDINGS:
		ControlsBindings.persist()
	return (effects & MenuDriver.OPTIONS_CONSUMED) != 0


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
	_set_checked("MR_CLIPPY_KEYBOARD", state.keyboard_tips)
	_set_checked("MR_CLIPPY_HINTS", state.gameplay_tips)
	var aspect_id := _driver.widget_id("16x9DISPLAY")
	if aspect_id >= 0 and _driver.widget_kind_of(aspect_id) == MnuDocument.TYPE_SPINLIST:
		_driver.select_row_by_value(aspect_id, str(state.aspect_mode), false)


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


func _set_checked(control_name: String, checked: bool) -> void:
	var id := _driver.widget_id(control_name)
	if id >= 0:
		_driver.set_widget_checked(id, checked)


func _on_screen_changed(_screen_name: String) -> void:
	# Screen/document switches invalidate an armed table selection.
	_driver.end_options_remap(ControlsBindings.model(), true)


func _on_widget_value_changed(widget_name: String, kind: String,
		index: int, _value: String) -> void:
	if _options == null or not _driver.is_options_surface():
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
		"16X9DISPLAY":
			if kind != "spinlist": return
			var aspect_id := _driver.widget_id("16x9DISPLAY")
			if aspect_id < 0: return
			state.aspect_mode = int(_driver.item_value(aspect_id, index))
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
	if not _driver.is_options_surface():
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
		"MR_CLIPPY_KEYBOARD":
			if _options == null:
				return
			var state := _options.current()
			state.keyboard_tips = _driver.is_widget_checked(id)
			_options.update(state)
		"MR_CLIPPY_HINTS":
			if _options == null:
				return
			var state := _options.current()
			state.gameplay_tips = _driver.is_widget_checked(id)
			_options.update(state)
	var effects := _driver.activate_options(ControlsBindings.model(), widget_name)
	if effects & MenuDriver.OPTIONS_PERSIST_BINDINGS:
		ControlsBindings.persist()
	if effects & MenuDriver.OPTIONS_COMMIT_PREVIEW:
		if _options != null:
			_entry_state = _options.current()
		_driver.show_ingame_main()
	if effects & MenuDriver.OPTIONS_RESTORE_PREVIEW:
		if _options != null and _entry_state != null:
			_options.update(_entry_state.copy())
			_seed_player_options()
		_driver.show_ingame_main()


func _on_list_activated(id: int, row: int) -> void:
	_driver.arm_options_remap(ControlsBindings.model(), id, row)
