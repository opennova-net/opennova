extends GutTest

# The NovaWorld panel's mods (ADR 0074): NovaWorld's mods of this folder's game
# (GET /api/mods, whose reply the test hands the panel through apply_mods_reply,
# no request made), each installed into expansion/<code>/ by the updater, and
# the joins that offer a listed mod's install. RuntimeUpdates is configured
# over a temp game folder with updates disabled, so no updater ever starts.

const PANEL_SCENE := preload("res://game/novaworld_panel.tscn")

const MODS_REPLY := (
	'{"mods": ['
	+ '{"key": "MYMOD", "code": "mymod", "name": "My Mod", "owner": "biggy", "version": "1.2",'
	+ ' "updater_login": "MYMOD", "command_file": "MYMOD.CMD"},'
	+ '{"key": "OTHER", "code": "other", "name": "", "owner": "ljim", "version": "3",'
	+ ' "updater_login": "OTHER", "command_file": "OTHER.CMD"},'
	+ '{"key": "BROKEN", "code": "broken", "name": "No product", "owner": "x", "version": "1",'
	+ ' "updater_login": "BROKEN"}'
	+ ']}')

var _game_dir := ""


func before_each() -> void:
	_game_dir = TestFs.cache_dir(self, "panel_mods")
	RuntimeUpdates.configure("acidgambit", _game_dir.path_join("runtime"), _game_dir,
			PackedStringArray(), false)


func after_each() -> void:
	RuntimeUpdates.configure("acidgambit", "", "", PackedStringArray(), false)
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_game_dir)


func _make_panel() -> NovaWorldPanel:
	var panel := PANEL_SCENE.instantiate() as NovaWorldPanel
	panel.start_client_on_ready = false
	panel.target_override = NovaWorldSettings.Target.OPENNOVA
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_game_dir), OK)
	panel.resource_root = root
	add_child_autofree(panel)
	return panel


func _signed_in(panel: NovaWorldPanel) -> void:
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")


func _row(fields: Dictionary) -> NovaWorldServerRow:
	var row := NovaWorldServerRow.new()
	for key in fields:
		row.set(key, fields[key])
	return row


func _install_folder(code: String) -> void:
	var dir := _game_dir.path_join("expansion").path_join(code)
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)


# The site whose /api/mods belongs to the OpenNova target's gate: a deployment's
# nw.<domain> is https://<domain>, loopback the local stack's portal.
func test_the_mod_list_asks_the_gates_own_site_by_the_games_command_file() -> void:
	assert_eq(NovaWorldSettings.web_base_for("nw.opennova.net"), "https://opennova.net")
	assert_eq(NovaWorldSettings.web_base_for("nw.staging.opennova.net"),
			"https://staging.opennova.net")
	assert_eq(NovaWorldSettings.web_base_for("127.0.0.1"), "http://127.0.0.1:8000")
	assert_eq(NovaWorldSettings.web_base_for("play.example.org"), "https://play.example.org")
	assert_eq(NovaWorldPanel.mods_url("https://opennova.net", "JOPTR.CMD"),
			"https://opennova.net/api/mods?game=JOPTR.CMD")
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", _game_dir]))
	assert_eq(LaunchFlags.product_command_file(), "ACIDGAMBIT.CMD", "no wizard: OP: Acid Gambit's")


func test_the_reply_lists_each_installable_mod_and_marks_the_installed() -> void:
	_install_folder("other")
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	assert_eq(panel.mod_count(), 2, "an entry naming no command file cannot install")
	assert_eq(panel.mod_cells(0), PackedStringArray(["My Mod", "biggy", "1.2", ""]))
	assert_eq(panel.mod_cells(1), PackedStringArray(["other", "ljim", "3", "Installed"]),
			"a mod with no name reads by its code; its folder makes it installed")
	assert_string_contains(panel.mods_intro_text(), "OP: Acid Gambit")
	panel.select_mod_for_test(0)
	assert_eq(panel.mod_buttons_enabled(), [true, false, false] as Array[bool], "Install alone")
	panel.select_mod_for_test(1)
	assert_eq(panel.mod_buttons_enabled(), [false, true, true] as Array[bool], "Update and Remove")


func test_a_failed_or_unreadable_reply_says_so() -> void:
	var panel := _make_panel()
	panel.apply_mods_reply(500, "")
	assert_eq(panel.mods_intro_text(), "NovaWorld's mods did not load (HTTP 500).")
	panel.apply_mods_reply(200, "{\"servers\": []}")
	assert_eq(panel.mods_intro_text(), "NovaWorld's mods did not load (HTTP 200).")
	assert_eq(panel.mod_count(), 0)
	panel.apply_mods_reply(200, "{\"mods\": []}")
	assert_string_contains(panel.mods_intro_text(), "No mods of OP: Acid Gambit")


func test_remove_deletes_the_mods_folder() -> void:
	_install_folder("other")
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	panel.select_mod_for_test(1)
	panel.press_mod_remove()
	assert_false(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/other")))
	assert_eq(panel.mod_cells(1)[3], "", "it reads as not installed")
	assert_true(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion")),
			"the other mods' folder stays")


# A mod installed here that the list does not hold (a private one, a retired one,
# another service's) shows by its folder's name, marked, and only Remove takes it.
func test_an_installed_mod_the_list_does_not_hold_shows_with_remove_alone() -> void:
	_install_folder("other")
	_install_folder("secret")
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	assert_eq(panel.mod_count(), 2, "the list's own")
	assert_eq(panel.mod_row_count(), 3, "and the installed one it does not hold")
	assert_eq(panel.mod_cells(2), PackedStringArray(["secret", "", "", "Installed, not listed"]))
	panel.select_mod_for_test(2)
	assert_eq(panel.mod_buttons_enabled(), [false, false, true] as Array[bool], "Remove alone")
	panel.press_mod_remove()
	assert_false(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/secret")))
	assert_eq(panel.mod_row_count(), 2, "gone from the page")
	var unlisted := _make_panel()
	unlisted.apply_mods_reply(500, "")
	assert_eq(unlisted.mod_cells(0), PackedStringArray(["other", "", "", "Installed"]),
			"with no list loaded, an installed mod shows, not marked")
	unlisted.select_mod_for_test(0)
	assert_eq(unlisted.mod_buttons_enabled(), [false, false, true] as Array[bool])


# The Mods page opens before the sign-in too, and stays up while the session
# connects behind it; its Back returns to where the session got to.
func test_the_mods_page_opens_before_the_sign_in() -> void:
	var panel := _make_panel()
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.CONNECTING)
	panel.press_mods()
	assert_true(panel.mods_visible())
	panel.client_for_test().emit_signal("connected")
	assert_true(panel.mods_visible(), "the connect lands behind the page")
	panel.press_mods_back()
	assert_true(panel.login_visible(), "back to the sign-in the session reached")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")
	panel.press_mods()
	panel.press_mods_back()
	assert_true(panel.browser_visible(), "from the games, back to the games")


# Nothing is written when the updater cannot start: the folder is no half install.
func test_an_install_the_updater_cannot_run_writes_nothing_and_says_why() -> void:
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	panel.select_mod_for_test(0)
	watch_signals(panel)
	panel.press_mod_install()
	assert_signal_not_emitted(panel, "update_started")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_string_contains(panel.message_text(), "My Mod could not be installed.")
	assert_string_contains(panel.message_text(), "Updates are disabled for this launch.")
	assert_false(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/mymod")))


# A server on a mod this install lacks: unjoinable while NovaWorld lists no mod
# of its code, then its Join offers the mod's install (and the join after it).
func test_joining_a_server_on_a_listed_mod_offers_its_install() -> void:
	var panel := _make_panel()
	_signed_in(panel)
	var rows: Array[NovaWorldServerRow] = [
		_row({"name": "ModHost", "exp": "MyMod", "rid": 42, "max_players": 16}),
	]
	panel.set_rows_for_test(rows)
	assert_string_contains(panel.join_block_reason(rows[0]), "expansion 'MyMod'",
			"no mod listed: the existing failure, naming the expansion")
	assert_true(panel.visible_row_dimmed(0))
	panel.apply_mods_reply(200, MODS_REPLY)
	assert_eq(panel.join_block_reason(rows[0]), "", "a listed mod (any case) is joinable")
	assert_false(panel.visible_row_dimmed(0))
	assert_true(panel.join_enabled())
	panel.press_join()
	assert_eq(panel.joining_rid(), 0, "no join starts before the install")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_string_contains(panel.message_text(), "ModHost plays My Mod")
	assert_true(panel.message_back_visible(), "the offer can be declined")
	watch_signals(panel)
	panel.press_message_action()
	assert_signal_not_emitted(panel, "update_started", "updates are disabled here")
	assert_string_contains(panel.message_text(), "Updates are disabled for this launch.")


# The relaunch after a mod's install for a join (`--novaworld-join`): the server
# is joined once a signed-in list holds it, and a server gone from the list says
# so and leaves the panel up.
func test_a_relaunch_joins_its_server_once_listed() -> void:
	var panel := _make_panel()
	_signed_in(panel)
	var rows: Array[NovaWorldServerRow] = [
		_row({"name": "BaseHost", "rid": 7, "max_players": 16}),
		_row({"name": "ModHost", "rid": 42, "max_players": 16}),
	]
	panel.set_rows_for_test(rows)
	var client := panel.client_for_test()
	var held: Array = client.get_signal_connection_list("join_failed")
	for connection: Dictionary in held:
		client.disconnect("join_failed", connection["callable"])
	panel.join_when_listed(42)
	for connection: Dictionary in held:
		client.connect("join_failed", connection["callable"])
	assert_eq(panel.joining_rid(), 42, "the listed server is joined")
	panel.join_when_listed(99)
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(),
			"The server you were joining before the update is no longer listed.")
	panel.press_message_action()
	assert_true(panel.browser_visible(), "back to the games")


func test_a_join_relaunch_waits_for_the_sign_in() -> void:
	var panel := _make_panel()
	panel.join_when_listed(42)
	assert_eq(panel.joining_rid(), 0)
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.CONNECTING)
