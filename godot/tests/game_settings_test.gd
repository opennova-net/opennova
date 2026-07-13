extends GutTest

const GameSettings := preload("res://game/game_settings.gd")
const CONFIG_PATH := "user://game_settings.cfg"
const TEST_ROOT := "opennova_game_settings_test"

var _saved_config: Variant = null
var _base := ""


func before_each() -> void:
	_saved_config = FileAccess.get_file_as_bytes(CONFIG_PATH) if FileAccess.file_exists(CONFIG_PATH) else null
	if FileAccess.file_exists(CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	_base = OS.get_cache_dir().path_join(TEST_ROOT).path_join("run_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_base)


func after_each() -> void:
	if _saved_config is PackedByteArray:
		var file := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func test_resource_directory_and_expansion_round_trip() -> void:
	var root := _make_dir("retail_install")
	GameSettings.set_resource_dir("  %s  " % root)
	GameSettings.set_expansion("  jox01  ")
	assert_eq(GameSettings.get_resource_dir(), root)
	assert_eq(GameSettings.get_expansion(), "jox01")


func test_crosshair_defaults_round_trips_and_clamps() -> void:
	assert_eq(GameSettings.get_crosshair_style(), 0, "Absent style uses cross01.tga.")
	GameSettings.set_crosshair_style(17)
	assert_eq(GameSettings.get_crosshair_style(), 17)
	GameSettings.set_crosshair_style(99)
	assert_eq(GameSettings.get_crosshair_style(), 24, "Style is capped at cross25.tga.")
	GameSettings.set_crosshair_style(-4)
	assert_eq(GameSettings.get_crosshair_style(), 0, "Negative styles clamp to cross01.tga.")


func test_player_preference_write_preserves_game_resource_state() -> void:
	var root := _make_dir("retail_install")
	GameSettings.set_resource_dir(root)
	GameSettings.set_expansion("jox01")
	GameSettings.set_crosshair_style(8)
	assert_eq(GameSettings.get_resource_dir(), root)
	assert_eq(GameSettings.get_expansion(), "jox01")


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
