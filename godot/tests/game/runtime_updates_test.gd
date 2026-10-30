extends GutTest


func before_each() -> void:
	RuntimeUpdates.configure("acidgambit", "C:/Runtime", "D:/Game Data",
			PackedStringArray(["--", "/game", "acidgambit", "/exp", "onx"]), true)


func after_each() -> void:
	RuntimeUpdates.configure("acidgambit", "", "", PackedStringArray(), false)
	MusicService.stop_context()
	LaunchFlags.clear_args_override()


# The node hands its product and relaunch to the engine's composition (ctest boot_policy pins the
# markers: --after-update, --novaworld-join).
func test_check_and_handoff_preserve_product_roots_and_relaunch_arguments() -> void:
	assert_eq(RuntimeUpdates.check_arguments(), PackedStringArray([
			"--check", "/game", "acidgambit", "--runtime-dir", "C:/Runtime",
			"--resource-dir", "D:/Game Data"]))
	assert_eq(RuntimeUpdates.install_arguments(1234), PackedStringArray([
			"/RUN", "--parent-pid", "1234", "/game", "acidgambit",
			"--runtime-dir", "C:/Runtime", "--resource-dir", "D:/Game Data",
			"--run-arg", "--", "--run-arg", "/game", "--run-arg", "acidgambit",
			"--run-arg", "/exp", "--run-arg", "onx"]))


# A mod's folder is its install (expansion/<code>/); its removal deletes it. An
# install the updater cannot start writes nothing.
func test_a_mods_folder_is_its_install() -> void:
	var dir := TestFs.cache_dir(self, "runtime_mods")
	var listed: Array[ModListing] = []
	assert_eq(ModListing.read_reply(
			'{"mods": [{"code": "mymod", "updater_login": "MYMOD", "command_file": "MYMOD.CMD"}]}',
			listed), "")
	RuntimeUpdates.configure("acidgambit", dir.path_join("runtime"), dir, PackedStringArray(), false)
	assert_false(RuntimeUpdates.has_mod("mymod"))
	assert_false(RuntimeUpdates.install_mod(listed[0]))
	assert_eq(RuntimeUpdates.last_error, "Updates are disabled for this launch.")
	RuntimeUpdates.configure("acidgambit", dir.path_join("runtime"), dir, PackedStringArray(), true)
	assert_false(RuntimeUpdates.install_mod(listed[0]))
	assert_eq(RuntimeUpdates.last_error, "update.exe is missing from the OpenNova installation.")
	assert_false(DirAccess.dir_exists_absolute(RuntimeUpdates.mod_dir("mymod")), "nothing written")
	TestFs.write_text(self, RuntimeUpdates.mod_dir("mymod").path_join("mymod.pff"), "archive")
	assert_true(RuntimeUpdates.has_mod("mymod"))
	assert_true(RuntimeUpdates.remove_mod("mymod"))
	assert_false(RuntimeUpdates.has_mod("mymod"))
	assert_false(RuntimeUpdates.remove_mod(".."), "no folder outside expansion/")
	assert_string_contains(RuntimeUpdates.last_error, "no mod folder's name")
	TestFs.remove_dir_recursive(dir)


func test_a_disabled_launch_starts_nothing() -> void:
	RuntimeUpdates.configure("jo", "C:/Runtime", "D:/Retail", PackedStringArray(), false)
	assert_false(RuntimeUpdates.launch_update(), "editor/test launches cannot start the updater")
	assert_eq(RuntimeUpdates.last_error, "Updates are disabled for this launch.")


# Only the check's "an update is waiting" (update.exe --check exit 10) makes one
# available; current, a failure (offline) and a launch with updates off leave it
# unknown, which gates nothing.
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
	RuntimeUpdates.begin_check()
	assert_false(RuntimeUpdates.available, "no check runs with updates off")


# The updater's controls show only while an update waits (ADR 0067 d3 as amended
# 2026-10-28): STARTUP's and OPTIONS's UPDATE, hidden and disabled until the check
# finds one, then shown, and a press starts the updater. An APPMSG row's "UPDATE"
# is a row, not a control the gate can hide: it starts the updater on any screen
# (engine menu_commands.h app_message_command). The runtime folder holds no
# update.exe here, so each launch fails and leaves the menu.
func test_update_controls_show_only_while_an_update_waits() -> void:
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
		assert_true(not driver.is_widget_shown(update) and driver.is_widget_disabled(update),
				"%s's UPDATE is hidden while no update waits" % screen)
	RuntimeUpdates.complete_check(10)
	for screen in ["STARTUP", "OPTIONS"]:
		assert_true(menu.open_menu("main.mnu", screen))
		var update := driver.widget_id("UPDATE")
		assert_true(driver.is_widget_shown(update) and not driver.is_widget_disabled(update),
				"%s's UPDATE shows once an update waits" % screen)
		RuntimeUpdates.last_error = ""
		driver.activate(update)
		assert_eq(RuntimeUpdates.last_error,
				"update.exe is missing from the OpenNova installation.",
				"%s's UPDATE starts the updater" % screen)
	assert_true(menu.open_menu("main.mnu", "ELSEWHERE"))
	assert_true(driver.is_widget_shown(driver.widget_id("UPDATE")),
			"another screen's UPDATE is the menu's own, as authored")
	RuntimeUpdates.last_error = ""
	driver.activate(driver.widget_id("UPDATE"))
	assert_eq(RuntimeUpdates.last_error, "", "another screen's UPDATE starts nothing")
	driver.activate(driver.widget_id("APP_OTHER"))
	assert_eq(RuntimeUpdates.last_error, "", "another APPMSG starts nothing")
	RuntimeUpdates.complete_check(0)
	driver.activate(driver.widget_id("APP_UPDATE"))
	assert_eq(RuntimeUpdates.last_error,
			"update.exe is missing from the OpenNova installation.",
			"an APPMSG UPDATE row starts the updater, an update waiting or not")
	assert_signal_not_emitted(menu, "exit_to_desktop_requested", "a failed launch keeps the game")
	root.clear()
	TestFs.remove_dir_recursive(dir)


# While an update waits, the multiplayer entries are disabled (NovaWorld's
# controls and LAN_MULTI_PLAYER), the NovaWorld route does nothing even when its
# control is pressed by script, and a reopened document is gated again; once the
# answer is no longer "waiting" the gate gives back what it took, and an unknown
# answer (the check running or failed) never takes anything.
func test_multiplayer_waits_for_a_waiting_update() -> void:
	var dir := TestFs.cache_dir(self, "update_gate_menu")
	TestFs.write_text(self, dir.path_join("main.mnu"),
			MenuDriverFixture.screen_xml("STARTUP",
					MenuDriverFixture.wnd("button", "LAN_MULTI_PLAYER", 30)
					+ MenuDriverFixture.wnd("button", "NW_MULTI_PLAYER", 60)
					+ MenuDriverFixture.wnd("button", "UPDATE", 90)))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var menu := MenuShell.new()
	add_child_autofree(menu)
	assert_true(menu.setup(root))
	watch_signals(menu)
	var driver := menu.get_driver()
	var enabled := func() -> bool:
		return not driver.is_widget_disabled(driver.widget_id("LAN_MULTI_PLAYER")) \
				and not driver.is_widget_disabled(driver.widget_id("NW_MULTI_PLAYER"))
	assert_true(enabled.call(), "unknown (no check yet): multiplayer stays enabled")
	RuntimeUpdates.complete_check(1)
	assert_true(enabled.call(), "a failed (offline) check: multiplayer stays enabled")
	driver.activate(driver.widget_id("NW_MULTI_PLAYER"))
	assert_signal_emit_count(menu, "novaworld_requested", 1, "NovaWorld opens while current")
	RuntimeUpdates.complete_check(10)
	assert_true(driver.is_widget_disabled(driver.widget_id("LAN_MULTI_PLAYER"))
			and driver.is_widget_disabled(driver.widget_id("NW_MULTI_PLAYER")),
			"an update waiting: NovaWorld and LAN are disabled")
	driver.activate(driver.widget_id("NW_MULTI_PLAYER"))
	assert_signal_emit_count(menu, "novaworld_requested", 1, "a scripted press opens nothing")
	assert_true(menu.open_menu("main.mnu", "STARTUP"))
	assert_true(driver.is_widget_disabled(driver.widget_id("NW_MULTI_PLAYER")),
			"a reopened document is gated again")
	RuntimeUpdates.complete_check(0)
	assert_true(enabled.call(), "current again: the gate gives the entries back")
	root.clear()
	TestFs.remove_dir_recursive(dir)
