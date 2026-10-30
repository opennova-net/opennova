class_name OptionsMenuController
extends RefCounted

## Binds the shared PlayerOptions model to either retail options surface:
## options.mnu from the front end or game.mnu's inline OPTIONS_WRAPPER. The
## controls words and the key bindings are the player profile's current
## record, which the engine's options screen seeds, edits and writes back
## (MenuDriver over menu::OptionsScreen); this controller hands it the profile
## and the device events, and relays the in-game Accept's apply.


## The in-game options Accept wrote the dialog into the current record: the
## owner applies the record onto the live bindings and the running session's
## live words, then saves the profile (engine OptionsScreen::ApplyControls).
signal controls_accepted
## The in-game options' Accept asks the HUD to reload its crosshair art in alpha
## mode (MenuDriver.OPTIONS_RELOAD_CROSSHAIR_ALPHA).
signal crosshair_reload_requested

var _driver: MenuDriver
var _options: PlayerOptions
# The native options controller requests commit/revert/discard; this is the
# store's detached value passed back to its owner for persistence and device
# preview.
var _entry_state: PlayerOptionsState


func setup(driver: MenuDriver, options: PlayerOptions) -> void:
	_driver = driver
	_options = options
	_driver.screen_changed.connect(_on_screen_changed)
	_driver.widget_value_changed.connect(_on_widget_value_changed)
	_driver.widget_activated.connect(_on_widget_activated)
	_driver.list_activated.connect(_on_list_activated)
	_driver.table_cell_clicked.connect(_on_table_cell_clicked)


## Rebuild every options-owned widget after MenuDriver opens a document, and
## again on every show of the front-end OPTIONS screen. All helpers are
## presence-gated, so non-options menu files are a cheap no-op.
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


# The player's options into their controls, each by its form (engine
# OptionsScreen::seed_words; options_policy.h kOptionsWordControls carries the
# controls, their forms and the witness).
func _seed_player_options() -> void:
	if _options != null:
		_driver.seed_options_words(_options.current().to_words())


# A control's edit, its `value` the edit's own (-1 for a click), into the
# player's options (engine OptionsScreen::edit_word), applied and saved at once
# (D-MNU-19); false when the control shows none of them.
func _apply_word_edit(id: int, value: int) -> bool:
	if _options == null or id < 0:
		return false
	var state := _options.current()
	var words := _driver.options_word_edit(id, value, state.to_words())
	if words.is_empty():
		return false
	state.set_words(words)
	_options.update(state)
	return true


func _on_screen_changed(screen_name: String) -> void:
	# Screen/document switches invalidate an armed table selection.
	_driver.end_options_remap(true)
	# Every show of the front OPTIONS screen runs its init again (engine
	# OptionsScreen::is_front_screen carries the witness): the controls seed
	# from the saved words and the owner's state is the visit's baseline, so an
	# accepted edit is what a later BACK keeps and a discarded one is not shown
	# again. A document open shows its screen first; the shell's
	# prepare_document after the open repeats this harmlessly.
	if MenuDriver.is_front_options_screen(screen_name):
		prepare_document()


func _on_widget_value_changed(widget_name: String, _kind: String,
		index: int, _value: String) -> void:
	if _options == null or not _driver.is_options_surface():
		return
	_apply_word_edit(_driver.widget_id(widget_name), index)


func _on_widget_activated(id: int, _widget_name: String) -> void:
	if not _driver.is_options_surface():
		return
	# A check box's click (XHAIR_SPREAD, the tip pair) writes its word.
	_apply_word_edit(id, -1)
	var effects := _driver.activate_options(PlayerProfile.store(), id)
	if effects & MenuDriver.OPTIONS_APPLY_CONTROLS:
		controls_accepted.emit()
	if effects & MenuDriver.OPTIONS_RELOAD_CROSSHAIR_ALPHA:
		crosshair_reload_requested.emit()
	if effects & MenuDriver.OPTIONS_COMMIT_PREVIEW:
		if _options != null:
			_entry_state = _options.current()
		_driver.show_ingame_main()
	if effects & MenuDriver.OPTIONS_RESTORE_PREVIEW:
		if _options != null and _entry_state != null:
			_options.update(_entry_state.copy())
			_seed_player_options()
		_driver.show_ingame_main()
	if effects & MenuDriver.OPTIONS_DISCARD_EDITS:
		# The front-end BACK keeps no widget edit (engine
		# OptionsScreen::DiscardEdits carries the witness): the owner, which
		# applies and saves per edit (D-MNU-19), returns to the entry state, and
		# the BACK's pop_screen leaves the widgets to the next open's seed.
		if _options != null and _entry_state != null:
			_options.update(_entry_state.copy())


# A single press on a table row: the controls table's ends a running capture
# and arms only when no row is left selected (engine OptionsScreen::press).
func _on_table_cell_clicked(id: int, _widget_name: String, row: int, _column: int,
		_state: int, _cell_value: int, double_click: bool) -> void:
	if not double_click:
		_driver.press_options_row(id, row)


func _on_list_activated(id: int, row: int) -> void:
	_driver.arm_options_remap(id, row)
