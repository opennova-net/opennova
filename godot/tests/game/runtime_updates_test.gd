extends GutTest


func before_each() -> void:
	RuntimeUpdates.configure("acidgambit", "C:/Runtime", "D:/Game Data",
			PackedStringArray(["--", "/game", "acidgambit", "/exp", "onx"]), true)


func after_each() -> void:
	RuntimeUpdates.configure("acidgambit", "", "", PackedStringArray(), false)
	MusicService.stop_context()
	LaunchFlags.clear_args_override()


func test_handoff_preserves_product_roots_and_relaunch_arguments() -> void:
	assert_eq(RuntimeUpdates.install_arguments(1234), PackedStringArray([
			"/RUN", "--parent-pid", "1234", "/game", "acidgambit",
			"--runtime-dir", "C:/Runtime", "--resource-dir", "D:/Game Data",
			"--run-arg", "--", "--run-arg", "/game", "--run-arg", "acidgambit",
			"--run-arg", "/exp", "--run-arg", "onx"]))


# A first-run install marks its relaunch once; the menu's UPDATE never does, and an
# earlier marker is not carried into a later install.
func test_a_bootstrap_install_marks_its_relaunch_once() -> void:
	var marked := RuntimeUpdates.install_arguments(1234, true)
	assert_eq(marked.slice(marked.size() - 2), PackedStringArray(["--run-arg", "--after-update"]))
	RuntimeUpdates.configure("acidgambit", "C:/Runtime", "D:/Game Data",
			PackedStringArray(["--verbose", "--", "/exp", "onx", "--after-update"]), true)
	assert_false(RuntimeUpdates.install_arguments(1234).has("--after-update"))
	var again := RuntimeUpdates.install_arguments(1234, true)
	assert_eq(again.count("--after-update"), 1)
	assert_eq(again.count("--"), 1)
	RuntimeUpdates.configure("acidgambit", "C:/Runtime", "D:/Game Data", PackedStringArray(), true)
	assert_eq(RuntimeUpdates.install_arguments(1234, true).slice(9), PackedStringArray([
			"--run-arg", "--", "--run-arg", "--after-update"]), "no game arguments: the marker after --")


func test_a_disabled_launch_starts_nothing() -> void:
	RuntimeUpdates.configure("jo", "C:/Runtime", "D:/Retail", PackedStringArray(), false)
	assert_false(RuntimeUpdates.launch_update(), "editor/test launches cannot start the updater")
	assert_eq(RuntimeUpdates.last_error, "Updates are disabled for this launch.")


# The updater's three routes, none gated on an update being available (engine
# menu_commands.h MenuCommand::Update, app_message_command): STARTUP's and
# OPTIONS's UPDATE button, in any file, and an APPMSG row's "UPDATE" on any
# screen. The control shows as authored. The runtime folder holds no update.exe
# here, so each route reaches the launch, which fails and leaves the menu.
func test_every_update_route_reaches_the_launch_ungated() -> void:
	var dir := TestFs.cache_dir(self, "update_menu")
	var app := MenuDriverFixture.wnd("button", "APP_UPDATE", 60,
			'<ACTION type="appmsg">UPDATE</ACTION>')
	var other := MenuDriverFixture.wnd("button", "APP_OTHER", 90,
			'<ACTION type="appmsg">CREDITS</ACTION>')
	TestFs.write_text(self, dir.path_join("main.mnu"),
			MenuDriverFixture.screen_xml("STARTUP", MenuDriverFixture.wnd("button", "UPDATE", 30))
			+ MenuDriverFixture.screen_xml("OPTIONS", MenuDriverFixture.wnd("button", "UPDATE", 30))
			+ MenuDriverFixture.screen_xml("ELSEWHERE",
					MenuDriverFixture.wnd("button", "UPDATE", 30) + app + other))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var menu := MenuShell.new()
	add_child_autofree(menu)
	assert_true(menu.setup(root))
	watch_signals(menu)
	var driver := menu.get_driver()
	for screen in ["STARTUP", "OPTIONS"]:
		assert_true(menu.open_menu("main.mnu", screen))
		var update := driver.widget_id("UPDATE")
		assert_true(driver.is_widget_shown(update) and not driver.is_widget_disabled(update),
				"%s's UPDATE shows as authored" % screen)
		RuntimeUpdates.last_error = ""
		driver.activate(update)
		assert_eq(RuntimeUpdates.last_error,
				"update.exe is missing from the OpenNova installation.",
				"%s's UPDATE starts the updater" % screen)
	assert_true(menu.open_menu("main.mnu", "ELSEWHERE"))
	RuntimeUpdates.last_error = ""
	driver.activate(driver.widget_id("UPDATE"))
	assert_eq(RuntimeUpdates.last_error, "", "another screen's UPDATE is the menu's own")
	driver.activate(driver.widget_id("APP_OTHER"))
	assert_eq(RuntimeUpdates.last_error, "", "another APPMSG starts nothing")
	driver.activate(driver.widget_id("APP_UPDATE"))
	assert_eq(RuntimeUpdates.last_error,
			"update.exe is missing from the OpenNova installation.",
			"an APPMSG UPDATE row starts the updater")
	assert_signal_not_emitted(menu, "exit_to_desktop_requested", "a failed launch keeps the game")
	root.clear()
	TestFs.remove_dir_recursive(dir)
