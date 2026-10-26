extends GutTest

## Authored menu shapes over synthetic boot data. No game-content checkout is needed.

const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
const STATE_CONFIG_PATH := "user://opennova.cfg"

var _config: TestFs.Snapshot
var _boot_dir := ""
var _dirs: Array[String] = []
var _shell: MainGame = null


func before_each() -> void:
	# The test's own opennova.cfg: none of the developer's keys (an earlier build's
	# [resources] expansion) reaches it, so what it finds there is what it wrote.
	_config = TestFs.isolate(STATE_CONFIG_PATH)
	# Authored menu coverage uses an explicit test data directory. The runtime
	# no longer discovers source assets or a remembered retail directory.
	_boot_dir = _stage_authored_menu()
	LaunchFlags.set_args_override(PackedStringArray([
			"--no-update", "--resource-dir", _boot_dir, "--loose-root", "/d"]))
	Strings.clear()


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	LaunchFlags.clear_args_override()
	_config.restore()
	Strings.clear()


func _boot() -> MainGame:
	var shell := MAIN_GAME_SCENE.instantiate() as MainGame
	add_child(shell)
	await get_tree().process_frame
	return shell


func _project_dir() -> String:
	return ProjectSettings.globalize_path("res://../fixtures/game")


func _stage_authored_menu() -> String:
	var dir := WorldFixture.stage_minimal_root("authored_menu")
	_dirs.append(dir)
	for file_name in DirAccess.get_files_at(_project_dir()):
		if file_name.get_extension().to_lower() in ["mnu", "mns", "bin"]:
			assert_eq(DirAccess.copy_absolute(_project_dir().path_join(file_name),
					dir.path_join(file_name)), OK)
	# Menu geometry and navigation are real; images and font glyphs are the
	# small synthetic fixtures used throughout the engine suite.
	for file_name in ["onmnu19.fnt", "onmnub23.fnt", "onttl40.fnt"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/fnt/synth_1page.fnt"), dir.path_join(file_name)), OK)
	assert_eq(DirAccess.copy_absolute(dir.path_join("newarow1.tga"),
			dir.path_join("onmnbg.tga")), OK)
	return dir


func _mount_fixture(dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, LaunchFlags.expansion(), false, "jo") == OK:
		return root
	return root if root.set_root_dir(dir) == OK else null


func _press(widget_name: String) -> void:
	var driver := _shell.get_menu_shell().get_driver()
	var id := driver.widget_id(widget_name)
	assert_true(id >= 0, "%s is on the current screen" % widget_name)
	driver.activate(id)


func _same_dir(a: String, b: String) -> bool:
	return a.replace("\\", "/").rstrip("/").to_lower() == b.replace("\\", "/").rstrip("/").to_lower()


func test_explicit_authored_data_boots_its_menu() -> void:
	_shell = await _boot()
	var root := _shell.current_resource_root()
	assert_not_null(root, "the staged menu fixtures mounted")
	if root == null:
		return
	assert_true(_same_dir(root.get_root_dir(), _boot_dir))
	var menu := _shell.get_menu_shell()
	assert_eq(menu.get_current_menu_file().to_lower(), "main.mnu")
	var driver := menu.get_driver()
	assert_eq(driver.get_current_screen(), "STARTUP")
	# The original main menu's buttons by its names (SINGLE_PLAYER, LAN_MULTI_PLAYER,
	# PLAYER_INFO, OPTIONS, EXIT), plus the updater button.
	for widget_name in ["SINGLE_PLAYER", "LAN_MULTI_PLAYER", "PLAYER_INFO", "OPTIONS",
			"UPDATE", "EXIT"]:
		assert_true(driver.has_widget(widget_name), "%s is authored" % widget_name)
	var frame := menu.get_frame()
	# One mouse sample resolves the screen's claimed pointer.
	driver.process_mouse(Vector2(400, 300), false)
	assert_not_null(frame.get_cursor_texture(),
			"MAIN's pointer newarow1.tga decodes (a plain git blob, never an LFS pointer)")
	assert_eq(driver.get_applied_pointer(), frame.get_cursor_texture(),
			"the claim's CURSOR is the OS cursor")
	assert_eq(frame.get_unresolved_asset_count(), 0, "every asset the menu names resolves")
	assert_gt(frame.get_draw_list_stats().glyphs, 0, "the synthetic font draws the menu text")


# The main menu's buttons reach their screens by their authored actions, as the
# original's do: the Mods list is a tab of OPTIONS (options.mnu), beside the
# options' own tabs and the credits, and BACK returns to STARTUP.
func test_the_authored_main_menu_reaches_its_screens() -> void:
	_shell = await _boot()
	var menu := _shell.get_menu_shell()
	var driver := menu.get_driver()
	for target in [["SINGLE_PLAYER", "sp.mnu", "SINGLE_PLAYER", ["IA_LIST", "BRIEFING", "ACCEPT"]],
			["LAN_MULTI_PLAYER", "mp.mnu", "LAN_MULTI_PLAYER", ["LAN_GAME_LIST", "LAN_SEARCH",
				"LAN_JOINGAME", "LAN_HOSTGAME", "SELECTED_MISSIONS", "START_GAME"]],
			["PLAYER_INFO", "player.mnu", "PLAYER_INFO", ["PLAYERNAME", "PLAYERCLASS",
				"NATIONALITY", "COMBO_LIST", "ACCEPT"]],
			["OPTIONS", "options.mnu", "OPTIONS", ["RADIO_TAB_MODS", "AVAIL_LIST", "MOD_DESC",
				"RADIO_TAB_CREDITS", "CREDITS", "CONTROL_MAPPING", "ACCEPT", "BACK"]]]:
		_press(target[0])
		await get_tree().process_frame
		assert_eq(menu.get_current_menu_file().to_lower(), target[1], "%s opens %s" % [target[0], target[1]])
		assert_eq(driver.get_current_screen(), target[2])
		for widget_name in target[3]:
			assert_true(driver.has_widget(widget_name), "%s holds %s" % [target[2], widget_name])
		driver.pop_screen()
		await get_tree().process_frame
		assert_eq(menu.get_current_menu_file().to_lower(), "main.mnu", "back from %s" % target[2])
		assert_eq(driver.get_current_screen(), "STARTUP")


func _temp_dir(label: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova_bundled_%s_%d" % [label, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	return dir


# Runtime selection never discovers an authoring checkout or a previous game.
func test_launch_directory_is_explicit_else_the_executable_directory() -> void:
	var project := _temp_dir("explicit")
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", project]))
	assert_true(_same_dir(BootRootMount.launch_directory(), project))
	LaunchFlags.set_args_override(PackedStringArray([]))
	assert_true(_same_dir(BootRootMount.launch_directory(), OS.get_executable_path().get_base_dir()))


# The Build's boot-table archives mount as an install's do; a folder with none
# (an unbuilt project) as a plain loose root.
func test_the_staged_game_mounts_a_build_packed_and_a_folder_loose() -> void:
	var build := _temp_dir("build")
	WorldFixture.stage_shell_archives(self, build, false)
	var packed := _mount_fixture(build)
	assert_true(packed != null and packed.is_runtime_mount(), "the build's archives mount")
	var folder := _temp_dir("folder")
	TestFs.write_text(self, folder.path_join("main.mnu"), "<SCREEN><NAME>STARTUP</NAME></SCREEN>\n")
	var loose := _mount_fixture(folder)
	assert_true(loose != null and not loose.is_runtime_mount(), "a folder of loose files mounts loose")
	if loose != null:
		assert_true(loose.has_file("main.mnu"), "its files are found by name")


# An expansion shipped beside the bundled game (expansion/<name>/, as the game
# zip ships one) mounts over it with /exp or /mod, its archive's files standing
# over the base's; one the folder lacks falls back to the base game, as the
# original's mount does. A Mods list's pick never reaches a launch: nothing
# keeps it past its run (D-MNU-31), not even a key an earlier build wrote.
func test_the_staged_game_mounts_an_expansion_beside_it() -> void:
	var build := _temp_dir("expansion")
	WorldFixture.stage_shell_archives(self, build, false)
	var folder := build.path_join("expansion/onx")
	assert_eq(DirAccess.make_dir_recursive_absolute(folder), OK)
	WorldFixture.write_pff(self, folder.path_join("onx.pff"),
			[{"name": "onxonly.txt", "bytes": "the expansion's".to_utf8_buffer()}])
	WorldFixture.write_pff(self, folder.path_join("onxL.pff"), [])
	var base := _mount_fixture(build)
	assert_true(base != null and base.get_expansion().is_empty(), "no /exp: the base game")
	if base != null:
		assert_false(base.has_file("onxonly.txt"))
	for flag in ["/exp", "/mod"]:
		LaunchFlags.set_args_override(PackedStringArray([flag, "onx"]))
		var mounted := _mount_fixture(build)
		assert_true(mounted != null and mounted.get_expansion() == "onx", "%s onx mounts the expansion" % flag)
		if mounted != null:
			assert_true(mounted.has_file("onxonly.txt"), "its archive's files are served")
	LaunchFlags.set_args_override(PackedStringArray([]))
	ConfigStore.write("user://opennova.cfg", "resources", "expansion", "onx")
	var stale := _mount_fixture(build)
	assert_true(stale != null and stale.get_expansion().is_empty(),
			"a remembered pick mounts nothing: no /exp, the base game")
	LaunchFlags.set_args_override(PackedStringArray(["/exp", "absent"]))
	var fallback := _mount_fixture(build)
	assert_true(fallback != null and fallback.is_runtime_mount() and fallback.get_expansion().is_empty(),
			"an expansion the folder lacks: the base game")


# The base game's Mods screen (options.mnu) over a build with expansions beside
# it, as the game zip ships one (D-MNU-31): the base game's row first (the key
# the original game's code looks up, its text where no table names it), then a
# row per folder under expansion/ in the folder's order (upper-cased names; a
# name starting with '.' skipped, a folder with no <n>.pff listed and unnamed),
# each by its <n>.bin's EXP_NAME. A pick describes itself; ACCEPT switches the
# live root for the run and the menu boots anew over it, the pick kept nowhere.
func test_the_authored_mods_screen_lists_the_base_game_and_its_expansions() -> void:
	var build := _temp_dir("mods")
	var resources := WorldFixture.shell_archive_entries(self, WorldFixture.SHELL_RESOURCE_FILES)
	resources.append({"name": "options.mnu", "bytes": FileAccess.get_file_as_bytes(
			_project_dir().path_join("options.mnu"))})
	WorldFixture.stage_shell_archives(self, build, false)
	WorldFixture.write_pff(self, build.path_join("resource.pff"), resources)
	var storm := build.path_join("expansion/onx")
	assert_eq(DirAccess.make_dir_recursive_absolute(storm), OK)
	WorldFixture.write_pff(self, storm.path_join("onx.pff"),
			[{"name": "onxonly.txt", "bytes": "the expansion's".to_utf8_buffer()}])
	WorldFixture.write_pff(self, storm.path_join("onxL.pff"),
			[{"name": "onx.bin", "bytes": _expansion_info_bin("Test Storm", "A storm.")}])
	# The loose copy the game ships beside the archives: the text-override table's only source.
	TestFs.write_bytes(self, storm.path_join("onx.bin"), _expansion_info_bin("Test Storm", "A storm."))
	assert_eq(DirAccess.make_dir_recursive_absolute(build.path_join("expansion/aab")), OK)
	assert_eq(DirAccess.make_dir_recursive_absolute(build.path_join("expansion/.hidden")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(build, "", false, "jo"), OK)
	var shell := MenuShell.new()
	shell.main_menu_file = "options.mnu"
	shell.size = Vector2(800, 600)
	add_child_autofree(shell)
	assert_true(shell.setup(root), "the build serves the Mods screen")
	var driver := shell.get_driver()
	var avail := driver.widget_id("AVAIL_LIST")
	assert_gte(avail, 0, "the Mods screen's list")
	assert_eq(driver.get_widget_items(avail), PackedStringArray(
			["Joint Operations: Typhoon Rising", "Unnamed Expansion", "Test Storm"]))
	assert_eq(driver.selected_row(avail), 0, "the base game running is highlighted")
	# With the base game running the populate describes no row, so MOD_DESC keeps the
	# screen's own text: the base game's description is options.mnu's.
	assert_true(driver.get_widget_text(driver.widget_id("MOD_DESC")).begins_with("OpenNova's own"),
			"the base game is described by the text the screen authors")
	# The Mods list is a tab of the whole options screen, as the original's is: one
	# ACCEPT writes the options' words and switches the game only when the pick is
	# another than the one running (Options_HandleAcceptOrBack's two legs).
	var profile := PlayerProfile.store()
	var invert_before: int = profile.get_word("invert_mouse")
	assert_true(profile.set_word("invert_mouse", 0))
	var invert := driver.widget_id("INVERT_MOUSE")
	assert_gte(invert, 0, "the options' controls tab is on the Mods screen")
	driver.set_widget_checked(invert, true)
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	shell.update_menu_frame()
	assert_eq(profile.get_word("invert_mouse"), 1, "ACCEPT wrote the options' words")
	assert_eq(String(root.get_expansion()), "", "the game running picked: no switch")
	assert_false(shell.has_pending_expansion_reload(), "nothing waits to reload")
	assert_true(profile.set_word("invert_mouse", invert_before))
	var desc := driver.widget_id("MOD_DESC")
	driver.select_row(avail, 2, true)
	assert_eq(driver.get_widget_text(desc), "A storm.", "a pick shows its EXP_DESC")
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	shell.update_menu_frame()
	assert_eq(String(root.get_expansion()), "onx", "ACCEPT switched the live root")
	assert_true(root.has_file("onxonly.txt"), "the expansion's archive is served")
	assert_false(root.get_expansion_override_table().is_empty(),
			"the reload closed every archive first: the loose <n>.bin is the override table, as at boot")
	assert_eq(driver.selected_row(driver.widget_id("AVAIL_LIST")), 2,
			"the menu booted anew highlights the expansion running")
	var config := ConfigFile.new()
	assert_false(config.load(STATE_CONFIG_PATH) == OK
			and config.has_section_key("resources", "expansion"),
			"the pick is kept nowhere: it lasts the run")
	root.clear()


func _expansion_info_bin(exp_name: String, exp_desc: String) -> PackedByteArray:
	var table := RtxtStringFile.new()
	var section := table.add_section("exp_info")
	table.add_entry("EXP_NAME", exp_name, section, Vector2i.ZERO)
	table.add_entry("EXP_DESC", exp_desc, section, Vector2i.ZERO)
	return table.to_byte_array()
