extends GutTest

# The join screen's input over the REAL shell (WorldFixture.boot_shell). The join
# screen (pre.mnu PRE_GAME_MENU, opened over the authored JoinScreenFixture copy
# with the shipped hotkeys: ACCEPT on Return, BACK on Escape) comes up over the
# menu the join started from, which stays shown under it. Retail's join screen is
# its own mode, so the keys are its own: the menu under it takes none while it is
# open (shipped mp.mnu's LAN screen binds the same keys, LAN_JOINGAME on Return
# and BACK on Escape), and gets them back once it closes. ACCEPT's commit and
# BACK's abort close the screen from inside its own key or click. Every event
# arrives through the process input path (Input.parse_input_event).

const TMP_DIR := "res://.godot/join_screen_keys_test"
const STATE_CONFIG_PATH := "user://opennova.cfg"
const LAN_SCREEN := "LAN_MULTI_PLAYER"

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: MainGame = null
var _window_size := Vector2i.ZERO
# The screen registers the text tables it finds missing: each test puts back what
# was there.
var _menutxt: RtxtStringFile = null
var _gameerr: RtxtStringFile = null
var _committed: Array = []
var _aborts := 0
var _menu_activations: Array = []


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	LaunchFlags.set_args_override(PackedStringArray([]))
	_window_size = get_window().size
	_menutxt = Strings.get_table(Strings.TABLE_MENUTXT)
	_gameerr = Strings.get_table(Strings.TABLE_GAMEERR)
	JoinScreenFixture.stage(self, TMP_DIR)
	_committed = []
	_aborts = 0
	_menu_activations.clear()


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()
	GameCfgStore.reset()
	get_window().size = _window_size
	Strings.register_table(Strings.TABLE_MENUTXT, _menutxt)
	Strings.register_table(Strings.TABLE_GAMEERR, _gameerr)
	_menutxt = null
	_gameerr = null


func after_all() -> void:
	JoinScreenFixture.unstage(TMP_DIR)


# The real shell on `file`'s `screen_name` with the join choice for `flags` up
# over it, null when the boot fails. The menu's own activations are recorded.
func _choice_over(file: String, screen_name: String, flags: int) -> PreGameMenuPresenter:
	# The join screen's frame is the viewport's size: design space is window space.
	get_window().size = Vector2i(800, 600)
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return null
	var menu := _shell.get_menu_shell()
	assert_true(menu.open_menu(file, screen_name))
	assert_eq(menu.get_driver().get_current_screen(), screen_name)
	menu.get_driver().widget_activated.connect(_on_menu_activated)
	var screen := _open_choice(flags)
	assert_true(menu.is_visible_in_tree(), "the menu stays shown under the join screen")
	return screen


# The shell's join screen opened over the authored pre.mnu (the shell fixture's
# archives carry none) with the join choice for `flags`.
func _open_choice(flags: int) -> PreGameMenuPresenter:
	var screen := _shell.get_join_screen()
	assert_true(screen.open(PresenterFixture.root_over(self, TMP_DIR), _shell),
			"the shell's join screen opens over the authored pre.mnu")
	assert_true(_shell.begin_join_choice(flags, _on_committed, _on_abort))
	return screen


func _on_committed(game_password: String, spectate: bool, team_password: String) -> void:
	_committed.append([game_password, spectate, team_password])


func _on_abort() -> void:
	_aborts += 1


func _on_menu_activated(_id: int, widget_name: String) -> void:
	_menu_activations.append(widget_name)


func _press(code: Key, unicode := 0) -> void:
	for pressed in [true, false]:
		var event := InputEventKey.new()
		event.keycode = code
		event.physical_keycode = code
		event.unicode = unicode if pressed else 0
		event.pressed = pressed
		Input.parse_input_event(event)
	await _frames(2)


func _frames(count: int) -> void:
	for i in count:
		await get_tree().process_frame


func _send_left(at: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = pressed
	event.position = at
	event.global_position = at
	if pressed:
		event.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(event)


# A real click on the join screen's `control`, the press held over a frame.
func _click(screen: PreGameMenuPresenter, control: String) -> void:
	var driver := screen.get_menu_driver()
	var at := driver.widget_frame_rect(driver.widget_id(control)).get_center()
	_send_left(at, true)
	await _frames(2)
	_send_left(at, false)
	await _frames(2)


func _type_side_password(screen: PreGameMenuPresenter) -> void:
	var driver := screen.get_menu_driver()
	driver.focus_widget(driver.widget_id("TEAM_PASSWORD"))
	await _press(KEY_S, "s".unicode_at(0))
	await _press(KEY_7, "7".unicode_at(0))
	assert_eq(driver.get_widget_text(driver.widget_id("TEAM_PASSWORD")), "s7",
			"the typed text reaches the join screen's edit")


func _lan_screen_shows() -> bool:
	return _shell.get_menu_shell().get_driver().get_current_screen() == LAN_SCREEN


# Return is ACCEPT: its commit closes the screen from inside the key and hands
# the typed side password to the join.
func test_return_commits_the_typed_password() -> void:
	var screen := await _choice_over("mp.mnu", LAN_SCREEN, JoinTarget.FLAG_BLUE_PASSWORD)
	if screen == null:
		return
	await _type_side_password(screen)
	await _press(KEY_ENTER)
	assert_eq_deep(_committed, [["", false, "s7"]])
	assert_eq(_aborts, 0)
	assert_false(_shell.is_join_screen_open(), "the commit closes the join screen")
	assert_true(_lan_screen_shows())


# Escape is BACK: the join's abort closes the screen from inside the key.
func test_escape_abandons_the_choice() -> void:
	var screen := await _choice_over("mp.mnu", LAN_SCREEN, JoinTarget.FLAG_SERVER_PASSWORD)
	if screen == null:
		return
	await _press(KEY_ESCAPE)
	assert_eq(_aborts, 1, "Escape is BACK on the join choice")
	assert_true(_committed.is_empty())
	assert_false(_shell.is_join_screen_open(), "the abort closes the join screen")
	assert_true(_lan_screen_shows(), "the menu kept its place under the join screen")


# ACCEPT's click closes the screen from inside the menu's pump.
func test_clicking_accept_commits_the_typed_password() -> void:
	var screen := await _choice_over("mp.mnu", LAN_SCREEN, JoinTarget.FLAG_BLUE_PASSWORD)
	if screen == null:
		return
	await _type_side_password(screen)
	await _click(screen, "ACCEPT")
	assert_eq_deep(_committed, [["", false, "s7"]])
	assert_false(_shell.is_join_screen_open(), "the commit closes the join screen")


# The shell's menu on an authored screen binding the keys shipped mp.mnu's LAN
# screen does (LAN_JOINGAME on Return, BACK on Escape; the shell fixture's mp.mnu
# binds none). The buttons carry no ACTION, so a press only reports itself.
func _menu_binds_return_and_escape() -> void:
	var w := MenuDriverFixture.wnd
	var body: String = (
			w.call("button", "JOIN_PROBE", 100, JoinScreenFixture.RETURN_HOTKEY)
			+ w.call("button", "BACK_PROBE", 140, JoinScreenFixture.ESCAPE_HOTKEY))
	var doc := MenuDriverFixture.doc_from_xml(self,
			MenuDriverFixture.screen_xml("KEYS", body))
	var menu := _shell.get_menu_shell()
	assert_true(menu.get_driver().open_document(doc, menu.get_resource_root(),
			menu.get_stylesheet(), Strings.get_override_table(), "keys.mnu", "KEYS"))


# While the join screen is up its Return and Escape are its own: the menu under
# it binds both and sees neither. Once it closes the menu has them again.
func test_the_menu_under_the_join_screen_takes_no_keys() -> void:
	var screen := await _choice_over("mp.mnu", LAN_SCREEN, JoinTarget.FLAG_BLUE_PASSWORD)
	if screen == null:
		return
	_menu_binds_return_and_escape()
	await _type_side_password(screen)
	await _press(KEY_ENTER)
	assert_eq_deep(_committed, [["", false, "s7"]])
	assert_eq_deep(_menu_activations, [])
	assert_false(_shell.is_join_screen_open())
	await _press(KEY_ENTER)
	assert_eq_deep(_menu_activations, ["JOIN_PROBE"])
	_open_choice(JoinTarget.FLAG_SERVER_PASSWORD)
	_menu_activations.clear()
	await _press(KEY_ESCAPE)
	assert_eq(_aborts, 1, "Escape is the join screen's BACK")
	assert_eq_deep(_menu_activations, [])
	await _press(KEY_ESCAPE)
	assert_eq_deep(_menu_activations, ["BACK_PROBE"])
	assert_eq(_committed.size(), 1)
