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
# Our own base game's menus (assets/, ADR 0048): they author the controls the
# shipped jo_options / jo_game / jo_sp screens carry, under the shipped names, so
# the shell's per-control policy runs over a whole real screen.
const ASSET_OPTIONS := "res://../assets/options.mnu"  # OPTIONS, MUSICVAR 9: sliders, video, crosshair, Mods, remap
const ASSET_GAME := "res://../assets/game.mnu"  # INGAME: the pause menu with its inline OPTIONS_WRAPPER
const ASSET_SP := "res://../assets/sp.mnu"  # SINGLE_PLAYER: the IA_LIST mission list + BRIEFING + ACCEPT
# A menu music pair the shell opens as MENUMUS.BIN / MENUMUS.SBF (the synthetic script and bank).
const MENU_MUSIC_SCRIPT_FIXTURE := "res://../fixtures/mus/synth_menumus.bin"
const MENU_MUSIC_BANK_FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"
# The shipped training mission's title, 00TRa.bin's Info/TITLE (the reference
# set's rtxt/00tra.bin, the row the SP-list pin read): the list row is the loose
# marker + that title [orig: SinglePlayer_PopulateMissionList @ 0x561840].
const TRAINING_ARMORY_TITLE := "Training: Basic Controls / Armory"
const BMS_ATTRIB_COOP := 0x1000000
const BMS_ATTRIB_TDM := 0x20000000


class _MissingBankMusicRoot extends RefCounted:
	var script_reads := 0

	func get_expansion() -> String:
		return ""

	func resolve_file(_name: String) -> String:
		return ""

	func has_file(name: String) -> bool:
		return name.to_lower() == "menumus.bin"

	func read_file(_name: String) -> PackedByteArray:
		script_reads += 1
		return PackedByteArray([1])


const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

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


# The bytes of a fixture: a res:// path reads directly (an assets/ menu, the
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
	for f in ["main.mnu", "sp.mnu", "options.mnu", "game.mnu", "test.bms", "menumus.bin", "menumus.sbf"]:
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
# static; the copyright line under the same name keeps its text.
# [orig: UI_OnStartupScreenActivate @0x5557f0 -> UI_FindScreenControl @0x63ae80]
func test_startup_screen_shows_the_version() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var version := driver.widget_id("VERSION")
	assert_gt(version, 0, "the VERSION static exists")
	assert_eq(driver.get_widget_text(version), "V1.7.5.7",
			"the first VERSION static shows the build's version")
	assert_eq(driver.get_widget_text(version + 1), "(c) 2009, NovaLogic, Inc.",
			"the second keeps its authored text")
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


func test_cross_mnu_jump_and_back_stack() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	# A cross-.mnu jump (file set) routes through menu_requested -> shell opens it.
	driver.menu_requested.emit("sp.mnu", "")
	assert_eq(shell.get_current_menu_file(), "sp.mnu", "shell loaded the requested menu")
	assert_eq(shell.get_menu_stack_depth(), 1, "previous menu pushed onto the back stack")
	# A top-level back (empty in-menu stack) pops the file stack back to main.mnu.
	shell.get_driver().pop_screen()
	assert_eq(shell.get_current_menu_file(), "main.mnu", "back returned to the main menu")
	assert_eq(shell.get_menu_stack_depth(), 0, "file back stack emptied")
	_cleanup(dir)


func test_failed_cross_mnu_jump_does_not_change_back_stack() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	shell.get_driver().menu_requested.emit("missing.mnu", "")
	assert_eq(shell.get_current_menu_file(), "main.mnu",
		"a rejected cross-menu jump keeps the current menu")
	assert_eq(shell.get_menu_stack_depth(), 0,
		"a rejected cross-menu jump cannot add a no-op Back step")
	_cleanup(dir)


func test_top_level_exit_control_requests_exit() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	var driver: MenuDriver = shell.get_driver()
	driver.widget_activated.emit(driver.widget_id("EXIT"), "EXIT")  # the EXIT Command
	assert_signal_emitted(shell, "exit_to_desktop_requested")
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


func test_pop_screen_with_no_history_does_nothing() -> void:
	# POP_SCREEN pops a history that is not empty and otherwise does nothing: it
	# never exits the game or resumes the mission (docs/mnu/menu-re.md).
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	shell.get_driver().pop_screen()
	assert_eq(shell.get_current_menu_file(), "main.mnu", "the main menu stays")
	shell.open_ingame_menu()
	shell.get_driver().pop_screen()
	assert_signal_not_emitted(shell, "exit_to_desktop_requested")
	assert_signal_not_emitted(shell, "resume_requested")
	_cleanup(dir)


func test_cross_mnu_jump_to_a_missing_screen_changes_nothing() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	shell.get_driver().menu_requested.emit("sp.mnu", "NO_SUCH_SCREEN")
	assert_eq(shell.get_current_menu_file(), "main.mnu",
		"a target the file does not hold keeps the current menu")
	assert_eq(shell.get_menu_stack_depth(), 0, "and pushes no Back step")
	_cleanup(dir)


# Leaving a mission returns to the screen it was started from, the history under
# it kept (D-MNU-28, docs/mnu/menu-re.md; the engine half is ctest screen_history).
# The start leaves the menu once: a restart's reload marks nothing more. In the
# mission a POP_SCREEN pops nothing past the mark, and the named back resumes.
func test_mission_return_shows_the_screen_it_was_started_from() -> void:
	var dir := _make_dir()
	TestFs.write_bytes(self, dir.path_join("game.mnu"), MenuDriverFixture.screen_xml(
			"INGAME", MenuDriverFixture.wnd("button", "HIDDEN_BACK", 20)).to_utf8_buffer())
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	driver.menu_requested.emit("sp.mnu", "")
	assert_eq(shell.get_current_menu_file(), "sp.mnu")
	assert_eq(driver.get_current_screen(), "LOADOUT")
	# The mission's start.
	shell.leave_menu_mode()
	shell.leave_menu_mode()
	assert_eq(driver.get_screen_history(), [
		{"file": "main.mnu", "screen": "STARTUP", "mark": false},
		{"file": "sp.mnu", "screen": "LOADOUT", "mark": false},
		{"file": "", "screen": "", "mark": true},
	], "the screen the mission starts from, under its mark, laid once")
	# The in-game menu: its back never reaches the menu's screens.
	assert_true(shell.open_ingame_menu(), "the in-game overlay loads")
	watch_signals(shell)
	driver.pop_screen()
	assert_signal_not_emitted(shell, "resume_requested", "a POP_SCREEN stops at the mark")
	assert_eq(shell.get_current_menu_file(), "game.mnu", "the pop reached no menu screen")
	assert_eq(shell.get_menu_stack_depth(), 3)
	driver.widget_activated.emit(driver.widget_id("HIDDEN_BACK"), "HIDDEN_BACK")
	assert_signal_emitted(shell, "resume_requested")
	assert_eq(shell.get_current_menu_file(), "game.mnu", "the back popped no menu screen")
	assert_eq(shell.get_menu_stack_depth(), 3)
	# LEAVE MISSION -> Yes: the shell's teardown re-enters the menu.
	assert_true(shell.setup(shell.get_resource_root()))
	assert_eq(shell.get_current_menu_file(), "sp.mnu", "back on the file the mission left")
	assert_eq(driver.get_current_screen(), "LOADOUT", "back on the screen the mission left")
	assert_eq(shell.get_menu_stack_depth(), 1, "STARTUP is still under it")
	# Its back is the menu's again.
	driver.pop_screen()
	assert_eq(shell.get_current_menu_file(), "main.mnu")
	assert_eq(driver.get_current_screen(), "STARTUP")
	assert_eq(shell.get_menu_stack_depth(), 0)
	_cleanup(dir)


# An entry that is no return from a mission (the same root again with no mission
# between) opens the main menu's first screen with an empty history.
func test_setup_without_a_mission_opens_the_main_menu() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	shell.get_driver().menu_requested.emit("sp.mnu", "")
	assert_eq(shell.get_menu_stack_depth(), 1)
	assert_true(shell.setup(shell.get_resource_root()))
	assert_eq(shell.get_current_menu_file(), "main.mnu")
	assert_eq(shell.get_driver().get_current_screen(), "STARTUP")
	assert_eq(shell.get_menu_stack_depth(), 0)
	_cleanup(dir)


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


func test_start_emits_selected_mission() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	# A mission-list selection relayed through the driver's aggregate value
	# signal (the seam contract), then a start control press.
	shell.get_driver().widget_value_changed.emit("MISSION_LIST", "list", 0, "test.bms")
	assert_eq(shell.get_selected_mission(), "test.bms", "selection tracked from the list relay")
	shell.get_driver().widget_activated.emit(
			shell.get_driver().widget_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted_with_parameters(shell, "start_requested", ["test.bms"])
	_cleanup(dir)


func test_start_without_selection_is_a_no_op() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	# The retail ACCEPT handler launches only through a shown list's current
	# entry; nothing selected means nothing launches, even though the dir holds
	# a .bms (docs/mnu/menu-re.md, SINGLE_PLAYER). The warning path reports it.
	shell.get_driver().widget_activated.emit(
			shell.get_driver().widget_id("ACCEPT"), "ACCEPT")
	assert_signal_not_emitted(shell, "start_requested",
		"no selection must not launch the catalog's first mission")
	_cleanup(dir)


class _RecordingCompanion extends MenuCompanion:
	var built := 0
	var released := 0

	func owns_menu(driver: MenuDriver) -> bool:
		return driver != null and driver.get_menu_file() == "main.mnu"

	func on_menu_built(driver: MenuDriver, file: String, screen: String,
			root: ResourceRoot) -> void:
		super(driver, file, screen, root)
		built += 1

	func on_menu_released() -> void:
		super()
		released += 1


func test_companion_released_when_document_changes_hands() -> void:
	var dir := _make_dir()
	_copy(OPTIONS_FIXTURE, dir.path_join("options.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	var stub := _RecordingCompanion.new()
	shell.add_companion(stub)
	assert_true(shell.open_menu("main.mnu", ""), "the claimed document opens")
	assert_eq(stub.built, 1, "the claiming companion is handed the wiring")
	assert_eq(stub.released, 0)
	assert_true(shell.open_menu("options.mnu", ""), "an unclaimed document opens")
	assert_eq(stub.released, 1,
			"losing the document releases the previously wired companion")
	assert_eq(stub.built, 1, "no rebuild for a document it does not own")
	assert_true(shell.open_menu("main.mnu", ""))
	assert_eq(stub.built, 2, "re-claiming wires the companion again")
	assert_eq(stub.released, 1, "a re-claim is not a release")
	_cleanup(dir)



# Every show of the front OPTIONS screen runs its init again (engine
# OptionsScreen::is_front_screen): with OPTIONS and its parent in one document,
# an ACCEPTed edit is the next visit's baseline, so that visit's BACK keeps it,
# and a discarded edit is not what the visit after shows. Through the
# controller: the in-document jump, the front ACCEPT's pop and the BACK's
# pop_screen row each change the screen in place.
func test_front_options_baseline_is_taken_per_visit() -> void:
	var options := PlayerOptions.new()
	var entry := options.current()
	entry.music_volume = 50
	options.update(entry)
	var dir := _make_dir()
	var to_options := MenuDriverFixture.wnd("button", "TO_OPTIONS", 20,
			'<ACTION type="screen" file="settings.mnu">OPTIONS</ACTION>')
	var body := MenuDriverFixture.wnd("scroll", "MUSICVOLUME", 20)
	body += MenuDriverFixture.wnd("table", "CONTROL_MAPPING", 50)
	body += MenuDriverFixture.wnd("button", "ACCEPT", 80)
	body += MenuDriverFixture.wnd("button", "BACK", 110, '<ACTION type="pop_screen"></ACTION>')
	TestFs.write_bytes(self, dir.path_join("settings.mnu"),
			(MenuDriverFixture.screen_xml("PARENT", to_options)
			+ MenuDriverFixture.screen_xml("OPTIONS", body)).to_utf8_buffer())
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("settings.mnu"))
		_cleanup(dir)
		return
	assert_true(shell.open_menu("settings.mnu", "PARENT"), "the two-screen document opens")
	var driver: MenuDriver = shell.get_driver()
	var go := driver.widget_id("TO_OPTIONS")
	var music := driver.widget_id("MUSICVOLUME")
	# The first visit accepts 25.
	driver.activate(go)
	assert_eq(driver.get_current_screen(), "OPTIONS")
	_edit_scroll(driver, music, "MUSICVOLUME", 25)
	driver.activate(driver.widget_id("ACCEPT"))
	assert_eq(driver.get_current_screen(), "PARENT", "the front ACCEPT pops in place")
	assert_eq(options.current().music_volume, 25)
	# The second visit starts from 25: its BACK keeps the accepted word.
	driver.activate(go)
	_edit_scroll(driver, music, "MUSICVOLUME", 75)
	driver.activate(driver.widget_id("BACK"))
	assert_eq(driver.get_current_screen(), "PARENT", "the BACK's pop_screen pops in place")
	assert_eq(options.current().music_volume, 25, "BACK keeps the earlier visit's ACCEPT")
	assert_eq(PlayerOptions.new().current().music_volume, 25, "and so does the cfg block")
	# The third visit shows the kept word, not the discarded one.
	driver.activate(go)
	assert_eq(driver.get_widget_scroll_range(music).value, 25,
			"the visit seeds the slider from the kept word")
	DirAccess.remove_absolute(dir.path_join("settings.mnu"))
	_cleanup(dir)


# A slider edit as the player makes it: the control moves and reports its value.
func _edit_scroll(driver: MenuDriver, id: int, control: String, value: int) -> void:
	var range := driver.get_widget_scroll_range(id)
	driver.set_widget_scroll_range(id, range.minimum, range.maximum, range.page, value)
	driver.widget_value_changed.emit(control, "scroll", value, str(value))


# The object-detail row is game.cfg's object_polydetail, served rather than
# pinned (engine runtime/menu/options_policy.h kObjectDetailControls): the
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
# (engine runtime/menu/options_policy.h kTextureFilterControls): the TEXFILTER
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
# runtime/menu/options_policy.h kParticleDensityControls,
# kTexCompressionControls): each combobox selects the row whose value is the
# persisted word, stays editable, and a pick writes the row's value back at once.
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
# same way (engine runtime/menu/options_policy.h kTerrainTexDetailControls):
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


# A throwaway companion: claims the menu (or not) and records whether it was driven.
class _FakeCompanion extends MenuCompanion:
	var owns: bool
	var built := false
	func _init(p_owns: bool) -> void:
		owns = p_owns
	func owns_menu(_menu) -> bool:
		return owns
	func on_menu_built(_menu, _file, _screen, _root) -> void:
		built = true


# The shell can hold several companions (mp.mnu + player.mnu); the first whose
# owns_menu() claims a built menu drives it, and a non-owning companion is skipped.
func test_multiple_companions_first_owner_drives_menu() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	var skipped := _FakeCompanion.new(false)
	var owner := _FakeCompanion.new(true)
	shell.add_companion(skipped)
	shell.add_companion(owner)
	shell.open_menu("main.mnu", "")  # re-wire with the companions installed
	assert_false(skipped.built, "a non-owning companion is skipped")
	assert_true(owner.built, "the first owning companion drives the menu")
	_cleanup(dir)


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
		"OBJECTTEX": "3",
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
	# from the shared options and editable (options_policy.h kObjectDetailControls).
	var object_poly := driver.widget_id("OBJECTPOLY")
	assert_gte(object_poly, 0, "OBJECTPOLY exists")
	assert_eq(driver.item_value(object_poly, driver.selected_row(object_poly)),
			str(PlayerOptions.new().current().object_polydetail),
			"OBJECTPOLY shows the persisted object detail")
	assert_false(driver.is_widget_disabled(object_poly), "OBJECTPOLY is editable")
	# The texture filter is served too: game.cfg's texfilter_level, seeded by
	# value and editable (options_policy.h kTextureFilterControls).
	var texfilter := driver.widget_id("TEXFILTER")
	assert_gte(texfilter, 0, "TEXFILTER exists")
	assert_eq(driver.item_value(texfilter, driver.selected_row(texfilter)),
			str(PlayerOptions.new().current().texfilter_level),
			"TEXFILTER shows the persisted texture filter")
	assert_false(driver.is_widget_disabled(texfilter), "TEXFILTER is editable")
	# The particle density and the texture compression are served as well:
	# game.cfg's particle_density and texcompression_level, seeded by value and
	# editable (options_policy.h kParticleDensityControls, kTexCompressionControls).
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
	# by value and editable (options_policy.h kTerrainTexDetailControls).
	var terrain_tex := driver.widget_id("TERRAINTEX")
	assert_gte(terrain_tex, 0, "TERRAINTEX exists")
	assert_eq(PlayerOptions.new().current().terrain_texdetail, 2, "the profile's word")
	assert_eq(driver.item_value(terrain_tex, driver.selected_row(terrain_tex)), "2",
			"TERRAINTEX shows the persisted terrain texture detail")
	assert_false(driver.is_widget_disabled(terrain_tex), "TERRAINTEX is editable")
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
	driver.set_widget_checked(spread, true)
	driver.widget_activated.emit(spread, "XHAIR_SPREAD")
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
	driver.set_widget_checked(keyboard_tips, false)
	driver.widget_activated.emit(keyboard_tips, "MR_CLIPPY_KEYBOARD")
	assert_false(options.current().keyboard_tips,
			"the keyboard-tips row writes the shared owner")
	assert_false(PlayerOptions.new().current().keyboard_tips,
			"the keyboard-tips word persists")
	assert_true(options.current().gameplay_tips)
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


func test_front_options_accept_keeps_immediate_changes_and_returns_to_main() -> void:
	var options := PlayerOptions.new()
	var dir := _make_dir()
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	driver.menu_requested.emit("options.mnu", "")
	assert_eq(shell.get_current_menu_file(), "options.mnu")
	assert_eq(shell.get_menu_stack_depth(), 1)
	driver.widget_value_changed.emit("MUSICVOLUME", "scroll", 88, "88")
	assert_eq(options.current().music_volume, 88,
			"the front surface writes the process-lifetime owner immediately")
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_eq(shell.get_current_menu_file(), "main.mnu",
			"the actionless front-menu Accept pops through the shell file stack")
	assert_eq(shell.get_menu_stack_depth(), 0)
	assert_eq(PlayerOptions.new().current().music_volume, 88,
			"Accept navigation retains the already-saved value")
	_cleanup(dir)


# The front-end OPTIONS BACK keeps no widget edit: retail writes the widgets'
# words only at the ACCEPT, and its BACK puts the saved music volume (and the gamma and the menu's
# sound-effects volume) back over the live previews, so an edit the owner applied
# and saved at once returns to the entry state, on the device too, while the
# BACK's pop_screen returns to main.mnu [orig: Options_HandleAcceptOrBack
# @0x55adcf..0x55ae01; UI_RegisterOptionsCallbacks @0x55d629].
func test_front_options_back_discards_the_screen_edits_and_returns_to_main() -> void:
	var options := PlayerOptions.new()
	var entry := options.current()
	entry.music_volume = 40
	entry.sound_fx_volume = 50
	options.update(entry)
	var dir := _make_dir()
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	driver.menu_requested.emit("options.mnu", "")
	assert_eq(shell.get_current_menu_file(), "options.mnu")
	driver.widget_value_changed.emit("MUSICVOLUME", "scroll", 88, "88")
	driver.widget_value_changed.emit("SOUNDFXVOLUME", "scroll", 99, "99")
	assert_eq(options.current().music_volume, 88, "the edit applies live as the preview")
	var back := driver.widget_id("BACK")
	assert_gte(back, 0, "the front Options authors BACK")
	driver.activate(back)
	assert_eq(shell.get_current_menu_file(), "main.mnu",
			"the BACK's pop_screen returns through the shell file stack")
	assert_eq(options.current().music_volume, 40, "BACK discards the music edit")
	assert_eq(options.current().sound_fx_volume, 50, "BACK discards the sound-effects edit")
	var reloaded := PlayerOptions.new().current()
	assert_eq(reloaded.music_volume, 40, "the config holds the entry music volume")
	assert_eq(reloaded.sound_fx_volume, 50, "the config holds the entry sound-effects volume")
	var music_bus := AudioServer.get_bus_index(PlayerOptions.MUSIC_BUS)
	if music_bus >= 0:
		assert_almost_eq(AudioServer.get_bus_volume_db(music_bus),
				SoundSelector.volume_db_from_255(40), 0.001,
				"the live music preview is rolled back")
	_cleanup(dir)


func test_pause_options_share_state_apply_accept_and_retain_cancel_changes() -> void:
	var options := PlayerOptions.new()
	var initial := options.current()
	initial.sound_fx_volume = 45
	initial.music_volume = 67
	initial.crosshair_style = 7
	initial.object_polydetail = 1
	options.update(initial)
	# The mouse and auto words are the player profile's current record's.
	var profile := PlayerProfile.store()
	assert_true(profile.set_word("mouse_sensitivity", 301))
	assert_true(profile.set_word("invert_mouse", 1))
	assert_true(profile.set_word("auto_medic_off", 1))

	var dir := _make_dir()
	_copy(ASSET_GAME, dir.path_join("game.mnu"))
	var shell = _make_shell(dir, options)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_ingame_menu(), "the pause document opens")
	var driver: MenuDriver = shell.get_driver()
	for pair in [
		["SOUNDFXVOLUME", 45],
		["MUSICVOLUME", 67],
		["MOUSE_SENSITIVITY", 301],
	]:
		var scroll = driver.get_widget_scroll_range(driver.widget_id(String(pair[0])))
		assert_not_null(scroll)
		assert_eq(scroll.value, int(pair[1]),
				"%s reads the same shared state as the front surface" % pair[0])
	assert_true(driver.is_widget_checked(driver.widget_id("INVERT_MOUSE")))
	# The in-game dialog seeds the auto pair from the record, the medic box
	# inverted [orig: UI_OptionsScreenInit @0x554d36 / @0x554d62].
	var auto_reload := driver.widget_id("OPTIONS_AUTORELOAD")
	var auto_medic := driver.widget_id("OPTIONS_AUTOMEDIC")
	assert_gte(auto_reload, 0, "the pause dialog authors OPTIONS_AUTORELOAD")
	assert_gte(auto_medic, 0, "the pause dialog authors OPTIONS_AUTOMEDIC")
	if auto_reload >= 0:
		assert_true(driver.is_widget_checked(auto_reload), "a fresh record's auto-reload is on")
		assert_false(driver.is_widget_disabled(auto_reload), "the auto-reload box is served")
	if auto_medic >= 0:
		assert_false(driver.is_widget_checked(auto_medic), "+1660 = 1 shows the box clear")
	assert_eq(driver.selected_row(driver.widget_id("XHAIR_APPEARANCE")), 7)
	assert_gt(driver.table_row_count(driver.widget_id("CONTROL_MAPPING")), 40,
			"the same remap controller seeds the pause table")

	var object_detail := driver.widget_id("OBJECTDETAIL")
	assert_gte(object_detail, 0)
	assert_eq(driver.item_value(object_detail, driver.selected_row(object_detail)), "1",
			"the in-game object-detail alias seeds by value from the shared options")
	assert_false(driver.is_widget_disabled(object_detail),
			"the in-game object-detail alias is editable")
	for unsupported_name: String in MenuFrame.options_unsupported_controls():
		var id := driver.widget_id(unsupported_name)
		if id >= 0:
			assert_true(driver.is_widget_disabled(id),
					"%s is read-only in the pause surface too" % unsupported_name)

	var main_wrapper := driver.widget_id("MAIN_WRAPPER")
	var options_wrapper := driver.widget_id("OPTIONS_WRAPPER")
	driver.set_widget_shown(main_wrapper, false)
	driver.set_widget_shown(options_wrapper, true)
	driver.widget_value_changed.emit("SOUNDFXVOLUME", "scroll", 72, "72")
	# The dialog's mouse and auto edits reach the record only at its Accept,
	# which asks the owner to apply them [orig: @0x5550de..0x5552bc].
	driver.set_widget_checked(driver.widget_id("INVERT_MOUSE"), false)
	if auto_medic >= 0:
		driver.set_widget_checked(auto_medic, true)
	assert_eq(profile.get_word("invert_mouse"), 1, "an edit waits for the Accept")
	watch_signals(shell)
	driver.widget_activated.emit(driver.widget_id("OPT_ACCEPT"), "OPT_ACCEPT")
	assert_true(driver.is_widget_shown(main_wrapper))
	assert_false(driver.is_widget_shown(options_wrapper),
			"the formerly actionless pause Accept returns to the pause menu")
	assert_eq(options.current().sound_fx_volume, 72)
	assert_eq(profile.get_word("invert_mouse"), 0, "the Accept writes the record")
	if auto_medic >= 0:
		assert_eq(profile.get_word("auto_medic_off"), 0, "a checked medic box stores 0")
	assert_signal_emitted(shell, "ingame_controls_accepted",
			"the owner applies the record and saves the profile")

	driver.set_widget_shown(main_wrapper, false)
	driver.set_widget_shown(options_wrapper, true)
	var music_before: int = options.current().music_volume
	driver.widget_value_changed.emit("MUSICVOLUME", "scroll", 84, "84")
	assert_eq(options.current().music_volume, 84,
			"edits apply live as the preview")
	driver.widget_activated.emit(driver.widget_id("OPT_CANCEL"), "OPT_CANCEL")
	assert_true(driver.is_widget_shown(main_wrapper))
	assert_false(driver.is_widget_shown(options_wrapper))
	# Retail's pause Cancel re-seeds the screen from the saved settings and
	# rolls the live preview back; Accept committed sound_fx as the baseline
	# (docs/mnu/menu-re.md "The in-game options dialog").
	assert_eq(options.current().music_volume, music_before,
			"Cancel reverts the staged music edit")
	assert_eq(options.current().sound_fx_volume, 72,
			"the accepted edit survives a later Cancel")
	var reloaded := PlayerOptions.new().current()
	assert_eq(reloaded.sound_fx_volume, 72)
	assert_eq(reloaded.music_volume, music_before,
			"the reverted edit never reaches the config")
	_cleanup(dir)

# Options -> Mods (D-MNU-31): AVAIL_LIST lists the base game's row first (the
# key "Joint Operations: Typhoon Rising" through the list's string table, which
# this folder lacks, so the key shows), then each expansion folder by its own
# EXP_NAME; the game running is highlighted. A pick shows its EXP_DESC alone
# (the base row: nothing); a double click does nothing; ACCEPT raises the reload
# request, and the next menu tick switches the live root, boots the menu anew
# over it and persists nothing [orig: Options_PopulateModList @ 0x559fb0;
# Options_OnModListSelect @ 0x55a530; Options_HandleAcceptOrBack @ 0x55ad05].
# Uses a runtime (packed PFF) mount so mount_runtime has real archives.
func test_mods_tab_lists_the_base_game_and_switches_for_the_run() -> void:
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	assert_eq(MusicService.current_context(), "menu", "base MENU music starts with the shell")
	assert_not_null(MusicService.current_script(), "base MENUMUS.BIN resolves")
	if MusicService.current_script() != null:
		assert_eq(MusicService.current_script().get_source_path(), "menumus.bin")
	assert_eq(MusicService.get_var(2), 9, "OPTIONS MUSICVAR drives Var2 before the swap")
	var driver: MenuDriver = shell.get_driver()
	var avail: int = driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0, "AVAIL_LIST authored")
	assert_eq(driver.item_count(avail), 2, "the base game's row, then the one expansion")
	assert_eq(driver.item_text(avail, 0), "Joint Operations: Typhoon Rising",
		"the base row's key shows where no string table names it")
	assert_eq(driver.item_text(avail, 1), "Kendari", "an expansion's row is its EXP_NAME")
	assert_eq(driver.selected_row(avail), 0, "the base game running is highlighted")
	var desc: int = driver.widget_id("MOD_DESC")
	assert_gte(desc, 0, "MOD_DESC authored")
	driver.select_row(avail, 1, true)
	assert_eq(driver.get_widget_text(desc), "Kendari island: the JO expansion.",
		"a pick shows its EXP_DESC alone")
	driver.select_row(avail, 0, true)
	assert_eq(driver.get_widget_text(desc), "", "the base row describes nothing")
	driver.list_activated.emit(avail, 1)
	assert_false(shell.has_pending_expansion_reload(), "a double click switches nothing")
	driver.select_row(avail, 1, true)
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0, "options ACCEPT control authored")
	driver.widget_activated.emit(accept, "ACCEPT")
	assert_true(shell.has_pending_expansion_reload(),
		"ACCEPT raises the request instead of remounting inline")
	assert_eq(String(shell.get_resource_root().get_expansion()), "", "nothing switches before the tick")
	watch_signals(shell)
	shell.update_menu_frame()
	assert_false(shell.has_pending_expansion_reload(), "the update tick lowers the request flag")
	assert_eq(String(shell.get_resource_root().get_expansion()), "jox01", "the live root switched")
	assert_signal_emitted(shell, "game_reloaded", "the shell says the game reloaded")
	assert_false(_persisted_expansion_key(), "the pick is not persisted: it lasts the run")
	assert_not_null(MusicService.current_script(), "expansion menu context reopens")
	if MusicService.current_script() != null:
		assert_eq(MusicService.current_script().get_source_path(), "Mjox01.bin",
			"live expansion selection swaps to the M<exp> script")
	assert_eq(MusicService.get_var(2), 9, "the menu booted anew drives its screen's MUSICVAR")
	# The menu booted anew over the switched root: its list highlights the game running.
	avail = driver.widget_id("AVAIL_LIST")
	desc = driver.widget_id("MOD_DESC")
	assert_eq(driver.selected_row(avail), 1, "the expansion running is highlighted")
	assert_eq(driver.get_widget_text(desc), "Kendari island: the JO expansion.")
	assert_eq(shell.get_resource_root().read_file("expmodel.3di").get_string_from_utf8(),
		"exp model", "expansion archive mounted over the base game")
	# The base game's row switches back.
	driver.select_row(avail, 0, true)
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	shell.update_menu_frame()
	assert_eq(String(shell.get_resource_root().get_expansion()), "",
		"the base row switches to the base game")
	assert_eq(driver.selected_row(driver.widget_id("AVAIL_LIST")), 0)
	shell.get_resource_root().clear()  # release PFF handles before deleting the temp archives
	TestFs.remove_dir_recursive(dir)


# Options -> Mods OK (the ACCEPT button) must APPLY the highlighted row, not
# launch a mission. ACCEPT is overloaded across JO screens (launch on Single Player,
# plain OK on Options); the shell scopes it by screen role, so on a Mods screen (mod
# list, no mission list) ACCEPT applies. Regression for the "OK loads a mission" bug.
# ACCEPT on the game running takes nothing. A switch saves the player profile
# first, under the expansion it leaves [orig: Options_HandleAcceptOrBack
# @ 0x55ad35 -> PlayerProfile_SaveToFiles; playerinfo/player-sav-re.md].
func test_mods_ok_applies_expansion_without_launching() -> void:
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir)
	if shell == null:
		pending("runtime resource root unavailable in this environment")
		TestFs.remove_dir_recursive(dir)
		return
	var run_dir := OS.get_cache_dir().path_join("menu_shell_mods_profile_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	PlayerProfile.load_for(shell.get_resource_root())
	var driver: MenuDriver = shell.get_driver()
	var avail: int = driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0, "AVAIL_LIST authored")
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0, "options ACCEPT control authored")
	driver.widget_activated.emit(accept, "ACCEPT")  # the base game running, its row highlighted
	assert_false(shell.has_pending_expansion_reload(), "ACCEPT on the game running takes nothing")
	assert_false(FileAccess.file_exists(run_dir.path_join("player.sav")), "and saves nothing")
	driver.select_row(avail, 1, false)  # highlight jox01 (no double-click / activation)
	watch_signals(shell)
	driver.widget_activated.emit(accept, "ACCEPT")  # press OK
	assert_signal_not_emitted(shell, "start_requested", "OK on the Mods screen must not launch")
	assert_true(FileAccess.file_exists(run_dir.path_join("player.sav")),
			"the switch saves the player profile before it takes place")
	assert_true(FileAccess.file_exists(run_dir.path_join("weapon.sav")),
			"weapon.sav under the expansion it leaves, the base game's")
	assert_false(FileAccess.file_exists(run_dir.path_join("expansion/jox01/weapon.sav")))
	shell.update_menu_frame()  # the deferred remount runs on the next menu tick
	assert_eq(String(shell.get_resource_root().get_expansion()), "jox01",
		"OK applied the highlighted mod")
	assert_false(_persisted_expansion_key(), "nothing persisted")
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(run_dir)
	shell.get_resource_root().clear()
	TestFs.remove_dir_recursive(dir)


# A loose authoring root (the ONED --loose-root play-test mount, ADR 0025) has no
# packed archives to relayer: applying a discoverable expansion must refuse and
# leave the live loose mount untouched, not remount it through mount_runtime into
# a cleared root (the zero-archives fatal would kill the running play-test).
func test_mods_apply_refuses_on_a_loose_root_and_keeps_the_mount() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_loose_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	_copy(MENU_MUSIC_SCRIPT_FIXTURE, dir.path_join("menumus.bin"))
	_copy(MENU_MUSIC_BANK_FIXTURE, dir.path_join("menumus.sbf"))
	# The expansion pair exists ON DISK (the scan lists its folder), but the
	# mounted root is a loose-only mount, which cannot layer it.
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var shell = MenuShellScript.new()
	shell.main_menu_file = "options.mnu"
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	assert_true(shell.setup(root), "the loose root serves the menu fixture")
	var driver: MenuDriver = shell.get_driver()
	var avail: int = driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0)
	assert_eq(driver.item_count(avail), 2, "the base row and the expansion folder on disk")
	assert_eq(driver.item_text(avail, 1), "Unnamed Expansion", "a folder with no <n>.bin is unnamed")
	driver.select_row(avail, 1, false)
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0)
	driver.widget_activated.emit(accept, "ACCEPT")
	assert_false(shell.has_pending_expansion_reload(),
			"an unusable pick never raises the reload request")
	shell.update_menu_frame()
	assert_eq(String(root.get_expansion()), "", "the loose mount refuses the switch")
	assert_false(_persisted_expansion_key(), "nothing persisted")
	assert_false(root.read_file("options.mnu").is_empty(),
			"the live loose mount survives untouched (no clear())")
	root.clear()
	for sub in ["options.mnu", "menumus.bin", "menumus.sbf",
			"expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# Whether opennova.cfg holds an expansion key (the pick a build before D-MNU-31
# remembered).
func _persisted_expansion_key() -> bool:
	var config := ConfigFile.new()
	return config.load(STATE_CONFIG_PATH) == OK \
			and config.has_section_key(ResourceDirSettings.SECTION, "expansion")


# D-MNU-14: the SP mission list rides the catalog — Co-op-family rows only,
# titled from the sibling .bin (the header's embedded name when no .bin), the
# loose "*" marker, the briefing pane cleared on populate and filled on
# selection, and ACCEPT gated on a pick
# [orig: SinglePlayer_PopulateMissionList @ 0x561840 +
# SinglePlayer_MissionListEventHandler @ 0x561ed0].
func test_sp_mission_list_titles_briefing_and_accept_gate() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_sp_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(ASSET_SP, dir.path_join("main.mnu"))
	_write_bms(dir.path_join("alpha.bms"), "Alpha Header", BMS_ATTRIB_COOP)
	TestFs.write_bytes(self, dir.path_join("alpha.bin"), _mission_bin(TRAINING_ARMORY_TITLE,
			"This mission covers the basic controls and the armory."))
	_write_bms(dir.path_join("bravo.bms"), "Bravo Header", BMS_ATTRIB_TDM)
	_write_bms(dir.path_join("charlie.bms"), "Charlie Header", BMS_ATTRIB_COOP)
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp root unavailable in this environment")
		return
	var driver: MenuDriver = shell.get_driver()
	var list_id := driver.widget_id("IA_LIST")
	assert_gt(list_id, -1, "sp.mnu authors the IA_LIST mission list")
	assert_eq(driver.item_count(list_id), 2, "the TDM mission is filtered off the SP list")
	assert_eq(driver.item_text(list_id, 0), "*" + TRAINING_ARMORY_TITLE,
			"loose titled row: the * marker + the .bin's info/Title")
	assert_eq(driver.item_text(list_id, 1), "*Charlie Header",
			"no .bin: the BMS header's embedded name stands in")
	var accept := driver.widget_id("ACCEPT")
	assert_gt(accept, -1)
	assert_true(driver.is_widget_disabled(accept), "ACCEPT is disabled before a pick")
	var briefing := driver.widget_id("BRIEFING")
	assert_gt(briefing, -1)
	assert_eq(driver.get_widget_text(briefing), "", "the populate clears the briefing pane")
	# The selection relay — the same seam the pump's list click drives.
	driver.widget_value_changed.emit("IA_LIST", "list", 0, driver.item_text(list_id, 0))
	assert_eq(shell.get_selected_mission(), "alpha.bms",
			"the launch resolves the FILE behind the titled row")
	assert_true(driver.get_widget_text(briefing).begins_with("This mission covers"),
			"selection fills the briefing pane from the .bin")
	assert_false(driver.is_widget_disabled(accept), "the pick arms ACCEPT")
	for f in ["main.mnu", "alpha.bms", "alpha.bin", "bravo.bms", "charlie.bms"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


# The opposite scope: on a play screen (mission list IA_LIST present) the same ACCEPT
# name still launches, so the screen-scoped wiring did not break the SP launch path.
func test_play_screen_accept_still_launches() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_sp_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(ASSET_SP, dir.path_join("main.mnu"))  # sp.mnu as the opened menu
	var f := FileAccess.open(dir.path_join("alpha.bms"), FileAccess.WRITE)
	if f != null:
		f.store_buffer(PackedByteArray([0]))
		f.close()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir.path_join("alpha.bms"))
		DirAccess.remove_absolute(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0, "SP ACCEPT control authored")
	var list_id := driver.widget_id("IA_LIST")
	assert_gt(list_id, -1, "sp.mnu authors the IA_LIST mission list")
	watch_signals(shell)
	# Retail's ACCEPT launches only through the shown list's current entry; with
	# nothing selected it does nothing (SinglePlayer_HandleAccept, menu-re.md).
	driver.widget_activated.emit(accept, "ACCEPT")
	assert_signal_not_emitted(shell, "start_requested",
			"ACCEPT with no mission selected is a no-op, never a first-entry fallback")
	driver.widget_value_changed.emit("IA_LIST", "list", 0, driver.item_text(list_id, 0))
	driver.widget_activated.emit(accept, "ACCEPT")
	# ACCEPT on a mission-list screen launches the selected entry.
	assert_signal_emitted_with_parameters(shell, "start_requested", ["alpha.bms"])
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir.path_join("alpha.bms"))
	DirAccess.remove_absolute(dir)


# The options controller listens on the same driver for every document. On a
# play screen (no CONTROL_MAPPING) its named controls must stay inert: ACCEPT
# belongs to the shell's launch path (no pop underneath the launch), and a
# stray DEFAULTS activation must not wipe the persisted bindings.
func test_options_controls_inert_without_control_table() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_sp_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(ASSET_SP, dir.path_join("main.mnu"))  # sp.mnu: ACCEPT, no control table
	var f := FileAccess.open(dir.path_join("alpha.bms"), FileAccess.WRITE)
	if f != null:
		f.store_buffer(PackedByteArray([0]))
		f.close()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir.path_join("alpha.bms"))
		DirAccess.remove_absolute(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	assert_lt(driver.widget_id("CONTROL_MAPPING"), 0, "sp.mnu authors no control table")
	var list_id := driver.widget_id("IA_LIST")
	assert_gt(list_id, -1, "sp.mnu authors the IA_LIST mission list")
	watch_signals(shell)
	# The launch needs a selected mission (retail's ACCEPT is a no-op otherwise).
	driver.widget_value_changed.emit("IA_LIST", "list", 0, driver.item_text(list_id, 0))
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_signal_emitted(shell, "start_requested", "the shell launch path still owns ACCEPT")
	assert_eq(shell.get_current_menu_file(), "main.mnu",
			"no options pop underneath the launch")
	assert_eq(shell.get_menu_stack_depth(), 0)
	# Edit the record's words, then fire the name the options surface would
	# own; a stray DEFAULTS must not reset them (words already at defaults
	# would make a no-op reset pass vacuously, hence the edit first).
	var profile := PlayerProfile.store()
	assert_true(profile.set_word("mouse_sensitivity", 300))
	driver.widget_activated.emit(-1, "DEFAULTS")
	assert_eq(profile.get_word("mouse_sensitivity"), 300,
			"a stray DEFAULTS on a non-options document leaves the record alone")
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir.path_join("alpha.bms"))
	DirAccess.remove_absolute(dir)


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
	# A failed replacement open tears down the old pair and obeys the witnessed
	# bank-first gate: retail never attempts to read the script after no .sbf.
	var missing_root := _MissingBankMusicRoot.new()
	assert_false(MusicService.open_menu_context(missing_root), "missing bank leaves silence")
	assert_eq(missing_root.script_reads, 0, "missing-bank gate precedes VFS script read")
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


# The Controls remap flow end-to-end at the shell seam: double-click arms the
# capture (the Control cell reads ???), the next key assigns through the witnessed
# record semantics, Esc cancels, and CLEAR_KEY/DEFAULTS drive the same records
# [orig: UI_ControlsRemapArmHandler @ 0x55d560; KeyBinding_HandleKeyAssignment
# @ 0x55bb20; CLEAR_KEY @ 0x55bfd0; DEFAULTS @ 0x55bd90].
# The remap flow edits the Options screen's own records, built from the player
# profile's table; its ACCEPT stores them into the record, and the live
# bindings take them at the next session start's controls apply
# [orig: UI_BuildKeyBindingLoadoutTable @0x559e50; Options_HandleAcceptOrBack @0x55ace5;
# Profile_ApplySessionControls @0x563620].
func test_control_mapping_remap_flow() -> void:
	# after_each restores the catalog defaults even on an early assert failure;
	# before_each gave the case a fresh profile record.
	ControlsBindings.model().restore_defaults()

	var dir := OS.get_temp_dir().path_join("menu_shell_remap_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var shell = MenuShellScript.new()
	shell.main_menu_file = "options.mnu"
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	assert_true(shell.setup(root), "the options fixture boots")
	var driver: MenuDriver = shell.get_driver()
	var table: int = driver.widget_id("CONTROL_MAPPING")
	assert_gte(table, 0, "the mapping table exists")
	assert_gt(driver.table_row_count(table), 40, "the live rows are seeded")
	assert_eq(driver.table_cell_text(table, 0, 2), "W or Up",
			"row 0 shows the Forward default")

	# Double-click row 0: the capture arms and the Control cell reads "???"
	# [orig: UI_ControlsRemapArmHandler @ 0x55d560, the push @ 0x55d5c8].
	driver.list_activated.emit(table, 0)
	assert_eq(driver.table_cell_text(table, 0, 2), "???",
			"the armed row's Control cell reads ???")

	# The next key assigns (Y replaces the primary: both slots were full).
	var key := InputEventKey.new()
	key.pressed = true
	key.physical_keycode = KEY_Y
	shell.get_viewport().push_input(key)
	assert_eq(driver.table_cell_text(table, 0, 2), "Y or Up",
			"the captured key lands in the screen's record and the cell restores")
	assert_false(ControlsBindings.model().godot_keys_for_token("move_forward").has(KEY_Y),
			"the live bindings wait for the ACCEPT and the next session start")

	# Esc cancels a fresh capture without changing the record.
	driver.list_activated.emit(table, 0)
	var esc := InputEventKey.new()
	esc.pressed = true
	esc.physical_keycode = KEY_ESCAPE
	shell.get_viewport().push_input(esc)
	assert_eq(driver.table_cell_text(table, 0, 2), "Y or Up",
			"Esc restores the cell unchanged")

	# CLEAR_KEY empties the selected row; DEFAULTS restores the catalog.
	driver.table_select_row(table, 0)
	driver.widget_activated.emit(driver.widget_id("CLEAR_KEY"), "CLEAR_KEY")
	assert_eq(driver.table_cell_text(table, 0, 2), "",
			"CLEAR_KEY empties the keyboard slots")
	driver.widget_activated.emit(driver.widget_id("DEFAULTS"), "DEFAULTS")
	assert_eq(driver.table_cell_text(table, 0, 2), "W or Up",
			"DEFAULTS restores the catalog binding")

	# Rebind again and ACCEPT: the record's table takes the screen's records;
	# the session start's controls apply hands them to the gameplay sampler.
	driver.list_activated.emit(table, 0)
	shell.get_viewport().push_input(key)
	assert_eq(driver.table_cell_text(table, 0, 2), "Y or Up")
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_false(ControlsBindings.model().godot_keys_for_token("move_forward").has(KEY_Y),
			"the ACCEPT writes the profile, not the live bindings")
	ControlsBindings.apply_profile(PlayerProfile.store())
	var keys: PackedInt32Array = ControlsBindings.model().godot_keys_for_token("move_forward")
	assert_true(keys.has(KEY_Y), "the rebound key reaches the sampler at the apply")
	assert_false(keys.has(KEY_W), "Y replaced the primary W")

	DirAccess.remove_absolute(dir.path_join("options.mnu"))
	DirAccess.remove_absolute(dir)


# Leaving the screen while a capture is armed tears the capture down: a later
# keypress must neither assign nor be swallowed as an invisible Esc target.
# Retail cannot exhibit the stale capture — its pump state lives with the
# Options screen [orig: UI_ControlsRemapArmHandler @ 0x55d560].
func test_control_mapping_capture_dies_on_screen_change() -> void:
	ControlsBindings.model().restore_defaults()
	var dir := OS.get_temp_dir().path_join("menu_shell_remap_nav_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(ASSET_OPTIONS, dir.path_join("options.mnu"))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var shell = MenuShellScript.new()
	shell.main_menu_file = "options.mnu"
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	assert_true(shell.setup(root), "the options fixture boots")
	var driver: MenuDriver = shell.get_driver()
	var table: int = driver.widget_id("CONTROL_MAPPING")
	assert_eq(driver.table_cell_text(table, 0, 2), "W or Up",
			"row 0 shows the Forward default")

	# Arm, then navigate: the screen change cancels the capture and restores
	# the armed cell.
	driver.list_activated.emit(table, 0)
	assert_eq(driver.table_cell_text(table, 0, 2), "???",
			"the armed row's Control cell reads ???")
	assert_true(driver.navigate_to_screen("OPTIONS"), "navigation succeeds")
	assert_eq(driver.table_cell_text(table, 0, 2), "W or Up",
			"the canceled capture restores the Control cell")

	# The next key must not assign to the stale action.
	var key := InputEventKey.new()
	key.pressed = true
	key.physical_keycode = KEY_U
	shell.get_viewport().push_input(key)
	assert_eq(driver.options_control_text(
			ControlsBindings.model().action_index_for_row(0),
			ControlsModel.DEVICE_KEYBOARD), "W or Up",
			"the screen's Forward record still holds its defaults")
	DirAccess.remove_absolute(dir.path_join("options.mnu"))
	DirAccess.remove_absolute(dir)


func test_ingame_abort_raises_confirm_and_only_yes_returns() -> void:
	var dir := _make_dir()
	_copy(ASSET_GAME, dir.path_join("game.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_ingame_menu(), "the pause document opens")
	var driver: MenuDriver = shell.get_driver()
	watch_signals(shell)
	var confirm := driver.widget_id("CONFIRM_EXIT")
	var main_wrapper := driver.widget_id("MAIN_WRAPPER")
	assert_gte(confirm, 0, "game.mnu authors the confirm panel")
	assert_false(driver.is_widget_shown(confirm),
			"the 'Are you sure?' panel starts hidden")

	# ABORT is authored actions only (SHOW CONFIRM_EXIT + HIDE MAIN_WRAPPER):
	# the shell must NOT treat it as the return-to-menu Command. A mouse click
	# cannot drive it here - the authored button has no BOTTOM, so its height
	# is font-derived and solves to zero without the mounted style/fonts - so
	# the activation seam and the public action executor stand in (the
	# on-activation ACTION dispatch itself is pinned by menu_driver_test).
	driver.widget_activated.emit(driver.widget_id("ABORT"), "ABORT")
	assert_signal_not_emitted(shell, "return_to_menu_requested",
			"ABORT alone leaves the mission alive")
	_raise_confirm(driver)
	assert_true(driver.is_widget_shown(confirm),
			"ABORT's authored actions raise the 'Are you sure?' panel")
	assert_false(driver.is_widget_shown(main_wrapper),
			"the main wrapper hides behind the confirmation")

	# ESC is the authored CONFIRM_NO hotkey (the hidden MAIN_WRAPPER's
	# HIDDEN_BACK cannot eat it): cancel restores the wrapper.
	assert_true(driver.handle_key_input(_pause_key(KEY_ESCAPE)))
	assert_false(driver.is_widget_shown(confirm), "No cancels the exit")
	assert_true(driver.is_widget_shown(main_wrapper))
	assert_signal_not_emitted(shell, "return_to_menu_requested")

	# ENTER is the authored CONFIRM_YES hotkey: the exit itself is the shell's
	# registered Command on CONFIRM_YES, like the engine's per-control seam.
	_raise_confirm(driver)
	assert_true(driver.handle_key_input(_pause_key(KEY_ENTER)))
	assert_signal_emitted(shell, "return_to_menu_requested",
			"CONFIRM_YES emits the mission-exit intent")
	_cleanup(dir)


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
