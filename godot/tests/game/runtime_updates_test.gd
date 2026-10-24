extends GutTest


func before_each() -> void:
	RuntimeUpdates.configure("opennova", "C:/Runtime", "D:/Game Data",
			PackedStringArray(["--", "/game", "opennova", "/exp", "onx"]), true)


func after_each() -> void:
	RuntimeUpdates.configure("opennova", "", "", PackedStringArray(), false)
	MusicService.stop_context()
	LaunchFlags.clear_args_override()


func test_check_and_handoff_preserve_product_roots_and_relaunch_arguments() -> void:
	assert_eq(RuntimeUpdates.check_arguments(), PackedStringArray([
			"--check", "/game", "opennova", "--runtime-dir", "C:/Runtime",
			"--resource-dir", "D:/Game Data"]))
	assert_eq(RuntimeUpdates.install_arguments(1234), PackedStringArray([
			"/RUN", "--parent-pid", "1234", "/game", "opennova",
			"--runtime-dir", "C:/Runtime", "--resource-dir", "D:/Game Data",
			"--run-arg", "--", "--run-arg", "/game", "--run-arg", "opennova",
			"--run-arg", "/exp", "--run-arg", "onx"]))


func test_availability_only_follows_successful_native_available_exit() -> void:
	assert_false(RuntimeUpdates.available)
	RuntimeUpdates.complete_check(10)
	assert_true(RuntimeUpdates.available)
	RuntimeUpdates.complete_check(0)
	assert_false(RuntimeUpdates.available)
	RuntimeUpdates.complete_check(10)
	RuntimeUpdates.complete_check(1)
	assert_false(RuntimeUpdates.available, "offline checks leave play available and UPDATE hidden")
	RuntimeUpdates.configure("jo", "C:/Runtime", "D:/Retail", PackedStringArray(), false)
	RuntimeUpdates.complete_check(10)
	assert_false(RuntimeUpdates.available, "editor/test launches cannot enable updates")
	assert_false(RuntimeUpdates.launch_update())


func test_main_menu_control_tracks_async_availability_and_document_reopen() -> void:
	var dir := TestFs.cache_dir(self, "update_menu")
	var body := MenuDriverFixture.wnd("button", "UPDATE", 30)
	TestFs.write_text(self, dir.path_join("main.mnu"),
			MenuDriverFixture.screen_xml("STARTUP", body))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var menu := MenuShell.new()
	add_child_autofree(menu)
	assert_true(menu.setup(root))
	var driver := menu.get_driver()
	var update := driver.widget_id("UPDATE")
	assert_gte(update, 0)
	assert_false(driver.is_widget_shown(update))
	RuntimeUpdates.complete_check(10)
	assert_true(driver.is_widget_shown(update), "availability reaches the displayed main menu")
	assert_false(driver.is_widget_disabled(update))
	assert_true(menu.open_menu("main.mnu", "STARTUP"))
	assert_true(driver.is_widget_shown(driver.widget_id("UPDATE")), "reopen keeps availability")
	RuntimeUpdates.complete_check(0)
	assert_false(driver.is_widget_shown(driver.widget_id("UPDATE")))
	root.clear()
	TestFs.remove_dir_recursive(dir)
