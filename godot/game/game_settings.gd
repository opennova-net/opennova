class_name NovaGameSettings
extends RefCounted

## Game-product persistence. This file is deliberately independent from ONED's
## authoring state: changing either product's resource root cannot affect the
## other product on its next launch.

const CONFIG_PATH := "user://game_settings.cfg"
const RESOURCES_SECTION := "resources"
const RESOURCE_DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const PLAYER_SECTION := "player"
const CROSSHAIR_STYLE_KEY := "crosshair_style"
const CROSSHAIR_STYLE_MIN := 0
const CROSSHAIR_STYLE_MAX := 24


static func get_resource_dir() -> String:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return ""
	var path := String(config.get_value(RESOURCES_SECTION, RESOURCE_DIR_KEY, ""))
	return path if is_valid_root(path) else ""


static func set_resource_dir(path: String) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(RESOURCES_SECTION, RESOURCE_DIR_KEY, path.strip_edges())
	config.save(CONFIG_PATH)


static func get_expansion() -> String:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return ""
	return String(config.get_value(RESOURCES_SECTION, EXPANSION_KEY, "")).strip_edges()


static func set_expansion(name: String) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(RESOURCES_SECTION, EXPANSION_KEY, name.strip_edges())
	config.save(CONFIG_PATH)


static func get_crosshair_style() -> int:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return CROSSHAIR_STYLE_MIN
	return clampi(
		int(config.get_value(PLAYER_SECTION, CROSSHAIR_STYLE_KEY, CROSSHAIR_STYLE_MIN)),
		CROSSHAIR_STYLE_MIN,
		CROSSHAIR_STYLE_MAX
	)


static func set_crosshair_style(style: int) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(
		PLAYER_SECTION,
		CROSSHAIR_STYLE_KEY,
		clampi(style, CROSSHAIR_STYLE_MIN, CROSSHAIR_STYLE_MAX)
	)
	config.save(CONFIG_PATH)


static func is_valid_root(path: String) -> bool:
	return NovaResourceRoot.is_valid_root(path)
