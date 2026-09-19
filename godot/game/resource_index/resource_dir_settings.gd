class_name ResourceDirSettings
extends RefCounted

## Persisted game and expansion selection. The data directory is CLI-only.

const CONFIG_PATH := "user://opennova.cfg"
const SECTION := "resources"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"


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
