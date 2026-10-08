class_name OptionsMenuController
extends RefCounted

## Binds the shared PlayerOptions model to either retail options surface:
## options.mnu from the front end or game.mnu's inline OPTIONS_WRAPPER. The
## controls words and the key bindings are the player profile's current
## record, which the engine's options screen seeds, edits and writes back
## (MenuDriver over menu::OptionsScreen); this controller hands it the profile
## and the device events, and relays the in-game Accept's apply.

const CROSSHAIR_STYLE_CONTROL := "XHAIR_APPEARANCE"

## The in-game options Accept wrote the dialog into the current record: the
## owner applies the record onto the live bindings and the running session's
## live words, then saves the profile (engine OptionsScreen::ApplyControls).
signal controls_accepted

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
	_driver.prepare_options(PlayerProfile.store())
	if not _driver.is_options_surface():
		return
	# The engine's Options policy (OptionsScreen::apply_policy): the slider ranges, the pinned
	# VIDEO rows and the controls not serviced yet, locked; then the profile's controls words
	# (OptionsScreen::seed_profile) and the player's options.
	_driver.apply_options_policy(PlayerProfile.store())
	if _options != null:
		_entry_state = _options.current()
	_seed_player_options()


## The native capture returns input ownership.
func consume_input(event: InputEvent) -> bool:
	return (_driver.consume_options_input(event) & MenuDriver.OPTIONS_CONSUMED) != 0


func _seed_player_options() -> void:
	if _options == null:
		return
	var state := _options.current()
	_seed_scroll("SOUNDFXVOLUME", state.sound_fx_volume)
	_seed_scroll("DIALOGVOLUME", state.dialog_volume)
	_seed_scroll("MUSICVOLUME", state.music_volume)
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
	# The object-detail row (OBJECTPOLY front, OBJECTDETAIL in game) seeds by
	# value from the persisted word (the engine's table, MenuFrame).
	for control_name: String in MenuFrame.object_detail_controls():
		var detail_id := _driver.widget_id(control_name)
		if detail_id >= 0:
			_driver.select_row_by_value(detail_id, str(state.object_polydetail), false)
	# The texture-filter row (TEXFILTER, front end only) seeds by value too.
	for control_name: String in MenuFrame.texture_filter_controls():
		var filter_id := _driver.widget_id(control_name)
		if filter_id >= 0:
			_driver.select_row_by_value(filter_id, str(state.texfilter_level), false)


func _seed_scroll(control_name: String, value: int) -> void:
	var id := _driver.widget_id(control_name)
	if id < 0 or _driver.widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		return
	var scroll := _driver.get_widget_scroll_range(id)
	if scroll != null:
		_driver.set_widget_scroll_range(id, scroll.minimum, scroll.maximum,
				scroll.page, value)


func _set_checked(control_name: String, checked: bool) -> void:
	var id := _driver.widget_id(control_name)
	if id >= 0:
		_driver.set_widget_checked(id, checked)


func _on_screen_changed(_screen_name: String) -> void:
	# Screen/document switches invalidate an armed table selection.
	_driver.end_options_remap(true)


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
			# The object-detail and texture-filter rows write the selected
			# row's value back.
			var row_id := _driver.widget_id(widget_name)
			if row_id < 0 or index < 0: return
			if MenuFrame.object_detail_controls().has(widget_name.to_upper()):
				state.object_polydetail = int(_driver.item_value(row_id, index))
			elif MenuFrame.texture_filter_controls().has(widget_name.to_upper()):
				state.texfilter_level = int(_driver.item_value(row_id, index))
			else:
				return
	_options.update(state)


func _on_widget_activated(id: int, widget_name: String) -> void:
	if not _driver.is_options_surface():
		return
	match widget_name.to_upper():
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
	var effects := _driver.activate_options(PlayerProfile.store(), widget_name)
	if effects & MenuDriver.OPTIONS_APPLY_CONTROLS:
		controls_accepted.emit()
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
	_driver.arm_options_remap(id, row)
