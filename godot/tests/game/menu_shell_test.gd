extends GutTest

# Runtime menu shell (MenuShell) gates: it boots the JO menu set, services the
# shell policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuShellScript := preload("res://game/menu_shell.gd")

# The retail menu set, mission text and music program come from the reference
# fixture set (docs/asset-gated-tests.md, RetailData.fixture); the whole script
# skips without it.
const MAIN_FIXTURE := "mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "mnu/jo_loadout.mnu"  # the cross-.mnu target
const OPTIONS_FIXTURE := "mnu/jo_options.mnu"  # has the Mods tab (AVAIL_LIST/MOD_DESC)
const GAME_FIXTURE := "mnu/jo_game.mnu"  # pause menu with inline OPTIONS_WRAPPER
const SP_PLAY_FIXTURE := "mnu/jo_sp.mnu"  # play screen: mission list IA_LIST + ACCEPT
const MISSION_BIN_FIXTURE := "rtxt/00tra.bin"  # real per-mission bin: info/Title + briefing
const MUS_FIXTURE := "mus/jo_gamemus.bin"  # decrypted SCR0 MUS program
const SBF_FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"  # synthetic SBF bank (banks stream loose)


const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var _saved_controls_cfg := PackedByteArray()
var _had_controls_cfg := false


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	_had_controls_cfg = FileAccess.file_exists(ControlsBindings.CONFIG_PATH)
	_saved_controls_cfg = FileAccess.get_file_as_bytes(ControlsBindings.CONFIG_PATH) \
			if _had_controls_cfg else PackedByteArray()


func after_each() -> void:
	# The music service is an autoload; leave no context behind for the next test.
	MusicService.stop_context()
	TestFs.restore_file(STATE_CONFIG_PATH, _had_state_config, _saved_state_config)
	# The live binding model is a static shared with the whole run: restore the
	# catalog defaults and the on-disk cfg even when a remap test fails early.
	ControlsBindings.model().restore_defaults()
	TestFs.restore_file(ControlsBindings.CONFIG_PATH, _had_controls_cfg,
			_saved_controls_cfg)


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
	var dir := OS.get_temp_dir().path_join("menu_shell_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	_copy(SP_FIXTURE, dir.path_join("sp.mnu"))
	var f := FileAccess.open(dir.path_join("test.bms"), FileAccess.WRITE)
	if f != null:
		f.store_buffer(PackedByteArray([0]))
		f.close()
	return dir


func _copy(source: String, dst: String) -> void:
	var f := FileAccess.open(dst, FileAccess.WRITE)
	if f != null:
		f.store_buffer(_fixture_bytes(source))
		f.close()


# The bytes of a fixture: a res:// path reads directly (the synthetic SBF bank);
# anything else is a path into the reference fixture set.
func _fixture_bytes(source: String) -> PackedByteArray:
	var body := ""
	var screen := "OPTIONS"
	if source == MAIN_FIXTURE:
		screen = "STARTUP"
		body = MenuDriverFixture.wnd("list", "MISSION_LIST", 20)
		body += MenuDriverFixture.wnd("button", "ACCEPT", 50)
		body += MenuDriverFixture.wnd("button", "EXIT", 80)
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


# bms::AttribFlags game-mode bits (engine/formats/mission/bms.h).
const BMS_ATTRIB_COOP := 0x1000000
const BMS_ATTRIB_TDM := 0x20000000


# A minimal parseable .bms: the 616-byte header with magic BMS v19, the
# embedded mission_name, and one game-mode attrib bit.
func _cleanup(dir: String) -> void:
	for f in ["main.mnu", "sp.mnu", "options.mnu", "game.mnu", "test.bms"]:
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


# The shell seeds the five named Options sliders with the exact original
# ranges/pages. Audio/input retain deterministic minimum fallbacks; gamma is
# pinned to the registered high-quality retail comparison profile and locked.
# [orig: UI_OptionsScreenInit @ 0x554800;
# UI_PopulateRenderAndAudioSettings @ 0x55c830]
func test_startup_drives_music_var() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	# jo_main STARTUP declares MUSICVAR 1; the menu pushes it into the menumus
	# discriminator var (MusicDirector.MENU_MUSIC_VAR_SLOT — the witness lives
	# at the engine home, audio/music_policy.h kMenuMusicVarSlot); at index 0
	# the MUSICVAR was inert and the menu played the wrong section.
	var idx: int = MusicDirector.MENU_MUSIC_VAR_SLOT
	assert_eq(MusicService.director().get_var(idx), 1, "STARTUP MUSICVAR -> director var %d" % idx)
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


func test_top_level_quit_requests_exit() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	shell.get_driver().quit_requested.emit()  # main menu, empty stack -> exit to desktop
	assert_signal_emitted(shell, "exit_to_desktop_requested")
	_cleanup(dir)


func test_in_game_back_requests_resume() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pending("temp resource root unavailable")
		_cleanup(dir)
		return
	# Enter the pause context. game.mnu is absent so the overlay fails to load, but
	# the in-game flag is set, so a top-level back now means resume, not exit.
	shell.open_ingame_menu()
	watch_signals(shell)
	shell.get_driver().quit_requested.emit()
	assert_signal_emitted(shell, "resume_requested")
	assert_signal_not_emitted(shell, "exit_to_desktop_requested")
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


# --- Runtime (packed PFF) fixtures for the expansion/mods test -----------------

# A base dir with options.mnu packed in a base archive (so it loads under a runtime
# mount) plus one discoverable expansion (expansion/jox01/jox01.pff carrying an asset).
# The archive must use a boot-table name (resource.pff): the runtime mounts only the
# witnessed fixed table [orig: PFF_OpenAllArchives @ 0x4a4310] (D-VFS-2).
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


# The Controls remap flow end-to-end at the shell seam: double-click arms the
# capture (Control cell clears), the next key assigns through the witnessed
# record semantics and persists, Esc cancels, and CLEAR_KEY/DEFAULTS drive the
# same live model [orig: UI_ControlsRemapArmHandler @ 0x55d560; KeyBinding_HandleKeyAssignment
# @ 0x55bb20; CLEAR_KEY @ 0x55bfd0; DEFAULTS @ 0x55bd90].
