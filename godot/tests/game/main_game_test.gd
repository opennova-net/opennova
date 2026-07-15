extends GutTest

# Runtime shell (main_game): the F9 "change game folder" hotkey may only summon the
# asset picker from the menu front-end and never while one is already open or while a
# mission is live. The native dialog can't be shown headless, so these gate the pure
# predicate that decides whether the hotkey acts (the hotkey just calls into it).

const MainGameScript := preload("res://game/main_game.gd")
const MainGameScene := preload("res://game/main_game.tscn")
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const FIXTURE_DIR := "res://../fixtures/minimal/resources"
const POLICY_FILE := "policy.bin"
const POLICY_PLAIN := "persisted game profile reached the mounted root"
const SCR_KEY_DEFAULT := 0xabee_face

var _saved_config := PackedByteArray()
var _had_config := false
var _temp_dir := ""
var _shell: Node = null


class FakeGameHud:
	extends RefCounted
	var crosshair_style := -1

	func set_crosshair_style(style: int) -> void:
		crosshair_style = style


func before_each() -> void:
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
			if _had_config else PackedByteArray()
	NovaStrings.clear()


func after_each() -> void:
	if is_instance_valid(_shell):
		var world = _shell.get_node_or_null("World")
		if world != null:
			world.unload()
			var world_root = world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_host = _shell.get_node_or_null("MenuLayer/MenuHost")
		if menu_host != null and menu_host.get_menu() != null:
			var menu_root = menu_host.get_menu().get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
		await get_tree().process_frame
	NovaMusicService.stop_context()
	if not _temp_dir.is_empty():
		_remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	NovaStrings.clear()


func _make() -> Node:
	# Not added to the tree on purpose: the predicate only reads _state/_picker, and
	# staying out of the tree keeps _ready/@onready (which need the full scene) from running.
	var game = MainGameScript.new()
	autofree(game)
	return game


func test_can_summon_in_menu_state() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	game._picker = null
	assert_true(game._can_summon_dir_picker(), "picker summonable from the menu front-end")


func test_cannot_summon_during_mission() -> void:
	var game := _make()
	game._picker = null
	game._state = MainGameScript.State.WORLD
	assert_false(game._can_summon_dir_picker(), "not summonable while a world is live")
	game._state = MainGameScript.State.PAUSED
	assert_false(game._can_summon_dir_picker(), "not summonable from the pause overlay")


func test_cannot_summon_while_picker_open() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	var picker := FileDialog.new()
	game._picker = picker
	assert_false(game._can_summon_dir_picker(), "no second picker while one is already open")
	picker.queue_free()


func test_loading_background_query_is_false_without_a_live_handoff() -> void:
	var game := _make()
	assert_false(game.has_loading_background(),
			"the public loading-art query is safe while the shell is idle")


func test_runtime_root_honors_the_persisted_game_profile() -> void:
	_shell = await _make_packed_shell("jodemo")
	if _shell == null:
		return
	var menu_host = _shell.get_node("MenuLayer/MenuHost")
	var menu = menu_host.get_menu()
	assert_not_null(menu, "the packed fixture boots the public menu host")
	if menu == null:
		return
	var root: NovaResourceRoot = menu.get_resource_root()
	assert_not_null(root, "the live menu exposes its mounted runtime root")
	if root != null:
		assert_eq(root.read_file(POLICY_FILE).get_string_from_utf8(), POLICY_PLAIN,
				"the persisted /game profile selects the root's SCR policy")


func test_mission_text_effect_reaches_hud_objective() -> void:
	# Drained effects carry {kind, a..d, str} (NovaSimulation::drain_effects); the
	# WAC text/ptext family lands as kind=="text" with the string in "str". The
	# old handler read nonexistent "text"/"message" keys, so mission text never
	# reached the HUD.
	# The surface lives on the shared NovaGameHudHost (main_game passes through);
	# out-of-tree _make() never runs _ready, so drive the host directly.
	var host := NovaGameHudHost.new()
	autofree(host)
	host.apply_mission_effects([
		{"kind": "dialog", "a": 3},
		{"kind": "text", "str": "Proceed to the beach"},
		{"kind": "text", "str": ""},
	])
	assert_eq(host.hud_objective_line(), "Proceed to the beach",
		"kind=='text' effect drives the HUD objective line; empty/other kinds ignored")


func test_console_debug_text_does_not_reach_hud_objective() -> void:
	# consol/pconsol ride the distinct debug_text channel. The game does not yet
	# present an on-screen debug console, so these effects remain intentionally
	# unrouted instead of replacing player-facing mission text.
	var host := NovaGameHudHost.new()
	autofree(host)
	host.apply_mission_effects([
		{"kind": "text", "str": "Hold this position"},
		{"kind": "debug_text", "str": "trigger 17 entered"},
	])
	assert_eq(host.hud_objective_line(), "Hold this position",
		"debug_text stays off the player-facing HUD mission-text channel")


func test_crosshair_option_updates_an_existing_hud() -> void:
	# The Options signal reaches the built HUD through the shared host's public
	# set_crosshair_style (main_game delegates its _on_crosshair_style_changed there).
	var host := NovaGameHudHost.new()
	autofree(host)
	var hud := FakeGameHud.new()
	host._game_hud = hud
	host.set_crosshair_style(13)
	assert_eq(hud.crosshair_style, 13, "A paused game's HUD adopts the menu selection immediately.")


func test_hud_loads_text_for_the_mission_that_actually_started() -> void:
	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)

	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(root)
	assert_eq(world.mission_file, "", "normal SP/host/join starts do not populate the exported boot option")
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_eq(world.mission_file, "", "the exported boot option remains separate after a normal load")
	var runtime = world.get_runtime()
	assert_not_null(runtime)
	assert_eq(runtime.get_mission_file(), "mnml.bms",
			"the shared F3 runtime receives the mission that actually loaded")

	# The mission string table selection lives on the shared HUD host now (the
	# exists-only mission-bin fallback rides its world wiring).
	var host := NovaGameHudHost.new()
	autofree(host)
	host.setup(world, null, null)
	host._load_hud_text_tables(root)

	assert_not_null(NovaStrings.get_table("mission"),
		"mnml.bin exists and must be selected from the successfully loaded BMS; medmssn.bin is absent")


func _make_packed_shell(game_code: String):
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_root_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	var entries: Array = []
	var fixture_path := ProjectSettings.globalize_path(FIXTURE_DIR)
	for filename in DirAccess.get_files_at(fixture_path):
		var bytes := FileAccess.get_file_as_bytes(fixture_path.path_join(filename))
		assert_false(bytes.is_empty(), "%s is available in the committed fixture" % filename)
		entries.append({"name": filename, "bytes": bytes})
	entries.append({
		"name": POLICY_FILE,
		"bytes": _scr_wrap_default_key(POLICY_PLAIN.to_utf8_buffer(), 1),
	})
	_write_pff(_temp_dir.path_join("resource.pff"), entries)

	NovaResourceDirSettings.set_resource_dir(_temp_dir)
	NovaResourceDirSettings.set_expansion("")
	NovaResourceDirSettings.set_game(game_code)
	var shell = MainGameScene.instantiate()
	assert_not_null(shell)
	if shell == null:
		return null
	add_child(shell)
	await get_tree().process_frame
	return shell


func _u32(value: int) -> int:
	return value & 0xffff_ffff


func _rol32(value: int, shift: int) -> int:
	value = _u32(value)
	return _u32((value << shift) | (value >> (32 - shift)))


func _xor_scr_keystream(bytes: PackedByteArray, key: int) -> PackedByteArray:
	var out := bytes.duplicate()
	for index in range(out.size()):
		key = _u32(_rol32(_u32(key + _rol32(key, 11)), 4) ^ 1)
		out[index] = int(out[index]) ^ (key & 0xff)
	return out


func _scr_wrap_default_key(plain: PackedByteArray, version: int) -> PackedByteArray:
	var payload := _xor_scr_keystream(plain, SCR_KEY_DEFAULT)
	var out := PackedByteArray()
	out.resize(4 + payload.size())
	out[0] = 83 # S
	out[1] = 67 # C
	out[2] = 82 # R
	out[3] = version
	for index in range(payload.size()):
		out[4 + index] = payload[payload.size() - 1 - index]
	return out


# PFF3: 20-byte header, 36-byte entries with 16-byte names, then payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "the packed runtime fixture is writable")
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes: PackedByteArray = entry.bytes
		var name_bytes := String(entry.name).to_utf8_buffer()
		assert_true(name_bytes.size() <= 16, "%s fits the PFF name field" % entry.name)
		file.store_32(0)
		file.store_32(next_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry.bytes)
	file.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
