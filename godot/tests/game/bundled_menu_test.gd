extends GutTest

## The no-argument boot (ADR 0048): OpenNova's bundled game and its main menu,
## and its PLAY RETAIL hand-over to a retail install.

const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _config: TestFs.Snapshot
var _retail_dir := ""
var _dirs: Array[String] = []
var _shell: MainGame = null


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	# No launch flags: the shell boots the bundled menu.
	LaunchFlags.set_args_override(PackedStringArray([]))
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game("jo")
	ResourceDirSettings.set_retail_dir("")
	Strings.clear()


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _retail_dir.is_empty():
		TestFs.remove_dir_recursive(_retail_dir)
		_retail_dir = ""
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


# A packed retail-shaped install (the three boot-table archives).
func _stage_retail() -> String:
	var dir := OS.get_cache_dir().path_join(
			"opennova_bundled_menu_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	WorldFixture.stage_shell_archives(self, dir, false)
	return dir


func _press(widget_name: String) -> void:
	var driver := _shell.get_menu_shell().get_driver()
	var id := driver.widget_id(widget_name)
	assert_true(id >= 0, "%s is on the current screen" % widget_name)
	driver.activate(id)


func _same_dir(a: String, b: String) -> bool:
	return a.replace("\\", "/").rstrip("/").to_lower() == b.replace("\\", "/").rstrip("/").to_lower()


func test_no_resource_dir_boots_the_bundled_placeholder_menu() -> void:
	_shell = await _boot()
	var root := _shell.current_resource_root()
	assert_not_null(root, "the bundled assets/ mounted")
	if root == null:
		return
	assert_true(_same_dir(root.get_root_dir(), BootRootMount.bundled_assets_dir()))
	var menu := _shell.get_menu_shell()
	assert_eq(menu.get_current_menu_file().to_lower(), "main.mnu")
	var driver := menu.get_driver()
	for widget_name in ["PLAY_RETAIL", "CHANGE_FOLDER", "EXIT"]:
		assert_true(driver.has_widget(widget_name), "%s is authored" % widget_name)
	var frame := menu.get_frame()
	# One mouse sample resolves the screen's claimed pointer.
	driver.process_mouse(Vector2(400, 300), false)
	assert_not_null(frame.get_cursor_texture(),
			"MAIN's pointer newarow1.tga decodes (a plain git blob, never an LFS pointer)")
	assert_eq(frame.get_unresolved_asset_count(), 0, "every asset the menu names resolves")
	assert_gt(frame.get_draw_list_stats().glyphs, 0, "the bundled font draws the menu text")


func _temp_dir(label: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova_bundled_%s_%d" % [label, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	return dir


# ADR 0048 d8: a source run plays the base game project's export, its export
# folder once an export of it is there; never exported, the project folder.
func test_a_project_plays_its_export_once_there_is_one() -> void:
	var project := _temp_dir("project")
	assert_true(_same_dir(BootRootMount.project_game_dir(project), project), "never exported: the project")
	var exported := project.path_join(BootRootMount.PROJECT_EXPORT_DIR)
	assert_eq(DirAccess.make_dir_recursive_absolute(exported), OK)
	assert_true(_same_dir(BootRootMount.project_game_dir(project), project),
			"an export folder holding no export is none")
	WorldFixture.stage_shell_archives(self, exported, false)
	assert_true(_same_dir(BootRootMount.project_game_dir(project), exported), "its export")


# The Build's boot-table archives mount as an install's do; a folder with none
# (an unbuilt project, the web build's staged files) as a plain loose root.
func test_the_bundled_game_mounts_a_build_packed_and_a_folder_loose() -> void:
	var build := _temp_dir("build")
	WorldFixture.stage_shell_archives(self, build, false)
	var packed := BootRootMount.mount_bundled(build)
	assert_true(packed != null and packed.is_runtime_mount(), "the build's archives mount")
	var folder := _temp_dir("folder")
	TestFs.write_text(self, folder.path_join("main.mnu"), "<SCREEN><NAME>STARTUP</NAME></SCREEN>\n")
	var loose := BootRootMount.mount_bundled(folder)
	assert_true(loose != null and not loose.is_runtime_mount(), "a folder of loose files mounts loose")
	if loose != null:
		assert_true(loose.has_file("main.mnu"), "its files are found by name")


# An expansion shipped beside the bundled game (expansion/<name>/, as the game
# zip ships one) mounts over it with /exp or /mod, or with the Mods list's
# pick, its archive's files standing over the base's; one the folder lacks
# falls back to the base game, as the original's mount does.
func test_the_bundled_game_mounts_an_expansion_beside_it() -> void:
	var build := _temp_dir("expansion")
	WorldFixture.stage_shell_archives(self, build, false)
	var folder := build.path_join("expansion/onx")
	assert_eq(DirAccess.make_dir_recursive_absolute(folder), OK)
	WorldFixture.write_pff(self, folder.path_join("onx.pff"),
			[{"name": "onxonly.txt", "bytes": "the expansion's".to_utf8_buffer()}])
	WorldFixture.write_pff(self, folder.path_join("onxL.pff"), [])
	var base := BootRootMount.mount_bundled(build)
	assert_true(base != null and base.get_expansion().is_empty(), "no /exp: the base game")
	if base != null:
		assert_false(base.has_file("onxonly.txt"))
	for flag in ["/exp", "/mod"]:
		LaunchFlags.set_args_override(PackedStringArray([flag, "onx"]))
		var mounted := BootRootMount.mount_bundled(build)
		assert_true(mounted != null and mounted.get_expansion() == "onx", "%s onx mounts the expansion" % flag)
		if mounted != null:
			assert_true(mounted.has_file("onxonly.txt"), "its archive's files are served")
	LaunchFlags.set_args_override(PackedStringArray([]))
	ResourceDirSettings.set_expansion("onx")
	var picked := BootRootMount.mount_bundled(build)
	assert_true(picked != null and picked.get_expansion() == "onx", "the Mods list's pick mounts it too")
	LaunchFlags.set_args_override(PackedStringArray(["/exp", "absent"]))
	var fallback := BootRootMount.mount_bundled(build)
	assert_true(fallback != null and fallback.is_runtime_mount() and fallback.get_expansion().is_empty(),
			"an expansion the folder lacks: the base game")


# EXIT is a named control, as retail's is (retail's ACTION has no quit verb): the
# companion that owns the bundled menu relays it for the shell to quit.
func test_exit_is_relayed_by_name() -> void:
	var text := FileAccess.get_file_as_string(
			BootRootMount.bundled_project_dir().path_join("main.mnu"))
	var driver := MenuDriverFixture.driver_over(self,
			MenuDriverFixture.doc_from_xml(self, text), "main.mnu")
	var companion := BundledMenuCompanion.new()
	assert_true(companion.owns_menu(driver), "the companion claims the bundled menu")
	companion.on_menu_built(driver, "main.mnu", "", null)
	watch_signals(companion)
	driver.widget_activated.emit(driver.widget_id("EXIT"), "EXIT")
	assert_signal_emitted(companion, "exit_requested")
	assert_signal_not_emitted(companion, "play_retail_requested")


func test_play_retail_mounts_the_saved_install_and_opens_its_menu() -> void:
	_retail_dir = _stage_retail()
	ResourceDirSettings.set_retail_dir(_retail_dir)
	_shell = await _boot()
	_press("PLAY_RETAIL")
	await get_tree().process_frame
	var root := _shell.current_resource_root()
	assert_true(root != null and _same_dir(root.get_root_dir(), _retail_dir),
			"the session switched to the saved install")
	var menu := _shell.get_menu_shell()
	assert_eq(menu.get_resource_root(), root, "the menu reads the same root")
	assert_eq(menu.get_current_menu_file().to_lower(), "main.mnu")
	var driver := menu.get_driver()
	assert_true(driver.has_widget("LAN_MULTI_PLAYER"), "the install's own main menu is up")
	assert_false(driver.has_widget("PLAY_RETAIL"), "the bundled menu is gone")
	assert_not_null(Strings.get_table(Strings.TABLE_MENUTXT),
			"the install's menu strings replaced the bundled root's none")


func test_play_retail_without_a_saved_install_keeps_the_bundled_menu() -> void:
	_shell = await _boot()
	var bundled := _shell.current_resource_root()
	# Nothing saved: PLAY RETAIL asks for a folder, which a headless run skips.
	_press("PLAY_RETAIL")
	await get_tree().process_frame
	assert_eq(_shell.current_resource_root(), bundled)
	assert_true(_shell.get_menu_shell().get_driver().has_widget("PLAY_RETAIL"))


func test_a_saved_install_that_is_absent_is_kept_for_when_it_returns() -> void:
	_retail_dir = _stage_retail()
	ResourceDirSettings.set_retail_dir(_retail_dir)
	_shell = await _boot()
	var bundled := _shell.current_resource_root()
	TestFs.remove_dir_recursive(_retail_dir)
	assert_false(_shell.play_retail(), "nothing mounts: PLAY RETAIL falls back to the picker")
	assert_eq(_shell.current_resource_root(), bundled, "the bundled menu stays")
	assert_true(_same_dir(String(ConfigStore.read(ResourceDirSettings.CONFIG_PATH,
			ResourceDirSettings.SECTION, ResourceDirSettings.RETAIL_DIR_KEY, "")), _retail_dir),
			"an absent install (an offline drive) keeps its saved path")


func test_a_saved_install_that_no_longer_mounts_is_forgotten() -> void:
	_retail_dir = OS.get_cache_dir().path_join(
			"opennova_bundled_menu_test_%d_unmountable" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_retail_dir), OK)
	ResourceDirSettings.set_retail_dir(_retail_dir)
	_shell = await _boot()
	var bundled := _shell.current_resource_root()
	assert_false(_shell.play_retail(), "a folder with no game archives does not mount")
	assert_eq(_shell.current_resource_root(), bundled, "the bundled menu stays")
	assert_eq(String(ConfigStore.read(ResourceDirSettings.CONFIG_PATH, ResourceDirSettings.SECTION,
			ResourceDirSettings.RETAIL_DIR_KEY, "")), "",
			"a present install that no longer mounts is cleared from the persisted settings")


func test_a_folder_without_game_archives_is_refused_and_not_saved() -> void:
	_retail_dir = OS.get_cache_dir().path_join(
			"opennova_bundled_menu_empty_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_retail_dir), OK)
	_shell = await _boot()
	var bundled := _shell.current_resource_root()
	assert_false(_shell.enter_retail_dir(_retail_dir))
	assert_eq(_shell.current_resource_root(), bundled, "the bundled menu stays")
	assert_eq(ResourceDirSettings.get_retail_dir(), "", "a refused folder is not saved")


func test_a_picked_install_is_saved_for_the_next_play_retail() -> void:
	_retail_dir = _stage_retail()
	_shell = await _boot()
	assert_true(_shell.enter_retail_dir(_retail_dir))
	assert_true(_same_dir(ResourceDirSettings.get_retail_dir(), _retail_dir))
