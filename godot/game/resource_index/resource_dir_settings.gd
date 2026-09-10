class_name ResourceDirSettings
extends RefCounted

## The runtime's persisted resource directory and player preferences. The editor's
## machine-local paths live separately in Godot project metadata.

const CONFIG_PATH := "user://opennova.cfg"
const SECTION := "resources"
const DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"


## The persisted resource directory, or "" when unset / no longer a valid dir.
static func get_resource_dir() -> String:
	var dir := String(ConfigStore.read(CONFIG_PATH, SECTION, DIR_KEY, ""))
	return dir if is_valid_root(dir) else ""


## Persist the runtime resource directory, preserving other config sections.
static func set_resource_dir(path: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, DIR_KEY, path.strip_edges())


## The persisted expansion name (e.g. "jox01"), or "" for the base game.
static func get_expansion() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


## Persist the expansion name, preserving any other sections in the config.
static func set_expansion(name: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


## The persisted game code (e.g. "jodemo"), or "jo" when unset. Selects the SCR decode
## key. A `/game` launch flag overrides this (see LaunchFlags.game).
static func get_game() -> String:
	var code := String(ConfigStore.read(
			CONFIG_PATH, SECTION, GAME_KEY, "jo")).strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


## Persist the game code, preserving any other sections in the config.
static func set_game(code: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


## A resource library is a real asset directory on disk, never inside the app's
## own user-data dir (rejects leaked temp/test paths).
static func is_valid_root(path: String) -> bool:
	return ResourceRoot.is_valid_root(path)


## A case/slash-insensitive comparison key for a directory path. Mirrors the C++
## ResourceRoot::normalize_dir rule (which is not bound to GDScript) so
## "D:\Game", "D:/Game/" and "d:/game" collapse to one entry. Comparison only:
## the original stripped path is what gets stored and displayed.
static func canonical_key(path: String) -> String:
	return path.strip_edges().replace("\\", "/").rstrip("/").to_lower()
