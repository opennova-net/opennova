class_name NovaResourceDirSettings
extends RefCounted

## Shared persistence for the OpenNova resource/asset directory — the on-disk
## folder of original .3di models and their textures. The editor's resource
## browser (modtools/editor/resource_library.gd) and the runtime
## (game/main_game.gd) read/write the SAME user:// config, so a directory picked
## in either app is shared. This lives under engine/ (not modtools/) because the
## runtime export excludes modtools/* and still needs the path + keys.

const CONFIG_PATH := "user://terrain_editor_state.cfg"
const SECTION := "resources"
const DIR_KEY := "resource_dir"
const RECURSIVE_KEY := "resource_recursive"


## The persisted resource directory, or "" when unset / no longer a valid dir.
static func get_resource_dir() -> String:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return ""
	var dir := String(config.get_value(SECTION, DIR_KEY, ""))
	return dir if is_valid_root(dir) else ""


static func get_recursive() -> bool:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return true
	return bool(config.get_value(SECTION, RECURSIVE_KEY, true))


## Persist the resource directory, preserving any other sections in the config.
static func set_resource_dir(path: String, recursive: bool = true) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(SECTION, DIR_KEY, path.strip_edges())
	config.set_value(SECTION, RECURSIVE_KEY, recursive)
	config.save(CONFIG_PATH)


## A resource library is a real asset directory on disk, never inside the app's
## own user-data dir (rejects leaked temp/test paths).
static func is_valid_root(path: String) -> bool:
	var trimmed := path.strip_edges()
	if trimmed.is_empty():
		return false
	if not DirAccess.dir_exists_absolute(trimmed):
		return false
	if trimmed.begins_with(OS.get_user_data_dir()):
		return false
	return true
