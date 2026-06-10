extends GutTest

# Runtime menu shell (NovaMenuHost) gates: it boots the JO menu set, services the
# host policy the menu leaves to it (cross-.mnu jumps + a file-level back stack,
# quit), drives the music director's screen var, launches a selected mission, and
# degrades gracefully when menu assets are missing - all headless, no blocking.

const MenuHostScript := preload("res://game/menu_shell.gd")

const MAIN_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"   # STARTUP, MUSICVAR 1
const SP_FIXTURE := "res://../fixtures/mnu/jo_loadout.mnu"  # the cross-.mnu target
const OPTIONS_FIXTURE := "res://../fixtures/mnu/jo_options.mnu"  # has the Mods tab (AVAIL_LIST/MOD_DESC)
const SP_PLAY_FIXTURE := "res://../fixtures/mnu/jo_sp.mnu"  # play screen: mission list IA_LIST + ACCEPT


# Build a throwaway resource dir holding main.mnu (+ a sp.mnu jump target and a
# stub mission), and a host pointed at it. Returns null when a real temp root is
# unavailable in this environment (the caller pass_test-skips, as mnu_menu_test does).
func _make_host(dir: String):
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	var host = MenuHostScript.new()
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
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable in this environment")
		_cleanup(dir)
		return
	assert_eq(host.get_current_menu_file(), "main.mnu", "main menu opened on setup")
	var menu = host.get_menu()
	assert_not_null(menu, "menu node built")
	assert_eq(menu.current_screen, "STARTUP", "STARTUP screen shown")
	_cleanup(dir)


func test_startup_drives_music_var() -> void:
	var dir := _make_dir()
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	# jo_main STARTUP declares MUSICVAR 1; the menu pushes it into the director var.
	assert_eq(host.get_music_director().get_var(0), 1, "STARTUP MUSICVAR -> director var 0")
	_cleanup(dir)


func test_cross_mnu_jump_and_back_stack() -> void:
	var dir := _make_dir()
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	var menu = host.get_menu()
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
	var host = _make_host(dir)
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
	var host = _make_host(dir)
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
	var host = _make_host(dir)
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
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		_cleanup(dir)
		return
	watch_signals(host)
	host._on_start_control()  # no selection -> first .bms in the dir (test.bms)
	assert_signal_emitted_with_parameters(host, "start_requested", ["test.bms"])
	_cleanup(dir)


# Options -> Mods: the host lists discoverable expansions in AVAIL_LIST by name, and
# activating one mounts it over the base game, fills MOD_DESC, persists the choice
# (read back by main_game at the next launch), and announces it. Uses a runtime
# (packed PFF) mount so list_expansions/mount_runtime have real archives to work on.
func test_mods_tab_lists_mounts_and_persists_expansion() -> void:
	var saved := NovaResourceDirSettings.get_expansion()
	NovaResourceDirSettings.set_expansion("")  # clean slate so the activate is not a no-op
	var dir := _make_runtime_dir()
	var host = _make_runtime_host(dir)
	if host == null:
		pass_test("runtime resource root unavailable in this environment")
		NovaResourceDirSettings.set_expansion(saved)
		_rm_runtime_dir(dir)
		return
	watch_signals(host)
	var menu = host.get_menu()
	var avail = menu.find_child("AVAIL_LIST", true, false)
	assert_not_null(avail, "AVAIL_LIST built")
	assert_eq(avail.item_count, 1, "one expansion discovered under expansion/")
	assert_eq(avail.get_item_text(0), "jox01")
	# Activation mounts + persists + describes + announces.
	host._on_mod_activated(0)
	assert_signal_emitted_with_parameters(host, "expansion_selected", ["jox01"])
	assert_eq(host.get_selected_expansion(), "jox01")
	assert_eq(NovaResourceDirSettings.get_expansion(), "jox01", "choice persisted to config")
	var desc = menu.find_child("MOD_DESC", true, false)
	assert_not_null(desc, "MOD_DESC built")
	assert_string_contains(desc.text, "Kendari", "friendly expansion name shown")
	# The expansion's packed asset is now reachable through the live root.
	assert_eq(host._root.read_file("expmodel.3di").get_string_from_utf8(), "exp model",
		"expansion archive mounted over the base game")
	host._root.clear()  # release PFF handles before deleting the temp archives
	NovaResourceDirSettings.set_expansion(saved)
	_rm_runtime_dir(dir)


# Options -> Mods OK (the ACCEPT button) must APPLY the highlighted expansion, not
# launch a mission. ACCEPT is overloaded across JO screens (launch on Single Player,
# plain OK on Options); the host scopes it by screen role, so on a Mods screen (mod
# list, no mission list) ACCEPT applies. Regression for the "OK loads a mission" bug.
func test_mods_ok_applies_expansion_without_launching() -> void:
	var saved := NovaResourceDirSettings.get_expansion()
	NovaResourceDirSettings.set_expansion("")  # so the apply is not a no-op
	var dir := _make_runtime_dir()
	var host = _make_runtime_host(dir)
	if host == null:
		pass_test("runtime resource root unavailable in this environment")
		NovaResourceDirSettings.set_expansion(saved)
		_rm_runtime_dir(dir)
		return
	var menu = host.get_menu()
	var avail = menu.find_child("AVAIL_LIST", true, false)
	assert_not_null(avail, "AVAIL_LIST built")
	avail.select(0)  # highlight jox01 (no double-click / activation)
	var accept = menu.find_child("ACCEPT", true, false)
	assert_not_null(accept, "options ACCEPT button built")
	watch_signals(host)
	(accept as BaseButton).pressed.emit()  # press OK
	assert_signal_not_emitted(host, "start_requested", "OK on the Mods screen must not launch")
	assert_signal_emitted_with_parameters(host, "expansion_selected", ["jox01"])  # OK applied the highlighted mod
	assert_eq(NovaResourceDirSettings.get_expansion(), "jox01", "applied choice persisted")
	host._root.clear()
	NovaResourceDirSettings.set_expansion(saved)
	_rm_runtime_dir(dir)


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
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir.path_join("alpha.bms"))
		DirAccess.remove_absolute(dir)
		return
	var accept = host.get_menu().find_child("ACCEPT", true, false)
	assert_not_null(accept, "SP ACCEPT button built")
	watch_signals(host)
	(accept as BaseButton).pressed.emit()  # no explicit pick -> first .bms
	# ACCEPT on a mission-list screen still launches (first .bms, none selected).
	assert_signal_emitted_with_parameters(host, "start_requested", ["alpha.bms"])
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir.path_join("alpha.bms"))
	DirAccess.remove_absolute(dir)


# The menu stylesheet (menu_style.mns) is PFF-archived and is not a "recognized kind",
# so list_files(".mns") never surfaces it. The host must load it by its canonical name
# through the VFS; otherwise %DEF_TEXT_*% colors (incl. the button hover colour) never
# resolve and mouse-over has no visible effect. Regression for that hover fix.
func test_runtime_loads_pff_archived_stylesheet_by_canonical_name() -> void:
	var dir := OS.get_temp_dir().path_join("menu_shell_style_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var mns := "// test stylesheet\nDEF_FONTNAME_LG Gunpl27b.fnt\nDEF_TEXT_FG FFFFFFFF\n" \
		+ "DEF_TEXT_MOUSEOVER_FG FFFF0000\nDEF_TEXT_SELECTED_FG FFFF0000\nDEF_TEXT_DISABLED_FG FF545252\n"
	_write_pff(dir.path_join("aa_base.pff"), [
		{"name": "main.mnu", "bytes": FileAccess.get_file_as_bytes(MAIN_FIXTURE)},
		{"name": "menu_style.mns", "bytes": mns},
	])
	var root := NovaResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		pass_test("runtime resource root unavailable in this environment")
		DirAccess.remove_absolute(dir.path_join("aa_base.pff"))
		DirAccess.remove_absolute(dir)
		return
	assert_eq(root.list_files(".mns").size(), 0, "precondition: .mns is not surfaced by list_files")
	var host = MenuHostScript.new()
	host.size = Vector2(800, 600)
	add_child_autofree(host)
	host.setup(root)
	var style = host.get_menu().get_stylesheet()
	assert_not_null(style, "menu_style.mns loaded from the PFF by canonical name")
	if style != null:
		assert_eq(style.substitute("%DEF_TEXT_MOUSEOVER_FG%"), "FFFF0000",
			"hover colour macro resolves, so button mouse-over highlights")
	root.clear()  # release the PFF handle before deleting
	DirAccess.remove_absolute(dir.path_join("aa_base.pff"))
	DirAccess.remove_absolute(dir)


func test_missing_assets_degrade_without_crashing() -> void:
	# A dir with only main.mnu (no stylesheet / sound / music / textures): the shell
	# still builds the menu and runs; the menu just reports unresolved assets.
	var dir := OS.get_temp_dir().path_join("menu_shell_bare_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	var host = _make_host(dir)
	if host == null:
		pass_test("temp resource root unavailable")
		DirAccess.remove_absolute(dir.path_join("main.mnu"))
		DirAccess.remove_absolute(dir)
		return
	assert_eq(host.get_current_menu_file(), "main.mnu", "menu still opens with no companion assets")
	assert_not_null(host.get_music_director(), "director created even without a music script")
	DirAccess.remove_absolute(dir.path_join("main.mnu"))
	DirAccess.remove_absolute(dir)


# --- Runtime (packed PFF) fixtures for the expansion/mods test -----------------

# A base dir with options.mnu packed in a base archive (so it loads under a runtime
# mount) plus one discoverable expansion (expansion/jox01/jox01.pff carrying an asset).
func _make_runtime_dir() -> String:
	var dir := OS.get_temp_dir().path_join("menu_shell_mods_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01"))
	_write_pff(dir.path_join("aa_base.pff"), [
		{"name": "options.mnu", "bytes": FileAccess.get_file_as_bytes(OPTIONS_FIXTURE)},
	])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "expmodel.3di", "bytes": "exp model"},
	])
	return dir


func _make_runtime_host(dir: String):
	var root := NovaResourceRoot.new()
	if root.mount_runtime(dir) != OK:
		return null
	var host = MenuHostScript.new()
	host.main_menu_file = "options.mnu"  # open the menu that carries the Mods tab
	host.size = Vector2(800, 600)
	add_child_autofree(host)
	host.setup(root)
	return host


func _rm_runtime_dir(dir: String) -> void:
	for sub in ["aa_base.pff", "expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
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
