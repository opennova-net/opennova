extends GutTest

const GameSettings := preload("res://game/game_settings.gd")
const OnedSettings := preload("res://modtools/editor/oned_settings.gd")

const LEGACY_CONFIG_PATH := "user://terrain_editor_state.cfg"
const GAME_CONFIG_PATH := "user://game_settings.cfg"
const ONED_CONFIG_PATH := "user://oned_settings.cfg"
const TEST_ROOT := "opennova_settings_product_isolation_test"

var _saved_configs := {}
var _base := ""


func before_each() -> void:
	for path in [LEGACY_CONFIG_PATH, GAME_CONFIG_PATH, ONED_CONFIG_PATH]:
		_saved_configs[path] = FileAccess.get_file_as_bytes(path) if FileAccess.file_exists(path) else null
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_base = OS.get_cache_dir().path_join(TEST_ROOT).path_join("run_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_base)


func after_each() -> void:
	for path in [LEGACY_CONFIG_PATH, GAME_CONFIG_PATH, ONED_CONFIG_PATH]:
		if _saved_configs[path] is PackedByteArray:
			var file := FileAccess.open(path, FileAccess.WRITE)
			if file != null:
				file.store_buffer(_saved_configs[path])
				file.close()
		elif FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func test_products_ignore_the_legacy_shared_settings_file() -> void:
	var legacy_root := _make_dir("legacy")
	var legacy := ConfigFile.new()
	legacy.set_value("resources", "resource_dir", legacy_root)
	legacy.set_value("resources", "expansion", "jox99")
	legacy.set_value("player", "crosshair_style", 17)
	assert_eq(legacy.save(LEGACY_CONFIG_PATH), OK)

	assert_eq(GameSettings.get_resource_dir(), "")
	assert_eq(GameSettings.get_expansion(), "")
	assert_eq(GameSettings.get_crosshair_style(), 0)
	assert_eq(OnedSettings.get_resource_dir(), "")
	assert_eq(OnedSettings.get_expansion(), "")
	assert_true(FileAccess.file_exists(LEGACY_CONFIG_PATH), "The ignored legacy file is left in place.")


func test_game_settings_round_trip_without_touching_oned_state() -> void:
	var game_root := _make_dir("game")
	var oned := ConfigFile.new()
	oned.set_value("resources", "resource_dir", _make_dir("oned"))
	oned.set_value("layout", "left_split_offset", 123)
	assert_eq(oned.save(ONED_CONFIG_PATH), OK)

	GameSettings.set_resource_dir(game_root)
	GameSettings.set_expansion("  jox01  ")
	GameSettings.set_crosshair_style(99)

	assert_eq(GameSettings.get_resource_dir(), game_root)
	assert_eq(GameSettings.get_expansion(), "jox01")
	assert_eq(GameSettings.get_crosshair_style(), 24)
	var unchanged_oned := ConfigFile.new()
	assert_eq(unchanged_oned.load(ONED_CONFIG_PATH), OK)
	assert_eq(int(unchanged_oned.get_value("layout", "left_split_offset", -1)), 123)
	assert_false(FileAccess.file_exists(LEGACY_CONFIG_PATH))


func test_oned_settings_round_trip_without_touching_game_state() -> void:
	var oned_root := _make_dir("oned")
	var game := ConfigFile.new()
	game.set_value("resources", "resource_dir", _make_dir("game"))
	game.set_value("player", "crosshair_style", 12)
	assert_eq(game.save(GAME_CONFIG_PATH), OK)

	OnedSettings.set_resource_dir(oned_root)
	OnedSettings.set_expansion("  jox02  ")

	assert_eq(OnedSettings.get_resource_dir(), oned_root)
	assert_eq(OnedSettings.get_expansion(), "jox02")
	var unchanged_game := ConfigFile.new()
	assert_eq(unchanged_game.load(GAME_CONFIG_PATH), OK)
	assert_eq(int(unchanged_game.get_value("player", "crosshair_style", -1)), 12)
	assert_false(FileAccess.file_exists(LEGACY_CONFIG_PATH))


func test_oned_editor_state_sections_coexist() -> void:
	var root := _make_dir("oned")
	OnedSettings.set_resource_dir(root)
	OnedSettings.save_layout_state(123, -207)
	OnedSettings.save_browser_state(true, 311)
	OnedSettings.save_panel_state("environment", false, Rect2i(-5, -7, 400, 600))
	OnedSettings.save_view_state(false, true)
	OnedSettings.save_terrain_paths("C:/open", "C:/save", "C:/export")
	OnedSettings.set_mcp_enabled(false)
	OnedSettings.set_mcp_port(9123)

	assert_eq(OnedSettings.get_resource_dir(), root)
	assert_eq(OnedSettings.load_layout_state().left, 123)
	assert_eq(OnedSettings.load_browser_state().split, 311)
	assert_eq(OnedSettings.load_panel_state("environment").rect, Rect2i(-5, -7, 400, 600))
	assert_eq(OnedSettings.load_view_state(), {"grid": false, "axes": true})
	assert_eq(OnedSettings.load_terrain_paths(), {"open": "C:/open", "save": "C:/save", "export": "C:/export"})
	assert_false(OnedSettings.get_mcp_enabled())
	assert_eq(OnedSettings.get_mcp_port(), 9123)


func _make_dir(name: String) -> String:
	var path := _base.path_join(name)
	DirAccess.make_dir_recursive_absolute(path)
	return path


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
