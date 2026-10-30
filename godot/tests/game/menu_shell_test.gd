extends GutTest

# Runtime menu shell (MenuShell) gates: it boots the JO menu set, services the
# shell policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuShellScript := preload("res://game/menu_shell.gd")

# The menu names below key the synthetic screens _fixture_bytes authors.
const MAIN_FIXTURE := "mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "mnu/jo_loadout.mnu"  # the cross-.mnu target
const OPTIONS_FIXTURE := "mnu/jo_options.mnu"  # has the Mods tab (AVAIL_LIST/MOD_DESC)
# Focused menu-contract fixtures: they author the controls the
# shipped jo_options / jo_game / jo_sp screens carry, under the shipped names, so
# the shell's per-control policy runs over a whole real screen.
const ASSET_OPTIONS := "res://../fixtures/game/options.mnu"  # OPTIONS, MUSICVAR 9: sliders, video, crosshair, Mods, remap
const ASSET_GAME := "res://../fixtures/game/game.mnu"  # INGAME: the pause menu with its inline OPTIONS_WRAPPER
const ASSET_SP := "res://../fixtures/game/sp.mnu"  # SINGLE_PLAYER: the IA_LIST mission list + BRIEFING + ACCEPT
# A menu music pair the shell opens as MENUMUS.BIN / MENUMUS.SBF (the synthetic script and bank).
const MENU_MUSIC_SCRIPT_FIXTURE := "res://../fixtures/mus/synth_menumus.bin"
const MENU_MUSIC_BANK_FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"
# The shipped training mission's title, 00TRa.bin's Info/TITLE (the reference
# set's rtxt/00tra.bin, the row the SP-list pin read): the list row is the loose
# marker + that title [orig: SinglePlayer_PopulateMissionList @ 0x561840].
const TRAINING_ARMORY_TITLE := "Training: Basic Controls / Armory"
const BMS_ATTRIB_COOP := 0x1000000
const BMS_ATTRIB_TDM := 0x20000000


const STATE_CONFIG_PATH := "user://opennova.cfg"

var _state_config: TestFs.Snapshot
var _run_dir := ""


func before_each() -> void:
	_state_config = TestFs.snapshot(STATE_CONFIG_PATH)
	if _state_config.existed:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	# The Options screens edit the player profile's current record: each case
	# starts from a fresh one in an empty run directory.
	_run_dir = OS.get_cache_dir().path_join("opennova_menu_shell_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	PlayerProfile.load_for(null)
	# The options' words live in the process's cfg block: a fresh one per case,
	# its game.cfg in the run directory.
	GameCfgStore.reset()


func after_each() -> void:
	# The music service is an autoload; leave no context behind for the next test.
	MusicService.stop_context()
	_state_config.restore()
	# The live binding model is a static shared with the whole run: restore the
	# catalog defaults even when a remap test fails early.
	ControlsBindings.model().restore_defaults()
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)


# Build a throwaway resource dir holding main.mnu (+ a sp.mnu jump target and a
# stub mission), and a shell pointed at it. Returns null when a real temp root is
# unavailable in this environment (the caller pends).
func _make_shell(dir: String, options: PlayerOptions = null):
	var root := ResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	var shell = MenuShellScript.new()
	if options != null:
		shell.set_player_options(options)
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)  # in-tree so the built menu's widgets are not orphans
	shell.setup(root)
	return shell


func _make_dir() -> String:
	var dir := TestFs.cache_dir(self, "menu_shell")
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	_copy(SP_FIXTURE, dir.path_join("sp.mnu"))
	TestFs.write_bytes(self, dir.path_join("test.bms"), PackedByteArray([0]))
	return dir


func _copy(source: String, dst: String) -> void:
	TestFs.write_bytes(self, dst, _fixture_bytes(source))


# The bytes of a fixture: a res:// path reads directly (a contract menu, the
# synthetic music pair); a fixture name above is the synthetic screen it keys.
func _fixture_bytes(source: String) -> PackedByteArray:
	if source.begins_with("res://"):
		return FileAccess.get_file_as_bytes(source)
	var body := ""
	var screen := "OPTIONS"
	if source == MAIN_FIXTURE:
		screen = "STARTUP"
		body = MenuDriverFixture.wnd("list", "MISSION_LIST", 20)
		body += MenuDriverFixture.wnd("button", "ACCEPT", 50)
		body += MenuDriverFixture.wnd("button", "EXIT", 80)
		# retail's main.mnu: an empty right-justified VERSION static, then the
		# copyright line under the same name.
		body += MenuDriverFixture.wnd("static", "VERSION", 110,
				'<STRING justify="RIGHT"></STRING>')
		body += MenuDriverFixture.wnd("static", "VERSION", 140,
				'<STRING justify="LEFT">(c) 2009, NovaLogic, Inc.</STRING>')
	elif source == SP_FIXTURE:
		screen = "LOADOUT"
	var xml := MenuDriverFixture.screen_xml(screen, body)
	if source == MAIN_FIXTURE:
		xml = xml.replace("<NAME>STARTUP</NAME>",
				"<NAME>STARTUP</NAME><MUSICVAR>1</MUSICVAR>")
	return xml.to_utf8_buffer()


# A screen that loads no CURSOR shows no pointer, the game having hidden the
# system one for good at boot: the driver makes a transparent image the OS
# cursor, so the mouse still moves and hits the menu unseen (D-MNU-26).
func test_a_screen_with_no_cursor_shows_no_pointer() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	driver.process_mouse(Vector2(400, 300), false)
	var pointer = driver.get_applied_pointer()
	assert_true(pointer is Image, "no CURSOR: the no-pointer image is the OS cursor")
	if pointer is Image:
		assert_eq(pointer.get_size(), Vector2i(1, 1))
		assert_eq(pointer.get_pixel(0, 0).a, 0.0, "a transparent pointer")
	shell.hide_menu()
	_cleanup(dir)


func test_hidden_menu_suspends_shell_frame_processing() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp root unavailable in this environment")
		return
	assert_true(shell.is_processing(), "the visible menu drives its frame model")
	shell.hide_menu()
	assert_false(shell.is_processing(),
			"a hidden gameplay menu does not remain in the process-frame remainder")
	shell.show_menu()
	assert_true(shell.is_processing(), "returning to the menu resumes its driver")
	for name in ["main.mnu", "sp.mnu", "test.bms"]:
		DirAccess.remove_absolute(dir.path_join(name))
	DirAccess.remove_absolute(dir)


func _cleanup(dir: String) -> void:
	for f in ["main.mnu", "sp.mnu", "options.mnu", "game.mnu", "lan.mnu", "test.bms", "menumus.bin",
			"menumus.sbf"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


func test_boots_into_main_menu_startup() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	assert_eq(shell.get_current_menu_file(), "main.mnu", "main menu opened on setup")
	var driver: MenuDriver = shell.get_driver()
	assert_not_null(driver, "the interaction driver is built")
	assert_not_null(shell.get_frame(), "the compiled frame surface is built")
	assert_eq(driver.get_current_screen(), "STARTUP", "STARTUP screen shown")
	_cleanup(dir)


# The STARTUP screen's activate draws the build's version in its first VERSION
# static, ours the launch's versions (LaunchFlags.version_line: the game and its
# installed release, ONE's build; D-MNU-76); the copyright line under the same
# name keeps its text.
# [orig: UI_OnStartupScreenActivate @0x5557f0 -> UI_FindScreenControl @0x63ae80]
func test_startup_screen_shows_the_version() -> void:
	var dir := _make_dir()
	var command := "[HEADER]\r\nVERSION=0.1.4\r\n\r\n[COMMAND]\r\n"
	TestFs.write_text(self, dir.path_join("ACIDGAMBIT.CMD"), command)
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", dir]))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable in this environment")
		LaunchFlags.clear_args_override()
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var version := driver.widget_id("VERSION")
	assert_gt(version, 0, "the VERSION static exists")
	var line := LaunchFlags.version_line()
	assert_true(line.begins_with("OP: Acid Gambit 0.1.4 · ONE "), line)
	var shown := driver.get_widget_text(version)
	assert_eq(shown, line, "the first VERSION static shows the launch's versions")
	assert_eq(driver.get_widget_text(version + 1), "(c) 2009, NovaLogic, Inc.",
			"the second keeps its authored text")
	LaunchFlags.clear_args_override()
	_cleanup(dir)


func test_startup_drives_music_var() -> void:
	var dir := _make_dir()
	# The menu context the shell opens before it shows a screen [orig: AudioVM_InitMenuMusicStreaming
	# @ 0x56aa60]: the script's instance holds the var the MUSICVAR lands in.
	TestFs.write_bytes(self, dir.path_join("menumus.bin"), FileAccess.get_file_as_bytes(MENU_MUSIC_SCRIPT_FIXTURE))
	TestFs.write_bytes(self, dir.path_join("menumus.sbf"), FileAccess.get_file_as_bytes(MENU_MUSIC_BANK_FIXTURE))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_eq(MusicService.current_context(), "menu", "the menu music context is open")
	# jo_main STARTUP declares MUSICVAR 1; the menu pushes it into the menumus
	# discriminator var (MusicDirector.MENU_MUSIC_VAR_SLOT — the witness lives
	# at the engine home, audio/music_policy.h kMenuMusicVarSlot); at index 0
	# the MUSICVAR was inert and the menu played the wrong section.
	var idx: int = MusicDirector.MENU_MUSIC_VAR_SLOT
	assert_eq(MusicService.director().get_var(idx), 1, "STARTUP MUSICVAR -> director var %d" % idx)
	MusicService.stop_context()
	_cleanup(dir)


# With no menu music pair the context does not open (the bank's open bails first, and only a
# loaded script sets the context active [orig: AudioVM_OpenContextFile @ 0x672177..0x6721a3;
# AudioVM_OpenMusicContext @ 0x6722db]), so the game's variable write finds no context and drops
# the MUSICVAR [orig: AudioVM_SetVariable @ 0x671fa0, the g_AudioVMContextActive test].
func test_startup_music_var_without_a_context_is_dropped() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_eq(MusicService.current_context(), "", "no menu music pair: no context")
	assert_eq(MusicService.director().get_var(MusicDirector.MENU_MUSIC_VAR_SLOT), 0,
			"the STARTUP MUSICVAR is dropped with no context open")
	_cleanup(dir)


# The menu's pump reads the left button alone: a right-button press and release
# over EXIT clicks nothing, the left button's does [orig: CWnd_ProcessMouseEvent
# @ 0x647b04 reads input_mask's MK_LBUTTON alone; the right button's messages
# reach no capture, CButtonWnd_HandleNamedEvent @ 0x658340].
func test_right_button_never_clicks() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	var driver: MenuDriver = shell.get_driver()
	# Through Godot's input dispatch, as a real mouse arrives (the shell's gui input).
	var shell_control: Control = shell
	var at := shell_control.get_global_transform_with_canvas() \
			* driver.widget_frame_rect(driver.widget_id("EXIT")).get_center()
	for button: MouseButton in [MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_LEFT]:
		for pressed: bool in [true, false]:
			var event := InputEventMouseButton.new()
			event.button_index = button
			event.pressed = pressed
			event.position = at
			event.global_position = at
			Input.parse_input_event(event)
			await get_tree().process_frame
		if button == MOUSE_BUTTON_RIGHT:
			await get_tree().process_frame
			assert_signal_not_emitted(shell, "exit_to_desktop_requested",
					"the right button never clicks a window")
	# The click is the frame pump's, once a frame after the events (update_menu_frame).
	await get_tree().process_frame
	assert_signal_emitted(shell, "exit_to_desktop_requested", "the left button's click reaches EXIT")
	_cleanup(dir)


# The shell takes each mouse event as its message, as it arrives, and pumps the
# menu once a frame after them over the last event's point and button [orig:
# Menu_ShellMouseCallback @ 0x54b860 -> UI_DispatchMouseEvent @ 0x63ab00;
# Menu_UpdateFrame @ 0x5528a0, Game_PumpWindowMessages @ 0x5528d3 then
# CUIScene_EndFrame @ 0x5528de]. The click is the pump's: the claim let go over
# that a sample before held with the button down. So a press and release on
# EXIT inside one frame click nothing, and nor does a press on the backdrop slid
# onto EXIT and let go there inside one frame, where a pump per event clicks
# both; held over a frame's pump first, the click lands. (A headless window never
# hears the mouse come in, so a motion reaches the shell only while its press
# holds the mouse focus.)
func test_the_menu_pumps_once_a_frame_after_the_events() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	var driver: MenuDriver = shell.get_driver()
	var shell_control: Control = shell
	var xform := shell_control.get_global_transform_with_canvas()
	var on_exit := xform * driver.widget_frame_rect(driver.widget_id("EXIT")).get_center()
	var backdrop := xform * Vector2(600, 400)
	# Pressed and let go on EXIT inside one frame.
	_send_left(on_exit, true)
	_send_left(on_exit, false)
	await _frames(2)
	assert_signal_not_emitted(shell, "exit_to_desktop_requested",
			"a press and release inside one frame click nothing")
	# Pressed on the backdrop, slid onto EXIT and let go there inside one frame.
	_send_left(backdrop, true)
	_send_held_motion(on_exit)
	_send_left(on_exit, false)
	await _frames(2)
	assert_signal_not_emitted(shell, "exit_to_desktop_requested",
			"a slide onto EXIT let go inside one frame clicks nothing")
	# The same slide over three frames: EXIT held under the mouse by a frame's
	# pump, then let go there.
	_send_left(backdrop, true)
	await _frames(2)
	_send_held_motion(on_exit)
	await _frames(2)
	_send_left(on_exit, false)
	await _frames(2)
	assert_signal_emitted(shell, "exit_to_desktop_requested",
			"held over a frame's pump, then let go there: EXIT's click")
	_cleanup(dir)


# Over a running mission the game loop's frame pumps the in-game screens at its
# head (MainGame._process -> pump_screen_pass), ahead of the frame's net frame, so
# the menu's own update pumps nothing: a press held over its frames and let go
# clicks nothing until the screen pass runs [orig: Game_ProcessMainFrame
# @ 0x5263f0 - UI_ProcessMenuScreenFrame @ 0x52667b ahead of Client_ProcessNetworkFrame
# @ 0x526692; Menu_UpdateFrame @ 0x5528a0 in the menus].
func test_the_in_game_screens_pump_in_the_game_loops_screen_pass() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	var driver: MenuDriver = shell.get_driver()
	var shell_control: Control = shell
	var on_exit := shell_control.get_global_transform_with_canvas() \
			* driver.widget_frame_rect(driver.widget_id("EXIT")).get_center()
	# The in-game overlay over the main menu's screen (no game.mnu in the fixture: the
	# open fails soft and keeps EXIT up).
	shell.open_ingame_menu()
	assert_true(shell.is_in_game())
	_send_left(on_exit, true)
	await _frames(2)
	_send_left(on_exit, false)
	await _frames(2)
	assert_signal_not_emitted(shell, "exit_to_desktop_requested",
			"over a mission the menu's own update pumps nothing")
	_send_left(on_exit, true)
	await _frames(2)
	shell.pump_screen_pass()
	_send_left(on_exit, false)
	await _frames(2)
	shell.pump_screen_pass()
	assert_signal_emitted(shell, "exit_to_desktop_requested",
			"the game loop's screen pass runs the click")
	_cleanup(dir)


func _frames(count: int) -> void:
	for i in count:
		await get_tree().process_frame


func _send_held_motion(at: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = at
	event.global_position = at
	event.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(event)


func _send_left(at: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = pressed
	event.position = at
	event.global_position = at
	if pressed:
		event.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(event)


func test_ingame_hidden_back_button_resumes() -> void:
	# game.mnu's ONLY resume affordance is the ESC-hotkeyed, actionless
	# HIDDEN_BACK button (empty appearances, NOT hidden) — the retail Command
	# seam the shell wires by name. The engine hotkey scan resolves VK_ESCAPE
	# to it and the activation routes through the shell's back handler exactly
	# like a click, so driving the real key path pins the ESC-resume behavior.
	var dir := _make_dir()
	var game_mnu := """
<SCREEN>
	<NAME>MAIN</NAME>
	<WINDOW type="window" name="MAIN">
		<APPEARANCE type="custom" state="default"></APPEARANCE>
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="window" name="MAIN_WRAPPER">
			<APPEARANCE type="custom" state="default"></APPEARANCE>
			<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
			<WINDOW type="button" name="HIDDEN_BACK">
				<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
				<APPEARANCE state="default"></APPEARANCE>
				<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>1</RIGHT><BOTTOM>1</BOTTOM></POSITION>
			</WINDOW>
		</WINDOW>
	</WINDOW>
</SCREEN>
"""
	var f := FileAccess.open(dir.path_join("game.mnu"), FileAccess.WRITE)
	if f != null:
		f.store_string(game_mnu)
		f.close()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_ingame_menu(), "the in-game overlay loads")
	watch_signals(shell)
	var driver: MenuDriver = shell.get_driver()
	assert_true(driver.has_widget("HIDDEN_BACK"),
		"the actionless BACK seam exists in the loaded document")
	var esc := InputEventKey.new()
	esc.keycode = KEY_ESCAPE
	esc.pressed = true
	assert_true(driver.handle_key_input(esc),
		"the engine hotkey scan resolves the authored VK_ESCAPE to HIDDEN_BACK")
	assert_signal_emitted(shell, "resume_requested")
	assert_signal_not_emitted(shell, "exit_to_desktop_requested")
	DirAccess.remove_absolute(dir.path_join("game.mnu"))
	_cleanup(dir)


# A slider edit as the player makes it: the control moves and the frame reports its value
# (the frame's scroll interaction, which the engine shell takes as the word's edit).
func _edit_scroll(driver: MenuDriver, id: int, _control: String, value: int) -> void:
	var range := driver.get_widget_scroll_range(id)
	driver.set_widget_scroll_range(id, range.minimum, range.maximum, range.page, value)
	driver.get_frame().scroll_value_changed.emit(driver.frame_index(id), value)


# The object-detail row is game.cfg's object_polydetail, served rather than
# pinned (engine runtime/menu/options_policy.h kOptionsWordControls): the
# options surface selects the row whose value is the persisted word, leaves
# it editable, and a pick writes the row's value back at once.
func test_object_detail_row_seeds_by_value_and_writes_the_word_back() -> void:
	var options := PlayerOptions.new()
	var seeded := options.current()
	seeded.object_polydetail = 1
	options.update(seeded)
	var dir := _make_dir()
	var list_box := ('<LIST_BOX><POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>200</RIGHT>'
			+ '<BOTTOM>100</BOTTOM></POSITION><ITEMS>%s</ITEMS></LIST_BOX>')
	var rows := ""
	for level in 4:
		rows += '<ITEM value="%d">Level %d</ITEM>' % [level, level]
	var body := MenuDriverFixture.wnd("scroll", "MUSICVOLUME", 20)
	body += MenuDriverFixture.wnd("combobox", "OBJECTPOLY", 60, list_box % rows)
	TestFs.write_bytes(self, dir.path_join("options.mnu"),
			MenuDriverFixture.screen_xml("OPTIONS", body).to_utf8_buffer())
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "the options document opens")
	var driver: MenuDriver = shell.get_driver()
	assert_true(driver.is_options_surface())
	var detail := driver.widget_id("OBJECTPOLY")
	assert_gte(detail, 0)
	assert_eq(driver.item_value(detail, driver.selected_row(detail)), "1",
			"the row whose value is the persisted word is selected")
	assert_false(driver.is_widget_disabled(detail), "the object-detail row is editable")
	driver.select_row(detail, 3)  # emits the combo's value change
	assert_eq(options.current().object_polydetail, 3,
			"the pick writes the row's value to the shared owner")
	assert_eq(PlayerOptions.new().current().object_polydetail, 3, "and persists")
	_cleanup(dir)


# The texture-filter row is game.cfg's texfilter_level, served the same way
# (engine runtime/menu/options_policy.h kOptionsWordControls): the TEXFILTER
# combobox selects the row whose value is the persisted word, stays editable,
# and a pick writes the row's value back at once.
func test_texture_filter_row_seeds_by_value_and_writes_the_word_back() -> void:
	var options := PlayerOptions.new()
	var seeded := options.current()
	seeded.texfilter_level = 2
	options.update(seeded)
	var dir := _make_dir()
	var list_box := ('<LIST_BOX><POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>200</RIGHT>'
			+ '<BOTTOM>100</BOTTOM></POSITION><ITEMS>%s</ITEMS></LIST_BOX>')
	var rows := ""
	for level in 4:
		rows += '<ITEM value="%d">Level %d</ITEM>' % [level, level]
	var body := MenuDriverFixture.wnd("scroll", "MUSICVOLUME", 20)
	body += MenuDriverFixture.wnd("combobox", "TEXFILTER", 60, list_box % rows)
	TestFs.write_bytes(self, dir.path_join("options.mnu"),
			MenuDriverFixture.screen_xml("OPTIONS", body).to_utf8_buffer())
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "the options document opens")
	var driver: MenuDriver = shell.get_driver()
	assert_true(driver.is_options_surface())
	var filter := driver.widget_id("TEXFILTER")
	assert_gte(filter, 0)
	assert_eq(driver.item_value(filter, driver.selected_row(filter)), "2",
			"the row whose value is the persisted word is selected")
	assert_false(driver.is_widget_disabled(filter), "the texture-filter row is editable")
	driver.select_row(filter, 0)  # emits the combo's value change
	assert_eq(options.current().texfilter_level, 0,
			"the pick writes the row's value to the shared owner")
	assert_eq(PlayerOptions.new().current().texfilter_level, 0, "and persists")
	_cleanup(dir)


# The particle-density and texture-compression rows are game.cfg's
# particle_density and texcompression_level, served the same way (engine
# runtime/menu/options_policy.h kOptionsWordControls): each combobox selects
# the row whose value is the persisted word, stays editable, and a pick writes
# the row's value back at once.
func test_particle_and_compression_rows_seed_by_value_and_write_the_words_back() -> void:
	var options := PlayerOptions.new()
	var seeded := options.current()
	seeded.particle_density = 1
	seeded.texcompression_level = 0
	options.update(seeded)
	var dir := _make_dir()
	var list_box := ('<LIST_BOX><POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>200</RIGHT>'
			+ '<BOTTOM>100</BOTTOM></POSITION><ITEMS>%s</ITEMS></LIST_BOX>')
	var rows := ""
	for level in 3:
		rows += '<ITEM value="%d">Level %d</ITEM>' % [level, level]
	var body := MenuDriverFixture.wnd("scroll", "MUSICVOLUME", 20)
	body += MenuDriverFixture.wnd("combobox", "PARTICLES", 60, list_box % rows)
	body += MenuDriverFixture.wnd("combobox", "TEXCOMPRESSION", 140, list_box % rows)
	TestFs.write_bytes(self, dir.path_join("options.mnu"),
			MenuDriverFixture.screen_xml("OPTIONS", body).to_utf8_buffer())
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "the options document opens")
	var driver: MenuDriver = shell.get_driver()
	assert_true(driver.is_options_surface())
	var density := driver.widget_id("PARTICLES")
	var compression := driver.widget_id("TEXCOMPRESSION")
	assert_gte(density, 0)
	assert_gte(compression, 0)
	assert_eq(driver.item_value(density, driver.selected_row(density)), "1",
			"the density row whose value is the persisted word is selected")
	assert_eq(driver.item_value(compression, driver.selected_row(compression)), "0",
			"the compression row whose value is the persisted word is selected")
	assert_false(driver.is_widget_disabled(density), "the particle-density row is editable")
	assert_false(driver.is_widget_disabled(compression), "the compression row is editable")
	driver.select_row(density, 2)
	driver.select_row(compression, 2)
	assert_eq(options.current().particle_density, 2, "the density pick writes the word")
	assert_eq(options.current().texcompression_level, 2, "the compression pick writes the word")
	var reloaded := PlayerOptions.new().current()
	assert_eq(reloaded.particle_density, 2, "the density persists")
	assert_eq(reloaded.texcompression_level, 2, "the compression persists")
	_cleanup(dir)


# The terrain-texture-detail row is game.cfg's terrain_texdetail, served the
# same way (engine runtime/menu/options_policy.h kOptionsWordControls):
# the combobox selects the row whose value is the persisted word (a profile
# below full detail shows its own rung), stays editable, and a pick writes the
# row's value back at once. The seeded 1 is neither row 0 (a list's unselected
# read) nor the fresh profile's 3.
func test_terrain_texture_row_seeds_by_value_and_writes_the_word_back() -> void:
	var options := PlayerOptions.new()
	var seeded := options.current()
	seeded.terrain_texdetail = 1
	options.update(seeded)
	var dir := _make_dir()
	var list_box := ('<LIST_BOX><POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>200</RIGHT>'
			+ '<BOTTOM>100</BOTTOM></POSITION><ITEMS>%s</ITEMS></LIST_BOX>')
	var rows := ""
	for level in 4:
		rows += '<ITEM value="%d">Level %d</ITEM>' % [level, level]
	var body := MenuDriverFixture.wnd("scroll", "MUSICVOLUME", 20)
	body += MenuDriverFixture.wnd("combobox", "TERRAINTEX", 60, list_box % rows)
	TestFs.write_bytes(self, dir.path_join("options.mnu"),
			MenuDriverFixture.screen_xml("OPTIONS", body).to_utf8_buffer())
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "the options document opens")
	var driver: MenuDriver = shell.get_driver()
	assert_true(driver.is_options_surface())
	var terrain := driver.widget_id("TERRAINTEX")
	assert_gte(terrain, 0)
	assert_eq(driver.item_value(terrain, driver.selected_row(terrain)), "1",
			"the row whose value is the persisted word is selected")
	assert_false(driver.is_widget_disabled(terrain), "the terrain-texture row is editable")
	driver.select_row(terrain, 0)
	assert_eq(options.current().terrain_texdetail, 0, "the pick writes the word")
	assert_eq(PlayerOptions.new().current().terrain_texdetail, 0, "the word persists")
	_cleanup(dir)


# A root switch reloads every text table: a root without menutxt.BIN clears
# the previous root's registration instead of keeping its strings alive.
func test_root_switch_drops_the_previous_roots_text_table() -> void:
	Strings.clear()
	var dir := _make_dir()
	var table := RtxtStringFile.new()
	table.add_section("Menu")
	table.add_entry("BTN_OK", "OK", 0, Vector2i())
	TestFs.write_bytes(self, dir.path_join("menutxt.BIN"), table.to_byte_array())
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		TestFs.remove_dir_recursive(dir)
		return
	assert_not_null(Strings.get_table(Strings.TABLE_MENUTXT),
			"the first root registers its menutxt table")
	var bare := _make_dir()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(bare), OK)
	assert_true(shell.setup(root), "the shell re-opens over the bare root")
	assert_null(Strings.get_table(Strings.TABLE_MENUTXT),
			"a root without menutxt.BIN leaves no stale table behind")
	Strings.clear()
	TestFs.remove_dir_recursive(dir)
	TestFs.remove_dir_recursive(bare)


func test_missing_assets_degrade_without_crashing() -> void:
	# A dir with only main.mnu (no stylesheet / sound / music / textures): the shell
	# still builds the menu and runs; the menu just reports unresolved assets.
	var dir := OS.get_temp_dir().path_join("menu_shell_bare_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir)
		return
	assert_eq(shell.get_current_menu_file(), "main.mnu", "menu still opens with no companion assets")
	assert_not_null(MusicService.director(), "director created even without a music script")
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir)


# A throwaway companion: a delegate of `name`, recording whether it was driven.
class _FakeCompanion extends MenuCompanion:
	var delegate: String
	var built := false
	func _init(p_delegate: String) -> void:
		delegate = p_delegate
	func owner_name() -> String:
		return delegate
	func on_menu_built(_menu, _file, _screen, _root) -> void:
		built = true


# --- The Options, pause and play screens over our own menus -------------------

# A saved game.cfg in the run directory (version 29 and `lines`), which the
# next use of the cfg block reads, as a fresh process's start does.
func _write_cfg(lines: Array[String]) -> void:
	var file := FileAccess.open(_run_dir.path_join(GameCfgStore.FILE_NAME), FileAccess.WRITE)
	file.store_string("version = 29\r\n" + "\r\n".join(lines) + "\r\n")
	file.close()
	GameCfgStore.reset()


# A minimal parseable .bms: the 616-byte header with magic BMS v19, the
# embedded mission_name, and one game-mode attrib bit.
func _write_bms(path: String, mission_name: String, attribs: int) -> void:
	var bytes := PackedByteArray()
	bytes.resize(616)
	bytes[0] = 0x42  # 'B'
	bytes[1] = 0x4D  # 'M'
	bytes[2] = 0x53  # 'S'
	bytes[3] = 19    # shipped JO header version
	var name := mission_name.to_utf8_buffer()
	for i in mini(name.size(), 31):
		bytes[4 + i] = name[i]
	bytes.encode_u32(136, attribs)
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f != null:
		f.store_buffer(bytes)
		f.close()


# A mission's text table (<mission>.bin): the Info section's TITLE and BRIEFING
# the SP list titles its row from and its briefing pane shows
# [orig: SinglePlayer_PopulateMissionList @ 0x561840; mission_catalog.h
# kTextInfoSection / kTextTitleKey / kTextBriefingKey], minted through the
# string-table writer.
func _mission_bin(title: String, briefing: String) -> PackedByteArray:
	var table := RtxtStringFile.new()
	var section := table.add_section("Info")
	table.add_entry("TITLE", title, section, Vector2i.ZERO)
	table.add_entry("BRIEFING", briefing, section, Vector2i.ZERO)
	return table.to_byte_array()


# The shell seeds the five named Options sliders with the exact original
# ranges/pages. Audio/input retain deterministic minimum fallbacks; gamma is
# pinned to the registered high-quality retail comparison profile and locked.
# [orig: UI_OptionsScreenInit @ 0x554800;
# UI_PopulateRenderAndAudioSettings @ 0x55c830]
func test_options_scrolls_seed_original_ranges_and_persisted_values() -> void:
	_write_cfg(["SFXLevel = 31", "DialogueLevel = 93", "music_volume = 159"])
	# The mouse words are the player profile's current record's.
	assert_true(PlayerProfile.store().set_word("mouse_sensitivity", 287))
	assert_true(PlayerProfile.store().set_word("invert_mouse", 1))
	var options := PlayerOptions.new()
	var dir := _make_dir()
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "Options fixture opens")
	var driver: MenuDriver = shell.get_driver()
	var expected := [
		["GAMMA", 5, 20, 2, 8],
		["SOUNDFXVOLUME", 0, 255, 10, 31],
		["DIALOGVOLUME", 0, 255, 10, 93],
		["MUSICVOLUME", 0, 255, 10, 159],
		["MOUSE_SENSITIVITY", 4, 511, 10, 287],
	]
	for row in expected:
		var control_name := String(row[0])
		var id := driver.widget_id(control_name)
		assert_gte(id, 0, "%s exists" % control_name)
		var scroll = driver.get_widget_scroll_range(id)
		assert_not_null(scroll, "%s receives scroll state" % control_name)
		assert_eq([scroll.minimum, scroll.maximum, scroll.page, scroll.value],
				row.slice(1),
				"%s receives its original range/page and persisted value" \
						% control_name)
	assert_true(driver.is_widget_checked(driver.widget_id("INVERT_MOUSE")),
			"the record's mouse inversion seeds the checkbox")
	assert_true(driver.is_widget_disabled(driver.widget_id("GAMMA")),
			"gamma is visible but locked to the comparison profile")
	for unlocked_name in ["SOUNDFXVOLUME", "DIALOGVOLUME", "MUSICVOLUME",
			"MOUSE_SENSITIVITY", "INVERT_MOUSE"]:
		assert_false(driver.is_widget_disabled(driver.widget_id(unlocked_name)),
				"%s remains an interactive core setting" % unlocked_name)
	for unsupported_name: String in MenuFrame.options_unsupported_controls():
		var id := driver.widget_id(unsupported_name)
		if id >= 0:
			assert_true(driver.is_widget_disabled(id),
					"%s is visible but read-only until supported" % unsupported_name)
	for crosshair_name in ["XHAIR_COLOR", "XHAIR_SPREAD"]:
		assert_false(driver.is_widget_disabled(driver.widget_id(crosshair_name)),
				"%s is a supported interactive setting" % crosshair_name)
	var xhair_color := driver.widget_id("XHAIR_COLOR")
	assert_eq(int(driver.item_value(xhair_color,
			driver.selected_row(xhair_color))),
			PlayerOptions.DEFAULT_CROSSHAIR_COLOR,
			"the colour list seeds by value onto the default white row")
	assert_true(driver.is_widget_checked(driver.widget_id("XHAIR_SPREAD")),
			"the default spread toggle seeds enabled")
	_cleanup(dir)


func test_video_options_are_highest_quality_and_read_only() -> void:
	# A terrain texture detail below full and off row 0, so its served row's
	# seed is observable.
	_write_cfg(["terrain_texdetail = 2"])
	var dir := _make_dir()
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "Options fixture opens")
	var driver: MenuDriver = shell.get_driver()
	var expected := {
		"TERRAINPOLY": "3",
		"ANTIALIAS": "2",
		"SHADERUSAGE": "2",
		"WATERQUALITY": "3",
		"SHADOWQUALITY": "3",
		"FBEFFECTS": "3",
	}
	for control_name in expected:
		var id := driver.widget_id(control_name)
		assert_gte(id, 0, "%s exists" % control_name)
		assert_eq(driver.item_value(id, driver.selected_row(id)), expected[control_name],
				"%s is pinned to the highest supported retail value" % control_name)
		assert_true(driver.is_widget_disabled(id), "%s is read-only" % control_name)
	for preset_name in ["VIDEODEFAULT", "VIDEOPERFORMANCE", "VIDEOQUALITY"]:
		assert_true(driver.is_widget_disabled(driver.widget_id(preset_name)),
				"obsolete retail preset %s is disabled" % preset_name)
	# Object detail is served: game.cfg's object_polydetail, seeded by value
	# from the shared options and editable (options_policy.h kOptionsWordControls).
	var object_poly := driver.widget_id("OBJECTPOLY")
	assert_gte(object_poly, 0, "OBJECTPOLY exists")
	assert_eq(driver.item_value(object_poly, driver.selected_row(object_poly)),
			str(PlayerOptions.new().current().object_polydetail),
			"OBJECTPOLY shows the persisted object detail")
	assert_false(driver.is_widget_disabled(object_poly), "OBJECTPOLY is editable")
	# The texture filter is served too: game.cfg's texfilter_level, seeded by
	# value and editable (options_policy.h kOptionsWordControls).
	var texfilter := driver.widget_id("TEXFILTER")
	assert_gte(texfilter, 0, "TEXFILTER exists")
	assert_eq(driver.item_value(texfilter, driver.selected_row(texfilter)),
			str(PlayerOptions.new().current().texfilter_level),
			"TEXFILTER shows the persisted texture filter")
	assert_false(driver.is_widget_disabled(texfilter), "TEXFILTER is editable")
	# The particle density and the texture compression are served as well:
	# game.cfg's particle_density and texcompression_level, seeded by value and
	# editable (options_policy.h kOptionsWordControls).
	var particles := driver.widget_id("PARTICLES")
	assert_gte(particles, 0, "PARTICLES exists")
	assert_eq(driver.item_value(particles, driver.selected_row(particles)),
			str(PlayerOptions.new().current().particle_density),
			"PARTICLES shows the persisted particle density")
	assert_false(driver.is_widget_disabled(particles), "PARTICLES is editable")
	var compression := driver.widget_id("TEXCOMPRESSION")
	assert_gte(compression, 0, "TEXCOMPRESSION exists")
	assert_eq(driver.item_value(compression, driver.selected_row(compression)),
			str(PlayerOptions.new().current().texcompression_level),
			"TEXCOMPRESSION shows the persisted texture compression")
	assert_false(driver.is_widget_disabled(compression), "TEXCOMPRESSION is editable")
	# The terrain texture detail is served: game.cfg's terrain_texdetail, seeded
	# by value and editable (options_policy.h kOptionsWordControls).
	var terrain_tex := driver.widget_id("TERRAINTEX")
	assert_gte(terrain_tex, 0, "TERRAINTEX exists")
	assert_eq(PlayerOptions.new().current().terrain_texdetail, 2, "the profile's word")
	assert_eq(driver.item_value(terrain_tex, driver.selected_row(terrain_tex)), "2",
			"TERRAINTEX shows the persisted terrain texture detail")
	assert_false(driver.is_widget_disabled(terrain_tex), "TERRAINTEX is editable")
	# The object texture detail is served too: game.cfg's object_texdetail, seeded
	# by value and editable (options_policy.h kOptionsWordControls).
	var object_tex := driver.widget_id("OBJECTTEX")
	assert_gte(object_tex, 0, "OBJECTTEX exists")
	assert_eq(driver.item_value(object_tex, driver.selected_row(object_tex)),
			str(PlayerOptions.new().current().object_texdetail),
			"OBJECTTEX shows the persisted object texture detail")
	assert_false(driver.is_widget_disabled(object_tex), "OBJECTTEX is editable")
	_cleanup(dir)


func test_crosshair_spinlist_uses_shared_options_and_persists_immediately() -> void:
	_write_cfg(["xhair_appearance = 11"])
	var options := PlayerOptions.new()
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir, options)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var spin: int = driver.widget_id("XHAIR_APPEARANCE")
	assert_gte(spin, 0, "Options authors the crosshair spin list.")
	assert_eq(driver.widget_kind_of(spin), MnuDocument.TYPE_SPINLIST,
			"XHAIR_APPEARANCE is a spin list.")
	assert_eq(driver.selected_row(spin), 11,
			"The spin list starts on the persisted crosshair.")
	watch_signals(options)
	driver.select_row(spin, 18)  # emits the "spinlist" value change
	assert_eq(options.current().crosshair_style, 18,
			"the shared owner changes immediately")
	assert_eq(PlayerOptions.new().current().crosshair_style, 18,
			"the selection persists through the shared owner")
	assert_signal_emit_count(options, "changed", 1)
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


func test_aspect_spinlist_restores_and_persists_the_selected_mode() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	state.aspect_mode = 1
	options.update(state)
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir, options)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var spin := driver.widget_id("16x9DISPLAY")
	assert_gte(spin, 0)
	assert_false(driver.is_widget_disabled(spin), "aspect selection is interactive")
	assert_eq(driver.spin_value_attr(spin), "1")
	driver.select_row_by_value(spin, "0")
	assert_eq(options.current().aspect_mode, 0)
	assert_eq(PlayerOptions.new().current().aspect_mode, 0)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	options.apply(sim)
	assert_eq(sim.get_local_player_aspect_mode(), 0,
			"the selected mode reaches camera and sights projection state")
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


func test_fresh_profile_seeds_the_aspect_row_from_the_desktop_ratio() -> void:
	# A fresh profile carries the first launch's video-test verdict: the cfg
	# word seeded from the primary desktop's ratio (1 past 1.34, else 0) and
	# saved at once (docs/mnu/menu-re.md, the 16x9DISPLAY paragraph), so the
	# fresh spin sits on that authored row and the simulation projects it.
	# Headless has no desktop to vary: both rows are reached through the seed
	# helper with explicit sizes, the live seed checked against the sampled
	# desktop's verdict.
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1024, 768)), 0,
			"a 4:3 desktop seeds the 4:3 row")
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1920, 1080)), 1,
			"a widescreen desktop seeds the widescreen row")
	var seeded := PlayerOptions.fresh_profile_aspect_mode(PlayerOptions.desktop_size())
	# No game.cfg: the mount's read finds the defaults and the video test runs.
	assert_true(GameCfgStore.enter_main_menu(null))
	var options := PlayerOptions.new()
	assert_eq(options.current().aspect_mode, seeded,
			"a fresh profile seeds the desktop's verdict")
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir, options)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var spin := driver.widget_id("16x9DISPLAY")
	assert_gte(spin, 0)
	assert_eq(driver.spin_value_attr(spin), str(seeded),
			"the fresh spin seeds the desktop's row")
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	options.apply(sim)
	assert_eq(sim.get_local_player_aspect_mode(), seeded,
			"the seeded mode reaches the projection without a saved profile")
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


func test_crosshair_color_and_spread_use_shared_options_and_persist() -> void:
	_write_cfg(["xhair_spread = 0"])
	var options := PlayerOptions.new()
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir, options)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var color: int = driver.widget_id("XHAIR_COLOR")
	assert_gte(color, 0, "Options authors the crosshair colour spin list.")
	assert_false(driver.is_widget_disabled(color),
			"the colour list is interactive")
	assert_eq(int(driver.item_value(color, driver.selected_row(color))),
			PlayerOptions.DEFAULT_CROSSHAIR_COLOR,
			"the default colour seeds by value onto the white row")
	var spread: int = driver.widget_id("XHAIR_SPREAD")
	assert_gte(spread, 0, "Options authors the spread checkbox.")
	assert_false(driver.is_widget_disabled(spread))
	assert_false(driver.is_widget_checked(spread),
			"the persisted spread toggle seeds the checkbox")

	var target_row := driver.selected_row(color)
	for row in driver.item_count(color):
		if int(driver.item_value(color, row)) \
				!= PlayerOptions.DEFAULT_CROSSHAIR_COLOR:
			target_row = row
			break
	driver.select_row(color, target_row)  # emits the "spinlist" value change
	assert_eq(options.current().crosshair_color,
			int(driver.item_value(color, target_row)),
			"the picked row's authored value reaches the shared owner")
	assert_eq(PlayerOptions.new().current().crosshair_color,
			options.current().crosshair_color,
			"the colour persists through the shared owner")
	driver.set_widget_checked(spread, false)
	driver.activate(spread)  # the click toggles it on
	assert_true(options.current().crosshair_spread,
			"the checkbox activation writes the shared owner")
	assert_true(PlayerOptions.new().current().crosshair_spread,
			"the spread toggle persists")
	# The two tip rows are served: seeded from the tip words, written back.
	for tip_row in ["MR_CLIPPY_KEYBOARD", "MR_CLIPPY_HINTS"]:
		var id: int = driver.widget_id(tip_row)
		assert_gte(id, 0, "Options authors %s." % tip_row)
		assert_false(driver.is_widget_disabled(id), "%s is interactive" % tip_row)
		assert_true(driver.is_widget_checked(id), "%s seeds the default-on word" % tip_row)
	var keyboard_tips: int = driver.widget_id("MR_CLIPPY_KEYBOARD")
	driver.set_widget_checked(keyboard_tips, true)
	driver.activate(keyboard_tips)  # the click toggles it off
	assert_false(options.current().keyboard_tips,
			"the keyboard-tips row writes the shared owner")
	assert_false(PlayerOptions.new().current().keyboard_tips,
			"the keyboard-tips word persists")
	assert_true(options.current().gameplay_tips)
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


# Whether opennova.cfg holds an expansion key (the pick a build before D-MNU-31
# remembered).
func _persisted_expansion_key() -> bool:
	var config := ConfigFile.new()
	return config.load(STATE_CONFIG_PATH) == OK \
			and config.has_section_key("resources", "expansion")


# The menu stylesheet (menu_style.mns) ships PFF-archived. It is indexed as the
# "menu_style" kind (so list_files surfaces it for editor browsing), but the shell
# still loads it by its canonical name through the VFS -- the engine contract is the
# fixed file name; otherwise %DEF_TEXT_*% colors (incl. the button hover colour) never
# resolve and mouse-over has no visible effect. Regression for that hover fix.
func test_runtime_loads_pff_archived_stylesheet_by_canonical_name() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_style_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	# CRLF: the game's reader stops responding on a value line ended by a lone LF.
	var mns := "// test stylesheet\r\nDEF_FONTNAME_LG Gunpl27b.fnt\r\nDEF_TEXT_FG FFFFFFFF\r\n" \
		+ "DEF_TEXT_MOUSEOVER_FG FFFF0000\r\nDEF_TEXT_SELECTED_FG FFFF0000\r\nDEF_TEXT_DISABLED_FG FF545252\r\n"
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": _fixture_bytes(MAIN_FIXTURE)},
		{"name": "menu_style.mns", "bytes": mns},
	])
	var root := ResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		pending("runtime resource root unavailable in this environment")
		DirAccess.remove_absolute(dir.path_join("resource.pff"))
		DirAccess.remove_absolute(dir)
		return
	var listed := root.list_files(".mns")
	assert_eq(listed.size(), 1, ".mns is a recognized kind (menu_style), so list_files surfaces it")
	if listed.size() == 1:
		assert_eq(String(listed[0]).to_lower(), "menu_style.mns", "the archived stylesheet is listed by name")
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	var style = shell.get_stylesheet()
	assert_not_null(style, "menu_style.mns loaded from the PFF by canonical name")
	if style != null:
		assert_eq(style.substitute("%DEF_TEXT_MOUSEOVER_FG%"), "FFFF0000",
			"hover colour macro resolves, so button mouse-over highlights")
	root.clear()  # release the PFF handle before deleting
	DirAccess.remove_absolute(dir.path_join("resource.pff"))
	DirAccess.remove_absolute(dir)


# --- Interactive music: the witnessed hardcoded pairs (D-BOOT-1) ----------------
#
# Retail hardcodes MENUMUS.SBF/.BIN + GAMEMUS.SBF/.BIN, renamed to M<n>/G<n> under
# expansion <n> [orig: Expansion_LoadAssets @ 0x4a4798]; the .bin scripts ship
# PFF-archived and must load by name through the VFS, while the .sbf banks stream
# loose from disk. The ONE music context lives on the MusicService autoload
# (the original streams one AudioVM context at a time): the shell opens the MENU
# context on setup, the world opens the GAME context at mission start. These pin
# the resolution order, the byte-path loading, and the context-swap semantics.

# Both contexts load from a packed archive (scripts by their hardcoded names
# through the VFS byte path) + loose banks; opening the game context is a
# full context reload [orig: AudioVM_OpenMusicContext @ 0x6722a0] and seeds the
# witnessed mission-start vars [orig: Game_StartMission @ 0x5255b3-0x52561b].
func test_music_contexts_load_pff_archived_by_hardcoded_names() -> void:
	var mus := _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)
	var sbf := _fixture_bytes(MENU_MUSIC_BANK_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_mus_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": _fixture_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	for bank_name in ["menumus.sbf", "gamemus.sbf"]:
		var f := FileAccess.open(dir.path_join(bank_name), FileAccess.WRITE)
		if f != null:
			f.store_buffer(sbf)
			f.close()
	var root := ResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(MusicService.current_context(), "menu", "setup opens the MENU music context")
	if MusicService.current_script() != null:
		assert_eq(MusicService.current_script().get_source_path(), "menumus.bin",
			"menumus.bin loaded from the PFF by hardcoded name")
	MusicService.set_var(14, 77)
	# Mission start = a full context reload onto the GAME pair + the witnessed seed.
	assert_true(MusicService.open_game_context(root), "game context opens")
	assert_eq(MusicService.current_context(), "game", "the one context swapped to GAME")
	if MusicService.current_script() != null:
		assert_eq(MusicService.current_script().get_source_path(), "gamemus.bin",
			"gamemus.bin loaded from the PFF by hardcoded name")
	assert_eq(MusicService.get_var(1), 0, "Var1 seeded 0 (never written in retail)")
	assert_eq(MusicService.get_var(7), 100, "Var7 seeded 100 (full health %)")
	assert_eq(MusicService.get_var(2), 0, "Var2 seeded 0")
	assert_eq(MusicService.get_var(14), 0, "full context reload clears unseeded globals")
	MusicService.set_var(14, 88)
	assert_true(MusicService.open_menu_context(root), "menu context reopens")
	assert_eq(MusicService.get_var(14), 0,
		"menu reload also clears globals under the headless audio driver")
	# A failed replacement open tears down the old pair (the bank-first gate's own
	# order is the ctest music_context's).
	var missing_root := ResourceRoot.new()
	assert_false(MusicService.open_menu_context(missing_root), "missing bank leaves silence")
	assert_eq(MusicService.current_context(), "", "a failed open keeps no context")
	assert_null(MusicService.director().get_bank(), "failed open retains no old bank")
	assert_null(MusicService.director().get_mus_script(), "failed open retains no old script")
	root.clear()
	TestFs.remove_dir_recursive(dir)


# With an expansion mounted, retail selects M<n>/G<n> unconditionally. A
# missing half therefore leaves that context silent; it never reselects the base
# pair [orig: Expansion_LoadAssets @ 0x4a4767-75; AudioVM_OpenContextFile
# @ 0x672160].
func test_music_resolution_keeps_incomplete_expansion_pair() -> void:
	var mus := _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musx_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": _fixture_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "Mjox01.bin", "bytes": mus},
		{"name": "Gjox01.bin", "bytes": mus},
	])
	# A loose expansion bank for the M stem (complete pair); the G stem
	# ships no bank (incomplete).
	_copy(MENU_MUSIC_BANK_FIXTURE, dir.path_join("expansion/jox01/Mjox01.sbf"))
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(MusicService.current_context(), "menu", "menu context opened")
	if MusicService.current_script() != null:
		assert_eq(MusicService.current_script().get_source_path(), "Mjox01.bin",
			"M<n>.bin preferred over menumus.bin (complete pair)")
	var menu_pair: MusicPair = shell.resolve_menu_music_pair()
	assert_true(String(menu_pair.bank).ends_with("Mjox01.sbf"),
		"the menu bank streams loose from the expansion folder")
	var game_pair: MusicPair = shell.resolve_game_music_pair()
	assert_eq(String(game_pair.script_name), "Gjox01.bin",
		"missing G<n>.sbf does not reselect the base script")
	assert_true(String(game_pair.bank).ends_with("Gjox01.sbf"),
		"missing G<n>.sbf keeps the expansion bank path so open fails to silence")
	root.clear()
	TestFs.remove_dir_recursive(dir)


# The converse incomplete pair also keeps the expansion stem. The bank opens,
# then the missing VFS script makes the context silent.
func test_music_incomplete_expansion_bank_only_stays_expansion() -> void:
	var mus := _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musk_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": _fixture_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	for stub_name in ["expansion/jox01/Gjox01.sbf", "gamemus.sbf"]:
		var stub := FileAccess.open(dir.path_join(stub_name), FileAccess.WRITE)
		if stub != null:
			stub.store_buffer(PackedByteArray([0]))
			stub.close()
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	var pair: MusicPair = shell.resolve_game_music_pair()
	assert_eq(String(pair.script_name), "Gjox01.bin",
		"bank-only G stem keeps the missing expansion script name")
	assert_true(String(pair.bank).ends_with("Gjox01.sbf"),
		"bank-only G stem keeps the expansion bank")
	root.clear()
	TestFs.remove_dir_recursive(dir)


# A mounted expansion with no music is silent even when the base pair exists.
func test_musicless_expansion_does_not_reselect_base_pair() -> void:
	var mus := _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musb_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": _fixture_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	_copy(MENU_MUSIC_BANK_FIXTURE, dir.path_join("menumus.sbf"))
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(MusicService.current_context(), "",
		"musicless mounted expansion leaves the menu context silent")
	assert_null(MusicService.current_script(),
		"musicless mounted expansion does not load MENUMUS.BIN")
	var pair: MusicPair = shell.resolve_menu_music_pair()
	assert_true(String(pair.bank).ends_with("Mjox01.sbf"),
		"musicless mounted expansion keeps the missing expansion bank path")
	assert_eq(String(pair.script_name), "Mjox01.bin",
		"musicless mounted expansion keeps the missing expansion script name")
	root.clear()
	TestFs.remove_dir_recursive(dir)


# A base dir with options.mnu packed in a base archive (so it loads under a runtime
# mount) plus one discoverable expansion (expansion/jox01/jox01.pff carrying an asset).
# The archive must use a boot-table name (resource.pff): the runtime mounts only the
# witnessed fixed table [orig: PFF_OpenAllArchives @ 0x4a4310] (D-VFS-2).
func _make_runtime_dir() -> String:
	var dir := OS.get_temp_dir().path_join("menu_shell_mods_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "options.mnu", "bytes": _fixture_bytes(ASSET_OPTIONS)},
		{"name": "menumus.bin", "bytes": _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)},
	])
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
		{"name": "Mjox01.bin", "bytes": _fixture_bytes(MENU_MUSIC_SCRIPT_FIXTURE)},
	])
	# The L archive carries the expansion's own name/description table, as the
	# retail pair does (jox01.bin lives in jox01L.pff).
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01L.pff"), [
		{"name": "jox01.bin", "bytes": _expansion_info_bin("Kendari",
				"Kendari island: the JO expansion.")},
	])
	_copy(MENU_MUSIC_BANK_FIXTURE, dir.path_join("menumus.sbf"))
	# Deliberately use retail-style uppercase to pin case-insensitive resolution
	# on Linux/macOS while preserving the actual shell path.
	_copy(MENU_MUSIC_BANK_FIXTURE, dir.path_join("expansion/jox01/MJOX01.SBF"))
	return dir


func _make_runtime_shell(dir: String, options: PlayerOptions = null):
	var root := ResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		return null
	var shell = MenuShellScript.new()
	if options != null:
		shell.set_player_options(options)
	shell.main_menu_file = "options.mnu"  # open the menu that carries the Mods tab
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	return shell


# An expansion's <n>.bin: the [exp_info] EXP_NAME / EXP_DESC pair retail's
# scan reads [orig: Expansion_ScanAndRegister @0x4a4578 / @0x4a45ef], minted
# through the string-table writer.
func _expansion_info_bin(exp_name: String, exp_desc: String) -> PackedByteArray:
	var table := RtxtStringFile.new()
	var section := table.add_section("exp_info")
	table.add_entry("EXP_NAME", exp_name, section, Vector2i.ZERO)
	table.add_entry("EXP_DESC", exp_desc, section, Vector2i.ZERO)
	return table.to_byte_array()


# ABORT's AUTHORED action rows raise the panel: read off the document and
# dispatched through the driver's own executor, so the pin is on game.mnu's
# SHOW CONFIRM_EXIT + HIDE MAIN_WRAPPER, never on literals a test typed.
func _raise_confirm(driver: MenuDriver) -> void:
	var rows: Array[MnuActionRow] = driver.widget_actions(driver.widget_id("ABORT"))
	var shape: Array[String] = []
	for row: MnuActionRow in rows:
		shape.append("%s %s %s" % [row.type.to_lower(),
				row.target.to_upper(),
				row.state.to_lower()])
	assert_eq(shape, ["window CONFIRM_EXIT show", "window MAIN_WRAPPER hide"],
			"game.mnu's ABORT authors SHOW CONFIRM_EXIT + HIDE MAIN_WRAPPER")
	for row: MnuActionRow in rows:
		assert_true(driver.dispatch_action_row(row),
				"the authored %s row dispatches" % row.target)


func _pause_key(keycode: Key) -> InputEventKey:
	var key := InputEventKey.new()
	key.keycode = keycode
	key.physical_keycode = keycode
	key.pressed = true
	return key
