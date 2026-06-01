extends GutTest

# Runtime menu shell (NovaMenuHost) gates: it boots the JO menu set, services the
# host policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuHostScript := preload("res://game/menu_shell.gd")

const MAIN_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "res://../fixtures/mnu/jo_loadout.mnu"  # the cross-.mnu target


# Build a throwaway resource dir holding main.mnu (+ a sp.mnu jump target and a
# stub mission), and a host pointed at it. Returns null when a real temp root is
# unavailable in this environment (the caller pass_test-skips, as mnu_menu_test does).
func _make_host(dir: String) -> NovaMenuHost:
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	var host: NovaMenuHost = MenuHostScript.new()
	host.size = Vector2(800, 600)
	add_child_autofree(host)  # in-tree so the built menu's widgets are not orphans
	host.setup(root)
	return host


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


func _cleanup(dir: String) -> void:
	for f in ["main.mnu", "sp.mnu", "test.bms"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


func test_boots_into_main_menu_startup() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	assert_eq(host.get_current_menu_file(), "main.mnu", "main menu opened on setup")
	var menu := host.get_menu()
	assert_not_null(menu, "menu node built")
	assert_eq(menu.current_screen, "STARTUP", "STARTUP screen shown")
	_cleanup(dir)


func test_startup_drives_music_var() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	# jo_main STARTUP declares MUSICVAR 1; the menu pushes it into the director var.
	assert_eq(host.get_music_director().get_var(0), 1, "STARTUP MUSICVAR -> director var 0")
	_cleanup(dir)


func test_cross_mnu_jump_and_back_stack() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	var menu := host.get_menu()
	# A cross-.mnu jump (file set) routes through menu_requested -> host opens it.
	menu.navigate_to_menu("sp.mnu", "")
	assert_eq(host.get_current_menu_file(), "sp.mnu", "host loaded the requested menu")
	assert_eq(host.get_menu_stack_depth(), 1, "previous menu pushed onto the back stack")
	# A top-level back (empty in-menu stack) pops the file stack back to main.mnu.
	menu.pop_screen()
	assert_eq(host.get_current_menu_file(), "main.mnu", "back returned to the main menu")
	assert_eq(host.get_menu_stack_depth(), 0, "file back stack emptied")
	_cleanup(dir)


func test_top_level_quit_requests_exit() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(host)
	host.get_menu().quit_game()  # main menu, empty stack -> exit to desktop
	assert_signal_emitted(host, "exit_to_desktop_requested")
	_cleanup(dir)


func test_in_game_back_requests_resume() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	# Enter the pause context. game.mnu is absent so the overlay fails to load, but
	# the in-game flag is set, so a top-level back now means resume, not exit.
	host.open_ingame_menu()
	watch_signals(host)
	host.get_menu().quit_game()
	assert_signal_emitted(host, "resume_requested")
	assert_signal_not_emitted(host, "exit_to_desktop_requested")
	_cleanup(dir)


func test_start_emits_selected_mission() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(host)
	# A mission-list selection relayed through the menu, then a start control press.
	host.get_menu().notify_widget_value("MISSION_LIST", "list", 0, "test.bms")
	assert_eq(host.get_selected_mission(), "test.bms", "selection tracked from the list relay")
	host._on_start_control()
	assert_signal_emitted_with_parameters(host, "start_requested", ["test.bms"])
	_cleanup(dir)


func test_start_without_selection_falls_back_to_first_mission() -> void:
	var dir := _make_dir()
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(host)
	host._on_start_control()  # no selection -> first .bms in the dir (test.bms)
	assert_signal_emitted_with_parameters(host, "start_requested", ["test.bms"])
	_cleanup(dir)


func test_missing_assets_degrade_without_crashing() -> void:
	# A dir with only main.mnu (no stylesheet / sound / music / textures): the shell
	# still builds the menu and runs; the menu just reports unresolved assets.
	var dir := OS.get_temp_dir().path_join("menu_shell_bare_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	var host := _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir)
		return
	assert_eq(host.get_current_menu_file(), "main.mnu", "menu still opens with no companion assets")
	assert_not_null(host.get_music_director(), "director created even without a music script")
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir)
