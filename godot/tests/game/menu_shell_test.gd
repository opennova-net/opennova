extends GutTest

# Runtime menu shell (MenuShell) gates: it boots the JO menu set, services the
# shell policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuShellScript := preload("res://game/menu_shell.gd")

# The menu names below key the synthetic screens _fixture_bytes authors; the
# retail half (tests/retail/game/menu_shell_test.gd) reads the reference
# fixture set under the same names.
const MAIN_FIXTURE := "mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "mnu/jo_loadout.mnu"  # the cross-.mnu target
const OPTIONS_FIXTURE := "mnu/jo_options.mnu"  # has the Mods tab (AVAIL_LIST/MOD_DESC)


const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _state_config: TestFs.Snapshot
var _controls_cfg: TestFs.Snapshot


func before_each() -> void:
	_state_config = TestFs.snapshot(STATE_CONFIG_PATH)
	if _state_config.existed:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	_controls_cfg = TestFs.snapshot(ControlsBindings.CONFIG_PATH)


func after_each() -> void:
	# The music service is an autoload; leave no context behind for the next test.
	MusicService.stop_context()
	_state_config.restore()
	# The live binding model is a static shared with the whole run: restore the
	# catalog defaults and the on-disk cfg even when a remap test fails early.
	ControlsBindings.model().restore_defaults()
	_controls_cfg.restore()


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
