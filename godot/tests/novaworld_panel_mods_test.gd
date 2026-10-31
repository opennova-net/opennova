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
# The stored NovaWorld choice, set aside for the test that writes it.
var _had_choice := false
var _saved_choice := PackedByteArray()


func before_each() -> void:
	_had_choice = FileAccess.file_exists(NovaWorldSettings.CONFIG_PATH)
	_saved_choice = FileAccess.get_file_as_bytes(NovaWorldSettings.CONFIG_PATH) \
			if _had_choice else PackedByteArray()
	_game_dir = TestFs.cache_dir(self, "panel_mods")
	RuntimeUpdates.configure("acidgambit", _game_dir.path_join("runtime"), _game_dir,
			PackedStringArray(), false)


func after_each() -> void:
	if _had_choice:
		var file := FileAccess.open(NovaWorldSettings.CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_choice)
			file.close()
	elif FileAccess.file_exists(NovaWorldSettings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(NovaWorldSettings.CONFIG_PATH))
	RuntimeUpdates.configure("acidgambit", "", "", PackedStringArray(), false)
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_game_dir)


func _make_panel(target := NovaWorldSettings.Target.OPENNOVA) -> NovaWorldPanel:
	var panel := PANEL_SCENE.instantiate() as NovaWorldPanel
	panel.start_client_on_ready = false
	panel.target_override = target
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


# A folder the Mods page installed: it holds the page's mark (engine mod_listing.h
# kModMarkFileName); `marked` false makes one of the game's own expansions instead.
func _install_folder(code: String, marked := true) -> void:
	var dir := _game_dir.path_join("expansion").path_join(code)
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	if marked:
		var mark := FileAccess.open(dir.path_join("OPENNOVA.MOD"), FileAccess.WRITE)
		assert_not_null(mark)
		if mark != null:
			mark.store_string(code)
			mark.close()


# The site whose /api/mods belongs to the OpenNova target's gate: a deployment's
# nw.<domain> is https://<domain>, loopback the local stack's portal.
func test_the_mod_list_asks_the_gates_own_site_by_the_games_command_file() -> void:
	assert_eq(NovaWorldSettings.web_base_for("nw.opennova.net"), "https://opennova.net")
	assert_eq(NovaWorldSettings.web_base_for("nw.staging.opennova.net"),
			"https://staging.opennova.net")
	assert_eq(NovaWorldSettings.web_base_for("127.0.0.1"), "http://127.0.0.1:8000")
	assert_eq(NovaWorldSettings.web_base_for("play.example.org"), "https://play.example.org")
	assert_eq(NovaWorldSettings.web_base_for("192.168.1.20"), "http://192.168.1.20:8000",
			"a second machine's stack, by its LAN address")
	assert_eq(NovaWorldSettings.web_base_for("localhost"), "http://localhost:8000")
	assert_eq(NovaWorldSettings.web_base_for("::1"), "http://[::1]:8000")
	assert_eq(NovaWorldSettings.web_base_for("gs.novaworld.net"), "",
			"NovaLogic's gate serves no mods: nothing is asked of it")
	assert_eq(NovaWorldSettings.web_base_for("GS.NovaWorld.net"), "")
	assert_eq(NovaWorldSettings.web_base_for("gs.novaworld.net."), "",
			"a root label's dot names NovaLogic's gate too (the engine's one rule)")
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
	assert_eq(panel.mods_intro_text(), "The mods from OpenNova's NovaWorld did not load (HTTP 500).")
	panel.apply_mods_reply(200, "{\"servers\": []}")
	assert_eq(panel.mods_intro_text(), "The mods from OpenNova's NovaWorld did not load (HTTP 200).")
	assert_eq(panel.mod_count(), 0)
	panel.apply_mods_reply(200, "{\"mods\": []}")
	assert_string_contains(panel.mods_intro_text(), "No mods of OP: Acid Gambit")


func test_remove_deletes_the_mods_folder() -> void:
	_install_folder("other")
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	panel.select_mod_for_test(1)
	panel.press_mod_remove()
	assert_true(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/other")),
			"Remove asks first")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_true(panel.message_back_visible(), "with a Back that keeps the folder")
	panel.press_message_action()
	assert_false(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/other")))
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MODS)
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
	panel.press_message_action()
	assert_false(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/secret")))
	assert_eq(panel.mod_row_count(), 2, "gone from the page")
	var unlisted := _make_panel()
	unlisted.apply_mods_reply(500, "")
	assert_eq(unlisted.mod_cells(0), PackedStringArray(["other", "", "", "Installed"]),
			"with no list loaded, an installed mod shows, not marked")
	unlisted.select_mod_for_test(0)
	assert_eq(unlisted.mod_buttons_enabled(), [false, false, true] as Array[bool])



# A folder the Mods page did not install (one of the game's own expansions, a retail
# install's jox01) is no mod of the page's: not listed, and a listed mod of its code
# shows installed with Update but no Remove.
func test_a_games_own_expansion_folder_is_never_removed() -> void:
	_install_folder("jox01", false)
	_install_folder("other", false)
	var panel := _make_panel()
	panel.apply_mods_reply(200, MODS_REPLY)
	assert_eq(panel.mod_row_count(), 2, "an unmarked folder the list lacks is not shown")
	panel.select_mod_for_test(1)
	assert_eq(panel.mod_buttons_enabled(), [false, true, false] as Array[bool], "Update alone")
	panel.press_mod_remove()
	panel.press_message_action()
	assert_true(DirAccess.dir_exists_absolute(_game_dir.path_join("expansion/other")))

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
	assert_string_contains(panel.message_text(), "from OpenNova's NovaWorld")
	assert_true(panel.message_back_visible(), "the offer can be declined")
	watch_signals(panel)
	panel.press_message_action()
	assert_signal_not_emitted(panel, "update_started", "updates are disabled here")
	assert_string_contains(panel.message_text(), "Updates are disabled for this launch.")


# On the original NovaWorld (which has no /api/mods) the mods are OpenNova's
# NovaWorld's, and the page and the join's offer say so; the join's install
# stores its target first, so the relaunch comes back on the original NovaWorld.
func test_on_the_original_novaworld_mods_come_from_opennovas() -> void:
	NovaWorldSettings.save_target(NovaWorldSettings.Target.OPENNOVA)
	var panel := _make_panel(NovaWorldSettings.Target.REAL)
	panel.apply_mods_reply(200, MODS_REPLY)
	assert_string_contains(panel.mods_intro_text(), "on OpenNova's NovaWorld")
	_signed_in(panel)
	var rows: Array[NovaWorldServerRow] = [
		_row({"name": "RetailModHost", "exp": "mymod", "rid": 77, "max_players": 16}),
	]
	panel.set_rows_for_test(rows)
	panel.press_join()
	assert_string_contains(panel.message_text(), "Install it from OpenNova's NovaWorld and join?")
	panel.press_message_action()
	assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.REAL,
			"the relaunch opens on the original NovaWorld again")


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
