extends GutTest

# Runtime menu shell (MenuShell) gates: it boots the JO menu set, services the
# shell policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuShellScript := preload("res://game/nova_menu_shell.gd")

const MAIN_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "res://../fixtures/mnu/jo_loadout.mnu"  # the cross-.mnu target
const OPTIONS_FIXTURE := "res://../fixtures/mnu/jo_options.mnu"  # has the Mods tab (AVAIL_LIST/MOD_DESC)
const SP_PLAY_FIXTURE := "res://../fixtures/mnu/jo_sp.mnu"  # play screen: mission list IA_LIST + ACCEPT
const MISSION_BIN_FIXTURE := "res://../fixtures/rtxt/00tra.bin"  # real per-mission bin: info/Title + briefing
const MUS_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"  # decrypted SCR0 MUS program
const SBF_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"  # real SBF bank (banks stream loose)


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


const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()


func after_each() -> void:
	# The music service is an autoload; leave no context behind for the next test.
	NovaMusicService.stop_context()
	if _had_state_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_state_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


# Build a throwaway resource dir holding main.mnu (+ a sp.mnu jump target and a
# stub mission), and a shell pointed at it. Returns null when a real temp root is
# unavailable in this environment (the caller pass_test-skips, as mnu_menu_test does).
func _make_shell(dir: String):
	var root := ResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	var shell = MenuShellScript.new()
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


func _copy(res_path: String, dst: String) -> void:
	var f := FileAccess.open(dst, FileAccess.WRITE)
	if f != null:
		f.store_buffer(FileAccess.get_file_as_bytes(res_path))
		f.close()


# bms::AttribFlags game-mode bits (engine/formats/mission/bms.h).
const BMS_ATTRIB_COOP := 0x1000000
const BMS_ATTRIB_TDM := 0x20000000


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


func _cleanup(dir: String) -> void:
	for f in ["main.mnu", "sp.mnu", "options.mnu", "test.bms"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


func test_boots_into_main_menu_startup() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	assert_eq(shell.get_current_menu_file(), "main.mnu", "main menu opened on setup")
	var driver: MenuDriver = shell.get_driver()
	assert_not_null(driver, "the interaction driver is built")
	assert_not_null(shell.get_frame(), "the compiled frame surface is built")
	assert_eq(driver.get_current_screen(), "STARTUP", "STARTUP screen shown")
	_cleanup(dir)


# The shell seeds the five named Options sliders with the exact original
# ranges/pages. Until OpenNova owns persisted render/audio/input settings, the
# current value is deterministically clamped to each range minimum.
# [orig: options_screen_init @ 0x554800;
# UI_PopulateRenderAndAudioSettings @ 0x55c830]
func test_options_scrolls_seed_original_ranges() -> void:
	var dir := _make_dir()
	_copy(OPTIONS_FIXTURE, dir.path_join("options.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	assert_true(shell.open_menu("options.mnu", ""), "Options fixture opens")
	var driver: MenuDriver = shell.get_driver()
	var expected := [
		["GAMMA", 5, 20, 2, 5],
		["SOUNDFXVOLUME", 0, 255, 10, 0],
		["DIALOGVOLUME", 0, 255, 10, 0],
		["MUSICVOLUME", 0, 255, 10, 0],
		["MOUSE_SENSITIVITY", 4, 511, 10, 4],
	]
	for row in expected:
		var control_name := String(row[0])
		var id := driver.widget_id(control_name)
		assert_gte(id, 0, "%s exists" % control_name)
		var scroll = driver.get_widget_scroll_range(id)
		assert_not_null(scroll, "%s receives scroll state" % control_name)
		assert_eq([scroll.minimum, scroll.maximum, scroll.page, scroll.value],
				row.slice(1),
				"%s receives its original range/page and min fallback" % control_name)
	_cleanup(dir)


func test_startup_drives_music_var() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	# jo_main STARTUP declares MUSICVAR 1; the menu pushes it into the menumus
	# discriminator var (MusicDirector.MENU_MUSIC_VAR_SLOT — the witness lives
	# at the engine home, audio/music_policy.h kMenuMusicVarSlot); at index 0
	# the MUSICVAR was inert and the menu played the wrong section.
	var idx: int = MusicDirector.MENU_MUSIC_VAR_SLOT
	assert_eq(shell.get_music_director().get_var(idx), 1, "STARTUP MUSICVAR -> director var %d" % idx)
	_cleanup(dir)


func test_cross_mnu_jump_and_back_stack() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
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
		pass_test("temp resource root unavailable")
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
		pass_test("temp resource root unavailable")
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
		pass_test("temp resource root unavailable")
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
		pass_test("temp resource root unavailable")
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
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	# A mission-list selection relayed through the driver's aggregate value
	# signal (the seam contract), then a start control press.
	shell.get_driver().widget_value_changed.emit("MISSION_LIST", "list", 0, "test.bms")
	assert_eq(shell.get_selected_mission(), "test.bms", "selection tracked from the list relay")
	shell._on_start_control()
	assert_signal_emitted_with_parameters(shell, "start_requested", ["test.bms"])
	_cleanup(dir)


func test_start_without_selection_falls_back_to_first_mission() -> void:
	var dir := _make_dir()
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(shell)
	shell._on_start_control()  # no selection -> first .bms in the dir (test.bms)
	assert_signal_emitted_with_parameters(shell, "start_requested", ["test.bms"])
	_cleanup(dir)


func test_crosshair_spinlist_seeds_persists_and_notifies() -> void:
	var saved := ResourceDirSettings.get_crosshair_style()
	ResourceDirSettings.set_crosshair_style(11)
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir)
	if shell == null:
		pass_test("runtime resource root unavailable in this environment")
		ResourceDirSettings.set_crosshair_style(saved)
		_rm_runtime_dir(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var spin: int = driver.widget_id("XHAIR_APPEARANCE")
	assert_gte(spin, 0, "Options authors the crosshair spin list.")
	assert_eq(driver.widget_kind_of(spin), MnuDocument.TYPE_SPINLIST,
		"XHAIR_APPEARANCE is a spin list.")
	assert_eq(driver.selected_row(spin), 11,
		"The spin list starts on the persisted crosshair.")
	watch_signals(shell)
	driver.select_row(spin, 18)  # emits the "spinlist" value change
	assert_eq(ResourceDirSettings.get_crosshair_style(), 18, "Selection persists.")
	assert_signal_emitted_with_parameters(shell, "crosshair_style_changed", [18])
	shell.get_resource_root().clear()
	ResourceDirSettings.set_crosshair_style(saved)
	_rm_runtime_dir(dir)


# Options -> Mods: the shell lists discoverable expansions in AVAIL_LIST by name, and
# activating one mounts it over the base game, fills MOD_DESC, persists the choice
# (read back by main_game at the next launch), and announces it. Uses a runtime
# (packed PFF) mount so list_expansions/mount_runtime have real archives to work on.
func test_mods_tab_lists_mounts_and_persists_expansion() -> void:
	var saved := ResourceDirSettings.get_expansion()
	ResourceDirSettings.set_expansion("")  # clean slate so the activate is not a no-op
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir)
	if shell == null:
		pass_test("runtime resource root unavailable in this environment")
		ResourceDirSettings.set_expansion(saved)
		_rm_runtime_dir(dir)
		return
	assert_eq(NovaMusicService.current_context(), "menu", "base MENU music starts with the shell")
	assert_not_null(NovaMusicService.current_script(), "base MENUMUS.BIN resolves")
	if NovaMusicService.current_script() != null:
		assert_eq(NovaMusicService.current_script().get_source_path(), "menumus.bin")
	assert_eq(NovaMusicService.get_var(2), 9, "OPTIONS MUSICVAR drives Var2 before the swap")
	var driver: MenuDriver = shell.get_driver()
	var avail: int = driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0, "AVAIL_LIST authored")
	assert_eq(driver.item_count(avail), 1, "one expansion discovered under expansion/")
	assert_eq(driver.item_text(avail, 0), "jox01")
	# Activation (list double-click) mounts + persists + describes.
	driver.list_activated.emit(avail, 0)
	assert_eq(shell.get_selected_expansion(), "jox01")
	assert_eq(ResourceDirSettings.get_expansion(), "jox01", "choice persisted to config")
	assert_not_null(NovaMusicService.current_script(), "expansion menu context reopens")
	if NovaMusicService.current_script() != null:
		assert_eq(NovaMusicService.current_script().get_source_path(), "Mjox01.bin",
			"live expansion selection swaps to the M<exp> script")
	assert_eq(NovaMusicService.get_var(2), 9,
		"full expansion reload re-drives the active screen MUSICVAR")
	var desc: int = driver.widget_id("MOD_DESC")
	assert_gte(desc, 0, "MOD_DESC authored")
	assert_string_contains(driver.get_widget_text(desc), "Kendari",
		"friendly expansion name shown")
	# The expansion's packed asset is now reachable through the live root.
	assert_eq(shell.get_resource_root().read_file("expmodel.3di").get_string_from_utf8(),
		"exp model", "expansion archive mounted over the base game")
	shell.get_resource_root().clear()  # release PFF handles before deleting the temp archives
	ResourceDirSettings.set_expansion(saved)
	_rm_runtime_dir(dir)


# Options -> Mods OK (the ACCEPT button) must APPLY the highlighted expansion, not
# launch a mission. ACCEPT is overloaded across JO screens (launch on Single Player,
# plain OK on Options); the shell scopes it by screen role, so on a Mods screen (mod
# list, no mission list) ACCEPT applies. Regression for the "OK loads a mission" bug.
func test_mods_ok_applies_expansion_without_launching() -> void:
	var saved := ResourceDirSettings.get_expansion()
	ResourceDirSettings.set_expansion("")  # so the apply is not a no-op
	var dir := _make_runtime_dir()
	var shell = _make_runtime_shell(dir)
	if shell == null:
		pass_test("runtime resource root unavailable in this environment")
		ResourceDirSettings.set_expansion(saved)
		_rm_runtime_dir(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var avail: int = driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0, "AVAIL_LIST authored")
	driver.select_row(avail, 0, false)  # highlight jox01 (no double-click / activation)
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0, "options ACCEPT control authored")
	watch_signals(shell)
	driver.widget_activated.emit(accept, "ACCEPT")  # press OK
	assert_signal_not_emitted(shell, "start_requested", "OK on the Mods screen must not launch")
	assert_eq(shell.get_selected_expansion(), "jox01", "OK applied the highlighted mod")
	assert_eq(ResourceDirSettings.get_expansion(), "jox01", "applied choice persisted")
	shell.get_resource_root().clear()
	ResourceDirSettings.set_expansion(saved)
	_rm_runtime_dir(dir)


# A loose authoring root (the ONED --loose-root play-test mount, ADR 0025) has no
# packed archives to relayer: applying a discoverable expansion must refuse and
# leave the live loose mount untouched, not remount it through mount_runtime into
# a cleared root (the zero-archives fatal would kill the running play-test).
func test_mods_apply_refuses_on_a_loose_root_and_keeps_the_mount() -> void:
	var saved := ResourceDirSettings.get_expansion()
	ResourceDirSettings.set_expansion("")
	var dir := OS.get_temp_dir().path_join("menu_shell_loose_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	var file := FileAccess.open(dir.path_join("options.mnu"), FileAccess.WRITE)
	assert_not_null(file)
	file.store_buffer(FileAccess.get_file_as_bytes(OPTIONS_FIXTURE))
	file.close()
	file = FileAccess.open(dir.path_join("menumus.bin"), FileAccess.WRITE)
	assert_not_null(file)
	file.store_buffer(FileAccess.get_file_as_bytes(MUS_FIXTURE))
	file.close()
	_copy(SBF_FIXTURE, dir.path_join("menumus.sbf"))
	# The expansion pair exists ON DISK (list_expansions scans the path), but the
	# mounted root is the editor's loose mount, which cannot layer it.
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
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
	assert_eq(driver.item_count(avail), 1, "the packed expansion is still discoverable on disk")
	driver.select_row(avail, 0, false)
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0)
	driver.widget_activated.emit(accept, "ACCEPT")
	assert_eq(shell.get_selected_expansion(), "", "the loose mount refuses the switch")
	assert_eq(ResourceDirSettings.get_expansion(), "", "nothing persisted")
	assert_false(root.read_file("options.mnu").is_empty(),
			"the live loose mount survives untouched (no clear())")
	root.clear()
	ResourceDirSettings.set_expansion(saved)
	for sub in ["options.mnu", "menumus.bin", "menumus.sbf",
			"expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# D-MNU-14: the SP mission list rides the catalog — Co-op-family rows only,
# titled from the sibling .bin (the header's embedded name when no .bin), the
# loose "*" marker, the briefing pane cleared on populate and filled on
# selection, and ACCEPT gated on a pick
# [orig: SinglePlayer_PopulateMissionList @ 0x561840 +
# SinglePlayer_MissionListEventHandler @ 0x561ed0].
func test_sp_mission_list_titles_briefing_and_accept_gate() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_sp_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(SP_PLAY_FIXTURE, dir.path_join("main.mnu"))
	_write_bms(dir.path_join("alpha.bms"), "Alpha Header", BMS_ATTRIB_COOP)
	_copy(MISSION_BIN_FIXTURE, dir.path_join("alpha.bin"))
	_write_bms(dir.path_join("bravo.bms"), "Bravo Header", BMS_ATTRIB_TDM)
	_write_bms(dir.path_join("charlie.bms"), "Charlie Header", BMS_ATTRIB_COOP)
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp root unavailable in this environment")
		return
	var driver: MenuDriver = shell.get_driver()
	var list_id := driver.widget_id("IA_LIST")
	assert_gt(list_id, -1, "jo_sp authors the IA_LIST mission list")
	assert_eq(driver.item_count(list_id), 2, "the TDM mission is filtered off the SP list")
	assert_eq(driver.item_text(list_id, 0), "*Training: Basic Controls / Armory",
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
	_copy(SP_PLAY_FIXTURE, dir.path_join("main.mnu"))  # jo_sp as the opened menu
	var f := FileAccess.open(dir.path_join("alpha.bms"), FileAccess.WRITE)
	if f != null:
		f.store_buffer(PackedByteArray([0]))
		f.close()
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir.path_join("alpha.bms"))
		DirAccess.remove_absolute(dir)
		return
	var driver: MenuDriver = shell.get_driver()
	var accept: int = driver.widget_id("ACCEPT")
	assert_gte(accept, 0, "SP ACCEPT control authored")
	watch_signals(shell)
	driver.widget_activated.emit(accept, "ACCEPT")  # no explicit pick -> first .bms
	# ACCEPT on a mission-list screen still launches (first .bms, none selected).
	assert_signal_emitted_with_parameters(shell, "start_requested", ["alpha.bms"])
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
	var mns := "// test stylesheet\nDEF_FONTNAME_LG Gunpl27b.fnt\nDEF_TEXT_FG FFFFFFFF\n" \
		+ "DEF_TEXT_MOUSEOVER_FG FFFF0000\nDEF_TEXT_SELECTED_FG FFFF0000\nDEF_TEXT_DISABLED_FG FF545252\n"
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
		{"name": "menu_style.mns", "bytes": mns},
	])
	var root := ResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		pass_test("runtime resource root unavailable in this environment")
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
# loose from disk. The ONE music context lives on the NovaMusicService autoload
# (the original streams one AudioVM context at a time): the shell opens the MENU
# context on setup, the world opens the GAME context at mission start. These pin
# the resolution order, the byte-path loading, and the context-swap semantics.

# Both contexts load from a packed archive (scripts by their hardcoded names
# through the VFS byte path) + loose real banks; opening the game context is a
# full context reload [orig: AudioVM_OpenMusicContext @ 0x6722a0] and seeds the
# witnessed mission-start vars [orig: Game_StartMission @ 0x5255b3-0x52561b].
func test_music_contexts_load_pff_archived_by_hardcoded_names() -> void:
	var mus := FileAccess.get_file_as_bytes(MUS_FIXTURE)
	var sbf := FileAccess.get_file_as_bytes(SBF_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_mus_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
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
		pass_test("runtime resource root unavailable in this environment")
		_rm_music_ctx_dir(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(NovaMusicService.current_context(), "menu", "setup opens the MENU music context")
	if NovaMusicService.current_script() != null:
		assert_eq(NovaMusicService.current_script().get_source_path(), "menumus.bin",
			"menumus.bin loaded from the PFF by hardcoded name")
	NovaMusicService.set_var(14, 77)
	# Mission start = a full context reload onto the GAME pair + the witnessed seed.
	assert_true(NovaMusicService.open_game_context(root), "game context opens")
	assert_eq(NovaMusicService.current_context(), "game", "the one context swapped to GAME")
	if NovaMusicService.current_script() != null:
		assert_eq(NovaMusicService.current_script().get_source_path(), "gamemus.bin",
			"gamemus.bin loaded from the PFF by hardcoded name")
	assert_eq(NovaMusicService.get_var(1), 0, "Var1 seeded 0 (never written in retail)")
	assert_eq(NovaMusicService.get_var(7), 100, "Var7 seeded 100 (full health %)")
	assert_eq(NovaMusicService.get_var(2), 0, "Var2 seeded 0")
	assert_eq(NovaMusicService.get_var(14), 0, "full context reload clears unseeded globals")
	NovaMusicService.set_var(14, 88)
	assert_true(NovaMusicService.open_menu_context(root), "menu context reopens")
	assert_eq(NovaMusicService.get_var(14), 0,
		"menu reload also clears globals under the headless audio driver")
	# A failed replacement open tears down the old pair and obeys the witnessed
	# bank-first gate: retail never attempts to read the script after no .sbf.
	var missing_root := _MissingBankMusicRoot.new()
	assert_false(NovaMusicService.open_menu_context(missing_root), "missing bank leaves silence")
	assert_eq(missing_root.script_reads, 0, "missing-bank gate precedes VFS script read")
	assert_null(NovaMusicService.director().get_bank(), "failed open retains no old bank")
	assert_null(NovaMusicService.director().get_mus_script(), "failed open retains no old script")
	root.clear()
	_rm_music_ctx_dir(dir)


func _rm_music_ctx_dir(dir: String) -> void:
	for sub in ["resource.pff", "menumus.sbf", "gamemus.sbf"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# With an expansion mounted, retail selects M<n>/G<n> unconditionally. A
# missing half therefore leaves that context silent; it never reselects the base
# pair [orig: Expansion_LoadAssets @ 0x4a4767-75; AudioVM_OpenContextFile
# @ 0x672160].
func test_music_resolution_keeps_incomplete_expansion_pair() -> void:
	var mus := FileAccess.get_file_as_bytes(MUS_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musx_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "Mjox01.bin", "bytes": mus},
		{"name": "Gjox01.bin", "bytes": mus},
	])
	# A real loose expansion bank for the M stem (complete pair); the G stem
	# ships no bank (incomplete).
	var sbf := FileAccess.get_file_as_bytes(SBF_FIXTURE)
	var stub := FileAccess.open(dir.path_join("expansion/jox01/Mjox01.sbf"), FileAccess.WRITE)
	if stub != null:
		stub.store_buffer(sbf)
		stub.close()
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pass_test("runtime resource root unavailable in this environment")
		_rm_music_exp_dir(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(NovaMusicService.current_context(), "menu", "menu context opened")
	if NovaMusicService.current_script() != null:
		assert_eq(NovaMusicService.current_script().get_source_path(), "Mjox01.bin",
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
	_rm_music_exp_dir(dir)


# The converse incomplete pair also keeps the expansion stem. The bank opens,
# then the missing VFS script makes the context silent.
func test_music_incomplete_expansion_bank_only_stays_expansion() -> void:
	var mus := FileAccess.get_file_as_bytes(MUS_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musk_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	for stub_name in ["expansion/jox01/Gjox01.sbf", "gamemus.sbf"]:
		var stub := FileAccess.open(dir.path_join(stub_name), FileAccess.WRITE)
		if stub != null:
			stub.store_buffer(PackedByteArray([0]))
			stub.close()
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pass_test("runtime resource root unavailable in this environment")
		_rm_music_bank_only_dir(dir)
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
	_rm_music_bank_only_dir(dir)


func _rm_music_bank_only_dir(dir: String) -> void:
	for sub in ["resource.pff", "expansion/jox01/jox01.pff", "expansion/jox01/Gjox01.sbf",
			"gamemus.sbf", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# A mounted expansion with no music is silent even when the base pair exists.
func test_musicless_expansion_does_not_reselect_base_pair() -> void:
	var mus := FileAccess.get_file_as_bytes(MUS_FIXTURE)
	var dir := OS.get_temp_dir().path_join("menu_shell_musb_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
		{"name": "menumus.bin", "bytes": mus},
		{"name": "gamemus.bin", "bytes": mus},
	])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	var stub := FileAccess.open(dir.path_join("menumus.sbf"), FileAccess.WRITE)
	if stub != null:
		stub.store_buffer(FileAccess.get_file_as_bytes(SBF_FIXTURE))
		stub.close()
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "jox01") != OK:
		pass_test("runtime resource root unavailable in this environment")
		_rm_music_base_dir(dir)
		return
	var shell = MenuShellScript.new()
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	assert_eq(NovaMusicService.current_context(), "",
		"musicless mounted expansion leaves the menu context silent")
	assert_null(NovaMusicService.current_script(),
		"musicless mounted expansion does not load MENUMUS.BIN")
	var pair: MusicPair = shell.resolve_menu_music_pair()
	assert_true(String(pair.bank).ends_with("Mjox01.sbf"),
		"musicless mounted expansion keeps the missing expansion bank path")
	assert_eq(String(pair.script_name), "Mjox01.bin",
		"musicless mounted expansion keeps the missing expansion script name")
	root.clear()
	_rm_music_base_dir(dir)


func _rm_music_exp_dir(dir: String) -> void:
	for sub in ["resource.pff", "expansion/jox01/jox01.pff", "expansion/jox01/Mjox01.sbf",
			"expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


func _rm_music_base_dir(dir: String) -> void:
	for sub in ["resource.pff", "menumus.sbf", "expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


func test_missing_assets_degrade_without_crashing() -> void:
	# A dir with only main.mnu (no stylesheet / sound / music / textures): the shell
	# still builds the menu and runs; the menu just reports unresolved assets.
	var dir := OS.get_temp_dir().path_join("menu_shell_bare_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	var shell = _make_shell(dir)
	if shell == null:
		pass_test("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir)
		return
	assert_eq(shell.get_current_menu_file(), "main.mnu", "menu still opens with no companion assets")
	assert_not_null(shell.get_music_director(), "director created even without a music script")
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir)


# --- Runtime (packed PFF) fixtures for the expansion/mods test -----------------

# A base dir with options.mnu packed in a base archive (so it loads under a runtime
# mount) plus one discoverable expansion (expansion/jox01/jox01.pff carrying an asset).
# The archive must use a boot-table name (resource.pff): the runtime mounts only the
# witnessed fixed table [orig: PFF_OpenAllArchives @ 0x4a4310] (D-VFS-2).
func _make_runtime_dir() -> String:
	var dir := OS.get_temp_dir().path_join("menu_shell_mods_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_write_pff(dir.path_join("resource.pff"), [
		{"name": "options.mnu", "bytes": FileAccess.get_file_as_bytes(OPTIONS_FIXTURE)},
		{"name": "menumus.bin", "bytes": FileAccess.get_file_as_bytes(MUS_FIXTURE)},
	])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
		{"name": "Mjox01.bin", "bytes": FileAccess.get_file_as_bytes(MUS_FIXTURE)},
	])
	_copy(SBF_FIXTURE, dir.path_join("menumus.sbf"))
	# Deliberately use retail-style uppercase to pin case-insensitive resolution
	# on Linux/macOS while preserving the actual shell path.
	_copy(SBF_FIXTURE, dir.path_join("expansion/jox01/MJOX01.SBF"))
	return dir


func _make_runtime_shell(dir: String):
	var root := ResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		return null
	var shell = MenuShellScript.new()
	shell.main_menu_file = "options.mnu"  # open the menu that carries the Mods tab
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	shell.setup(root)
	return shell


func _rm_runtime_dir(dir: String) -> void:
	for sub in ["resource.pff", "menumus.sbf", "expansion/jox01/jox01.pff",
			"expansion/jox01/MJOX01.SBF", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# Minimal PFF3 writer (mirrors resource_root_contract_test._write_pff): 20-byte
# header, 36-byte entries with a 16-byte name field, then the payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "PFF fixture should be writable: %s" % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)  # "PFF3"
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes := _entry_bytes(entry)
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		var name_bytes := String(entry.name).to_utf8_buffer()
		for i in range(16):
			file.store_8(name_bytes[i] if i < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()
	for entry in entries:
		file.store_buffer(_entry_bytes(entry))
	file.close()


func _entry_bytes(entry: Dictionary) -> PackedByteArray:
	return entry.bytes if entry.bytes is PackedByteArray else String(entry.bytes).to_utf8_buffer()


# A throwaway companion: claims the menu (or not) and records whether it was driven.
class _FakeCompanion extends RefCounted:
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
		pass_test("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	assert_true(shell.has_method("add_companion"), "the shell exposes the multi-companion hook")
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
# same live model [orig: sub_55D560 @ 0x55d560; KeyBinding_HandleKeyAssignment
# @ 0x55bb20; CLEAR_KEY @ 0x55bfd0; DEFAULTS @ 0x55bd90].
func test_control_mapping_remap_flow() -> void:
	var had_cfg := FileAccess.file_exists(ControlsBindings.CONFIG_PATH)
	var saved_cfg := FileAccess.get_file_as_bytes(ControlsBindings.CONFIG_PATH) \
			if had_cfg else PackedByteArray()
	ControlsBindings.model().restore_defaults()

	var dir := OS.get_temp_dir().path_join("menu_shell_remap_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(dir.path_join("options.mnu"), FileAccess.WRITE)
	assert_not_null(file)
	file.store_buffer(FileAccess.get_file_as_bytes(OPTIONS_FIXTURE))
	file.close()
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

	# Double-click row 0: the capture arms and the Control cell clears.
	driver.list_activated.emit(table, 0)
	assert_eq(driver.table_cell_text(table, 0, 2), "",
			"the armed row's Control cell clears")

	# The next key assigns (Y replaces the primary: both slots were full).
	var key := InputEventKey.new()
	key.pressed = true
	key.physical_keycode = KEY_Y
	shell.get_viewport().push_input(key)
	assert_eq(driver.table_cell_text(table, 0, 2), "Y or Up",
			"the captured key lands in the record and the cell restores")
	assert_true(FileAccess.file_exists(ControlsBindings.CONFIG_PATH),
			"the edit persists")

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

	# The gameplay lookup follows the live records again.
	var keys: PackedInt32Array = ControlsBindings.model().godot_keys_for_token("move_forward")
	assert_eq(keys.size(), 2, "defaults restored for the sampler")

	ControlsBindings.model().restore_defaults()
	if had_cfg:
		var out := FileAccess.open(ControlsBindings.CONFIG_PATH, FileAccess.WRITE)
		if out != null:
			out.store_buffer(saved_cfg)
			out.close()
	elif FileAccess.file_exists(ControlsBindings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(ControlsBindings.CONFIG_PATH))
	DirAccess.remove_absolute(dir.path_join("options.mnu"))
	DirAccess.remove_absolute(dir)
